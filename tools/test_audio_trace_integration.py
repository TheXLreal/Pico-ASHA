import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[brace + 1:index]
    raise AssertionError(f"unterminated function: {signature}")


class AudioTraceIntegrationTest(unittest.TestCase):
    def test_usb_main_regularly_queues_trace_before_draining(self):
        usb_source = (ROOT / "src" / "usb_audio.cpp").read_text(encoding="utf-8")
        usb_body = function_body(usb_source, "void usb_main(void)")
        loop = function_body(usb_body, "while (1)")
        self.assertRegex(
            loop,
            re.compile(
                r"#ifdef\s+PICO_ASHA_AUDIO_STALL_TRACE[\s\S]*?"
                r"comm::try_send_audio_trace\(\);\s+#endif",
                re.MULTILINE,
            ),
        )
        self.assertIn("tud_task();", loop)
        self.assertIn("comm::try_send_usb_packets();", loop)
        self.assertLess(
            loop.index("tud_task();"), loop.index("comm::try_send_audio_trace();")
        )
        self.assertLess(
            loop.index("comm::try_send_audio_trace();"),
            loop.index("comm::try_send_usb_packets();"),
        )

        bt_source = (ROOT / "src" / "asha_bt.cpp").read_text(encoding="utf-8")
        timer = function_body(
            bt_source, "static void audio_timer_handler(btstack_timer_source_t* timer)"
        )
        self.assertNotIn("try_send_audio_trace", timer)

        cpp_sources = "\n".join(
            path.read_text(encoding="utf-8") for path in (ROOT / "src").glob("*.cpp")
        )
        self.assertEqual(1, cpp_sources.count("comm::try_send_audio_trace();"))

    def test_usb_tx_queue_is_bounded_nonblocking_mpsc(self):
        source = (ROOT / "src" / "asha_comms.cpp").read_text(encoding="utf-8")
        enqueue = function_body(source, "static bool enqueue_usb_packet")
        usb_send = function_body(source, "void try_send_usb_packets")

        self.assertIn("usb_tx_enqueue_attempts", enqueue)
        self.assertIn("compare_exchange_weak", enqueue)
        self.assertIn("frame.ready.store(true", enqueue)
        self.assertIn("frame.ready.load", usb_send)
        self.assertIn("frame.ready.store(false", usb_send)
        self.assertNotIn("mutex", enqueue.lower())
        self.assertNotIn("spin_lock", enqueue)

    def test_trace_payload_uses_single_usb_tx_path(self):
        source = (ROOT / "src" / "asha_comms.cpp").read_text(encoding="utf-8")
        trace_send = function_body(source, "static bool send_audio_trace_payload")
        usb_send = function_body(source, "void try_send_usb_packets")

        self.assertIn("enqueue_usb_packet", trace_send)
        self.assertNotIn("tud_cdc_write", trace_send)
        self.assertIn("tud_cdc_write", usb_send)

    def test_snapshot_period_is_250_ms_without_nucleus_dependency(self):
        header = (ROOT / "src" / "audio_stall_trace.h").read_text(encoding="utf-8")
        source = (ROOT / "src" / "asha_comms.cpp").read_text(encoding="utf-8")
        poll = function_body(source, "void try_send_audio_trace")

        self.assertRegex(
            header,
            re.compile(
                r"#define\s+AUDIO_STALL_TRACE_SNAPSHOT_INTERVAL_US\s+250000u"
            ),
        )
        self.assertIn("AUDIO_STALL_TRACE_SNAPSHOT_INTERVAL_US", poll)
        self.assertNotIn("HearingAid", poll)
        self.assertNotIn("nucleus", poll.lower())

    def test_trace_enabled_bt_paths_do_not_write_tinyusb(self):
        bt_source = (ROOT / "src" / "asha_bt.cpp").read_text(encoding="utf-8")
        timer = function_body(
            bt_source, "static void audio_timer_handler(btstack_timer_source_t* timer)"
        )
        self.assertNotIn("try_send_audio_trace", timer)
        self.assertNotIn("tud_cdc_write", timer)

        comms_source = (ROOT / "src" / "asha_comms.cpp").read_text(encoding="utf-8")
        hci_callback = function_body(comms_source, "void send_hci_packet")
        self.assertRegex(
            hci_callback,
            re.compile(
                r"#ifdef\s+PICO_ASHA_AUDIO_STALL_TRACE[\s\S]*?"
                r"enqueue_usb_packet\([\s\S]*?#else[\s\S]*?tud_cdc_write",
                re.MULTILINE,
            ),
        )

    def test_audio_tx_watchdog_recovers_without_duplicate_request(self):
        source = (ROOT / "src" / "hearing_aid.cpp").read_text(encoding="utf-8")
        request = function_body(source, "bool HearingAid::request_audio_can_send")
        watchdog = function_body(source, "bool HearingAid::process_audio_tx_watchdog")
        send = function_body(source, "bool HearingAid::send_pending_audio")
        callback = function_body(source, "void HearingAid::handle_l2cap_cbm")

        # State is armed before a request that BTstack may satisfy synchronously.
        self.assertLess(
            request.index("AudioTxState::WaitingCanSendNow"),
            request.index("l2cap_request_can_send_now_event(cid)"),
        )
        self.assertEqual(1, source.count("l2cap_request_can_send_now_event(cid)"))

        # A timed-out request is recovered by a direct send only when the
        # existing channel can accept it; the watchdog never posts another
        # request and reconnects on either local failure path.
        self.assertIn("l2cap_can_send_packet_now(cid)", watchdog)
        self.assertIn("send_pending_audio(true, write_index)", watchdog)
        self.assertGreaterEqual(
            watchdog.count("reconnect_after_audio_tx_stall"), 3
        )
        self.assertLess(
            send.index("AudioTxState::WaitingPacketSent"),
            send.index("l2cap_send(cid, audio_data, ASHA_SDU_SIZE_BYTES)"),
        )

        # Late callbacks cannot complete a different phase or clear AudioBusy.
        self.assertIn(
            "audio_tx_state != AudioTxState::WaitingCanSendNow", callback
        )
        self.assertIn(
            "audio_tx_state != AudioTxState::WaitingPacketSent", callback
        )

    def test_tx_stall_reconnect_resets_and_restarts_scan(self):
        source = (ROOT / "src" / "hearing_aid.cpp").read_text(encoding="utf-8")
        reconnect = function_body(source, "void HearingAid::reconnect_after_audio_tx_stall")
        disconnected = function_body(source, "void HearingAid::on_disconnected")
        audio_loop = function_body(source, "bool HearingAid::process_audio()")

        self.assertIn("disconnect();", reconnect)
        self.assertIn("ha->reset();", disconnected)
        self.assertIn("start_scan();", disconnected)
        self.assertIn("process_audio_tx_watchdog(w_index, audio_active)", audio_loop)
        self.assertIn("audio_tx_buffer.data()", audio_loop)

    def test_stale_pending_sdu_is_replaced_without_second_request(self):
        source = (ROOT / "src" / "hearing_aid.cpp").read_text(encoding="utf-8")
        header = (ROOT / "src" / "hearing_aid.hpp").read_text(encoding="utf-8")
        watchdog = function_body(source, "bool HearingAid::process_audio_tx_watchdog")

        self.assertIn("audio_tx_max_audio_age_us = 60'000", header)
        self.assertIn("audio_can_send_watchdog_us = 30'000", header)
        self.assertIn("latest_index = write_index - 1U", watchdog)
        self.assertIn("curr_read_index = write_index", watchdog)
        self.assertIn("audio_stall_trace_tx_stale_drop", watchdog)
        self.assertNotIn("l2cap_request_can_send_now_event(cid)", watchdog)
        self.assertLess(
            watchdog.index("memcpy(audio_tx_buffer.data()"),
            watchdog.index("send_pending_audio(true, write_index)"),
        )

    def test_low_overhead_platform_instrumentation_and_timer_threshold(self):
        hooks = (ROOT / "src" / "cyw43_trace_hooks.c").read_text(encoding="utf-8")
        trace = (ROOT / "src" / "audio_stall_trace.c").read_text(encoding="utf-8")
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        hearing = (ROOT / "src" / "hearing_aid.cpp").read_text(encoding="utf-8")

        self.assertIn("__wrap_cyw43_bluetooth_hci_write", hooks)
        self.assertIn("__wrap_cyw43_thread_enter", hooks)
        self.assertIn("--wrap=cyw43_bluetooth_hci_write", cmake)
        self.assertIn("gap_us > AUDIO_STALL_TRACE_AUDIO_TIMER_LATE_US", trace)
        self.assertNotIn("gap_us > 2000u", trace)
        self.assertIn("if (rssi_request_pending)", hearing)
        self.assertIn("audio_stall_trace_rssi_request_skipped", hearing)


if __name__ == "__main__":
    unittest.main()
