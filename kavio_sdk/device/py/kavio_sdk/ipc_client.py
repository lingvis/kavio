"""
Kavio IPC Client - Python SDK (updated protocol)

Python port of KavioIPCClient (C++) matching the updated kavio_sdk protocol.
Handles Unix Domain Socket IPC, protocol handshake, SHM frame read/write,
and asynchronous message handling.
"""

from __future__ import annotations

import errno
import logging
import os
import select
import socket
import struct
import threading
import time
from datetime import datetime
from typing import Any, Callable, Dict, List, Optional, Union

from .proto_py.ipc_pb2 import (
    # Header
    IPCHeaderPb,
    # Enums
    MessageTypePb,
    PixelFormatPb,
    StatusCodePb,
    # Messages
    ReqSessionCreatePb,
    NtfSessionCreatedPb,
    ReqCloseSessionPb,
    RxParamsPb,
    TxParamsPb,
    ReqSetRxParamsPb,
    ReqSetTxParamsPb,
    NtfStatusReplyPb,
    NtfRxShmReadyPb,
    NtfRxShmInvalidatedPb,
    ReqCreateTxShmPb,
    NtfTxShmCreatedPb,
    ReqDestroyTxShmPb,
    NtfTxShmDestroyedPb,
    ReqQueryRuntimeDataPb,
    ReqQueryUserConfigPb,
    ReqQueryStatsPb,
    NtfStatsReplyPb,
    IPCPayloadPb,
    IPCPackagePb,
    ErrorPb,
    GlobalRuntimeStatsPb,
    NtfCommChannelDataPb,
    NtfUsbHostOnlineStatePb,
    NtfCommPeerStatePb,
    NtfAppConfigRequestPb,
    NtfAppConfigResponsePb,
    AppConfigOperationPb,
    # Shared memory
    ShmInfoPb,
)
from .proto_py.input_pb2 import InputEventPb

from . import types
from .sdk_version import (
    SDK_VERSION_MAJOR,
    SDK_VERSION_MINOR,
    SDK_VERSION_PATCH,
)
from .frame_shm import FrameShm

# Header format: 4 bytes little-endian payload length
HEADER_FMT = "<I"
HEADER_SIZE = struct.calcsize(HEADER_FMT)

# Track ID generation
_track_counter = 0

logger = logging.getLogger("kavio.ipc_client")


def _next_track_id() -> int:
    global _track_counter
    _track_counter = (_track_counter + 1) & 0xFFFF
    return _track_counter


