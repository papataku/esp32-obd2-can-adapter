# M5CAN Native Protocol v1

Phase 2 adds a binary USB protocol while keeping the CAN controller receive-only.

## Mode switch

The device boots in Phase-1-compatible text mode. The host should:

```text
STREAM OFF
NATIVE ON
```

The **final text line** before binary framing starts is:

```text
#OK,NATIVE,ON,COBS+CRC32,version=1
```

Native CAN streaming starts **OFF**. After the host sees that marker it must send the Native `START_STREAM` command.

This explicit boundary avoids treating trailing text as binary data.

## Wire framing

Each packet on USB is:

```text
COBS(decoded_packet) 00
```

Decoded packet layout, little-endian:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 1 | protocol version (1) |
| 1 | 1 | packet type |
| 2 | 1 | flags |
| 3 | 2 | device/host sequence |
| 5 | 2 | payload length |
| 7 | N | payload |
| 7+N | 4 | CRC32 |

CRC32 is standard reflected CRC-32/ISO-HDLC. Reference:

```text
CRC32("123456789") = 0xCBF43926
```

Device sequence increments for every device-to-host Native packet. Sequence gaps therefore indicate missing USB/decoder packets or a device reset/reconnect.

## Packet types

| Value | Name | Direction |
|---:|---|---|
| 0x01 | HELLO | device -> host |
| 0x02 | STATS | device -> host |
| 0x03 | CAN_FRAME | device -> host |
| 0x04 | EVENT | device -> host |
| 0x05 | ERROR | device -> host |
| 0x10 | COMMAND | host -> device |
| 0x11 | COMMAND_REPLY | device -> host |

## Commands

Phase 2 supports only:

- PING (0x01)
- GET_INFO (0x02)
- GET_STATS (0x03)
- START_STREAM (0x04)
- STOP_STREAM (0x05)
- TEXT_MODE (0x06)

There is deliberately **no CAN transmit command, OBD request, UDS request, or TX Lease in Phase 2**.

## CAN_FRAME payload

```text
u64 timestamp_us
u32 identifier
u8  dlc
u8  frame_flags
u8  data[dlc]     // absent for RTR
```

Flags:

- bit0: extended ID
- bit1: RTR
- bit2: self reception

`timestamp_us` is captured on the ESP32 receive path with `esp_timer_get_time()`.

## HELLO payload

```text
u8  protocol_version
u32 bitrate
u8  tx_enabled       // always 0 in Phase 2
u8  stream_enabled
u8  device_name_len
u8  device_name[]
u8  firmware_len
u8  firmware[]
```

## STATS payload

```text
u64 rx
u64 std
u64 ext
u64 rtr
u64 app_drop
u32 missed
u32 overrun
u32 buserr
u32 arb_lost
u32 driver_q
u8  state
u64 native_rx_errors
```

## Exact capture

`tools/native_capture.py` writes complete Native wire packets, including the trailing `0x00`, to `*.m5can` **before semantic decoding**.

This binary file is the source of truth for later replay/debugging. `tools/decode_log.py` can decode it afterward without altering the original capture.
