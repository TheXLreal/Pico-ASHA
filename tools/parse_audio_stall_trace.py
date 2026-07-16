#!/usr/bin/env python3
"""Decode Pico-ASHA binary audio-stall traces and build per-sequence timelines."""

from __future__ import annotations

import argparse
import csv
import dataclasses
import struct
import sys
import time
from collections import defaultdict
from pathlib import Path
from typing import Iterable, Iterator, Sequence


TRACE_MAGIC = 0x52545341  # b"ASTR" decoded as little endian
TRACE_VERSION = 1
ASHA_TYPE_AUDIO_TRACE = 7
INVALID_HANDLE = 0xFFFF
INVALID_SEQUENCE = 0xFF
ANOMALY_US = 25_000
RING_CAPACITY = 8

PICO_HEADER = struct.Struct("<BBHI")
TRACE_HEADER = struct.Struct("<IBBBB")
TRACE_RECORD = struct.Struct("<QIIIIIiHHBBBB")
SNAPSHOT = struct.Struct("<Q" + "I" * 21)
RUNTIME_SNAPSHOT = struct.Struct("<Q" + "I" * 14 + "i" * 2 + "I" * 4)

PAYLOAD_RECORDS = 1
PAYLOAD_SNAPSHOT = 2
PAYLOAD_RUNTIME_SNAPSHOT = 3

EVENT_NAMES = {
    1: "AUDIO_SDU_GENERATED",
    2: "AUDIO_CAN_SEND_REQUESTED",
    3: "AUDIO_CAN_SEND_NOW",
    4: "AUDIO_L2CAP_SEND",
    5: "AUDIO_PACKET_SENT",
    6: "AUDIO_BUSY_SET",
    7: "AUDIO_BUSY_CLEAR",
    8: "RING_UNDERRUN",
    9: "RING_OVERRUN",
    10: "BLE_DISCONNECT",
    11: "BLE_CONNECT",
    12: "AUDIO_BUSY_STALL",
    13: "AUDIO_TIMER_LATE",
    14: "USB_PCM_UNDERRUN",
    15: "RSSI_SAMPLE",
    16: "AUDIO_TX_WATCHDOG",
    17: "AUDIO_TX_RECOVERY",
    18: "AUDIO_TX_RECONNECT",
    19: "AUDIO_TX_STALE_DROP",
    20: "HCI_WRITE_BEGIN",
    21: "HCI_WRITE_END",
    22: "CYW43_LOCK_WAIT_BEGIN",
    23: "CYW43_LOCK_ACQUIRED",
    24: "CYW43_LOCK_HELD",
    25: "RSSI_CONTEXT",
}

SNAPSHOT_FIELDS = (
    "sdu_generated_count",
    "l2cap_send_attempt_count",
    "l2cap_send_success_count",
    "l2cap_send_error_count",
    "can_send_wait_last_us",
    "can_send_wait_max_us",
    "packet_sent_wait_max_us",
    "audio_busy_current_duration_us",
    "audio_busy_max_duration_us",
    "ring_fill_current",
    "ring_fill_min",
    "ring_fill_max",
    "ring_underrun_count",
    "ring_overrun_count",
    "sequence_generated",
    "sequence_sent",
    "sequence_skip_count",
    "audio_timer_gap_max_us",
    "audio_timer_late_count",
    "usb_pcm_underrun_count",
    "trace_dropped",
)

RUNTIME_SNAPSHOT_FIELDS = (
    "hci_write_count",
    "hci_write_error_count",
    "hci_write_last_us",
    "hci_write_max_us",
    "cyw43_lock_wait_last_us",
    "cyw43_lock_wait_max_us",
    "cyw43_lock_hold_last_us",
    "cyw43_lock_hold_max_us",
    "btstack_run_loop_gap_last_us",
    "btstack_run_loop_gap_max_us",
    "hci_to_packet_sent_last_us",
    "hci_to_packet_sent_max_us",
    "rssi_sample_count",
    "rssi_request_skipped_count",
    "rssi_slot0_dbm",
    "rssi_slot1_dbm",
    "rssi_slot0_age_us",
    "rssi_slot1_age_us",
    "tx_stale_drop_count",
    "tx_stale_drop_frames",
)


