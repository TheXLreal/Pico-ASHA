# Pico-ASHA audio stall trace

The diagnostics are off by default. Build a Pico 2 W image with:

```powershell
cmake -S . -B build/firmware-pico2_w -DPICO_BOARD=pico2_w `
  -DENABLE_AUDIO_STALL_TRACE=ON `
  -DENABLE_AUDIO_STALL_TRACE_RSSI=ON
cmake --build build/firmware-pico2_w
```

Optional RSSI sampling uses BTstack's asynchronous `gap_read_rssi()` request,
round-robins connected aids, and issues at most one request per 500 ms globally:

```text
-DENABLE_AUDIO_STALL_TRACE_RSSI=ON
```

Trace records are fixed 40-byte little-endian binary structures. Core 0 and
core 1 each have a 256-entry SPSC lock-free queue. A full queue increments
`trace_dropped`; producers never wait. The firmware sends up to five records
per COBS-framed CDC packet and sends an aggregate snapshot at most once every
250 ms. Normal packets are never formatted as text.

To capture without the GUI (close the GUI first so it releases the CDC port):

```powershell
python tools/parse_audio_stall_trace.py --capture-port COM7 `
  --capture-seconds 120 --raw-output audio_stall_trace.bin
```

`pyserial` is required only for live capture. An existing raw CDC capture can
be parsed without dependencies:

```powershell
python tools/parse_audio_stall_trace.py audio_stall_trace.bin
```

Live capture writes every received chunk directly to `--raw-output` and
flushes periodically. Ctrl+C, a COM-port error, or device removal closes the
port, preserves the partial raw file, and then parses every complete COBS frame
captured before the interruption. An incomplete trailing frame is ignored.

The parser writes event, aggregate-snapshot, per-sequence timeline, and
five-seconds-before-disconnect CSV files. Durations above 25 ms, timer gaps
above 5 ms, L2CAP errors, ring underruns/overruns, and disconnects are marked
as anomalies.

The firmware also emits `AUDIO_SEND_DELAY_WARNING` when one of these
send-path gaps exceeds 50 ms:

- SDU generation to `AUDIO_SEND_REQUESTED`;
- request to `AUDIO_CAN_SEND_NOW_RECEIVED`;
- callback to `AUDIO_L2CAP_SEND_BEGIN`;
- previous successful L2CAP call to the next request.

`AUDIO_L2CAP_SEND_COMPLETE` is emitted after the synchronous `l2cap_send()`
call and contains its result and call duration. To cap normal-path overhead,
`AUDIO_SEND_STATE_CHANGED` is reserved for exceptional transitions such as
local recovery and disconnect/reset; the normal state progression is already
represented by the request, callback, send-begin, send-complete, and
packet-sent records.

Trace version 1 keeps the original 40-byte record and 92-byte snapshot wire
layouts. Runtime diagnostics are sent as optional payload kind 3 and merged by
the parser with the snapshot at the same timestamp. Its fields are:

- `hci_write_count`, `hci_write_error_count`, `hci_write_last_us`,
  `hci_write_max_us`;
- `cyw43_lock_wait_last_us`, `cyw43_lock_wait_max_us`,
  `cyw43_lock_hold_last_us`, `cyw43_lock_hold_max_us`;
- `btstack_run_loop_gap_last_us`, `btstack_run_loop_gap_max_us`;
- `hci_to_packet_sent_last_us`, `hci_to_packet_sent_max_us`;
- RSSI values/ages, sample and skipped-request counts;
- stale-SDU drop count and dropped-frame count.

Event IDs 19-25 are `AUDIO_TX_STALE_DROP`, `HCI_WRITE_BEGIN`,
`HCI_WRITE_END`, `CYW43_LOCK_WAIT_BEGIN`, `CYW43_LOCK_ACQUIRED`,
`CYW43_LOCK_HELD`, and `RSSI_CONTEXT`. Begin/end and lock events are emitted
only for slow operations; normal calls update aggregates without filling the
trace ring.

Event IDs 26-39 add the precise stall/lifecycle diagnostics:

- `AUDIO_L2CAP_SEND_BEGIN`, `AUDIO_SEND_STATE_CHANGED`, and
  `AUDIO_SEND_DELAY_WARNING`;
- `AUDIO_STALE_FRAMES_DROPPED`, `AUDIO_STALE_DROP_CONTEXT`, and
  `AUDIO_SEQUENCE_SKIP_COUNT_CHANGED`;
- `HCI_CONNECTION_OPENED`, `HCI_DISCONNECTION_COMPLETE`,
  `L2CAP_CHANNEL_OPENED`, `L2CAP_CHANNEL_CLOSED`,
  `ASHA_DEVICE_CONNECTED`, and `ASHA_DEVICE_DISCONNECTED`;
- `SYSTEM_BOOT` and `WATCHDOG_RESET_REQUESTED`.

Stale recovery uses two records with the same timestamp so the version-1
40-byte wire record remains unchanged. `AUDIO_STALE_FRAMES_DROPPED` captures
the pre-drop read/write indices, occupancy, busy state/duration, old/new
sequences, dropped count, and sequence-skip total. Its companion context adds
CAN_SEND request state, request/send ages, PCM state, connected-device count,
and available credits. The parser expands both records into labelled `details`
fields.

Lifecycle records pack the six-byte Bluetooth address plus HCI status/reason
and L2CAP status (or `unavailable`) while retaining handle and CID in their
normal columns. `SYSTEM_BOOT` reports the watchdog flags, raw SDK watchdog
reset reason, a watchdog-scratch-backed session counter, firmware version,
and timer-based uptime. A pre-reset watchdog record is best-effort because the
existing 10-ms reset delay is intentionally unchanged; the following boot
record's `watchdog_enable_reboot` flag remains authoritative.

The timeline generator treats sequence bytes as per-generation values, not
global IDs. It associates requests using timestamp/order and the encoder ring
write index, maintains an extended sequence counter, and therefore preserves
sequence `255` across `254 -> 255 -> 0`.
