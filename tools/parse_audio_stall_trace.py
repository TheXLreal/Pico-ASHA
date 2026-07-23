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
PICO_TYPE_COMMAND = 4
COMMAND_AUDIO_GLITCH_MARKER = 8
INVALID_HANDLE = 0xFFFF
INVALID_SEQUENCE = 0xFF
ANOMALY_US = 25_000
SEND_DELAY_WARNING_US = 50_000
RING_CAPACITY = 8

PICO_HEADER = struct.Struct("<BBHI")
TRACE_HEADER = struct.Struct("<IBBBB")
TRACE_RECORD = struct.Struct("<QIIIIIiHHBBBB")
COMMAND_PACKET = struct.Struct("<BB10x")
SNAPSHOT = struct.Struct("<Q" + "I" * 21)
RUNTIME_SNAPSHOT = struct.Struct("<Q" + "I" * 14 + "i" * 2 + "I" * 4)
RUNTIME_SNAPSHOT_EXTENDED = struct.Struct(
    "<Q" + "I" * 14 + "i" * 2 + "I" * 12
)

PAYLOAD_RECORDS = 1
PAYLOAD_SNAPSHOT = 2
PAYLOAD_RUNTIME_SNAPSHOT = 3

EVENT_NAMES = {
    1: "AUDIO_SDU_GENERATED",
    2: "AUDIO_SEND_REQUESTED",
    3: "AUDIO_CAN_SEND_NOW_RECEIVED",
    4: "AUDIO_L2CAP_SEND_COMPLETE",
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
    26: "AUDIO_L2CAP_SEND_BEGIN",
    27: "AUDIO_SEND_STATE_CHANGED",
    28: "AUDIO_SEND_DELAY_WARNING",
    29: "AUDIO_STALE_FRAMES_DROPPED",
    30: "AUDIO_STALE_DROP_CONTEXT",
    31: "AUDIO_SEQUENCE_SKIP_COUNT_CHANGED",
    32: "HCI_CONNECTION_OPENED",
    33: "HCI_DISCONNECTION_COMPLETE",
    34: "L2CAP_CHANNEL_OPENED",
    35: "L2CAP_CHANNEL_CLOSED",
    36: "ASHA_DEVICE_CONNECTED",
    37: "ASHA_DEVICE_DISCONNECTED",
    38: "SYSTEM_BOOT",
    39: "WATCHDOG_RESET_REQUESTED",
    40: "AUDIO_NO_PROGRESS",
    41: "AUDIO_NO_PROGRESS_REARMED",
    42: "AUDIO_NO_PROGRESS_RECONNECT",
    43: "RECONNECT_SCHEDULED",
    44: "SCAN_START_REQUESTED",
    45: "CONNECT_ATTEMPT",
    46: "CONNECT_COMPLETE",
    47: "RECONNECT_TIMEOUT",
    48: "AUDIO_CREDITS_ZERO_ENTER",
    49: "AUDIO_CREDITS_ZERO_EXIT",
    50: "CORE1_RUN_LOOP_HEARTBEAT",
    51: "PROCESS_AUDIO_ENTER",
    52: "TX_BLOCKED",
    53: "CAN_SEND_REQUEST_AGE",
    54: "PACKET_SENT_WAIT_AGE",
    55: "AUDIO_PCM_DISCONTINUITY",
    56: "AUDIO_G722_INTEGRITY_ERROR",
    57: "USER_AUDIO_GLITCH_MARKER",
}

DELAY_NAMES = {
    1: "sdu_generated_to_send_requested",
    2: "send_requested_to_can_send_now",
    3: "can_send_now_to_l2cap_send",
    4: "previous_successful_send_to_next_request",
}

TX_STATE_NAMES = {
    0: "Idle",
    1: "WaitingCanSendNow",
    2: "WaitingPacketSent",
}

TX_STATE_REASON_NAMES = {
    1: "local_recovery",
    2: "disconnect",
    3: "reset",
}