@dataclasses.dataclass(frozen=True)
class TraceRecord:
    timestamp_us: int
    write_index: int
    read_index: int
    duration_us: int
    detail0: int
    detail1: int
    result: int
    connection_handle: int
    l2cap_cid: int
    event_type: int
    sequence: int
    ring_fill: int
    audio_busy: int

    @property
    def event(self) -> str:
        return EVENT_NAMES.get(self.event_type, f"UNKNOWN_{self.event_type}")


@dataclasses.dataclass(frozen=True)
class TraceSnapshot:
    timestamp_us: int
    counters: dict[str, int]


@dataclasses.dataclass
class TimelineRow:
    connection_handle: int
    l2cap_cid: int
    sequence: int
    generated_us: int | None = None
    requested_us: int | None = None
    can_send_now_us: int | None = None
    send_us: int | None = None
    packet_sent_us: int | None = None
    l2cap_result: int | None = None
    ring_fill_at_request: int | None = None
    sequence_unwrapped: int | None = None


@dataclasses.dataclass(frozen=True)
class CaptureResult:
    data: bytes
    interrupted: bool = False
    error: str | None = None


def cobs_decode(frame: bytes) -> bytes:
    """Decode one COBS frame, including its trailing zero delimiter."""
    if len(frame) < 2 or frame[-1] != 0 or frame[0] == 0:
        raise ValueError("invalid COBS frame")
    output = bytearray()
    index = 0
    end = len(frame) - 1
    while index < end:
        code = frame[index]
        if code == 0:
            raise ValueError("zero COBS code")
        index += 1
        block_end = index + code - 1
        if block_end > end:
            raise ValueError("COBS block exceeds frame")
        output.extend(frame[index:block_end])
        index = block_end
        if code != 0xFF and index < end:
            output.append(0)
    return bytes(output)


def iter_cobs_frames(data: bytes) -> Iterator[bytes]:
    """Yield valid decoded frames from a raw CDC byte capture."""
    start = 0
    while True:
        delimiter = data.find(b"\0", start)
        if delimiter < 0:
            return
        encoded = data[start:delimiter]
        start = delimiter + 1
        if not encoded:
            continue
        try:
            yield cobs_decode(encoded + b"\0")
        except ValueError:
            # The capture can begin mid-frame; resynchronise at the next zero.
            continue


def parse_trace_payload(payload: bytes) -> tuple[list[TraceRecord], list[TraceSnapshot]]:
    if len(payload) < TRACE_HEADER.size:
        return [], []
    magic, version, kind, count, payload_size = TRACE_HEADER.unpack_from(payload)
    if magic != TRACE_MAGIC or version != TRACE_VERSION:
        return [], []
    body = payload[TRACE_HEADER.size:]
    if payload_size != len(body):
        return [], []

    records: list[TraceRecord] = []
    snapshots: list[TraceSnapshot] = []
    if kind == PAYLOAD_RECORDS:
        if count * TRACE_RECORD.size != len(body):
            return [], []
        for offset in range(0, len(body), TRACE_RECORD.size):
            records.append(TraceRecord(*TRACE_RECORD.unpack_from(body, offset)))
    elif kind == PAYLOAD_SNAPSHOT:
        if count != 1 or len(body) != SNAPSHOT.size:
            return [], []
        values = SNAPSHOT.unpack(body)
        snapshots.append(TraceSnapshot(values[0], dict(zip(SNAPSHOT_FIELDS, values[1:]))))
    elif kind == PAYLOAD_RUNTIME_SNAPSHOT:
        if count != 1 or len(body) != RUNTIME_SNAPSHOT.size:
            return [], []
        values = RUNTIME_SNAPSHOT.unpack(body)
        snapshots.append(TraceSnapshot(
            values[0], dict(zip(RUNTIME_SNAPSHOT_FIELDS, values[1:]))
        ))
    return records, snapshots


def merge_snapshots(snapshots: Iterable[TraceSnapshot]) -> list[TraceSnapshot]:
    merged: dict[int, dict[str, int]] = {}
    for snapshot in snapshots:
        merged.setdefault(snapshot.timestamp_us, {}).update(snapshot.counters)
    return [TraceSnapshot(timestamp, merged[timestamp]) for timestamp in sorted(merged)]


