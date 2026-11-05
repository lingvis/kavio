--- Shared Kavio Demo datagram protocol (device and Windows Host).
-- Wire integers are little-endian. Header = 20 bytes; maximum datagram = 1 MiB.
-- FNV-1a detects accidental corruption; it is NOT a security checksum.

local M = {
    MAGIC = 0x3144534B, -- "KSD1" in little-endian wire order.
    VERSION = 1,
    HEADER = 20,
    MAX_PAYLOAD = 1024 * 1024 - 20,
    HELLO = 1, HELLO_ACK = 2,
    PING = 3, PONG = 4,
    NAVIGATE = 5, TRANSFER_DATA = 6, TRANSFER_ACK = 7,
    QUERY_STATE = 8, STATE_REPORT = 9,
}

-- NAVIGATE carries a zero-based index into this ordered page table.
M.pages = {
    "home", "render", "transfer", "hdmi_rx", "hdmi_tx",
    "runtime", "keyboard", "h264", "schema", "mouse",
}

--- Return the FNV-1a checksum of a binary string, reduced to 32 bits.
-- @param data string Bytes to checksum.
function M.checksum(data)
    local hash = 2166136261
    for index = 1, #data do
        hash = ((hash ~ data:byte(index)) * 16777619) & 0xffffffff
    end
    return hash
end

--- Serialize a binary packet with an optional precomputed checksum.
-- @param message_type integer Protocol identifier.
-- @param sequence integer Unsigned request/reply sequence.
-- @param payload string|nil Binary packet payload.
-- @param checksum_override integer|nil Optional payload checksum.
function M.packet(message_type, sequence, payload, checksum_override)
    payload = payload or ""
    assert(#payload <= M.MAX_PAYLOAD, "payload too large")
    local digest = checksum_override or M.checksum(payload)
    return string.pack("<I4I2I2I4I4I4", M.MAGIC, M.VERSION, message_type,
                       sequence, #payload, digest) .. payload
end

--- Parse and verify a complete packet; return nil for invalid data.
-- @param data string Entire datagram, header included.
-- @return integer|nil Message type.
-- @return integer|nil Sequence.
-- @return string|nil Payload.
-- @return integer|nil Verified checksum.
function M.parse(data)
    if #data < M.HEADER then return nil end
    local magic, version, message_type, sequence, size, digest, offset =
        string.unpack("<I4I2I2I4I4I4", data)
    if magic ~= M.MAGIC or version ~= M.VERSION or size > M.MAX_PAYLOAD
        or #data ~= M.HEADER + size then
        return nil
    end
    local payload = data:sub(offset)
    if M.checksum(payload) ~= digest then return nil end
    return message_type, sequence, payload, digest
end

--- Encode the acknowledged payload length and checksum (two u32 values).
function M.transfer_ack(length, digest)
    return string.pack("<I4I4", length, digest)
end

return M
