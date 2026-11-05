"""Wire format shared by the Kavio device Demo and Windows Host.

All integer fields use little-endian byte order. A packet has a 20-byte header
(magic, version, type, sequence, payload size, FNV-1a checksum), followed by
up to MAX_PAYLOAD bytes. The checksum detects accidental corruption; it is not
an authentication mechanism.
"""

import struct

# Header fields and the maximum datagram size must match DemoProtocol.h.
MAGIC = 0x3144534B  # ASCII "KSD1" when serialized as little-endian bytes.
VERSION = 1
HEADER = 20  # 4 + 2 + 2 + 4 + 4 + 4 bytes.
MAX_PAYLOAD = 1024 * 1024 - HEADER  # 1 MiB maximum complete packet.

# Message identifiers are stable across C++, Python, Lua, and Windows Host.
HELLO, HELLO_ACK, PING, PONG, NAVIGATE = 1, 2, 3, 4, 5
TRANSFER_DATA, TRANSFER_ACK, QUERY_STATE, STATE_REPORT = 6, 7, 8, 9
PAGES = [
    "home", "render", "transfer", "hdmi_rx", "hdmi_tx",
    "runtime", "keyboard", "h264", "schema", "mouse",
]


def checksum(data):
    """Return the 32-bit FNV-1a checksum of a bytes-like payload."""
    result = 2166136261
    for value in data:
        result = ((result ^ value) * 16777619) & 0xFFFFFFFF
    return result


def packet(message_type, sequence, payload=b"", checksum_override=None):
    """Serialize one datagram; raise ValueError for an oversized payload.

    Args:
        message_type: Protocol message identifier.
        sequence: Unsigned 32-bit request/reply identifier.
        payload: Binary payload appended after the fixed-size header.
        checksum_override: Optional precomputed payload checksum.
    """
    if len(payload) > MAX_PAYLOAD:
        raise ValueError("payload too large")
    digest = checksum(payload) if checksum_override is None else checksum_override
    return struct.pack("<IHHIII", MAGIC, VERSION, message_type,
                       sequence, len(payload), digest) + payload


def parse(data):
    """Validate and decode a complete datagram, or return None on failure."""
    if len(data) < HEADER:
        return None
    magic, version, message_type, sequence, size, digest = struct.unpack_from(
        "<IHHIII", data, 0
    )
    if (magic != MAGIC or version != VERSION or size > MAX_PAYLOAD
            or len(data) != HEADER + size):
        return None
    payload = data[HEADER:]
    if checksum(payload) != digest:
        return None
    return message_type, sequence, payload, digest


def u64(value):
    """Encode an unsigned 64-bit integer for protocol payloads."""
    return struct.pack("<Q", value)


def transfer_ack(length, digest):
    """Encode a transfer acknowledgment: byte count and payload checksum."""
    return struct.pack("<II", length, digest)