class IPCClient:
    """
    Kavio IPC Client for the updated protocol.

    Usage:
        client = IPCClient()
        client.connect(client_options, session_options, callbacks)
        client.set_rx_params(types.RxParams(...))
        client.set_tx_params(types.TxParams(...))
        client.create_tx_shm(1920, 1080, PixelFormatPb.BGRA8888)
        client.write_frame(data)
        client.disconnect()
    """

    class NetworkCallbacks:
        """Callback interface for async network events."""
        def __init__(
            self,
            on_initialized: Optional[Callable[[types.Status, types.SessionInfo], None]] = None,
            on_message: Optional[Callable[[IPCPackagePb], None]] = None,
            on_error: Optional[Callable[[types.Status], None]] = None,
            on_disconnected: Optional[Callable[[], None]] = None,
            on_rx_shm_ready: Optional[Callable[[types.ShmInfo, int], None]] = None,
            on_rx_shm_invalidated: Optional[Callable[..., None]] = None,
            on_stats_reply: Optional[Callable[[types.QueryStatsResult], None]] = None,
            on_comm_channel_data: Optional[Callable[[bytes], None]] = None,
            on_usb_online: Optional[Callable[[bool, str], None]] = None,
            on_comm_channel_ready_changed: Optional[Callable[[bool], None]] = None,
            on_runtime_data: Optional[Callable[[Any], None]] = None,
            on_user_config: Optional[Callable[[Any], None]] = None,
            on_app_config_request: Optional[Callable[[NtfAppConfigRequestPb], None]] = None,
        ):
            self.on_initialized = on_initialized
            self.on_message = on_message
            self.on_error = on_error
            self.on_disconnected = on_disconnected
            self.on_rx_shm_ready = on_rx_shm_ready
            self.on_rx_shm_invalidated = on_rx_shm_invalidated
            self.on_stats_reply = on_stats_reply
            self.on_comm_channel_data = on_comm_channel_data
            self.on_usb_online = on_usb_online
            self.on_comm_channel_ready_changed = on_comm_channel_ready_changed
            self.on_runtime_data = on_runtime_data
            self.on_user_config = on_user_config
            self.on_app_config_request = on_app_config_request

    def __init__(self):
        # Network
        self._socket: Optional[socket.socket] = None
        self._rx_thread: Optional[threading.Thread] = None
        self._rx_running = False
        self._send_lock = threading.Lock()

        # Protocol state
        self._state: types.ProtocolState = types.ProtocolState.IDLE
        self._state_lock = threading.Lock()
        self._state_event = threading.Event()

        # Session info
        self._session_info = types.SessionInfo()
        self._usb_host_online = False
        self._comm_peer_online = False
        self._session_options: Optional[types.SessionOptions] = None
        self._cbs: Optional[IPCClient.NetworkCallbacks] = None
        self._client_options: Optional[types.ClientOptions] = None

        # RX frame
        self._rx_shm: Optional[FrameShm] = None
        self._rx_shm_info: Optional[types.ShmInfo] = None
        self._rx_generation: int = 0

        # TX SHM tracking
        self._tx_shms: Dict[int, types.ShmRegionInfo] = {}
        self._tx_frame_shms: Dict[int, FrameShm] = {}
        self._pending_tx_events: Dict[int, threading.Event] = {}

        # Tracked requests
        self._pending_requests: Dict[int, threading.Event] = {}
        self._tracked_results: Dict[int, Any] = {}

    # ─── Public API ────────────────────────────────────────────────────────

    def connect(
        self,
        client_options: types.ClientOptions,
        session_options: types.SessionOptions,
        callbacks: NetworkCallbacks,
    ) -> types.Status:
        """
        Connect to the Kavio server and create a session.

        This is a blocking call. For the full async flow, check state via
        get_state() after connect() returns OK.
        """
        self._client_options = client_options
        self._session_options = session_options
        self._cbs = callbacks

        # 1. Connect socket
        status = self._connect_socket(client_options.socket_path)
        if not status.ok():
            self._set_state(types.ProtocolState.ERROR)
            return status

        # 2. Start receive thread before sending any messages
        self._rx_running = True
        self._rx_thread = threading.Thread(target=self._recv_loop, daemon=True)
        self._rx_thread.start()

        # 3. Send session create request. Register the tracked waiter BEFORE
        # sending so an immediate server error cannot race with waiter setup.
        # The Platform echoes the same track_id for both MSG_NTF_SESSION_CREATED
        # and MSG_ERROR, therefore one tracked result handles both outcomes.
        self._set_state(types.ProtocolState.SEND_SESSION_CREATE)
        req = ReqSessionCreatePb()
        req.client_name = session_options.client_name
        req.client_pid = session_options.client_pid or os.getpid()
        req.sdk_version.major = SDK_VERSION_MAJOR
        req.sdk_version.minor = SDK_VERSION_MINOR
        req.sdk_version.patch = SDK_VERSION_PATCH
        req.rx_params.enabled = session_options.rx_params.enabled
        req.rx_params.frame_sample_interval = session_options.rx_params.frame_sample_interval
        req.tx_params.enabled = session_options.tx_params.enabled
        req.tx_params.z_order = session_options.tx_params.z_order

        track_id = _next_track_id()
        event = threading.Event()
        self._pending_requests[track_id] = event
        self._tracked_results.pop(track_id, None)
        self._set_state(types.ProtocolState.WAIT_SESSION_CREATED)

        status = self._send_msg(
            MessageTypePb.MSG_REQ_SESSION_CREATE, req, track_id
        )
        if not status.ok():
            self._pending_requests.pop(track_id, None)
            self._tracked_results.pop(track_id, None)
            self._disconnect_internal()
            return status

        # 4. Wait for either NtfSessionCreatedPb or ErrorPb carrying track_id.
        if not event.wait(client_options.request_timeout_ms / 1000.0):
            self._pending_requests.pop(track_id, None)
            self._tracked_results.pop(track_id, None)
            self._disconnect_internal()
            return types.Status.Error(
                types.StatusCode.PROTOCOL_ERROR,
                f"Timeout waiting for session create reply, track_id={track_id}",
            )

        self._pending_requests.pop(track_id, None)
        result = self._tracked_results.pop(track_id, None)
        if isinstance(result, types.Status):
            self._disconnect_internal()
            return result
        if not isinstance(result, NtfSessionCreatedPb):
            self._disconnect_internal()
            return types.Status.Error(
                types.StatusCode.PROTOCOL_ERROR,
                f"Connection closed before session create completed, track_id={track_id}",
            )

        ntf: NtfSessionCreatedPb = result
        self._session_info.session_id = ntf.session_id
        self._session_info.max_sessions = ntf.max_sessions
        self._session_info.max_rx_shms_per_session = ntf.max_rx_shms_per_session
        self._session_info.max_tx_shms_per_session = ntf.max_tx_shms_per_session
        self._session_info.comm_channel_id = ntf.comm_channel_id
        self._session_info.comm_peer_online = ntf.comm_peer_online
        self._comm_peer_online = bool(ntf.comm_peer_online)

        # The server may normalize the requested values. Keep the applied
        # values as the current session options, matching the C++ SDK.
        self._session_options.rx_params = types.RxParams(
            enabled=ntf.rx_params.enabled,
            frame_sample_interval=ntf.rx_params.frame_sample_interval,
        )
        self._session_options.tx_params = types.TxParams(
            enabled=ntf.tx_params.enabled,
            z_order=ntf.tx_params.z_order,
        )

        # RX SHM readiness is asynchronous and is reported through
        # on_rx_shm_ready. Session creation must not wait for a video signal.
        self._set_state(types.ProtocolState.INITIALIZED)

        if session_options.auto_query_runtime_on_connect:
            self.query_runtime_data()
        if session_options.auto_query_user_config_on_connect:
            self.query_user_config()

        # Notify callbacks
        if self._cbs and self._cbs.on_initialized:
            self._cbs.on_initialized(
                types.Status.Ok(), self.get_session_info()
            )
        return types.Status.Ok()

    def disconnect(self):
        """Disconnect from the server."""
        if self._state == types.ProtocolState.DISCONNECTED:
            return
        self._rx_running = False
        self._disconnect_internal()
        self._set_state(types.ProtocolState.DISCONNECTED)

    def get_state(self) -> types.ProtocolState:
        """Get current protocol state."""
        return self._state

    def get_session_info(self) -> types.SessionInfo:
        """Get session information."""
        return self._session_info

    def get_comm_channel_id(self) -> int:
        """Return the stable ChannelID bound to this App session."""
        return int(self._session_info.comm_channel_id)

    def is_comm_channel_ready(self) -> bool:
        """Return whether USB Host and the peer Host Component are both online."""
        return bool(
            self._usb_host_online
            and self._session_info.comm_channel_id != 0
            and self._comm_peer_online
        )

    def get_rx_shm_info(self) -> Optional[types.ShmInfo]:
        """Get RX SHM info if available."""
        return self._rx_shm_info

    # ─── Session Params ────────────────────────────────────────────────────

    def set_rx_params(
        self, rx_params: types.RxParams, track_id_out: Optional[List[int]] = None
    ) -> types.Status:
        """Set the complete RX parameter set and wait for the status reply."""
        if rx_params.frame_sample_interval < 1:
            return types.Status.Error(
                types.StatusCode.INVALID_ARGUMENT,
                "frame_sample_interval must be >= 1",
            )

        req = ReqSetRxParamsPb()
        req.params.enabled = rx_params.enabled
        req.params.frame_sample_interval = rx_params.frame_sample_interval
        status = self._send_and_wait_status(
            MessageTypePb.MSG_REQ_SET_RX_PARAMS, req, track_id_out
        )
        if status.ok() and self._session_options is not None:
            self._session_options.rx_params = rx_params
            if not rx_params.enabled:
                if self._rx_shm:
                    try:
                        self._rx_shm.close()
                    except Exception:
                        pass
                self._rx_shm = None
                self._rx_shm_info = None
                self._session_info.rx_shm = None
        return status

    def set_tx_params(
        self, tx_params: types.TxParams, track_id_out: Optional[List[int]] = None
    ) -> types.Status:
        """Set the complete TX parameter set and wait for the status reply."""
        req = ReqSetTxParamsPb()
        req.params.enabled = tx_params.enabled
        req.params.z_order = tx_params.z_order
        status = self._send_and_wait_status(
            MessageTypePb.MSG_REQ_SET_TX_PARAMS, req, track_id_out
        )
        if status.ok() and self._session_options is not None:
            self._session_options.tx_params = tx_params
        return status

    # ─── TX SHM ────────────────────────────────────────────────────────────

    def create_tx_shm(
        self, width: int, height: int, pixel_format: types.PixelFormat
    ) -> types.Status:
        """Request a TX shared memory region."""
        req = ReqCreateTxShmPb()
        req.width = width
        req.height = height
        req.pixel_format = int(pixel_format)

        status = self._send_msg(
            MessageTypePb.MSG_REQ_CREATE_TX_SHM, req, _next_track_id()
        )
        if not status.ok():
            return status

        # Wait for NtfTxShmCreatedPb
        result = self._wait_for_message(
            MessageTypePb.MSG_NTF_TX_SHM_CREATED,
            timeout_ms=self._client_options.request_timeout_ms
            if self._client_options
            else 3000,
        )
        if result is None:
            return types.Status.Error(
                types.StatusCode.PROTOCOL_ERROR, "Timeout waiting for TX SHM created"
            )

        ntf: NtfTxShmCreatedPb = result
        shm_pb = ntf.tx_shm
        shm_info = self._register_tx_shm(shm_pb)
        self._session_info.tx_shms.append(shm_info)
        return types.Status.Ok()

    def destroy_tx_shm(self, shm_id: int) -> types.Status:
        """Destroy a TX shared memory region."""
        req = ReqDestroyTxShmPb()
        req.shm_id = shm_id
        return self._send_msg(
            MessageTypePb.MSG_REQ_DESTROY_TX_SHM, req, _next_track_id()
        )

    def wait_for_tx_shm(self, timeout_s: float = 5.0) -> bool:
        """Wait for any pending TX SHM creation to complete."""
        deadline = time.time() + timeout_s
        while time.time() < deadline:
            for shm_id, event in list(self._pending_tx_events.items()):
                if event.wait(timeout=0.1):
                    return True
            time.sleep(0.01)
        return False

    def write_tx_frame(
        self,
        shm_id: int,
        data: Union[bytes, bytearray],
        width: Optional[int] = None,
        height: Optional[int] = None,
        stride: Optional[int] = None,
        valid_rect: Optional[types.Rect] = None,
        frame_id: int = 0,
        timestamp_ns: int = 0,
        dest_pos_x: int = 0,
        dest_pos_y: int = 0,
    ) -> types.Status:
        """Write a frame to a TX shared memory region via double-buffer protocol."""
        frame_shm = self._tx_frame_shms.get(shm_id)
        if not frame_shm or not frame_shm.is_valid():
            return types.Status.Error(
                types.StatusCode.NOT_FOUND,
                f"TX SHM {shm_id} not found or invalid"
            )
        try:
            region = self._tx_shms.get(shm_id)
            if region is None:
                return types.Status.Error(
                    types.StatusCode.NOT_FOUND, f"TX SHM {shm_id} metadata not found"
                )

            frame_width = region.width if width is None else width
            frame_height = region.height if height is None else height
            frame_stride = region.stride if stride is None else stride
            if valid_rect is None or valid_rect.width == 0 or valid_rect.height == 0:
                valid_rect = types.Rect(0, 0, frame_width, frame_height)

            ok = frame_shm.write_latest(
                bytes(data), frame_id, timestamp_ns,
                frame_width, frame_height, frame_stride, region.pixel_format,
                valid_rect=valid_rect,
                dest_pos_x=dest_pos_x, dest_pos_y=dest_pos_y,
            )
            if not ok:
                return types.Status.Error(
                    types.StatusCode.SHM_ERROR,
                    "write_latest failed (no writable buffer)"
                )
        except Exception as e:
            return types.Status.Error(
                types.StatusCode.SHM_ERROR, f"write_latest exception: {e}"
            )
        return types.Status.Ok()

    def clear_tx_frame(self, shm_id: int) -> types.Status:
        """Clear a TX SHM frame — writes empty payload with valid=0 via double-buffer protocol."""
        frame_shm = self._tx_frame_shms.get(shm_id)
        if not frame_shm or not frame_shm.is_valid():
            return types.Status.Error(
                types.StatusCode.NOT_FOUND,
                f"TX SHM {shm_id} not found or invalid"
            )
        try:
            frame_shm.clear_all_buffers()
        except Exception as e:
            return types.Status.Error(
                types.StatusCode.SHM_ERROR, f"clear_all_buffers exception: {e}"
            )
        return types.Status.Ok()

    # ─── RX Frame ─────────────────────────────────────────────────────────

    def read_latest_rx_frame(self) -> Optional[types.ImageFrame]:
        """Read the latest RX frame from shared memory (non-blocking).

        Returns an ImageFrame with frame metadata and pixel data.
        """
        if not self._rx_shm or not self._rx_shm_info:
            return None
        try:
            view = self._rx_shm.read_latest()
            if view is None:
                return None
        except Exception:
            return None

        try:
            frame = types.ImageFrame(
                frame_id=view.frame_id,
                timestamp_ns=view.timestamp_ns,
                data=view.payload,
                width=view.width,
                height=view.height,
                stride=view.stride,
                pixel_format=view.pixel_format,
                valid_rect=view.valid_rect,
                dest_pos_x=view.dest_pos_x,
                dest_pos_y=view.dest_pos_y,
            )
            return frame
        finally:
            self._rx_shm.end_read(view.index)

    # ─── Query ────────────────────────────────────────────────────────────

    def query_stats(self, track_id_out: Optional[List[int]] = None) -> types.Status:
        """Request server global stats."""
        track_id = _next_track_id()
        if track_id_out is not None:
            track_id_out.append(track_id)

        req = ReqQueryStatsPb()
        return self._send_msg(MessageTypePb.MSG_REQ_QUERY_STATS, req, track_id)

    def query_runtime_data(
        self, track_id_out: Optional[List[int]] = None
    ) -> types.Status:
        """Request runtime data from the server."""
        track_id = _next_track_id()
        if track_id_out is not None:
            track_id_out.append(track_id)

        req = ReqQueryRuntimeDataPb()
        return self._send_msg(
            MessageTypePb.MSG_REQ_QUERY_RUNTIME_DATA, req, track_id
        )

    def query_user_config(
        self, track_id_out: Optional[List[int]] = None
    ) -> types.Status:
        """Request the current user configuration snapshot."""
        track_id = _next_track_id()
        if track_id_out is not None:
            track_id_out.append(track_id)
        return self._send_msg(
            MessageTypePb.MSG_REQ_QUERY_USER_CONFIG, ReqQueryUserConfigPb(), track_id
        )

    # ─── Input ─────────────────────────────────────────────────────────────

    def send_input_event(self, event: InputEventPb) -> types.Status:
        """Send an input event to the server."""
        return self._send_msg(
            MessageTypePb.MSG_REQ_INPUT_EVENT, event, _next_track_id()
        )

    def send_comm_channel_data(self, payload: bytes) -> types.Status:
        """Send one raw datagram through the Channel bound to this App."""
        channel_id = int(self._session_info.comm_channel_id)
        if not payload:
            return types.Status.Error(types.StatusCode.INVALID_ARGUMENT, "comm channel payload is empty")
        if not self.is_comm_channel_ready():
            return types.Status.Error(types.StatusCode.INVALID_STATE, "app comm channel is not ready")
        logger.debug(
            "[comm:out] channel_id=%d len=%d hex=%s",
            channel_id, len(payload),
            payload.hex() if len(payload) <= 256 else payload[:256].hex() + "...",
        )
        data = NtfCommChannelDataPb()
        data.channel_id = channel_id
        data.payload = payload
        return self._send_msg(
            MessageTypePb.MSG_NTF_COMM_CHANNEL_DATA, data, _next_track_id()
        )

    def send_app_config_response(
        self, response: NtfAppConfigResponsePb
    ) -> types.Status:
        """Return the result of a app configuration GET or SET request."""
        if response.request_id == 0:
            return types.Status.Error(
                types.StatusCode.INVALID_ARGUMENT, "request_id must not be 0"
            )
        if response.operation not in (
            AppConfigOperationPb.APP_CONFIG_OPERATION_GET,
            AppConfigOperationPb.APP_CONFIG_OPERATION_SET,
        ):
            return types.Status.Error(
                types.StatusCode.INVALID_ARGUMENT, "invalid app config operation"
            )
        if len(response.config_json) > 1024 * 1024:
            return types.Status.Error(
                types.StatusCode.INVALID_ARGUMENT,
                "app config JSON exceeds 1 MiB",
            )
        return self._send_msg(
            MessageTypePb.MSG_NTF_APP_CONFIG_RESPONSE,
            response,
            response.request_id,
        )

    # ─── Internal: Socket ─────────────────────────────────────────────────

    @staticmethod
    def _parse_inet(address: str):
        """Return (host, port) if *address* is a host:port string, else None."""
        if not address or ":" not in address:
            return None
        host, _, port_str = address.rpartition(":")
        # Strip brackets from IPv6 literal like [::1]
        if host.startswith("[") and host.endswith("]"):
            host = host[1:-1]
        try:
            port = int(port_str)
        except ValueError:
            return None
        return host, port

    def _connect_socket(self, socket_path: str) -> types.Status:
        """Connect via Unix domain socket or INET (TCP) socket.

        When *socket_path* is a ``host:port`` string, an INET connection is
        established. Otherwise a Unix domain socket is used.
        """
        inet = self._parse_inet(socket_path)
        try:
            if inet:
                host, port = inet
                self._socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                self._socket.settimeout(10.0)
                self._socket.connect((host, port))
            else:
                self._socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                self._socket.settimeout(10.0)
                self._socket.connect(socket_path)
            self._socket.settimeout(None)
            return types.Status.Ok()
        except FileNotFoundError:
            return types.Status.Error(
                types.StatusCode.SOCKET_ERROR,
                f"Socket not found: {socket_path}",
            )
        except ConnectionRefusedError:
            return types.Status.Error(
                types.StatusCode.SOCKET_ERROR,
                f"Connection refused: {socket_path}",
            )
        except socket.gaierror as e:
            return types.Status.Error(
                types.StatusCode.SOCKET_ERROR,
                f"Host resolution failed: {socket_path} — {e}",
            )
        except Exception as e:
            return types.Status.Error(
                types.StatusCode.SOCKET_ERROR, str(e)
            )

    def _disconnect_internal(self):
        """Internal disconnect without state change."""
        self._rx_running = False
        sock = self._socket
        self._socket = None
        if sock:
            try:
                sock.shutdown(socket.SHUT_RDWR)
            except Exception:
                pass
            try:
                sock.close()
            except Exception:
                pass
        # Wait for recv thread to exit (skip if we are the recv thread).
        rx_thread = self._rx_thread
        if rx_thread and rx_thread.is_alive() and rx_thread is not threading.current_thread():
            rx_thread.join(timeout=2.0)
        self._rx_thread = None
        # Cleanup SHM
        if self._rx_shm:
            try:
                self._rx_shm.close()
            except Exception:
                pass
            self._rx_shm = None
        for region in self._tx_shms.values():
            if region and region.fd >= 0:
                try:
                    os.close(region.fd)
                except Exception:
                    pass
        self._tx_shms.clear()
        for frame_shm in self._tx_frame_shms.values():
            try:
                frame_shm.close()
            except Exception:
                pass
        self._tx_frame_shms.clear()
        # Wake up any waiters
        for ev in self._pending_requests.values():
            ev.set()
        for ev in self._pending_tx_events.values():
            ev.set()

    def _set_state(self, state: types.ProtocolState):
        with self._state_lock:
            self._state = state
            logger.debug("State → %s", state.name)

    # ─── Internal: Message I/O ─────────────────────────────────────────────

    def _send_msg(
        self,
        msg_type: MessageTypePb.ValueType,
        payload: Any,
        track_id: int = 0,
    ) -> types.Status:
        """Serialize and send a message with IPCHeader."""
        if not self._socket:
            return types.Status.Error(
                types.StatusCode.SOCKET_ERROR, "Not connected"
            )

        try:
            # Build header
            header = IPCHeaderPb()
            header.msg_type = msg_type
            header.track_id = track_id

            # Build package
            pkg = IPCPackagePb()
            pkg.header.CopyFrom(header)
            _set_payload(pkg.payload, msg_type, payload)

            data = pkg.SerializeToString()
            logger.debug("[mcp:wire] pkg_size=%d hex=%s", len(data), data.hex() if len(data) <= 256 else data[:256].hex() + "...")

            # Send: 4-byte length prefix + data
            with self._send_lock:
                header_bytes = struct.pack(HEADER_FMT, len(data))
                self._socket.sendall(header_bytes + data)

            return types.Status.Ok()
        except Exception as e:
            logger.error("Send error: %s", e)
            return types.Status.Error(
                types.StatusCode.SOCKET_ERROR, f"Send failed: {e}"
            )

    def _send_and_wait_status(
        self,
        msg_type: int,
        payload: Any,
        track_id_out: Optional[List[int]] = None,
    ) -> types.Status:
        """Send a tracked request and wait for NtfStatusReplyPb or ErrorPb."""
        track_id = _next_track_id()
        if track_id_out is not None:
            track_id_out.append(track_id)

        event = threading.Event()
        self._pending_requests[track_id] = event
        self._tracked_results.pop(track_id, None)

        status = self._send_msg(msg_type, payload, track_id)
        if not status.ok():
            self._pending_requests.pop(track_id, None)
            return status

        timeout_ms = (
            self._client_options.request_timeout_ms
            if self._client_options is not None
            else 3000
        )
        if not event.wait(timeout_ms / 1000.0):
            self._pending_requests.pop(track_id, None)
            self._tracked_results.pop(track_id, None)
            return types.Status.Error(
                types.StatusCode.PROTOCOL_ERROR,
                f"Timeout waiting for status reply, track_id={track_id}",
            )

        self._pending_requests.pop(track_id, None)
        result = self._tracked_results.pop(track_id, None)
        if isinstance(result, types.Status):
            return result
        return types.Status.Error(
            types.StatusCode.PROTOCOL_ERROR,
            f"Invalid status reply, track_id={track_id}",
        )

    def _recv_loop(self):
        """Background receive thread."""
        buf = b""
        while self._rx_running:
            sock = self._socket
            if not sock:
                return
            try:
                # Check if socket is readable
                ready, _, _ = select.select([sock], [], [], 0.5)
                if not ready:
                    continue

                chunk = sock.recv(4096)
                if not chunk:
                    if self._rx_running:
                        logger.warning("Connection closed by server")
                        self._on_disconnected()
                    return

                buf += chunk
                while len(buf) >= HEADER_SIZE:
                    payload_len = struct.unpack(HEADER_FMT, buf[:HEADER_SIZE])[0]
                    total = HEADER_SIZE + payload_len
                    if len(buf) < total:
                        break

                    payload_data = buf[HEADER_SIZE:total]
                    buf = buf[total:]

                    try:
                        pkg = IPCPackagePb()
                        pkg.ParseFromString(payload_data)
                        self._handle_msg(pkg)
                    except Exception as e:
                        logger.error("Parse error: %s", e, exc_info=True)

            except (OSError, socket.error, ValueError, AttributeError) as e:
                if isinstance(e, (socket.timeout, ValueError)):
                    continue
                if hasattr(e, "errno") and e.errno in (
                    errno.EAGAIN,
                    errno.EWOULDBLOCK,
                    errno.EINTR,
                    errno.EBADF,  # socket closed during shutdown
                ):
                    continue
                logger.error("Recv error: %s", e)
                if self._rx_running:
                    self._on_disconnected()
                return

    def _handle_msg(self, pkg: IPCPackagePb):
        """Dispatch received message."""
        msg_type = pkg.header.msg_type
        track_id = pkg.header.track_id
        payload = pkg.payload

        logger.debug("Recv msg_type=0x%02X track_id=%d", msg_type, track_id)

        try:
            if msg_type == MessageTypePb.MSG_ERROR:
                self._on_error(payload.error)
                return

            if msg_type == MessageTypePb.MSG_NTF_STATUS_REPLY:
                self._on_status_reply(payload.ntf_status_reply, track_id)
                return

            if msg_type == MessageTypePb.MSG_NTF_SESSION_CREATED:
                # Session creation is a tracked request. Resolve by track_id so
                # success and MSG_ERROR share the same race-free wait path.
                if track_id in self._pending_requests:
                    self._tracked_results[track_id] = payload.ntf_session_created
                    self._pending_requests[track_id].set()
                else:
                    # Keep the generic notification path for callers that are not
                    # using the tracked connect handshake.
                    self._set_pending_result(
                        MessageTypePb.MSG_NTF_SESSION_CREATED,
                        payload.ntf_session_created,
                    )
                return

            if msg_type == MessageTypePb.MSG_NTF_RX_SHM_READY:
                self._on_rx_shm_ready(payload.ntf_rx_shm_ready)
                self._set_pending_result(
                    MessageTypePb.MSG_NTF_RX_SHM_READY, payload.ntf_rx_shm_ready
                )
                return

            if msg_type == MessageTypePb.MSG_NTF_RX_SHM_INVALIDATED:
                self._on_rx_shm_invalidated(payload.ntf_rx_shm_invalidated)
                return

            if msg_type == MessageTypePb.MSG_NTF_TX_SHM_CREATED:
                self._set_pending_result(
                    MessageTypePb.MSG_NTF_TX_SHM_CREATED,
                    payload.ntf_tx_shm_created,
                )
                return

            if msg_type == MessageTypePb.MSG_NTF_TX_SHM_DESTROYED:
                shm_id = payload.ntf_tx_shm_destroyed.shm_id
                if shm_id in self._tx_shms:
                    region = self._tx_shms.pop(shm_id)
                    if region and region.fd >= 0:
                        try:
                            os.close(region.fd)
                        except Exception:
                            pass
                # Remove from session_info tx_shms list
                self._session_info.tx_shms = [
                    s for s in self._session_info.tx_shms if s.id != shm_id
                ]
                return

            if msg_type == MessageTypePb.MSG_NTF_RUNTIME_DATA:
                if self._cbs and self._cbs.on_runtime_data:
                    self._cbs.on_runtime_data(payload.ntf_runtime_data)
                return

            if msg_type == MessageTypePb.MSG_NTF_USER_CONFIG:
                if self._cbs and self._cbs.on_user_config:
                    self._cbs.on_user_config(payload.ntf_user_config.user_config)
                return

            if msg_type == MessageTypePb.MSG_NTF_STATS_REPLY:
                self._on_stats_reply(payload.ntf_stats_reply)
                return

            if msg_type == MessageTypePb.MSG_NTF_COMM_CHANNEL_DATA:
                ntf = payload.ntf_comm_channel_data
                if ntf.channel_id != self._session_info.comm_channel_id:
                    return
                if self._cbs and self._cbs.on_comm_channel_data:
                    raw = ntf.payload
                    logger.debug(
                        "[comm:in] channel_id=%d len=%d hex=%s",
                        ntf.channel_id, len(raw),
                        raw.hex() if len(raw) <= 256 else raw[:256].hex() + "...",
                    )
                    self._cbs.on_comm_channel_data(raw)
                return

            if msg_type == MessageTypePb.MSG_NTF_USB_HOST_ONLINE_STATE:
                ntf = payload.ntf_usb_host_online_state
                old_ready = self.is_comm_channel_ready()
                self._usb_host_online = bool(ntf.online)
                if self._cbs and self._cbs.on_usb_online:
                    self._cbs.on_usb_online(ntf.online, ntf.reason)
                new_ready = self.is_comm_channel_ready()
                if old_ready != new_ready and self._cbs and self._cbs.on_comm_channel_ready_changed:
                    self._cbs.on_comm_channel_ready_changed(new_ready)
                return

            if msg_type == MessageTypePb.MSG_NTF_COMM_PEER_STATE:
                ntf = payload.ntf_comm_peer_state
                if ntf.channel_id != self._session_info.comm_channel_id:
                    return
                old_ready = self.is_comm_channel_ready()
                self._comm_peer_online = bool(ntf.online)
                self._session_info.comm_peer_online = self._comm_peer_online
                new_ready = self.is_comm_channel_ready()
                if old_ready != new_ready and self._cbs and self._cbs.on_comm_channel_ready_changed:
                    self._cbs.on_comm_channel_ready_changed(new_ready)
                return

            if msg_type == MessageTypePb.MSG_NTF_APP_CONFIG_REQUEST:
                if self._cbs and self._cbs.on_app_config_request:
                    self._cbs.on_app_config_request(
                        payload.ntf_app_config_request
                    )
                return

            # Forward to message callback
            if self._cbs and self._cbs.on_message:
                self._cbs.on_message(pkg)

        except Exception as e:
            logger.error("Handle msg error: %s", e, exc_info=True)

    # ─── Internal: Handlers ────────────────────────────────────────────────

    def _on_error(self, err: ErrorPb):
        code = _get_field(err, "code", StatusCodePb.STATUS_INTERNAL_ERROR)
        message = _get_field(err, "message", "")
        status = types.Status.Error(
            types.StatusCode(code), message
        )
        logger.error("Server error: %s (code=%d)", message, code)
        track_id = _get_field(err, "track_id", 0)
        if track_id in self._pending_requests:
            self._tracked_results[track_id] = status
            self._pending_requests[track_id].set()
        if self._cbs and self._cbs.on_error:
            self._cbs.on_error(status)

    def _on_status_reply(self, reply: NtfStatusReplyPb, track_id: int):
        """Handle a status reply message."""
        code = _get_field(reply, "code", StatusCodePb.STATUS_OK)
        msg = _get_field(reply, "message", "")
        if code != StatusCodePb.STATUS_OK:
            logger.warning("Status reply error: %s (code=%d)", msg, code)
        if track_id in self._pending_requests:
            self._tracked_results[track_id] = types.Status(
                types.StatusCode(code), msg
            )
            self._pending_requests[track_id].set()

    def _on_rx_shm_ready(self, ntf: NtfRxShmReadyPb):
        """Handle RX SHM ready notification."""
        self._setup_rx_shm(ntf.rx_shm, _get_field(ntf, "generation", 0))
        self._session_info.rx_shm = self._rx_shm_info
        if self._cbs and self._cbs.on_rx_shm_ready:
            self._cbs.on_rx_shm_ready(self._rx_shm_info, self._rx_generation)

    def _on_rx_shm_invalidated(self, ntf: NtfRxShmInvalidatedPb):
        """Handle RX SHM invalidated notification."""
        if (
            self._rx_shm_info is not None
            and self._rx_shm_info.id == ntf.shm_id
            and self._rx_generation <= ntf.generation
        ):
            if self._rx_shm:
                try:
                    self._rx_shm.close()
                except Exception:
                    pass
            self._rx_shm = None
            self._rx_shm_info = None
            self._session_info.rx_shm = None
        if self._cbs and self._cbs.on_rx_shm_invalidated:
            reason = _get_field(ntf, "reason", 0)
            self._cbs.on_rx_shm_invalidated(
                _get_field(ntf, "shm_id", 0),
                _get_field(ntf, "generation", 0),
                reason,
            )

    def _on_stats_reply(self, reply: NtfStatsReplyPb):
        """Handle stats reply."""
        result = types.QueryStatsResult(stats=reply)
        if self._cbs and self._cbs.on_stats_reply:
            self._cbs.on_stats_reply(result)

    def _on_disconnected(self):
        """Handle unexpected disconnection."""
        if self._state in (types.ProtocolState.DISCONNECTED, types.ProtocolState.IDLE):
            return
        self._set_state(types.ProtocolState.DISCONNECTED)
        self._disconnect_internal()
        if self._cbs and self._cbs.on_disconnected:
            try:
                self._cbs.on_disconnected()
            except Exception as e:
                logger.error("on_disconnected callback error: %s", e)

    # ─── Internal: Helpers ─────────────────────────────────────────────────

    def _setup_rx_shm(self, shm_pb, generation: int):
        """Set up RX shared memory mapping from ShmInfoPb."""
        self._rx_shm_info = types.ShmInfo(
            id=shm_pb.id,
            name=shm_pb.name,
            size=shm_pb.size,
            width=shm_pb.width,
            height=shm_pb.height,
            stride=shm_pb.stride,
            pixel_format=types.PixelFormat(shm_pb.pixel_format),
        )
        self._rx_generation = generation

        # Close old SHM (ignore errors from active reads)
        old_shm = self._rx_shm
        self._rx_shm = None
        if old_shm:
            try:
                old_shm.close()
            except (BufferError, Exception):
                pass

        # Open new SHM
        try:
            self._rx_shm = FrameShm.open(shm_pb.name)
        except Exception as e:
            logger.error("Failed to open RX SHM: %s", e)
            self._rx_shm = None

    def _register_tx_shm(self, shm_pb) -> types.ShmInfo:
        """Register a TX SHM region from ShmInfoPb using FrameShm."""
        frame_shm = FrameShm.open(shm_pb.name)
        if not frame_shm.is_valid():
            logger.warning("Failed to open TX SHM double buffer: %s", shm_pb.name)

        region = types.ShmRegionInfo(
            type=types.ShmType.TX,
            id=shm_pb.id,
            name=shm_pb.name,
            fd=-1,
            addr=None,
            size=shm_pb.size,
            width=shm_pb.width,
            height=shm_pb.height,
            stride=shm_pb.stride,
            pixel_format=types.PixelFormat(shm_pb.pixel_format),
        )
        self._tx_shms[shm_pb.id] = region
        self._tx_frame_shms[shm_pb.id] = frame_shm

        return types.ShmInfo(
            id=shm_pb.id,
            name=shm_pb.name,
            size=shm_pb.size,
            width=shm_pb.width,
            height=shm_pb.height,
            stride=shm_pb.stride,
            pixel_format=types.PixelFormat(shm_pb.pixel_format),
        )

    def _wait_for_message(
        self,
        msg_type: int,
        timeout_ms: int = 3000,
    ) -> Any:
        """Block until a specific message type is received or timeout."""
        key = msg_type
        event = threading.Event()
        self._pending_requests[key] = event
        # Only clear the tracked result if it didn't arrive before we registered
        if key in self._tracked_results:
            # Already received! Return immediately
            self._pending_requests.pop(key, None)
            return self._tracked_results.pop(key)

        if event.wait(timeout=timeout_ms / 1000.0):
            self._pending_requests.pop(key, None)
            return self._tracked_results.pop(key, None)

        self._pending_requests.pop(key, None)
        return None

    def _set_pending_result(self, msg_type: int, value: Any):
        """Set the result for a pending message wait."""
        key = msg_type
        self._tracked_results[key] = value
        event = self._pending_requests.get(key)
        if event:
            event.set()


