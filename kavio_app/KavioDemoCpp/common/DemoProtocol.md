# KavioDemo Protocol v1

KavioDemo C++, Python and Lua variants use the same private Host/Device demo protocol. This protocol belongs to KavioDemo and is not part of the Device SDK public ABI.

## Header

All integer fields are little-endian.

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 4 | Magic `0x3144534B` (`KSD1`) |
| 4 | 2 | Protocol version `1` |
| 6 | 2 | Message type |
| 8 | 4 | Sequence |
| 12 | 4 | Payload size |
| 16 | 4 | Payload FNV-1a checksum |
| 20 | N | Payload |

Maximum payload is `1 MiB - 20 bytes`.

## Message types

| Value | Type |
| ---: | --- |
| 1 | Hello |
| 2 | HelloAck |
| 3 | Ping |
| 4 | Pong |
| 5 | Navigate |
| 6 | TransferData |
| 7 | TransferAck |
| 8 | QueryState |
| 9 | StateReport |

## Pages

| Value | Page |
| ---: | --- |
| 0 | Home |
| 1 | Render |
| 2 | Transfer |
| 3 | HDMI RX |
| 4 | HDMI TX |
| 5 | Runtime |
| 6 | Keyboard |
| 7 | H.264 |
| 8 | Schema |
| 9 | Mouse |

`TransferAck` contains two little-endian `uint32` values: payload bytes and payload checksum.