def parse_decoded_packet(packet: bytes) -> tuple[list[TraceRecord], list[TraceSnapshot]]:
    if packet.startswith(b"ASTR"):
        return parse_trace_payload(packet)
    if len(packet) < PICO_HEADER.size:
        return [], []
    packet_type, declared_length, _connection_id, _timestamp_ms = PICO_HEADER.unpack_from(packet)
    if packet_type != ASHA_TYPE_AUDIO_TRACE or declared_length != len(packet):
        return [], []
    return parse_trace_payload(packet[PICO_HEADER.size:])


def parse_capture(data: bytes) -> tuple[list[TraceRecord], list[TraceSnapshot]]:
    """Parse raw CDC COBS data or one already-decoded packet/payload."""
    direct_records, direct_snapshots = parse_decoded_packet(data)
    if direct_records or direct_snapshots:
        return direct_records, direct_snapshots

    records: list[TraceRecord] = []
    snapshots: list[TraceSnapshot] = []
    for packet in iter_cobs_frames(data):
        packet_records, packet_snapshots = parse_decoded_packet(packet)
        records.extend(packet_records)
        snapshots.extend(packet_snapshots)
    records.sort(key=lambda record: record.timestamp_us)
    return records, merge_snapshots(snapshots)


def anomaly_reasons(record: TraceRecord) -> list[str]:
    reasons: list[str] = []
    if record.event_type == 1 and record.duration_us > ANOMALY_US:
        reasons.append("sdu_generation_interval_gt_25ms")
    if record.event_type == 3 and record.duration_us > ANOMALY_US:
        reasons.append("can_send_wait_gt_25ms")
    if record.event_type == 5 and record.duration_us > ANOMALY_US:
        reasons.append("packet_sent_wait_gt_25ms")
    if record.event_type in (7, 12) and record.duration_us > ANOMALY_US:
        reasons.append("audio_busy_gt_25ms")
    if record.event_type == 4 and record.result != 0:
        reasons.append("l2cap_send_error")
    if record.event_type == 8:
        reasons.append("ring_underrun")
    if record.event_type == 9:
        reasons.append("ring_overrun")
    if record.event_type == 10:
        reasons.append("ble_disconnect")
    if record.event_type == 13:
        reasons.append("audio_timer_late_gt_5ms")
    if record.event_type == 14:
        reasons.append("usb_pcm_underrun")
    if record.event_type == 16:
        reasons.append("audio_tx_watchdog")
    if record.event_type == 18:
        reasons.append("audio_tx_reconnect")
    if record.event_type == 19:
        reasons.append("audio_tx_stale_drop")
    if record.event_type == 21:
        reasons.append("hci_write_slow")
    if record.event_type == 23:
        reasons.append("cyw43_lock_wait_slow")
    if record.event_type == 24:
        reasons.append("cyw43_lock_held_slow")
    if record.ring_fill >= RING_CAPACITY:
        reasons.append("ring_full")
    return reasons


def event_details(record: TraceRecord) -> str:
    if record.event_type == 4:
        return f"sdu_size={record.detail0}"
    if record.event_type in (6, 7, 12):
        return f"busy_context={record.result}"
    if record.event_type == 10:
        status = record.result & 0xFF
        reason = (record.result >> 8) & 0xFF
        last_send = "unknown" if record.detail0 == 0xFFFFFFFF else str(record.timestamp_us - record.detail0)
        return (f"status=0x{status:02x};reason=0x{reason:02x};"
                f"last_send_age_us={record.detail0};last_send_timestamp_us={last_send}")
    if record.event_type == 11:
        latency = record.detail1 & 0xFFFF
        timeout = record.detail1 >> 16
        return (f"connection_interval_units={record.detail0};"
                f"connection_interval_ms={record.detail0 * 1.25:g};"
                f"peripheral_latency={latency};supervision_timeout_units={timeout};"
                f"supervision_timeout_ms={timeout * 10}")
    if record.event_type == 15:
        return f"rssi_dbm={record.result}"
    if record.event_type in (16, 17, 18):
        rssi_valid = bool(record.detail1 & 0x100)
        rssi = (record.detail1 & 0xFF)
        if rssi >= 128:
            rssi -= 256
        return (f"rssi_dbm={rssi if rssi_valid else 'unknown'};"
                f"rssi_age_us={record.detail0}")
    if record.event_type == 19:
        return (f"old_sequence={record.sequence};new_sequence={record.detail1 & 0xff};"
                f"dropped_frames={record.detail0};age_us={record.duration_us}")
    if record.event_type in (20, 21):
        return f"hci_packet_type={record.detail0};duration_us={record.duration_us}"
    if record.event_type in (22, 23, 24):
        return f"duration_us={record.duration_us}"
    if record.event_type == 25:
        return f"rssi_dbm={record.result};sample_age_us={record.duration_us}"
    return ""


