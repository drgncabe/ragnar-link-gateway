# ESP-NOW Protocol

The ESP32 gateway broadcasts compact binary packets over ESP-NOW. IRIS receivers must validate the packet before updating their UI.

## Radio

- Wi-Fi mode: station
- Encryption: off for v1
- Peer: broadcast `ff:ff:ff:ff:ff:ff`
- Default channel: 6
- Channel source: `/etc/ragnar-link/config.yaml` on Ragnar

IRIS must be on the same Wi-Fi channel as the gateway. If IRIS also uses normal Wi-Fi, its infrastructure connection determines the channel; configure Ragnar Link to match that channel.

## Packet: `RagnarStatusPacket`

All fields are little-endian. Total length is 24 bytes.

| Offset | Size | Field | Description |
| --- | ---: | --- | --- |
| 0 | 2 | `magic` | ASCII `RL` |
| 2 | 1 | `version` | `1` |
| 3 | 1 | `type` | `0x01` status |
| 4 | 4 | `sequence` | Monotonic gateway transmit sequence |
| 8 | 4 | `uptime_s` | Ragnar uptime/status age source value |
| 12 | 2 | `camera_count` | Camera/device count |
| 14 | 2 | `wifi_count` | Wi-Fi count |
| 16 | 2 | `ble_count` | BLE count |
| 18 | 1 | `ragnar_state` | See enum below |
| 19 | 1 | `gps_state` | See enum below |
| 20 | 1 | `capture_state` | See enum below |
| 21 | 1 | `gateway_rssi` | Reserved in v1, set to `0` |
| 22 | 2 | `crc16` | CRC-16/CCITT-FALSE over bytes 0-21 |

Enums:

| Enum | Value |
| --- | ---: |
| `RAGNAR_STATE_UNKNOWN` | 0 |
| `RAGNAR_STATE_IDLE` | 1 |
| `RAGNAR_STATE_ACTIVE` | 2 |
| `RAGNAR_STATE_ERROR` | 3 |
| `GPS_STATE_UNKNOWN` | 0 |
| `GPS_STATE_NO_FIX` | 1 |
| `GPS_STATE_FIX` | 2 |
| `CAPTURE_STATE_IDLE` | 0 |
| `CAPTURE_STATE_RUNNING` | 1 |
| `CAPTURE_STATE_PAUSED` | 2 |
| `CAPTURE_STATE_ERROR` | 3 |

## Receiver Rules

1. Drop packets shorter or longer than 24 bytes.
2. Drop packets whose magic is not `RL`.
3. Drop packets whose version is not `1`.
4. Drop packets whose type is not `0x01`.
5. Recompute CRC-16/CCITT-FALSE over bytes 0-21 and compare it to bytes 22-23 as little-endian.
6. Treat sequence as unsigned 32-bit and use it to detect stale or repeated packets. Do not assume every sequence arrives.
7. If no valid packet arrives for 15 seconds, show Ragnar as disconnected/stale.