# ─── Module-level helpers ────────────────────────────────────────────────────


def _get_field(obj, field_name: str, default=None):
    """Safely get a protobuf field, returning default if not set."""
    try:
        return getattr(obj, field_name)
    except AttributeError:
        return default


def _set_payload(payload_pb: IPCPayloadPb, msg_type: int, payload: Any):
    """Set the correct oneof field in IPCPayloadPb based on message type."""
    try:
        name = MessageTypePb.Name(msg_type)
    except ValueError:
        logger.warning("Unknown msg_type 0x%02X for payload", msg_type)
        return

    if name == "MSG_REQ_SESSION_CREATE":
        payload_pb.req_session_create.CopyFrom(payload)
    elif name == "MSG_REQ_CLOSE_SESSION":
        payload_pb.req_close_session.CopyFrom(payload)
    elif name == "MSG_REQ_SET_RX_PARAMS":
        payload_pb.req_set_rx_params.CopyFrom(payload)
    elif name == "MSG_REQ_CREATE_TX_SHM":
        payload_pb.req_create_tx_shm.CopyFrom(payload)
    elif name == "MSG_REQ_DESTROY_TX_SHM":
        payload_pb.req_destroy_tx_shm.CopyFrom(payload)
    elif name == "MSG_REQ_SET_TX_PARAMS":
        payload_pb.req_set_tx_params.CopyFrom(payload)
    elif name == "MSG_REQ_INPUT_EVENT":
        payload_pb.req_input_event.CopyFrom(payload)
    elif name == "MSG_REQ_QUERY_RUNTIME_DATA":
        payload_pb.req_query_runtime_data.CopyFrom(payload)
    elif name == "MSG_REQ_QUERY_RUNTIME_RX_INFO":
        payload_pb.req_query_runtime_rx_info.CopyFrom(payload)
    elif name == "MSG_REQ_QUERY_RUNTIME_TX_INFO":
        payload_pb.req_query_runtime_tx_info.CopyFrom(payload)
    elif name == "MSG_REQ_QUERY_RUNTIME_INPUT":
        payload_pb.req_query_runtime_input.CopyFrom(payload)
    elif name == "MSG_REQ_QUERY_USER_CONFIG":
        payload_pb.req_query_user_config.CopyFrom(payload)
    elif name == "MSG_REQ_QUERY_STATS":
        payload_pb.req_query_stats.CopyFrom(payload)
    elif name == "MSG_NTF_COMM_CHANNEL_DATA":
        payload_pb.ntf_comm_channel_data.CopyFrom(payload)
    elif name == "MSG_NTF_APP_CONFIG_RESPONSE":
        payload_pb.ntf_app_config_response.CopyFrom(payload)
    else:
        raise ValueError(f"Unsupported outbound message type: {name}")
