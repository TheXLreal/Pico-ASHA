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
  --capture-seconds 120 --raw-output audio_stall_trace.bin `
  --mark-glitches
```

With `--mark-glitches`, press `M` whenever an interruption or bad sound is
heard. The parser sends the existing nonblocking CDC command format and the
firmware inserts `USER_AUDIO_GLITCH_MARKER` into its own timestamp domain. The
marker includes the age/checksum of the latest PCM block and checksums of the
latest completed left/right G.722 SDU. Without the option, capture remains
strictly read-only.

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
- stale-SDU drop count and dropped-frame count;
- core-1 run-loop and `process_audio()` counts plus their latest ages;
- HCI transport-write and controller-event progress counts plus their latest
  ages. The 64-bit snapshot timestamp minus an age gives the progress
  timestamp without a 32-bit timer-wrap ambiguity.

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

Event IDs 40-47 cover the final recovery flow:

- `AUDIO_NO_PROGRESS`, `AUDIO_NO_PROGRESS_REARMED`, and
  `AUDIO_NO_PROGRESS_RECONNECT`;
- `RECONNECT_SCHEDULED`, `SCAN_START_REQUESTED`, `CONNECT_ATTEMPT`,
  `CONNECT_COMPLETE`, and `RECONNECT_TIMEOUT`.

Event IDs 48-49 are `AUDIO_CREDITS_ZERO_ENTER` and
`AUDIO_CREDITS_ZERO_EXIT`. They are emitted only on a zero-credit state change
and report its duration, restored credit count, and the latest RSSI sample/age.

Event IDs 50-54 isolate the unresolved TX stall without adding per-packet
logging:

- `CORE1_RUN_LOOP_HEARTBEAT` and `PROCESS_AUDIO_ENTER` are rate-limited to one
  record per second; their counters/ages are also sampled every 250 ms;
- `TX_BLOCKED` is emitted only after 50 ms without successful send progress
  while a fresh SDU is waiting, then only when its blocker mask changes or once
  per second;
- `CAN_SEND_REQUEST_AGE` is emitted once per request after 25 ms in
  `WaitingCanSendNow`;
- `PACKET_SENT_WAIT_AGE` is emitted once per SDU after 50 ms in
  `WaitingPacketSent`.

`TX_BLOCKED` blocker bits are: `not_connected`, `not_streaming`,
`l2cap_not_ready`, `no_sdu_available`, `sdu_not_fresh`,
`waiting_can_send_now`, `waiting_packet_sent`, `can_send_pending`,
`audio_busy`, `no_credits`, `pcm_not_streaming`, `audio_disabled`,
`process_not_audio`, `connections_disabled`, `tx_buffer_owned`, and
`invariant_not_armed`. The last value means the full ready/fresh/idle invariant
still held after the normal scheduling pass but no request owned the TX path.
The parser expands the bitmask into names and reports credits, TX state, SDU
age, and HCI transport/controller progress ages.

Event IDs 55-57 target audible corruption without adding continuous logging:

- `AUDIO_PCM_DISCONTINUITY` is emitted only when the boundary between adjacent
  PCM blocks jumps by at least 24,576 sample units on either channel. It is a
  candidate click/corruption marker, not proof that normal loud content is
  invalid;
- `AUDIO_G722_INTEGRITY_ERROR` is emitted only if the encoder-ring generation
  changes during selection, the copied SDU checksum differs from its published
  checksum, or the private TX buffer changes before `l2cap_send()`;
- `USER_AUDIO_GLITCH_MARKER` is emitted only for an explicit `M` key marker.

The per-SDU checks use a small FNV-1a fingerprint and never gate, retry, drop,
or otherwise change audio scheduling. PCM candidates and each integrity-error
stage are rate-limited to one record per 100 ms. Progress and RSSI age
snapshots clamp a concurrent timestamp newer than the snapshot to zero,
avoiding unsigned near-`UINT32_MAX` diagnostic values.

`AUDIO_NO_PROGRESS` requires an open, connected and actively streaming ASHA
channel, SDU production observed within 50 ms, at least one SDU generated since
the send baseline, and no successful L2CAP send for 100 ms. Detection is
deliberately independent of ring-fill/read-index bookkeeping, `AudioBusy`, TX
phase, and available credits. A 150-ms startup grace is reset for each stream
or L2CAP generation. First-tier recovery retains the newest complete SDU and
re-arms one normal CAN_SEND request when the private TX buffer is safe to
replace. If BTstack already owns a request/buffer, the existing phase watchdog
continues without a duplicate request. No successful send within the following
150 ms causes the bounded reconnect flow.

Snapshot `ring_fill_current` is recomputed from the producer write index and
registered consumer read indices at snapshot time. An individual event's
`ring_fill` still describes that event's captured indices (including pre-drop
indices), so it can legitimately differ from a later snapshot.

Transient L2CAP credit exhaustion no longer issues ACP Stop/Start. The active
ASHA stream waits for flow-control credit while the existing phase and
no-progress watchdogs remain armed. When credit returns, up to three queued
20-ms SDUs (at most 60 ms) are sent in sequence so a single fresh G.722 frame
is not discarded. Recovery or a larger backlog still collapses to the newest
complete SDU. USB PCM gap handling remains unchanged.

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
