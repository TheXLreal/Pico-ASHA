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
