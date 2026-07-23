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
        self.assertIn("schedule_reconnect(ha, status, reason)", disconnected)
        self.assertNotIn("start_scan();", disconnected)
        reconnect_loop = function_body(source, "void HearingAid::process_reconnect")
        self.assertIn("start_scan();", reconnect_loop)
        self.assertIn("process_audio_tx_watchdog(w_index, audio_active)", audio_loop)
        self.assertIn("schedule_audio_if_idle(w_index, now_us, false)", audio_loop)
        scheduler = function_body(source, "bool HearingAid::schedule_audio_if_idle")
        self.assertIn("audio_tx_buffer.data()", scheduler)

    def test_audio_no_progress_policy_and_two_tier_recovery(self):
        source = (ROOT / "src" / "hearing_aid.cpp").read_text(encoding="utf-8")
        header = (ROOT / "src" / "hearing_aid.hpp").read_text(encoding="utf-8")
        no_progress = function_body(
            source, "bool HearingAid::process_audio_no_progress"
        )
        producer = (ROOT / "src" / "asha_audio.c").read_text(encoding="utf-8")
        schedule = function_body(source, "bool HearingAid::schedule_audio_if_idle")
        send = function_body(source, "bool HearingAid::send_pending_audio")

        self.assertIn("audio_no_progress_timeout_us = 100'000", header)
        self.assertIn("audio_no_progress_startup_grace_us = 150'000", header)
        self.assertIn("audio_sdu_activity_timeout_us = 50'000", header)
        for gate in (
            "desired_connection", "!manual_shutdown", "is_connected()",
            "is_streaming()", "audio_streaming_enabled", "pcm_is_streaming",
            "l2cap_channel_open", "current_sdu_generation",
            "audio_sdu_generated_count", "successful_audio_send_count",
        ):
            self.assertIn(gate, no_progress)
        self.assertIn("AUDIO_STALL_TRACE_AUDIO_NO_PROGRESS", no_progress)
        self.assertIn("AUDIO_STALL_TRACE_AUDIO_NO_PROGRESS_REARMED", no_progress)
        self.assertIn("AUDIO_STALL_TRACE_AUDIO_NO_PROGRESS_RECONNECT", no_progress)
        self.assertIn("audio_no_progress_rearm_deadline_us", no_progress)
        self.assertIn("reconnect_after_audio_tx_stall", no_progress)

        # Detection must not depend on consumer bookkeeping or transient TX
        # ownership. Those values remain diagnostics/recovery constraints only.
        detection = no_progress[:no_progress.index(
            "AUDIO_STALL_TRACE_AUDIO_NO_PROGRESS,"
        )]
        self.assertNotIn("ring_not_empty", detection)
        self.assertNotIn("write_index != curr_read_index", detection)
        self.assertNotIn("credits > 0U", detection)
        self.assertNotIn("audio_tx_state != AudioTxState::Idle", detection)
        self.assertNotIn("AudioBusy) == 0U", detection)
        self.assertIn("now_us < audio_progress_grace_deadline_us", detection)
        self.assertIn(
            "audio_sdu_generated_count == audio_sdu_count_at_last_success",
            detection,
        )

        # Backlog is collapsed to the newest complete SDU before read advances.
        self.assertIn("selected_index = write_index - 1U", schedule)
        self.assertIn("audio_stall_trace_stale_frames_dropped", schedule)
        self.assertLess(schedule.index("audio_stall_trace_stale_frames_dropped"),
                        schedule.index("curr_read_index = selected_index + 1U"))

        # Any successful L2CAP call cancels second-tier escalation.
        self.assertIn(
            "audio_no_progress_state = AudioNoProgressState::Idle", send
        )
        self.assertIn(
            "successful_audio_send_count != audio_no_progress_success_count",
            no_progress,
        )
        self.assertIn("++successful_audio_send_count", send)
        self.assertIn("last_sdu_generated_time_us", producer)
        self.assertIn("time_us_32()", producer)
        self.assertIn(
            "asha_audio_get_last_sdu_generated_time_us()", no_progress
        )

        # Recovery may replace the private buffer only while IDLE. A pending
        # phase falls through to the unchanged phase-specific watchdog.
        self.assertIn("if (audio_tx_state == AudioTxState::Idle)", no_progress)
        self.assertIn("return requested;", no_progress)

    def test_snapshot_ring_fill_is_live_not_last_event_fill(self):
        source = (ROOT / "src" / "audio_stall_trace.c").read_text(
            encoding="utf-8"
        )
        snapshot = function_body(source, "void audio_stall_trace_snapshot")
        emit = function_body(source, "static void emit_record_at")

        self.assertIn("asha_audio_get_write_index()", snapshot)
        self.assertIn("slowest_consumer_read_index(live_write_index)", snapshot)
        self.assertIn(".ring_fill_current = live_fill", snapshot)
        self.assertNotIn(
            "atomic_store_explicit(&counters.ring_fill_current", emit
        )

    def test_zero_credits_waits_and_positive_credits_use_normal_scheduler(self):
        source = (ROOT / "src" / "hearing_aid.cpp").read_text(encoding="utf-8")
        header = (ROOT / "src" / "hearing_aid.hpp").read_text(encoding="utf-8")
        producer = (ROOT / "src" / "asha_audio.c").read_text(encoding="utf-8")
        trace_source = (ROOT / "src" / "audio_stall_trace.c").read_text(
            encoding="utf-8"
        )
        audio_loop = function_body(source, "bool HearingAid::process_audio()")
        zero_credit = function_body(audio_loop, "if (ha->credits == 0)")
        scheduler = function_body(source, "bool HearingAid::schedule_audio_if_idle")

        self.assertNotIn("send_acp_stop", zero_credit)
        self.assertNotIn("send_acp_start", zero_credit)
        self.assertNotIn("audio_state", zero_credit)
        self.assertNotIn("reset", zero_credit)
        self.assertNotIn("asha_audio", zero_credit)
        self.assertIn("break;", zero_credit)

        zero_pos = audio_loop.index("if (ha->credits == 0)")
        normal_schedule_pos = audio_loop.index(
            "schedule_audio_if_idle(w_index, now_us, false)", zero_pos
        )
        self.assertLess(zero_pos, normal_schedule_pos)
        self.assertIn("if (ha->first_audio_send)", audio_loop)
        self.assertNotIn("awaiting_encoder_generation", source)
        self.assertNotIn("stream_generation_requested", producer)
        self.assertNotIn("asha_audio_request_stream_generation_reset", source)

        # Normal scheduling is level-triggered. A short ordered backlog remains
        # below the collapse threshold; recovery or more than 60 ms selects the
        # newest complete SDU before owning the immutable TX buffer.
        self.assertIn("audio_short_backlog_max_frames = 3", header)
        self.assertIn("audio_tx_state != AudioTxState::Idle", scheduler)
        self.assertIn("write_index == curr_read_index", scheduler)
        self.assertIn(
            "queued_frames > audio_short_backlog_max_frames", scheduler
        )
        self.assertNotIn("queued_frames > 1U", scheduler)
        self.assertIn("selected_index = write_index - 1U", scheduler)
        self.assertIn("curr_read_index = selected_index + 1U", scheduler)
        self.assertIn("return request_audio_can_send(write_index)", scheduler)
        self.assertEqual(1, source.count("l2cap_request_can_send_now_event(cid)"))
        self.assertIn("process_audio_no_progress", audio_loop)
        self.assertIn("process_audio_tx_watchdog", audio_loop)

        # Credit diagnostics are transition-only, retain RSSI context, and do
        # not alter the zero-credit scheduling branch.
        self.assertIn("!ha->trace_zero_credits_active", audio_loop)
        self.assertIn("ha->credits > 0U &&", audio_loop)
        self.assertIn("AUDIO_STALL_TRACE_CREDITS_ZERO_ENTER", audio_loop)
        self.assertIn("AUDIO_STALL_TRACE_CREDITS_ZERO_EXIT", audio_loop)
        credit_trace = function_body(
            trace_source, "void audio_stall_trace_credits_zero"
        )
        self.assertIn("have_rssi", credit_trace)
        self.assertIn("available_credits", credit_trace)

    def test_previous_pcm_gap_and_explicit_stop_behavior_is_restored(self):
        usb = (ROOT / "src" / "usb_audio.cpp").read_text(encoding="utf-8")
        source = (ROOT / "src" / "hearing_aid.cpp").read_text(encoding="utf-8")
        audio_section = usb[usb.index("// AUDIO Task"):]
        audio_task = function_body(audio_section, "void audio_task(void)")
        set_interface = function_body(usb, "tud_audio_set_itf_cb")
        close_interface = function_body(usb, "tud_audio_set_itf_close_ep_cb")
        audio_loop = function_body(source, "bool HearingAid::process_audio()")
        stopped = function_body(
            audio_loop,
            "if (!audio_streaming_enabled || !pcm_is_streaming)",
        )

        self.assertIn("silence_timeout = 10'000", usb)
        self.assertIn("packet_gap_us > 5000", audio_task)
        self.assertIn("asha_audio_set_pcm_streaming_enabled(false)", audio_task)
        self.assertNotIn("usb_pcm_gap_grace_us", usb)
        self.assertNotIn("silence_pcm", usb)
        self.assertIn("(void) p_request", set_interface)
        self.assertIn("(void) p_request", close_interface)
        self.assertIn("asha_audio_set_encoding_enabled(false)", stopped)
        self.assertIn("send_acp_stop()", stopped)

    def test_idle_scheduler_is_idempotent_and_old_callbacks_are_ignored(self):
        source = (ROOT / "src" / "hearing_aid.cpp").read_text(encoding="utf-8")
        request = function_body(source, "bool HearingAid::request_audio_can_send")
        schedule = function_body(source, "bool HearingAid::schedule_audio_if_idle")
        callback = function_body(source, "void HearingAid::handle_l2cap_cbm")

        self.assertIn("audio_tx_state != AudioTxState::Idle", request)
        self.assertIn("audio_tx_state != AudioTxState::Idle", schedule)
        self.assertEqual(1, source.count("l2cap_request_can_send_now_event(cid)"))
        self.assertIn("audio_tx_pending_generation", request)
        self.assertIn("audio_tx_pending_generation != ha->audio_tx_generation", callback)
        self.assertIn("audio_tx_stale_can_send_callbacks", callback)

    def test_unexpected_disconnect_reconnects_but_manual_stop_does_not(self):
        source = (ROOT / "src" / "hearing_aid.cpp").read_text(encoding="utf-8")
        allowed = function_body(source, "void HearingAid::set_connections_allowed")
        disconnected = function_body(source, "void HearingAid::on_disconnected")
        reconnect = function_body(source, "void HearingAid::process_reconnect")
        connected = function_body(source, "void HearingAid::on_connected")
        process = function_body(source, "void HearingAid::process()")
        header = (ROOT / "src" / "hearing_aid.hpp").read_text(encoding="utf-8")

        self.assertIn("desired_connection = allowed", allowed)
        self.assertIn("manual_shutdown = !allowed", allowed)
        self.assertIn("desired_connection && !manual_shutdown", disconnected)
        self.assertIn("schedule_reconnect(ha, status, reason)", disconnected)
        # Reason 0x08 and all other unexpected reasons take the same path.
        self.assertNotIn("reason ==", disconnected)
        self.assertIn("!desired_connection || manual_shutdown", reconnect)
        self.assertIn("reconnect_max_attempts = 3", header)
        self.assertIn("reconnect_attempt_count < reconnect_max_attempts", reconnect)
        self.assertIn("AUDIO_STALL_TRACE_RECONNECT_TIMEOUT", reconnect)
        self.assertIn("AUDIO_STALL_TRACE_WATCHDOG_RECONNECT_FALLBACK", reconnect)
        self.assertIn("watchdog_hw->scratch[5]", reconnect)
        self.assertIn("ReconnectState::Initializing", reconnect)
        self.assertNotIn("clear_reconnect_reboot_guard", connected)
        self.assertIn("clear_reconnect_reboot_guard();", process)

    def test_stale_pending_sdu_is_replaced_without_second_request(self):
        source = (ROOT / "src" / "hearing_aid.cpp").read_text(encoding="utf-8")
        header = (ROOT / "src" / "hearing_aid.hpp").read_text(encoding="utf-8")
        watchdog = function_body(source, "bool HearingAid::process_audio_tx_watchdog")

        self.assertIn("audio_tx_max_audio_age_us = 60'000", header)
        self.assertIn("audio_can_send_watchdog_us = 30'000", header)
        self.assertIn("latest_index = write_index - 1U", watchdog)
        self.assertIn("curr_read_index = write_index", watchdog)
        self.assertIn("audio_stall_trace_stale_frames_dropped", watchdog)
        self.assertNotIn("l2cap_request_can_send_now_event(cid)", watchdog)
        self.assertLess(
            watchdog.index("audio_stall_trace_stale_frames_dropped"),
            watchdog.index("memcpy(audio_tx_buffer.data()"),
        )
        self.assertLess(
            watchdog.index("audio_stall_trace_stale_frames_dropped"),
            watchdog.index("curr_read_index = write_index"),
        )
        for context in (
            "busy_duration_us", "last_request_age_us",
            "last_successful_send_age_us",
            "asha_audio_get_pcm_streaming_enabled()",
            "num_connected()", "available_credits",
        ):
            self.assertIn(context, watchdog)
        self.assertLess(
            watchdog.index("memcpy(audio_tx_buffer.data()"),
            watchdog.index("send_pending_audio(true, write_index)"),
        )

    def test_lifecycle_boot_delay_and_race_safe_snapshot_events(self):
        trace_header = (ROOT / "src" / "audio_stall_trace.h").read_text(
            encoding="utf-8"
        )
        trace_source = (ROOT / "src" / "audio_stall_trace.c").read_text(
            encoding="utf-8"
        )
        hearing = (ROOT / "src" / "hearing_aid.cpp").read_text(
            encoding="utf-8"
        )
        bt = (ROOT / "src" / "asha_bt.cpp").read_text(encoding="utf-8")

        for event in (
            "AUDIO_STALL_TRACE_HCI_CONNECTION_OPENED",
            "AUDIO_STALL_TRACE_HCI_DISCONNECTION_COMPLETE",
            "AUDIO_STALL_TRACE_L2CAP_CHANNEL_OPENED",
            "AUDIO_STALL_TRACE_L2CAP_CHANNEL_CLOSED",
            "AUDIO_STALL_TRACE_ASHA_DEVICE_CONNECTED",
            "AUDIO_STALL_TRACE_ASHA_DEVICE_DISCONNECTED",
            "AUDIO_STALL_TRACE_SYSTEM_BOOT",
            "AUDIO_STALL_TRACE_WATCHDOG_RESET_REQUESTED",
            "AUDIO_STALL_TRACE_SEND_DELAY_WARNING",
            "AUDIO_STALL_TRACE_SEND_STATE_CHANGED",
        ):
            self.assertIn(event, trace_header)

        self.assertIn("AUDIO_STALL_TRACE_HCI_CONNECTION_OPENED", bt)
        self.assertIn("AUDIO_STALL_TRACE_HCI_DISCONNECTION_COMPLETE", hearing)
        self.assertIn("AUDIO_STALL_TRACE_L2CAP_CHANNEL_OPENED", hearing)
        self.assertIn("AUDIO_STALL_TRACE_L2CAP_CHANNEL_CLOSED", hearing)
        self.assertEqual(3, bt.count("audio_stall_trace_watchdog_reset_requested"))

        snapshot = function_body(trace_source, "void audio_stall_trace_snapshot")
        self.assertIn("duration_us = now_us_low - since_us", snapshot)
        self.assertIn("if (duration_us > INT32_MAX) continue", snapshot)
        self.assertLess(
            snapshot.index("if (duration_us > INT32_MAX) continue"),
            snapshot.index("atomic_update_max"),
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

    def test_targeted_tx_stall_diagnostics_preserve_existing_watchdogs(self):
        source = (ROOT / "src" / "hearing_aid.cpp").read_text(
            encoding="utf-8"
        )
        header = (ROOT / "src" / "hearing_aid.hpp").read_text(
            encoding="utf-8"
        )
        trace_header = (ROOT / "src" / "audio_stall_trace.h").read_text(
            encoding="utf-8"
        )
        trace_source = (ROOT / "src" / "audio_stall_trace.c").read_text(
            encoding="utf-8"
        )
        bt_source = (ROOT / "src" / "asha_bt.cpp").read_text(
            encoding="utf-8"
        )
        audio_loop = function_body(source, "bool HearingAid::process_audio()")
        scheduler = function_body(source, "bool HearingAid::schedule_audio_if_idle")
        request = function_body(source, "bool HearingAid::request_audio_can_send")
        watchdog = function_body(source, "bool HearingAid::process_audio_tx_watchdog")
        blockers = function_body(source, "uint32_t HearingAid::trace_audio_tx_blockers")
        blocked_report = function_body(
            source, "void HearingAid::trace_audio_tx_blocked_if_needed"
        )
        timer = function_body(
            bt_source, "static void audio_timer_handler(btstack_timer_source_t* timer)"
        )
        hci_signature = "static void hci_event_handler(PACKET_HANDLER_PARAMS)"
        hci_handler = function_body(
            bt_source[bt_source.rindex(hci_signature):], hci_signature
        )

        self.assertIn("audio_stall_trace_audio_timer_tick", timer)
        self.assertIn("audio_stall_trace_process_audio_enter", audio_loop)
        self.assertIn("audio_stall_trace_hci_controller_progress", hci_handler)
        self.assertIn("AUDIO_STALL_TRACE_HEARTBEAT_INTERVAL_US", trace_source)
        self.assertIn("AUDIO_STALL_TRACE_CORE1_RUN_LOOP_HEARTBEAT", trace_source)
        self.assertIn("AUDIO_STALL_TRACE_PROCESS_AUDIO_ENTER", trace_source)
        self.assertIn("core1_run_loop_age_us", trace_header)
        self.assertIn("hci_controller_progress_age_us", trace_header)

        for blocker in (
            "BLOCK_NOT_CONNECTED", "BLOCK_NOT_STREAMING",
            "BLOCK_L2CAP_NOT_READY", "BLOCK_NO_SDU_AVAILABLE",
            "BLOCK_SDU_NOT_FRESH", "BLOCK_WAIT_CAN_SEND_NOW",
            "BLOCK_WAIT_PACKET_SENT", "BLOCK_CAN_SEND_PENDING",
            "BLOCK_AUDIO_BUSY", "BLOCK_NO_CREDITS",
            "BLOCK_PCM_NOT_STREAMING", "BLOCK_AUDIO_DISABLED",
            "BLOCK_PROCESS_NOT_AUDIO", "BLOCK_CONNECTIONS_DISABLED",
            "BLOCK_TX_BUFFER_OWNED",
        ):
            self.assertIn(blocker, blockers)
        self.assertIn("BLOCK_INVARIANT_NOT_ARMED", blocked_report)
        self.assertIn("AUDIO_STALL_TRACE_TX_BLOCKED_US", blocked_report)
        self.assertIn("AUDIO_STALL_TRACE_CAN_SEND_REQUEST_AGE", watchdog)
        self.assertIn("AUDIO_STALL_TRACE_PACKET_SENT_WAIT_AGE", watchdog)

        # The invariant remains level-triggered and idempotent: ownership is
        # established before the sole normal BTstack request call.
        self.assertIn("return request_audio_can_send(write_index)", scheduler)
        self.assertEqual(1, source.count("l2cap_request_can_send_now_event(cid)"))
        self.assertLess(request.index("AudioTxState::WaitingCanSendNow"),
                        request.index("l2cap_request_can_send_now_event(cid)"))

        # Diagnostics must not change the recovery deadlines.
        self.assertIn("audio_can_send_watchdog_us = 30'000", header)
        self.assertIn("audio_packet_sent_watchdog_us = 150'000", header)
        self.assertIn("audio_no_progress_timeout_us = 100'000", header)
        self.assertIn("reconnect_attempt_timeout_us = 12'000'000", header)

    def test_audio_content_diagnostics_are_exception_only_and_age_safe(self):
        audio = (ROOT / "src" / "asha_audio.c").read_text(encoding="utf-8")
        hearing = (ROOT / "src" / "hearing_aid.cpp").read_text(
            encoding="utf-8"
        )
        trace_source = (ROOT / "src" / "audio_stall_trace.c").read_text(
            encoding="utf-8"
        )
        trace_header = (ROOT / "src" / "audio_stall_trace.h").read_text(
            encoding="utf-8"
        )
        bt = (ROOT / "src" / "asha_bt.cpp").read_text(encoding="utf-8")

        age = function_body(trace_source, "static uint32_t timestamp_age_us")
        self.assertIn("age_us > INT32_MAX ? 0u : age_us", age)
        self.assertIn("timestamp_age_us((uint32_t)now_us, sampled_us)",
                      trace_source)

        encoder = function_body(audio, "void asha_audio_encode_1ms_pcm")
        scheduler = function_body(hearing, "bool HearingAid::schedule_audio_if_idle")
        sender = function_body(hearing, "bool HearingAid::send_pending_audio")
        self.assertIn("AUDIO_STALL_TRACE_PCM_DISCONTINUITY_THRESHOLD", encoder)
        self.assertIn("trace_published_write_index", audio)
        self.assertIn("asha_audio_trace_checksum", scheduler)
        self.assertIn("AUDIO_STALL_TRACE_G722_RING_GENERATION", scheduler)
        self.assertIn("AUDIO_STALL_TRACE_G722_RING_CHECKSUM", scheduler)
        self.assertIn("AUDIO_STALL_TRACE_G722_TX_BUFFER_CHANGED", sender)
        self.assertIn("AUDIO_STALL_TRACE_USER_AUDIO_GLITCH_MARKER",
                      trace_header)
        self.assertIn("Command::AudioGlitchMarker", bt)

        # Mismatches are observed but never gate, retry, discard, or reconnect.
        integrity = function_body(
            trace_source, "void audio_stall_trace_g722_integrity_error"
        )
        self.assertIn("emit_record", integrity)
        self.assertNotIn("reconnect", integrity)
        self.assertNotIn("watchdog", integrity)


if __name__ == "__main__":
    unittest.main()
