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


def record(timestamp, event, sequence=7, duration=0, result=0,
           handle=0x0040, cid=0x0041, write_index=10, read_index=9,
           detail0=0, detail1=0):
    return trace.TRACE_RECORD.pack(
        timestamp, write_index, read_index, duration, detail0, detail1, result,
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

    def test_timeline_associates_sequence_255_across_wrap(self):
        trace_records = [
            record(100_000, 1, sequence=254, handle=trace.INVALID_HANDLE,
                   cid=0, write_index=100, read_index=99),
            record(101_000, 2, sequence=254, write_index=100,
                   read_index=100),
            record(102_000, 3, sequence=254, write_index=100,
                   read_index=100),
            record(103_000, 4, sequence=254, write_index=100,
                   read_index=100),
            record(120_000, 1, sequence=255, handle=trace.INVALID_HANDLE,
                   cid=0, write_index=101, read_index=100),
            record(121_000, 2, sequence=255, write_index=101,
                   read_index=101),
            record(122_000, 3, sequence=255, write_index=101,
                   read_index=101),
            record(123_000, 4, sequence=255, write_index=101,
                   read_index=101),
            record(140_000, 1, sequence=0, handle=trace.INVALID_HANDLE,
                   cid=0, write_index=102, read_index=101),
            record(141_000, 2, sequence=0, write_index=102,
                   read_index=102),
            record(142_000, 3, sequence=0, write_index=102,
                   read_index=102),
            record(143_000, 4, sequence=0, write_index=102,
                   read_index=102),
        ]
        capture = b"".join(packet(trace_records[index:index + 5])
                            for index in range(0, len(trace_records), 5))

        records, _ = trace.parse_capture(capture)
        timeline = trace.build_timeline(records)

        self.assertEqual([254, 255, 0], [row.sequence for row in timeline])
        self.assertEqual([254, 255, 256],
                         [row.sequence_unwrapped for row in timeline])
        event_rows = list(trace.records_as_rows(records))
        sequence_255_rows = [row for row in event_rows
                             if row["timestamp_us"] in (120_000, 121_000,
                                                         122_000, 123_000)]
        self.assertTrue(all(row["sequence"] == 255
                            for row in sequence_255_rows))

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

    def test_audio_no_progress_and_reconnect_transition_events(self):
        capture = packet([
            record(500_000, 40, duration=101_000, detail0=10_000,
                   detail1=(0 << 16) | 7, write_index=20, read_index=14),
            record(500_100, 41, duration=101_100, detail0=10_100,
                   detail1=(1 << 16) | 7),
            record(650_100, 42, duration=251_100, detail0=9_000,
                   detail1=(2 << 16) | 6, result=0x08),
            record(650_200, 43, duration=1, result=0x08),
            record(12_650_200, 47, duration=3, result=0x08),
        ])

        records, _ = trace.parse_capture(capture)
        self.assertEqual(
            ["AUDIO_NO_PROGRESS", "AUDIO_NO_PROGRESS_REARMED",
             "AUDIO_NO_PROGRESS_RECONNECT", "RECONNECT_SCHEDULED",
             "RECONNECT_TIMEOUT"],
            [item.event for item in records],
        )
        self.assertIn("audio_no_progress", trace.anomaly_reasons(records[0]))
        self.assertIn("audio_no_progress_reconnect",
                      trace.anomaly_reasons(records[2]))
        self.assertIn("reconnect_timeout", trace.anomaly_reasons(records[4]))
        self.assertIn("tx_state=Idle", trace.event_details(records[0]))
        self.assertIn("attempt=3", trace.event_details(records[4]))

    def test_zero_credit_transitions_decode_duration_credits_and_rssi(self):
        capture = packet([
            record(700_000, 48, sequence=111, duration=0, detail0=0,
                   detail1=100_000, result=0x100 | ((-88) & 0xff)),
            record(747_599, 49, sequence=111, duration=47_599, detail0=2,
                   detail1=125_000, result=0x100 | ((-95) & 0xff)),
        ])

        records, _ = trace.parse_capture(capture)
        self.assertEqual(
            ["AUDIO_CREDITS_ZERO_ENTER", "AUDIO_CREDITS_ZERO_EXIT"],
            [item.event for item in records],
        )
        self.assertIn("available_audio_credits=0",
                      trace.event_details(records[0]))
        details = trace.event_details(records[1])
        self.assertIn("zero_credit_duration_us=47599", details)
        self.assertIn("available_audio_credits=2", details)
        self.assertIn("rssi_dbm=-95", details)
        self.assertIn("rssi_age_us=125000", details)
        self.assertEqual([], trace.anomaly_reasons(records[0]))
        self.assertIn("audio_credits_zero",
                      trace.anomaly_reasons(records[1]))

    def test_tx_stall_phase_diagnostics_decode_blockers_and_progress_ages(self):
        blocked_state = (31 | (0 << 16))
        wait_state = (31 | (1 << 16))
        packet_state = (30 | (2 << 16))
        capture = packet([
            record(1_000_000, 50, duration=2_000, detail0=450,
                   detail1=3_000, result=449,
                   handle=trace.INVALID_HANDLE, cid=0),
            record(1_000_010, 51, duration=10, detail0=450,
                   detail1=2_010, result=3_010,
                   handle=trace.INVALID_HANDLE, cid=0),
            record(1_050_000, 52, duration=50_000,
                   detail0=(1 << 8) | (1 << 15), detail1=1_200,
                   result=blocked_state),
            record(1_075_000, 53, duration=25_000, detail0=500,
                   detail1=700, result=wait_state),
            record(1_125_000, 54, duration=50_000, detail0=50_000,
                   detail1=51_000, result=packet_state),
        ])

        records, _ = trace.parse_capture(capture)
        self.assertEqual(
            ["CORE1_RUN_LOOP_HEARTBEAT", "PROCESS_AUDIO_ENTER",
             "TX_BLOCKED", "CAN_SEND_REQUEST_AGE",
             "PACKET_SENT_WAIT_AGE"],
            [item.event for item in records],
        )
        blocked = trace.event_details(records[2])
        self.assertIn("blocker_mask=0x00008100", blocked)
        self.assertIn("audio_busy|invariant_not_armed", blocked)
        self.assertIn("available_audio_credits=31", blocked)
        self.assertIn("tx_blocked", trace.anomaly_reasons(records[2]))
        self.assertIn("tx_state=WaitingCanSendNow",
                      trace.event_details(records[3]))
        self.assertIn("hci_transport_progress_age_us=500",
                      trace.event_details(records[3]))
        self.assertIn("packet_sent_wait_aged",
                      trace.anomaly_reasons(records[4]))

    def test_extended_runtime_snapshot_adds_progress_counters(self):
        values = [900_000] + list(range(1, 29))
        body = trace.RUNTIME_SNAPSHOT_EXTENDED.pack(*values)
        payload = trace.TRACE_HEADER.pack(
            trace.TRACE_MAGIC, trace.TRACE_VERSION,
            trace.PAYLOAD_RUNTIME_SNAPSHOT, 1, len(body),
        ) + body

        records, snapshots = trace.parse_trace_payload(payload)
        self.assertEqual([], records)
        self.assertEqual(1, len(snapshots))
        self.assertEqual(21, snapshots[0].counters["core1_run_loop_count"])
        self.assertEqual(28,
                         snapshots[0].counters["hci_controller_progress_age_us"])

    def test_audio_content_diagnostics_and_manual_marker_decode(self):
        capture = packet([
            record(1_200_000, 55, sequence=255, duration=1_002,
                   detail0=30_001, detail1=42,
                   result=(1 << 16) | 48),
            record(1_200_100, 56, sequence=255, duration=2,
                   detail0=0x11223344, detail1=0x55667788,
                   result=123, read_index=122),
            record(1_200_200, 57, sequence=255, duration=91,
                   detail0=0x01020304, detail1=0x05060708,
                   result=0x10203040, write_index=123),
        ])

        records, _ = trace.parse_capture(capture)
        self.assertEqual(
            ["AUDIO_PCM_DISCONTINUITY", "AUDIO_G722_INTEGRITY_ERROR",
             "USER_AUDIO_GLITCH_MARKER"],
            [item.event for item in records],
        )
        self.assertIn("channels=left", trace.event_details(records[0]))
        self.assertIn("stage=ring_checksum", trace.event_details(records[1]))
        self.assertIn("latest_pcm_age_us=91",
                      trace.event_details(records[2]))
        self.assertIn("pcm_boundary_discontinuity",
                      trace.anomaly_reasons(records[0]))
        self.assertIn("g722_integrity_error",
                      trace.anomaly_reasons(records[1]))
        self.assertEqual([], trace.anomaly_reasons(records[2]))

    def test_glitch_marker_command_uses_existing_cdc_command_frame(self):
        frames = list(trace.iter_cobs_frames(
            trace.build_audio_glitch_marker_command()
        ))
        self.assertEqual(1, len(frames))
        packet_type, declared_length, connection_id, timestamp_ms = (
            trace.PICO_HEADER.unpack_from(frames[0])
        )
        self.assertEqual(trace.PICO_TYPE_COMMAND, packet_type)
        self.assertEqual(len(frames[0]), declared_length)
        self.assertEqual(0, connection_id)
        self.assertEqual(0, timestamp_ms)
        command, status = trace.COMMAND_PACKET.unpack_from(
            frames[0], trace.PICO_HEADER.size
        )
        self.assertEqual(trace.COMMAND_AUDIO_GLITCH_MARKER, command)
        self.assertEqual(0, status)

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

    def test_live_capture_can_send_nonblocking_glitch_marker(self):
        class MarkerSerial:
            writes: list[bytes] = []

            def __init__(self, _port, *, baudrate, timeout):
                self.baudrate = baudrate
                self.timeout = timeout

            def __enter__(self):
                return self

            def __exit__(self, _exc_type, _exc, _traceback):
                return None

            @property
            def in_waiting(self):
                return 0

            def write(self, data):
                type(self).writes.append(data)
                return len(data)

            def read(self, _size):
                return b""

        with tempfile.TemporaryDirectory(dir=MODULE_PATH.parent) as directory:
            raw_output = Path(directory) / "marker.bin"
            marker_polls = iter((True,))
            result = trace.capture_serial(
                "COM-test", 0.01, 115200, raw_output,
                serial_factory=MarkerSerial,
                serial_errors=(OSError,),
                mark_glitches=True,
                marker_poll=lambda: next(marker_polls, False),
            )

        self.assertFalse(result.interrupted)
        self.assertIsNone(result.error)
        self.assertEqual(1, result.marker_count)
        self.assertEqual([trace.build_audio_glitch_marker_command()],
                         MarkerSerial.writes)

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
