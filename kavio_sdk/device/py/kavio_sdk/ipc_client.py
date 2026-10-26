"""Kavio Python Device IPC client.

Business requests are nonblocking: each returns a `track_id`, and all server
replies and notifications reach one `on_message(IPCPackagePb)` callback on a
single dispatch thread. Only the initial Session handshake is synchronous.

Slow message handlers delay later deliveries but do not block socket reads.
Callbacks must not be used for prolonged CPU- or I/O-bound processing.
"""

from __future__ import annotations

import errno
import logging
import os
import queue
import select
import socket
import struct
import threading
from dataclasses import dataclass
from typing import Callable, Optional

from .app_config import AppConfig
from .encoded_stream_shm import EncodedStreamShm
from .frame_shm import FrameShm
from .proto_py import ipc_pb2 as proto
from .sdk_version import SDK_VERSION_MAJOR, SDK_VERSION_MINOR, SDK_VERSION_PATCH
from .types import ImageFrame, PixelFormat, Rect, Status, StatusCode, StatusError

logger = logging.getLogger("kavio.ipc_client")
_MAX_IPC_MESSAGE = 16 * 1024 * 1024
_MAX_QUEUED_MESSAGES = 512
_MAX_QUEUED_BYTES = 16 * 1024 * 1024


@dataclass
class ClientOptions:
    socket_path: str = "/tmp/kavio.sock"
    # Only the initial Session handshake has a timeout. Business operations
    # are asynchronous, and applications own their own request deadlines.
    handshake_timeout_s: float = 3.0


