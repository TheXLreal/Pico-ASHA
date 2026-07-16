import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("parse_audio_stall_trace.py")
SPEC = importlib.util.spec_from_file_location("parse_audio_stall_trace", MODULE_PATH)
assert SPEC and SPEC.loader
trace = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = trace
SPEC.loader.exec_module(trace)


def cobs_encode(data: bytes) -> bytes:
    output = bytearray(b"\0")
    code_index = 0
    code = 1
    for value in data:
        if value == 0:
            output[code_index] = code
            code_index = len(output)
            output.append(0)
            code = 1
        else:
            output.append(value)
            code += 1
            if code == 0xFF:
                output[code_index] = code
                code_index = len(output)
                output.append(0)
                code = 1
    output[code_index] = code
    output.append(0)
    return bytes(output)


def record(timestamp, event, sequence=7, duration=0, result=0, handle=0x0040, cid=0x0041):
    return trace.TRACE_RECORD.pack(
        timestamp, 10, 9, duration, 0, 0, result,
        handle, cid, event, sequence, 1, int(event not in (1, 7, 10, 11)),
    )


def packet(records: list[bytes]) -> bytes:
    body = b"".join(records)
    envelope = trace.TRACE_HEADER.pack(
        trace.TRACE_MAGIC, trace.TRACE_VERSION, trace.PAYLOAD_RECORDS,
        len(records), len(body),
    ) + body
    header = trace.PICO_HEADER.pack(
        trace.ASHA_TYPE_AUDIO_TRACE, trace.PICO_HEADER.size + len(envelope), 0, 0,
    )
    return b"\0" + cobs_encode(header + envelope)


