# Pico-ASHA audio stall trace

The diagnostics are off by default. Build a Pico 2 W image with:

```powershell
cmake -S . -B build/firmware-pico2_w -DPICO_BOARD=pico2_w `
  -DENABLE_AUDIO_STALL_TRACE=ON
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
above 2 ms, L2CAP errors, ring underruns/overruns, and disconnects are marked
as anomalies.