class IPCClient:
    """Thread-safe IPC transport with one serial App message handler."""

    def __init__(self):
        self._socket = None
        self._recv_thread = None
        self._dispatch_thread = None
        self._running = False
        self._send_lock = threading.Lock()
        self._state_lock = threading.RLock()
        self._resources_lock = threading.RLock()
        self._request_lock = threading.Lock()
        self._next_id = 0
        self._session_id = 0
        self._session_info = None
        self._usb_online = False
        self._comm_peer_online = False
        self._rx_info = None
        self._rx_shm = None
        self._rx_generation = 0
        self._tx_shms = {}
        self._h264_shms = {}
        self._handler = None
        self._disconnect_handler = None
        self._handshake_id = 0
        self._handshake_event = threading.Event()
        self._handshake_result = None
        self._queue = queue.Queue(maxsize=_MAX_QUEUED_MESSAGES)
        self._queue_lock = threading.Lock()
        self._queued_bytes = 0
        self._disconnected_reported = False
        self._app_config = AppConfig()

    def get_app_config(self):
        return self._app_config.get()

    def update_app_config(self, value):
        self._app_config.update(value)

    def reset_app_config_to_defaults(self):
        self._app_config.reset_to_defaults()

    def connect(self, client_options: ClientOptions, session: proto.A2PReqCreateSessionPb,
                on_message: Callable[[proto.IPCPackagePb], None],
                on_disconnected: Optional[Callable[[], None]] = None,
                *, auto_query_runtime: bool = True) -> proto.P2ARspCreateSessionPb:
        """Establish Session, then deliver subsequent protobufs on a worker.

        Returns the server's Session-created protobuf. Raises StatusError for
        connection or handshake failures. `on_message` is serial, never called
        on the socket receive thread. The handler must remain valid until
        `disconnect` completes.
        """
        self._app_config.initialize()
        if not callable(on_message):
            raise TypeError("on_message must be callable")
        with self._state_lock:
            if self._running or (self._dispatch_thread and self._dispatch_thread.is_alive()):
                raise RuntimeError("client is active; call disconnect() before reconnecting")
            self._handler, self._disconnect_handler = on_message, on_disconnected
            self._handshake_event.clear()
            self._handshake_result = None
            self._disconnected_reported = False
            self._session_id = 0
            self._session_info = None
            self._queue = queue.Queue(maxsize=_MAX_QUEUED_MESSAGES)
            self._queued_bytes = 0
        path = client_options.socket_path
        try:
            if path.startswith("/") or ":" not in path:
                sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                target = path
            else:
                host, sep, port = path.rpartition(":")
                if not sep or not host:
                    raise ValueError("socket_path must be a Unix socket or host:port")
                sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                target = (host.strip("[]"), int(port))
            sock.settimeout(client_options.handshake_timeout_s)
            sock.connect(target)
            sock.settimeout(None)
        except Exception as exc:
            try:
                sock.close()
            except (UnboundLocalError, OSError):
                pass
            raise StatusError(Status.Error(StatusCode.SOCKET_ERROR, str(exc))) from exc

        with self._state_lock:
            self._socket, self._running = sock, True
        self._recv_thread = threading.Thread(target=self._recv_loop, name="kavio-python-recv", daemon=True)
        self._recv_thread.start()
        request = proto.A2PReqCreateSessionPb()
        request.CopyFrom(session)
        request.client_pid = os.getpid()
        request.sdk_version.major = SDK_VERSION_MAJOR
        request.sdk_version.minor = SDK_VERSION_MINOR
        request.sdk_version.patch = SDK_VERSION_PATCH
        if not request.HasField("rx_params"):
            request.rx_params.enabled = True
            request.rx_params.frame_sample_interval = 1
        if request.rx_params.enabled and request.rx_params.frame_sample_interval == 0:
            self.disconnect()
            raise ValueError("frame_sample_interval must be >= 1")
        track_id = self._new_track_id()
        self._handshake_id = track_id
        try:
            self._send(proto.MSG_A2P_REQ_CREATE_SESSION, request, track_id)
            if not self._handshake_event.wait(client_options.handshake_timeout_s):
                raise TimeoutError("Session create handshake timed out")
            reply = self._handshake_result
            if reply is None:
                raise ConnectionError("disconnected during Session handshake")
            if reply.header.msg_type == proto.MSG_P2A_RSP_ERROR:
                raise RuntimeError(reply.payload.error.message)
            session_info = proto.P2ARspCreateSessionPb()
            session_info.CopyFrom(reply.payload.p2a_rsp_create_session)
            with self._state_lock:
                self._session_id = session_info.session_id
                self._session_info = session_info
                self._comm_peer_online = bool(session_info.comm_peer_online)
            # The Session and local caches are ready before any App event is
            # dispatched, including the queued Session-created notification.
            self._dispatch_thread = threading.Thread(target=self._dispatch_loop,
                                                     name="kavio-python-dispatch", daemon=True)
            self._dispatch_thread.start()
            if auto_query_runtime:
                self.query_runtime_data()
            return session_info
        except Exception as exc:
            self.disconnect()
            if isinstance(exc, StatusError):
                raise
            raise StatusError(Status.Error(StatusCode.PROTOCOL_ERROR, str(exc))) from exc
        finally:
            self._handshake_id = 0

    def disconnect(self):
        """Close socket and SHM; wait for workers unless called from a worker."""
        with self._state_lock:
            self._running = False
            sock, self._socket = self._socket, None
            self._handshake_event.set()
        if sock is not None:
            try:
                sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            sock.close()
        self._clear_queue()
        for thread in (self._recv_thread, self._dispatch_thread):
            if thread and thread is not threading.current_thread():
                thread.join()
        self._recv_thread = None
        self._dispatch_thread = None
        with self._resources_lock:
            self._close_resources()
        with self._state_lock:
            self._session_id = 0
            self._session_info = None
            self._usb_online = self._comm_peer_online = False

    def _close_resources(self):
        if self._rx_shm:
            self._rx_shm.close()
        self._rx_shm, self._rx_info = None, None
        for item in self._tx_shms.values():
            item[1].close()
        self._tx_shms.clear()
        for stream in self._h264_shms.values():
            stream.close()
        self._h264_shms.clear()

    def _new_track_id(self) -> int:
        with self._request_lock:
            self._next_id = (self._next_id % 0xffffffff) + 1
            return self._next_id

    def _send(self, msg_type: int, body, track_id: Optional[int] = None) -> int:
        if track_id is None:
            track_id = self._new_track_id()
        package = proto.IPCPackagePb()
        package.header.msg_type = msg_type
        package.header.track_id = track_id
        name = proto.MessageTypePb.Name(msg_type).lower()
        if name.startswith("msg_"):
            name = name[4:]
        getattr(package.payload, name).CopyFrom(body)
        data = package.SerializeToString()
        if len(data) > _MAX_IPC_MESSAGE:
            raise ValueError("IPC package exceeds maximum supported size")
        with self._send_lock:
            sock = self._socket
            if not sock or not self._running:
                raise ConnectionError("IPC client is not connected")
            sock.sendall(struct.pack("<I", len(data)) + data)
        return track_id

    # Business requests: send only; inspect header.track_id on response.
    def set_rx_params(self, params: proto.RxParamsPb) -> int:
        if params.enabled and params.frame_sample_interval < 1:
            raise ValueError("frame_sample_interval must be >= 1")
        req = proto.A2PReqSetRxParamsPb()
        req.params.CopyFrom(params)
        return self._send(proto.MSG_A2P_REQ_SET_RX_PARAMS, req)

    def set_tx_params(self, params: proto.TxParamsPb) -> int:
        req = proto.A2PReqSetTxParamsPb()
        req.params.CopyFrom(params)
        return self._send(proto.MSG_A2P_REQ_SET_TX_PARAMS, req)

    def create_tx_shm(self, width: int, height: int, pixel_format: int) -> int:
        if width <= 0 or height <= 0:
            raise ValueError("TX dimensions must be positive")
        req = proto.A2PReqCreateTxShmPb(width=width, height=height, pixel_format=int(pixel_format))
        return self._send(proto.MSG_A2P_REQ_CREATE_TX_SHM, req)

    def destroy_tx_shm(self, shm_id: int) -> int:
        return self._send(proto.MSG_A2P_REQ_DESTROY_TX_SHM, proto.A2PReqDestroyTxShmPb(shm_id=shm_id))

    def subscribe_h264_stream(self) -> int:
        return self._send(proto.MSG_A2P_REQ_CREATE_ENCODE_STREAM, proto.A2PReqCreateEncodeStreamPb())

    def unsubscribe_h264_stream(self, stream_id: int) -> int:
        return self._send(proto.MSG_A2P_REQ_DESTROY_ENCODE_STREAM,
                          proto.A2PReqDestroyEncodeStreamPb(stream_id=stream_id))

    def query_stats(self, all_sessions: bool = False) -> int:
        return self._send(proto.MSG_A2P_REQ_QUERY_STATS,
                          proto.A2PReqQueryStatsPb(all_sessions=all_sessions))

    def query_runtime_data(self) -> int:
        return self._send(proto.MSG_A2P_REQ_QUERY_RUNTIME_DATA, proto.A2PReqQueryRuntimeDataPb())

    def send_input_event(self, event) -> int:
        return self._send(proto.MSG_A2P_NTF_INPUT_EVENT, event)

    def send_comm_channel_data(self, payload: bytes) -> int:
        if not payload or not self.is_comm_channel_ready():
            raise ValueError("comm channel is not ready or payload is empty")
        return self._send(proto.MSG_A2P_DATA_COMM_CHANNEL,
                          proto.A2PDataCommChannelPb(channel_id=self.get_comm_channel_id(), payload=payload))

    # Explicit, *local* resource operations; no IPC request is sent.
    def open_tx_shm(self, info: proto.ShmInfoPb) -> int:
        with self._resources_lock:
            if not self._running:
                raise ConnectionError("IPC Session is disconnected")
            shm = FrameShm.open(info.name)
            old = self._tx_shms.get(info.id)
            copy = proto.ShmInfoPb()
            copy.CopyFrom(info)
            self._tx_shms[info.id] = (copy, shm)
            if old:
                old[1].close()
            return info.id

    def close_tx_shm(self, shm_id: int):
        with self._resources_lock:
            item = self._tx_shms.pop(shm_id, None)
            if item:
                item[1].close()

    def open_h264_stream(self, info: proto.EncodedStreamShmInfoPb) -> int:
        with self._resources_lock:
            if not self._running:
                raise ConnectionError("IPC Session is disconnected")
            shm = EncodedStreamShm(info.name, info.size, info.stream_id)
            old = self._h264_shms.pop(info.stream_id, None)
            if old:
                old.close()
            self._h264_shms[info.stream_id] = shm
            return info.stream_id

    def close_h264_stream(self, stream_id: int):
        with self._resources_lock:
            shm = self._h264_shms.pop(stream_id, None)
            if shm:
                shm.close()

    def latest_h264_packet_seq(self, stream_id: int) -> int:
        with self._resources_lock:
            shm = self._h264_shms.get(stream_id)
            return shm.latest_seq() if shm else 0

    def read_h264_packet(self, stream_id: int, seq: int):
        with self._resources_lock:
            shm = self._h264_shms.get(stream_id)
            return shm.read_packet(seq) if shm else None

    def write_tx_frame(self, shm_id: int, data: bytes, *, width=None, height=None,
                       stride=None, valid_rect: Optional[Rect] = None,
                       frame_id=0, timestamp_ns=0, dest_pos_x=0, dest_pos_y=0) -> bool:
        with self._resources_lock:
            item = self._tx_shms.get(shm_id)
            if item is None:
                return False
            info, shm = item
            width = info.width if width is None else width
            height = info.height if height is None else height
            stride = info.stride if stride is None else stride
            rect = valid_rect or Rect(0, 0, width, height)
            return shm.write_latest(data, frame_id, timestamp_ns, width, height, stride,
                                    PixelFormat(info.pixel_format), rect, dest_pos_x, dest_pos_y)

    def clear_tx_frame(self, shm_id: int):
        with self._resources_lock:
            item = self._tx_shms.get(shm_id)
            if item is not None:
                item[1].clear_all_buffers()

    def read_latest_rx_frame(self) -> Optional[ImageFrame]:
        with self._resources_lock:
            if self._rx_shm is None:
                return None
            view = self._rx_shm.read_latest()
            if view is None:
                return None
            try:
                return ImageFrame(data=view.payload, frame_id=view.frame_id,
                                  timestamp_ns=view.timestamp_ns, width=view.width,
                                  height=view.height, stride=view.stride,
                                  pixel_format=view.pixel_format,
                                  valid_rect=view.valid_rect, dest_pos_x=view.dest_pos_x,
                                  dest_pos_y=view.dest_pos_y)
            finally:
                self._rx_shm.end_read(view.index)

    def get_comm_channel_id(self) -> int:
        with self._state_lock:
            return int(self._session_info.comm_channel_id) if self._session_info else 0

    def is_comm_channel_ready(self) -> bool:
        with self._state_lock:
            return bool(self._usb_online and self._comm_peer_online and self.get_comm_channel_id())

    def get_session_info(self):
        with self._state_lock:
            info = proto.P2ARspCreateSessionPb()
            if self._session_info:
                info.CopyFrom(self._session_info)
            return info

    def _clear_queue(self):
        with self._queue_lock:
            while True:
                try:
                    self._queue.get_nowait()
                except queue.Empty:
                    break
            self._queued_bytes = 0

    def _enqueue(self, package):
        cost = package.ByteSize()
        with self._queue_lock:
            if (self._queue.full() or self._queued_bytes + cost > _MAX_QUEUED_BYTES):
                raise OverflowError("IPC dispatch queue overflow")
            self._queue.put_nowait((package, cost))
            self._queued_bytes += cost

    def _dispatch_loop(self):
        while self._running or not self._queue.empty():
            try:
                package, cost = self._queue.get(timeout=0.1)
            except queue.Empty:
                continue
            with self._queue_lock:
                self._queued_bytes = max(0, self._queued_bytes - cost)
            try:
                self._handler(package)
            except Exception:
                logger.exception("App OnMessage failed")
        if self._disconnected_reported and self._disconnect_handler:
            try:
                self._disconnect_handler()
            except Exception:
                logger.exception("App OnDisconnected failed")

    def _recv_loop(self):
        data = bytearray()
        try:
            while self._running:
                sock = self._socket
                if sock is None:
                    break
                readable, _, _ = select.select([sock], [], [], 0.2)
                if not readable:
                    continue
                chunk = sock.recv(65536)
                if not chunk:
                    break
                data.extend(chunk)
                while len(data) >= 4:
                    size = struct.unpack_from("<I", data)[0]
                    if size == 0 or size > _MAX_IPC_MESSAGE:
                        raise ValueError("invalid IPC frame length")
                    if len(data) < 4 + size:
                        break
                    package = proto.IPCPackagePb()
                    package.ParseFromString(bytes(data[4:4 + size]))
                    del data[:4 + size]
                    self._process_message(package)
        except (OSError, ValueError, OverflowError):
            if self._running:
                logger.exception("IPC receive failed")
        finally:
            self._handshake_event.set()
            if self._running:
                self._report_disconnect()

    def _process_message(self, package):
        msg_type = package.header.msg_type
        if msg_type == proto.MSG_P2A_RSP_CREATE_SESSION:
            info = package.payload.p2a_rsp_create_session
            snapshot = proto.P2ARspCreateSessionPb()
            snapshot.CopyFrom(info)
            with self._state_lock:
                self._session_id = info.session_id
                self._session_info = snapshot
                self._comm_peer_online = bool(info.comm_peer_online)
        if (self._handshake_id and package.header.track_id == self._handshake_id and
                msg_type in (proto.MSG_P2A_RSP_CREATE_SESSION, proto.MSG_P2A_RSP_ERROR)):
            self._handshake_result = package
            self._handshake_event.set()
        if msg_type == proto.MSG_P2A_NTF_RX_SHM_READY:
            info = package.payload.p2a_ntf_rx_shm_ready
            try:
                new_shm = FrameShm.open(info.rx_shm.name)
            except Exception:
                logger.exception("Failed to open RX SHM")
            else:
                with self._resources_lock:
                    old = self._rx_shm
                    self._rx_shm = new_shm
                    self._rx_info = info.rx_shm
                    self._rx_generation = info.generation
                    if old:
                        old.close()
        elif msg_type == proto.MSG_P2A_NTF_RX_SHM_INVALIDATED:
            info = package.payload.p2a_ntf_rx_shm_invalidated
            with self._resources_lock:
                if self._rx_info and self._rx_info.id == info.shm_id and self._rx_generation <= info.generation:
                    if self._rx_shm:
                        self._rx_shm.close()
                    self._rx_shm = self._rx_info = None
        elif msg_type == proto.MSG_P2A_NTF_USB_HOST_STATE:
            with self._state_lock:
                self._usb_online = bool(package.payload.p2a_ntf_usb_host_state.online)
        elif msg_type == proto.MSG_P2A_NTF_COMM_PEER_STATE:
            state = package.payload.p2a_ntf_comm_peer_state
            with self._state_lock:
                if self._session_info and state.channel_id == self._session_info.comm_channel_id:
                    self._comm_peer_online = bool(state.online)
        elif msg_type == proto.MSG_P2A_NTF_ENCODE_STREAM_INVALIDATED:
            self.close_h264_stream(package.payload.p2a_ntf_encode_stream_invalidated.stream_id)
        elif msg_type == proto.MSG_P2A_RSP_STATUS:
            # For destroy requests, the App owns local resource closure after
            # correlating the reply by track_id. Do not guess the affected SHM.
            pass
        if msg_type == proto.MSG_P2A_REQ_GET_APP_CONFIG:
            if not package.header.track_id or not package.payload.HasField("p2a_req_get_app_config"):
                return
            response = proto.A2PRspGetAppConfigPb()
            response.schema_description_json = self._app_config.description_bytes()
            response.config_json = self._app_config.config_bytes()
            self._send(proto.MSG_A2P_RSP_GET_APP_CONFIG, response, package.header.track_id)
            return
        if msg_type == proto.MSG_P2A_REQ_SET_APP_CONFIG:
            if not package.header.track_id or not package.payload.HasField("p2a_req_set_app_config"):
                return
            raw = package.payload.p2a_req_set_app_config.config_json
            if not raw or len(raw) > 1024 * 1024:
                return
            response = proto.A2PRspSetAppConfigPb()
            try:
                import json
                self._app_config.update(json.loads(raw.decode("utf-8")))
                response.config_json = self._app_config.config_bytes()
            except (ValueError, OSError, UnicodeError) as exc:
                response.error_code = 1
                response.error_message = str(exc)
            self._send(proto.MSG_A2P_RSP_SET_APP_CONFIG, response, package.header.track_id)
            if response.error_code:
                return
        self._enqueue(package)

    def _report_disconnect(self):
        """Invalidate local resources; report after queued messages are dispatched."""
        with self._state_lock:
            if self._disconnected_reported:
                return
            self._disconnected_reported = True
            self._running = False
            self._handshake_event.set()
            sock, self._socket = self._socket, None
        if sock:
            try:
                sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            sock.close()
        # Drop pending messages for the disconnected Session.
        self._clear_queue()
        with self._resources_lock:
            self._close_resources()
