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
    def test_usb_main_regular_loop_polls_trace(self):
        source = (ROOT / "src" / "usb_audio.cpp").read_text(encoding="utf-8")
        body = function_body(source, "void usb_main(void)")
        loop = function_body(body, "while (1)")

        self.assertIn("tud_task();", loop)
        self.assertRegex(
            loop,
            re.compile(
                r"#ifdef\s+PICO_ASHA_AUDIO_STALL_TRACE\s+"
                r"comm::try_send_audio_trace\(\);\s+#endif",
                re.MULTILINE,
            ),
        )
        self.assertIn("comm::try_send_usb_packets();", loop)

    def test_trace_enabled_bt_paths_do_not_write_tinyusb(self):
        bt_source = (ROOT / "src" / "asha_bt.cpp").read_text(encoding="utf-8")
        timer = function_body(bt_source, "static void audio_timer_handler")
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


if __name__ == "__main__":
    unittest.main()