def records_as_rows(records: Iterable[TraceRecord]) -> Iterator[dict[str, object]]:
    for record in records:
        reasons = anomaly_reasons(record)
        yield {
            "timestamp_us": record.timestamp_us,
            "timestamp_s": f"{record.timestamp_us / 1_000_000:.6f}",
            "event": record.event,
            "connection_handle": f"0x{record.connection_handle:04x}",
            "l2cap_cid": f"0x{record.l2cap_cid:04x}",
            "sequence": "" if record.sequence == INVALID_SEQUENCE else record.sequence,
            "write_index": record.write_index,
            "read_index": record.read_index,
            "ring_fill": record.ring_fill,
            "audio_busy": record.audio_busy,
            "duration_us": record.duration_us,
            "result": record.result,
            "detail0": record.detail0,
            "detail1": record.detail1,
            "details": event_details(record),
            "anomaly": bool(reasons),
            "anomaly_reasons": ";".join(reasons),
        }


def build_timeline(records: Sequence[TraceRecord]) -> list[TimelineRow]:
    generated: dict[int, list[int]] = defaultdict(list)
    current: dict[tuple[int, int], TimelineRow] = {}
    timeline: list[TimelineRow] = []

    for record in sorted(records, key=lambda item: item.timestamp_us):
        if record.event_type == 1 and record.sequence != INVALID_SEQUENCE:
            generated[record.sequence].append(record.timestamp_us)
            continue
        if record.connection_handle == INVALID_HANDLE:
            continue
        key = (record.connection_handle, record.l2cap_cid)
        if record.event_type == 2:
            generated_us = next(
                (timestamp for timestamp in reversed(generated.get(record.sequence, []))
                 if timestamp <= record.timestamp_us),
                None,
            )
            row = TimelineRow(
                connection_handle=record.connection_handle,
                l2cap_cid=record.l2cap_cid,
                sequence=record.sequence,
                generated_us=generated_us,
                requested_us=record.timestamp_us,
                ring_fill_at_request=record.ring_fill,
            )
            timeline.append(row)
            current[key] = row
            continue
        row = current.get(key)
        if row is None or row.sequence != record.sequence:
            continue
        if record.event_type == 3:
            row.can_send_now_us = record.timestamp_us
        elif record.event_type == 4:
            row.send_us = record.timestamp_us
            row.l2cap_result = record.result
        elif record.event_type == 5:
            row.packet_sent_us = record.timestamp_us

    unwrap_state: dict[tuple[int, int], tuple[int, int]] = {}
    for row in timeline:
        key = (row.connection_handle, row.l2cap_cid)
        epoch, previous = unwrap_state.get(key, (0, row.sequence))
        if row.sequence < previous and previous - row.sequence > 128:
            epoch += 256
        row.sequence_unwrapped = epoch + row.sequence
        unwrap_state[key] = (epoch, row.sequence)
    return timeline


def elapsed(end: int | None, start: int | None) -> int | None:
    return None if end is None or start is None else end - start