TX_BLOCKER_NAMES = {
    1 << 0: "not_connected",
    1 << 1: "not_streaming",
    1 << 2: "l2cap_not_ready",
    1 << 3: "no_sdu_available",
    1 << 4: "sdu_not_fresh",
    1 << 5: "waiting_can_send_now",
    1 << 6: "waiting_packet_sent",
    1 << 7: "can_send_pending",
    1 << 8: "audio_busy",
    1 << 9: "no_credits",
    1 << 10: "pcm_not_streaming",
    1 << 11: "audio_disabled",
    1 << 12: "process_not_audio",
    1 << 13: "connections_disabled",
    1 << 14: "tx_buffer_owned",
    1 << 15: "invariant_not_armed",
}

G722_INTEGRITY_STAGE_NAMES = {
    1: "ring_generation",
    2: "ring_checksum",
    3: "tx_buffer_changed",
}

WATCHDOG_REASON_NAMES = {
    1: "hci_dump_setting",
    2: "restart_command",
    3: "usb_setting",
    4: "reconnect_fallback",
}

SEQUENCE_EVENT_TYPES = {
    1, 2, 3, 4, 5, 6, 7, 12, 16, 17, 18, 19,
    26, 27, 28, 29, 30, 31, 40, 41, 42, 52, 53, 54, 55, 56, 57,
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

RUNTIME_SNAPSHOT_EXTENDED_FIELDS = RUNTIME_SNAPSHOT_FIELDS + (
    "core1_run_loop_count",
    "core1_run_loop_age_us",
    "process_audio_enter_count",
    "process_audio_enter_age_us",
    "hci_transport_progress_count",
    "hci_transport_progress_age_us",
    "hci_controller_progress_count",
    "hci_controller_progress_age_us",
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
    send_complete_us: int | None = None
    packet_sent_us: int | None = None
    l2cap_result: int | None = None
    ring_fill_at_request: int | None = None
    sequence_unwrapped: int | None = None


@dataclasses.dataclass(frozen=True)
class GeneratedFrame:
    timestamp_us: int
    write_index: int
    sequence: int
    sequence_unwrapped: int


@dataclasses.dataclass(frozen=True)
class CaptureResult:
    data: bytes
    interrupted: bool = False
    error: str | None = None
    marker_count: int = 0


def cobs_encode(payload: bytes) -> bytes:
    """Encode *payload* as one COBS frame with a trailing delimiter."""
    output = bytearray(b"\0")
    code_index = 0
    code = 1
    for value in payload:
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


def build_audio_glitch_marker_command() -> bytes:
    """Build the existing CDC command frame for a firmware trace marker."""
    command = COMMAND_PACKET.pack(COMMAND_AUDIO_GLITCH_MARKER, 0)
    header = PICO_HEADER.pack(
        PICO_TYPE_COMMAND, PICO_HEADER.size + len(command), 0, 0
    )
    return b"\0" + cobs_encode(header + command)


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
        if count != 1 or len(body) not in (
            RUNTIME_SNAPSHOT.size, RUNTIME_SNAPSHOT_EXTENDED.size
        ):
            return [], []
        snapshot_struct = (RUNTIME_SNAPSHOT_EXTENDED
                           if len(body) == RUNTIME_SNAPSHOT_EXTENDED.size
                           else RUNTIME_SNAPSHOT)
        fields = (RUNTIME_SNAPSHOT_EXTENDED_FIELDS
                  if snapshot_struct is RUNTIME_SNAPSHOT_EXTENDED
                  else RUNTIME_SNAPSHOT_FIELDS)
        values = snapshot_struct.unpack(body)
        snapshots.append(TraceSnapshot(
            values[0], dict(zip(fields, values[1:]))
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
    if record.event_type == 28:
        reasons.append("audio_send_delay_gt_50ms")
    if record.event_type == 29:
        reasons.append("audio_stale_frames_dropped")
    if record.event_type == 31:
        reasons.append("audio_sequence_skip_count_changed")
    if record.event_type == 33:
        reasons.append("hci_disconnect")
    if record.event_type == 35:
        reasons.append("l2cap_channel_closed")
    if record.event_type == 40:
        reasons.append("audio_no_progress")
    if record.event_type == 42:
        reasons.append("audio_no_progress_reconnect")
    if record.event_type == 47:
        reasons.append("reconnect_timeout")
    if record.event_type == 49 and record.duration_us > ANOMALY_US:
        reasons.append("audio_credits_zero")
    if record.event_type == 52:
        reasons.append("tx_blocked")
    if record.event_type == 53:
        reasons.append("can_send_request_aged")
    if record.event_type == 54:
        reasons.append("packet_sent_wait_aged")
    if record.event_type == 55:
        reasons.append("pcm_boundary_discontinuity")
    if record.event_type == 56:
        reasons.append("g722_integrity_error")
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
    if record.event_type == 2:
        generated_age = "unknown" if record.duration_us == 0xFFFFFFFF else record.duration_us
        previous_send_age = "unknown" if record.detail0 == 0xFFFFFFFF else record.detail0
        return (f"sdu_generated_to_request_us={generated_age};"
                f"previous_successful_send_to_request_us={previous_send_age};"
                f"selected_ring_index={record.detail1}")
    if record.event_type == 3:
        return f"request_to_can_send_now_us={record.duration_us}"
    if record.event_type == 4:
        return (f"sdu_size={record.detail0};"
                f"l2cap_call_duration_us={record.duration_us}")
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
    if record.event_type == 26:
        return (f"can_send_now_to_l2cap_send_us={record.duration_us};"
                f"local_recovery={bool(record.detail0 & 1)}")
    if record.event_type == 27:
        old_state = TX_STATE_NAMES.get(record.detail0, str(record.detail0))
        new_state = TX_STATE_NAMES.get(record.detail1, str(record.detail1))
        reason = TX_STATE_REASON_NAMES.get(record.result, str(record.result))
        return (f"old_state={old_state};new_state={new_state};reason={reason};"
                f"old_state_duration_us={record.duration_us}")
    if record.event_type == 28:
        stage = DELAY_NAMES.get(record.result, f"unknown_{record.result}")
        return (f"delay={stage};delay_us={record.duration_us};"
                f"threshold_us={record.detail0}")
    if record.event_type == 29:
        return (f"old_sequence={record.sequence};new_sequence={record.result & 0xff};"
                f"dropped_frames={record.detail0};"
                f"sequence_skip_count={record.detail1};"
                f"audio_busy_duration_us={record.duration_us}")
    if record.event_type == 30:
        flags = record.detail1 >> 24
        last_request_age = "unknown" if record.duration_us == 0xFFFFFFFF else record.duration_us
        last_send_age = "unknown" if record.detail0 == 0xFFFFFFFF else record.detail0
        return (f"last_send_request_age_us={last_request_age};"
                f"last_successful_l2cap_send_age_us={last_send_age};"
                f"available_audio_credits={record.detail1 & 0xffff};"
                f"connected_asha_devices={(record.detail1 >> 16) & 0xff};"
                f"can_send_now_request_pending={bool(flags & 1)};"
                f"pcm_streaming={bool(flags & 2)}")
    if record.event_type == 31:
        return (f"old_sequence={record.result & 0xff};new_sequence={record.sequence};"
                f"skipped_frames={record.detail0};"
                f"sequence_skip_count={record.detail1}")
    if record.event_type in (32, 33, 34, 35, 36, 37):
        address_bytes = (
            record.detail0 & 0xff,
            (record.detail0 >> 8) & 0xff,
            (record.detail0 >> 16) & 0xff,
            (record.detail0 >> 24) & 0xff,
            record.detail1 & 0xff,
            (record.detail1 >> 8) & 0xff,
        )
        address = ":".join(f"{byte:02X}" for byte in address_bytes)
        hci_status = (record.detail1 >> 16) & 0xff
        hci_reason = (record.detail1 >> 24) & 0xff
        l2cap_status = record.result & 0xff
        unavailable = 0xff
        return (f"address={address};"
                f"hci_status={'unavailable' if hci_status == unavailable else f'0x{hci_status:02x}'};"
                f"hci_reason={'unavailable' if hci_reason == unavailable else f'0x{hci_reason:02x}'};"
                f"l2cap_status={'unavailable' if l2cap_status == unavailable else f'0x{l2cap_status:02x}'}")
    if record.event_type == 38:
        major = (record.detail1 >> 24) & 0xff
        minor = (record.detail1 >> 16) & 0xff
        patch = record.detail1 & 0xffff
        return (f"watchdog_reboot={bool(record.sequence & 1)};"
                f"watchdog_enable_reboot={bool(record.sequence & 2)};"
                f"reset_reason=0x{record.result & 0xffffffff:08x};"
                f"boot_session_id={record.detail0};"
                f"firmware_version={major}.{minor}.{patch};"
                f"uptime_us={record.timestamp_us}")
    if record.event_type == 39:
        reason = WATCHDOG_REASON_NAMES.get(record.detail0, str(record.detail0))
        return (f"reason={reason};delay_us={record.duration_us};"
                f"boot_session_id={record.detail1}")
    if record.event_type in (40, 41, 42):
        return (f"last_successful_send_age_us={record.duration_us};"
                f"newest_sdu_age_us={record.detail0};"
                f"tx_state={TX_STATE_NAMES.get((record.detail1 >> 16) & 0xff, record.detail1 >> 16)};"
                f"available_audio_credits={record.detail1 & 0xffff};"
                f"status=0x{record.result & 0xffffffff:08x}")
    if record.event_type in (43, 44, 45, 46, 47):
        hci_status = (record.detail1 >> 16) & 0xff
        hci_reason = (record.detail1 >> 24) & 0xff
        return (f"attempt={record.duration_us};"
                f"hci_status=0x{hci_status:02x};"
                f"hci_reason=0x{hci_reason:02x};"
                f"status=0x{record.result & 0xffffffff:08x}")
    if record.event_type in (48, 49):
        rssi_valid = bool(record.result & 0x100)
        rssi = record.result & 0xff
        if rssi >= 128:
            rssi -= 256
        rssi_age = ("unknown" if record.detail1 == 0xffffffff
                    else record.detail1)
        return (f"zero_credit_duration_us={record.duration_us};"
                f"available_audio_credits={record.detail0 & 0xffff};"
                f"rssi_dbm={rssi if rssi_valid else 'unknown'};"
                f"rssi_age_us={rssi_age}")
    if record.event_type == 50:
        transport_age = ("unknown" if record.duration_us == 0xffffffff
                         else record.duration_us)
        controller_age = ("unknown" if record.detail1 == 0xffffffff
                          else record.detail1)
        return (f"core1_run_loop_count={record.detail0};"
                f"process_audio_enter_count={record.result & 0xffffffff};"
                f"hci_transport_progress_age_us={transport_age};"
                f"hci_controller_progress_age_us={controller_age}")
    if record.event_type == 51:
        run_loop_age = ("unknown" if record.duration_us == 0xffffffff
                        else record.duration_us)
        transport_age = ("unknown" if record.detail1 == 0xffffffff
                         else record.detail1)
        controller_value = record.result & 0xffffffff
        controller_age = ("unknown" if controller_value == 0xffffffff
                          else controller_value)
        return (f"process_audio_enter_count={record.detail0};"
                f"core1_run_loop_age_us={run_loop_age};"
                f"hci_transport_progress_age_us={transport_age};"
                f"hci_controller_progress_age_us={controller_age}")
    if record.event_type == 52:
        blocker_names = [name for bit, name in TX_BLOCKER_NAMES.items()
                         if record.detail0 & bit]
        tx_state = (record.result >> 16) & 0xff
        return (f"last_successful_send_age_us={record.duration_us};"
                f"blocker_mask=0x{record.detail0:08x};"
                f"blockers={'|'.join(blocker_names) or 'none'};"
                f"newest_sdu_age_us={record.detail1};"
                f"tx_state={TX_STATE_NAMES.get(tx_state, tx_state)};"
                f"available_audio_credits={record.result & 0xffff};"
                f"can_send_now_request_pending={bool(record.result & (1 << 24))}")
    if record.event_type in (53, 54):
        transport_age = ("unknown" if record.detail0 == 0xffffffff
                         else record.detail0)
        controller_age = ("unknown" if record.detail1 == 0xffffffff
                          else record.detail1)
        tx_state = (record.result >> 16) & 0xff
        return (f"wait_age_us={record.duration_us};"
                f"tx_state={TX_STATE_NAMES.get(tx_state, tx_state)};"
                f"available_audio_credits={record.result & 0xffff};"
                f"hci_transport_progress_age_us={transport_age};"
                f"hci_controller_progress_age_us={controller_age}")
    if record.event_type == 55:
        channel_flags = (record.result >> 16) & 0xff
        channels = []
        if channel_flags & 1:
            channels.append("left")
        if channel_flags & 2:
            channels.append("right")
        return (f"pcm_gap_us={record.duration_us};"
                f"left_boundary_jump={record.detail0};"
                f"right_boundary_jump={record.detail1};"
                f"sample_count={record.result & 0xffff};"
                f"channels={'|'.join(channels) or 'none'}")
    if record.event_type == 56:
        stage = G722_INTEGRITY_STAGE_NAMES.get(
            record.duration_us, f"unknown_{record.duration_us}"
        )
        return (f"stage={stage};"
                f"expected=0x{record.detail0:08x};"
                f"actual=0x{record.detail1:08x};"
                f"observed_generation={record.result & 0xffffffff};"
                f"selected_ring_index={record.read_index}")
    if record.event_type == 57:
        pcm_age = ("unknown" if record.duration_us == 0xffffffff
                   else record.duration_us)
        return (f"latest_pcm_age_us={pcm_age};"
                f"latest_pcm_checksum=0x{record.detail0:08x};"
                f"latest_sdu_left_checksum=0x{record.detail1:08x};"
                f"latest_sdu_right_checksum=0x{record.result & 0xffffffff:08x};"
                f"latest_sdu_write_index={record.write_index}")
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
            # 0xff is both the legacy no-sequence sentinel and a valid 8-bit
            # audio sequence. Event context disambiguates it.
            "sequence": (record.sequence
                         if record.event_type in SEQUENCE_EVENT_TYPES
                         else "" if record.sequence == INVALID_SEQUENCE
                         else record.sequence),
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
    generated: dict[int, list[GeneratedFrame]] = defaultdict(list)
    current: dict[tuple[int, int], TimelineRow] = {}
    timeline: list[TimelineRow] = []
    previous_generated_sequence: int | None = None
    previous_generated_unwrapped: int | None = None

    def find_generated(sequence: int, timestamp_us: int,
                       expected_write_index: int | None = None) -> GeneratedFrame | None:
        candidates = generated.get(sequence, [])
        if expected_write_index is not None:
            exact = next(
                (frame for frame in reversed(candidates)
                 if frame.timestamp_us <= timestamp_us and
                 frame.write_index == expected_write_index),
                None,
            )
            if exact is not None:
                return exact
        return next(
            (frame for frame in reversed(candidates)
             if frame.timestamp_us <= timestamp_us),
            None,
        )

    def rebind_row(row: TimelineRow, sequence: int, timestamp_us: int,
                   expected_write_index: int | None) -> None:
        frame = find_generated(sequence, timestamp_us, expected_write_index)
        row.sequence = sequence
        if frame is not None:
            row.generated_us = frame.timestamp_us
            row.sequence_unwrapped = frame.sequence_unwrapped

    for record in sorted(records, key=lambda item: item.timestamp_us):
        if record.event_type == 1:
            if previous_generated_sequence is None:
                sequence_unwrapped = record.sequence
            else:
                forward = (record.sequence - previous_generated_sequence) & 0xFF
                if forward == 0:
                    sequence_unwrapped = previous_generated_unwrapped
                elif forward < 128:
                    sequence_unwrapped = previous_generated_unwrapped + forward
                else:
                    # Preserve capture order for a rare out-of-order record
                    # without treating the raw byte as globally unique.
                    sequence_unwrapped = previous_generated_unwrapped + 1
            frame = GeneratedFrame(
                timestamp_us=record.timestamp_us,
                write_index=record.write_index,
                sequence=record.sequence,
                sequence_unwrapped=sequence_unwrapped,
            )
            generated[record.sequence].append(frame)
            previous_generated_sequence = record.sequence
            previous_generated_unwrapped = sequence_unwrapped
            continue
        if record.connection_handle == INVALID_HANDLE:
            continue
        key = (record.connection_handle, record.l2cap_cid)
        if record.event_type == 2:
            # At request time curr_read_index has already advanced past the
            # selected frame, so it equals that frame's generation write index.
            frame = find_generated(record.sequence, record.timestamp_us,
                                   record.read_index)
            row = TimelineRow(
                connection_handle=record.connection_handle,
                l2cap_cid=record.l2cap_cid,
                sequence=record.sequence,
                generated_us=None if frame is None else frame.timestamp_us,
                requested_us=record.timestamp_us,
                ring_fill_at_request=record.ring_fill,
                sequence_unwrapped=(None if frame is None
                                    else frame.sequence_unwrapped),
            )
            timeline.append(row)
            current[key] = row
            continue
        row = current.get(key)
        if row is None:
            continue
        if record.event_type in (19, 29) and row.sequence == record.sequence:
            new_sequence = ((record.detail1 if record.event_type == 19
                             else record.result) & 0xFF)
            rebind_row(row, new_sequence, record.timestamp_us,
                       record.write_index)
            continue
        if row.sequence != record.sequence:
            # If a stale-drop context record was lost, the first send record
            # still identifies the replacement frame. Rebind by generation
            # order/write index instead of dropping sequence 255 or attaching
            # a wrapped byte to an older packet.
            if record.event_type in (4, 26) and row.send_us is None:
                rebind_row(row, record.sequence, record.timestamp_us,
                           record.write_index)
            else:
                continue
        if record.event_type == 3:
            row.can_send_now_us = record.timestamp_us
        elif record.event_type == 26:
            row.send_us = record.timestamp_us
        elif record.event_type == 4:
            if row.send_us is None:
                row.send_us = record.timestamp_us
            row.send_complete_us = record.timestamp_us
            row.l2cap_result = record.result
        elif record.event_type == 5:
            row.packet_sent_us = record.timestamp_us

    # Truncated captures can omit the matching SDU_GENERATED record. Keep their
    # extended sequence monotonic per connection using request order as fallback.
    unwrap_state: dict[tuple[int, int], tuple[int, int]] = {}
    for row in timeline:
        key = (row.connection_handle, row.l2cap_cid)
        if row.sequence_unwrapped is None:
            previous_raw, previous_extended = unwrap_state.get(
                key, (row.sequence, row.sequence)
            )
            forward = (row.sequence - previous_raw) & 0xFF
            row.sequence_unwrapped = (previous_extended + forward
                                      if forward < 128 else
                                      previous_extended + 1)
        unwrap_state[key] = (row.sequence, row.sequence_unwrapped)
    return timeline


def elapsed(end: int | None, start: int | None) -> int | None:
    return None if end is None or start is None else end - start


def timeline_as_rows(timeline: Iterable[TimelineRow]) -> Iterator[dict[str, object]]:
    for row in timeline:
        generated_to_request = elapsed(row.requested_us, row.generated_us)
        generated_to_can = elapsed(row.can_send_now_us, row.generated_us)
        request_to_can = elapsed(row.can_send_now_us, row.requested_us)
        can_to_send = elapsed(row.send_us, row.can_send_now_us)
        l2cap_call = elapsed(row.send_complete_us, row.send_us)
        send_to_packet = elapsed(row.packet_sent_us,
                                 row.send_complete_us or row.send_us)
        total = elapsed(row.packet_sent_us, row.generated_us)
        reasons = []
        for label, value in (
            ("generated_to_send_requested_gt_25ms", generated_to_request),
            ("generated_to_can_send_now_gt_25ms", generated_to_can),
            ("request_to_can_send_now_gt_25ms", request_to_can),
            ("can_send_now_to_l2cap_send_gt_25ms", can_to_send),
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
            "l2cap_send_begin_us": row.send_us,
            "l2cap_send_complete_us": row.send_complete_us,
            "packet_sent_us": row.packet_sent_us,
            "generated_to_send_requested_us": generated_to_request,
            "generated_to_can_send_now_us": generated_to_can,
            "request_to_can_send_now_us": request_to_can,
            "can_send_now_to_send_us": can_to_send,
            "l2cap_call_duration_us": l2cap_call,
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
    disconnects = [record for record in records if record.event_type == 33]
    if not disconnects:
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


def console_glitch_marker_pressed() -> bool:
    """Poll the console without delaying serial capture."""
    if sys.platform == "win32":
        import msvcrt

        marked = False
        while msvcrt.kbhit():
            key = msvcrt.getwch()
            if key == "\x03":
                raise KeyboardInterrupt
            if key.lower() == "m":
                marked = True
        return marked

    import select

    if not sys.stdin.isatty():
        return False
    ready, _, _ = select.select([sys.stdin], [], [], 0)
    return bool(ready and sys.stdin.read(1).lower() == "m")


def capture_serial(
    port: str,
    seconds: float,
    baudrate: int,
    raw_output: Path,
    *,
    serial_factory=None,
    serial_errors: tuple[type[BaseException], ...] | None = None,
    flush_interval: float = 1.0,
    mark_glitches: bool = False,
    marker_poll=None,
    marker_notify=None,
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
    marker_count = 0
    if mark_glitches and marker_poll is None:
        marker_poll = console_glitch_marker_pressed

    # Open the raw file first, so even failure while opening the COM port leaves
    # a well-defined capture artifact. Each received chunk is persisted before
    # the next potentially interruptible serial read.
    with raw_output.open("wb") as output:
        try:
            with serial_factory(port, baudrate=baudrate, timeout=0.1) as connection:
                while time.monotonic() < deadline:
                    if mark_glitches and marker_poll is not None and marker_poll():
                        connection.write(build_audio_glitch_marker_command())
                        marker_count += 1
                        if marker_notify is not None:
                            marker_notify(marker_count)
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
        marker_count=marker_count,
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
    parser.add_argument(
        "--mark-glitches", action="store_true",
        help="press M during live capture to insert USER_AUDIO_GLITCH_MARKER",
    )
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = build_argument_parser().parse_args(argv)
    if args.capture_port:
        if args.mark_glitches:
            print("glitch marking enabled: press M whenever bad audio is heard")
        try:
            capture = capture_serial(
                args.capture_port,
                args.capture_seconds,
                args.baudrate,
                args.raw_output,
                mark_glitches=args.mark_glitches,
                marker_notify=lambda count: print(
                    f"glitch marker {count} requested"
                ),
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
        if capture.marker_count:
            print(f"requested {capture.marker_count} firmware glitch marker(s)")
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