class ParseAudioStallTraceTest(unittest.TestCase):
    def test_decode_timeline_and_anomalies(self):
        capture = packet([
            record(100_000, 1, duration=20_000, handle=trace.INVALID_HANDLE, cid=0),
            record(101_000, 2),
            record(131_500, 3, duration=30_500),
            record(132_000, 4),
            record(160_000, 5, duration=28_000),
        ]) + packet([
            record(200_000, 10, duration=40_000, result=0x0800),
        ])

        records, snapshots = trace.parse_capture(capture)
        self.assertEqual(6, len(records))
        self.assertEqual([], snapshots)
        timeline = trace.build_timeline(records)
        self.assertEqual(1, len(timeline))
        self.assertEqual(31_500, timeline[0].can_send_now_us - timeline[0].generated_us)
        self.assertEqual(28_000, timeline[0].packet_sent_us - timeline[0].send_us)
        self.assertIn("can_send_wait_gt_25ms", trace.anomaly_reasons(records[2]))
        self.assertIn("ble_disconnect", trace.anomaly_reasons(records[-1]))

    def test_snapshot(self):
        values = [123_456] + list(range(1, 22))
        body = trace.SNAPSHOT.pack(*values)
        envelope = trace.TRACE_HEADER.pack(
            trace.TRACE_MAGIC, trace.TRACE_VERSION, trace.PAYLOAD_SNAPSHOT, 1, len(body),
        ) + body
        records, snapshots = trace.parse_capture(envelope)
        self.assertEqual([], records)
        self.assertEqual(1, len(snapshots))
        self.assertEqual(21, snapshots[0].counters["trace_dropped"])

    def test_tx_watchdog_events_keep_record_format_and_mark_reconnect(self):
        capture = packet([
            record(300_000, 16, duration=100_000),
            record(300_100, 17, duration=100_100),
            record(450_100, 18, duration=150_000, result=0x08),
        ])

        records, snapshots = trace.parse_capture(capture)
        self.assertEqual([], snapshots)
        self.assertEqual(
            ["AUDIO_TX_WATCHDOG", "AUDIO_TX_RECOVERY", "AUDIO_TX_RECONNECT"],
            [item.event for item in records],
        )
        self.assertIn("audio_tx_watchdog", trace.anomaly_reasons(records[0]))
        self.assertEqual([], trace.anomaly_reasons(records[1]))
        self.assertIn("audio_tx_reconnect", trace.anomaly_reasons(records[2]))

    def test_runtime_snapshot_merges_with_legacy_snapshot(self):
        legacy_values = [500_000] + list(range(1, 22))
        legacy_body = trace.SNAPSHOT.pack(*legacy_values)
        runtime_values = [500_000] + list(range(101, 115)) + [-55, -61] + list(range(115, 119))
        runtime_body = trace.RUNTIME_SNAPSHOT.pack(*runtime_values)

        def snapshot_packet(kind, body):
            envelope = trace.TRACE_HEADER.pack(
                trace.TRACE_MAGIC, trace.TRACE_VERSION, kind, 1, len(body)
            ) + body
            header = trace.PICO_HEADER.pack(
                trace.ASHA_TYPE_AUDIO_TRACE,
                trace.PICO_HEADER.size + len(envelope), 0, 0,
            )
            return b"\0" + cobs_encode(header + envelope)

        capture = snapshot_packet(trace.PAYLOAD_SNAPSHOT, legacy_body)
        capture += snapshot_packet(trace.PAYLOAD_RUNTIME_SNAPSHOT, runtime_body)
        records, snapshots = trace.parse_capture(capture)

        self.assertEqual([], records)
        self.assertEqual(1, len(snapshots))
        self.assertEqual(1, snapshots[0].counters["sdu_generated_count"])
        self.assertEqual(101, snapshots[0].counters["hci_write_count"])
        self.assertEqual(-55, snapshots[0].counters["rssi_slot0_dbm"])

    def test_serious_timer_and_platform_events_are_anomalies(self):
        records = [
            trace.TraceRecord(*trace.TRACE_RECORD.unpack(record(1, 13, duration=5_001))),
            trace.TraceRecord(*trace.TRACE_RECORD.unpack(record(2, 19, duration=70_000))),
            trace.TraceRecord(*trace.TRACE_RECORD.unpack(record(3, 21, duration=6_000))),
            trace.TraceRecord(*trace.TRACE_RECORD.unpack(record(4, 23, duration=2_000))),
        ]
        self.assertIn("audio_timer_late_gt_5ms", trace.anomaly_reasons(records[0]))
        self.assertIn("audio_tx_stale_drop", trace.anomaly_reasons(records[1]))
        self.assertIn("hci_write_slow", trace.anomaly_reasons(records[2]))
        self.assertIn("cyw43_lock_wait_slow", trace.anomaly_reasons(records[3]))

    def test_interrupted_capture_saves_bytes_and_discards_partial_frame(self):
        complete_packet = packet([record(100_000, 1, handle=trace.INVALID_HANDLE, cid=0)])
        partial_frame = b"\x05incomplete"

        class InterruptedSerial:
            closed = False

            def __init__(self, _port, *, baudrate, timeout):
                self.chunks = iter((complete_packet, partial_frame))
                self.next_size = len(complete_packet)
                self.baudrate = baudrate
                self.timeout = timeout

            def __enter__(self):
                return self

            def __exit__(self, _exc_type, _exc, _traceback):
                type(self).closed = True

            @property
            def in_waiting(self):
                return self.next_size

            def read(self, _size):
                try:
                    chunk = next(self.chunks)
                except StopIteration:
                    raise KeyboardInterrupt
                self.next_size = len(partial_frame)
                return chunk

        with tempfile.TemporaryDirectory(dir=MODULE_PATH.parent) as directory:
            raw_output = Path(directory) / "interrupted.bin"
            result = trace.capture_serial(
                "COM-test",
                60.0,
                115200,
                raw_output,
                serial_factory=InterruptedSerial,
                serial_errors=(OSError,),
                flush_interval=0.0,
            )

            expected = complete_packet + partial_frame
            self.assertTrue(result.interrupted)
            self.assertIsNone(result.error)
            self.assertTrue(InterruptedSerial.closed)
            self.assertEqual(expected, result.data)
            self.assertEqual(expected, raw_output.read_bytes())

            records, snapshots = trace.parse_capture(result.data)
            self.assertEqual(1, len(records))
            self.assertEqual([], snapshots)

    def test_serial_error_preserves_partial_capture_and_closes_port(self):
        complete_packet = packet([record(200_000, 10, result=0x0800)])

        class FailingSerial:
            closed = False

            def __init__(self, _port, *, baudrate, timeout):
                self.read_count = 0

            def __enter__(self):
                return self

            def __exit__(self, _exc_type, _exc, _traceback):
                type(self).closed = True

            @property
            def in_waiting(self):
                return len(complete_packet)

            def read(self, _size):
                if self.read_count == 0:
                    self.read_count += 1
                    return complete_packet
                raise OSError("device disconnected")

        with tempfile.TemporaryDirectory(dir=MODULE_PATH.parent) as directory:
            raw_output = Path(directory) / "disconnected.bin"
            result = trace.capture_serial(
                "COM-test",
                60.0,
                115200,
                raw_output,
                serial_factory=FailingSerial,
                serial_errors=(OSError,),
            )

            self.assertFalse(result.interrupted)
            self.assertEqual("OSError: device disconnected", result.error)
            self.assertTrue(FailingSerial.closed)
            self.assertEqual(complete_packet, result.data)
            self.assertEqual(complete_packet, raw_output.read_bytes())


if __name__ == "__main__":
    unittest.main()