def timeline_as_rows(timeline: Iterable[TimelineRow]) -> Iterator[dict[str, object]]:
    for row in timeline:
        generated_to_can = elapsed(row.can_send_now_us, row.generated_us)
        request_to_can = elapsed(row.can_send_now_us, row.requested_us)
        can_to_send = elapsed(row.send_us, row.can_send_now_us)
        send_to_packet = elapsed(row.packet_sent_us, row.send_us)
        total = elapsed(row.packet_sent_us, row.generated_us)
        reasons = []
        for label, value in (
            ("generated_to_can_send_now_gt_25ms", generated_to_can),
            ("request_to_can_send_now_gt_25ms", request_to_can),
            ("send_to_packet_sent_gt_25ms", send_to_packet),
        ):
            if value is not None and value > ANOMALY_US:
                reasons.append(label)
        if row.l2cap_result not in (None, 0):
            reasons.append("l2cap_send_error")
        yield {
            "connection_handle": f"0x{row.connection_handle:04x}",
            "l2cap_cid": f"0x{row.l2cap_cid:04x}",
            "sequence": row.sequence,
            "sequence_unwrapped": row.sequence_unwrapped,
            "generated_us": row.generated_us,
            "can_send_requested_us": row.requested_us,
            "can_send_now_us": row.can_send_now_us,
            "l2cap_send_us": row.send_us,
            "packet_sent_us": row.packet_sent_us,
            "generated_to_can_send_now_us": generated_to_can,
            "request_to_can_send_now_us": request_to_can,
            "can_send_now_to_send_us": can_to_send,
            "send_to_packet_sent_us": send_to_packet,
            "generated_to_packet_sent_us": total,
            "ring_fill_at_request": row.ring_fill_at_request,
            "l2cap_result": row.l2cap_result,
            "anomaly": bool(reasons),
            "anomaly_reasons": ";".join(reasons),
        }


def snapshot_as_rows(snapshots: Iterable[TraceSnapshot]) -> Iterator[dict[str, object]]:
    for snapshot in snapshots:
        yield {
            "timestamp_us": snapshot.timestamp_us,
            "timestamp_s": f"{snapshot.timestamp_us / 1_000_000:.6f}",
            **snapshot.counters,
        }


def pre_disconnect_as_rows(records: Sequence[TraceRecord]) -> Iterator[dict[str, object]]:
    disconnects = [record for record in records if record.event_type == 10]
    for disconnect_number, disconnect in enumerate(disconnects, start=1):
        window_start = max(0, disconnect.timestamp_us - 5_000_000)
        for row in records_as_rows(
            record for record in records
            if window_start <= record.timestamp_us <= disconnect.timestamp_us
        ):
            yield {
                "disconnect_number": disconnect_number,
                "disconnect_timestamp_us": disconnect.timestamp_us,
                **row,
            }


def write_csv(path: Path, rows: Iterable[dict[str, object]]) -> int:
    materialized = list(rows)
    path.parent.mkdir(parents=True, exist_ok=True)
    if not materialized:
        path.write_text("", encoding="utf-8")
        return 0
    with path.open("w", newline="", encoding="utf-8") as output:
        writer = csv.DictWriter(output, fieldnames=list(materialized[0]))
        writer.writeheader()
        writer.writerows(materialized)
    return len(materialized)


def capture_serial(
    port: str,
    seconds: float,
    baudrate: int,
    raw_output: Path,
    *,
    serial_factory=None,
    serial_errors: tuple[type[BaseException], ...] | None = None,
    flush_interval: float = 1.0,
) -> CaptureResult:
    """Capture CDC bytes while continuously preserving them in *raw_output*.

    KeyboardInterrupt and serial-port failures are converted into a partial
    CaptureResult. The serial context manager and output-file context manager
    are both exited before the captured bytes are returned.
    """
    if serial_factory is None:
        try:
            import serial  # type: ignore[import-not-found]
        except ImportError as error:
            raise RuntimeError("live capture requires pyserial: python -m pip install pyserial") from error
        serial_factory = serial.Serial
        serial_errors = (serial.SerialException, OSError)
    elif serial_errors is None:
        serial_errors = (OSError,)

    raw_output.parent.mkdir(parents=True, exist_ok=True)
    deadline = time.monotonic() + seconds
    next_flush = time.monotonic() + max(0.0, flush_interval)
    interrupted = False
    capture_error: str | None = None

    # Open the raw file first, so even failure while opening the COM port leaves
    # a well-defined capture artifact. Each received chunk is persisted before
    # the next potentially interruptible serial read.
    with raw_output.open("wb") as output:
        try:
            with serial_factory(port, baudrate=baudrate, timeout=0.1) as connection:
                while time.monotonic() < deadline:
                    chunk = connection.read(max(1, connection.in_waiting))
                    if chunk:
                        output.write(chunk)
                    now = time.monotonic()
                    if now >= next_flush:
                        output.flush()
                        next_flush = now + max(0.0, flush_interval)
        except KeyboardInterrupt:
            interrupted = True
        except serial_errors as error:
            capture_error = f"{type(error).__name__}: {error}"
        finally:
            output.flush()

    return CaptureResult(
        data=raw_output.read_bytes(),
        interrupted=interrupted,
        error=capture_error,
    )


