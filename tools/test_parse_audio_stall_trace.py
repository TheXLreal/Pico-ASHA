import importlib.util
import sys
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


if __name__ == "__main__":
    unittest.main()