def output_paths(base: Path, args: argparse.Namespace) -> tuple[Path, Path, Path, Path]:
    stem = base.with_suffix("")
    return (
        args.csv or Path(f"{stem}_events.csv"),
        args.timeline or Path(f"{stem}_timeline.csv"),
        args.snapshots or Path(f"{stem}_snapshots.csv"),
        args.pre_disconnect or Path(f"{stem}_pre_disconnect_5s.csv"),
    )


def build_argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", nargs="?", type=Path, help="raw CDC COBS capture")
    parser.add_argument("--csv", type=Path, help="event CSV output")
    parser.add_argument("--timeline", type=Path, help="per-sequence timeline CSV output")
    parser.add_argument("--snapshots", type=Path, help="aggregate snapshot CSV output")
    parser.add_argument("--pre-disconnect", type=Path, help="last-five-seconds CSV output")
    parser.add_argument("--capture-port", help="capture directly from a serial port (close the GUI first)")
    parser.add_argument("--capture-seconds", type=float, default=60.0)
    parser.add_argument("--baudrate", type=int, default=115200, help="CDC nominal baudrate")
    parser.add_argument("--raw-output", type=Path, default=Path("audio_stall_trace.bin"))
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = build_argument_parser().parse_args(argv)
    if args.capture_port:
        try:
            capture = capture_serial(
                args.capture_port,
                args.capture_seconds,
                args.baudrate,
                args.raw_output,
            )
        except RuntimeError as error:
            print(f"error: {error}", file=sys.stderr)
            return 2
        data = capture.data
        source = args.raw_output
        print(f"captured {len(data)} bytes; raw capture saved to {source}")
        if capture.interrupted:
            print("capture interrupted by Ctrl+C; parsing saved partial data", file=sys.stderr)
        if capture.error:
            print(f"serial capture stopped: {capture.error}; parsing saved partial data",
                  file=sys.stderr)
    elif args.input:
        source = args.input
        data = source.read_bytes()
    else:
        print("error: provide INPUT or --capture-port", file=sys.stderr)
        return 2

    records, snapshots = parse_capture(data)
    if not records and not snapshots:
        print("error: no Pico-ASHA audio-stall trace packets found", file=sys.stderr)
        return 1

    event_path, timeline_path, snapshot_path, pre_disconnect_path = output_paths(source, args)
    timeline = build_timeline(records)
    event_count = write_csv(event_path, records_as_rows(records))
    timeline_count = write_csv(timeline_path, timeline_as_rows(timeline))
    snapshot_count = write_csv(snapshot_path, snapshot_as_rows(snapshots))
    pre_disconnect_count = write_csv(pre_disconnect_path, pre_disconnect_as_rows(records))

    anomaly_count = sum(bool(anomaly_reasons(record)) for record in records)
    disconnects = [record for record in records if record.event_type == 10]
    print(f"decoded {event_count} records, {snapshot_count} snapshots, {anomaly_count} anomalies")
    print(f"timeline rows: {timeline_count}")
    for number, record in enumerate(disconnects, start=1):
        status = record.result & 0xFF
        reason = (record.result >> 8) & 0xFF
        print(
            f"disconnect {number}: {record.timestamp_us / 1_000_000:.6f}s "
            f"handle=0x{record.connection_handle:04x} cid=0x{record.l2cap_cid:04x} "
            f"status=0x{status:02x} reason=0x{reason:02x} "
            f"busy={record.duration_us}us last_send_age={record.detail0}us"
        )
    print(f"events: {event_path}")
    print(f"timeline: {timeline_path}")
    print(f"snapshots: {snapshot_path}")
    print(f"last 5s before disconnect ({pre_disconnect_count} rows): {pre_disconnect_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
