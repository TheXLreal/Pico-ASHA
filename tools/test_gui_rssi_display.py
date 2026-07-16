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


class GuiRssiDisplayTest(unittest.TestCase):
    def test_serial_decoder_keeps_required_cobs_delimiter(self):
        comm = (ROOT / "gui" / "picoashacomm.cpp").read_text(encoding="utf-8")
        reader = function_body(comm, "void PicoAshaComm::onSerialReadyRead")

        append_delimiter = reader.index("m_currPacket.append(COBS_FRAME_DELIMITER)")
        decode = reader.index("cobs_decode(")
        dispatch = reader.index("handleDecodedData(decoded)")
        self.assertLess(append_delimiter, decode)
        self.assertLess(decode, dispatch)
        self.assertIn("result == COBS_RET_SUCCESS", reader)
        self.assertIn("size_t dec_len = 0", reader)

    def test_trace_wire_structs_are_portable_to_msvc(self):
        header = (ROOT / "src" / "audio_stall_trace.h").read_text(
            encoding="utf-8"
        )

        self.assertIn("defined(_MSC_VER)", header)
        self.assertIn("__declspec(align(4))", header)
        self.assertIn("#pragma pack(push, 1)", header)
        self.assertIn("#pragma pack(pop)", header)
        self.assertIn("__attribute__((packed, aligned(4)))", header)
        self.assertEqual(4, header.count("typedef struct AUDIO_STALL_TRACE_WIRE_STRUCT"))

    def test_gui_consumes_existing_trace_samples_without_requesting_rssi(self):
        comm = (ROOT / "gui" / "picoashacomm.cpp").read_text(encoding="utf-8")
        parser = function_body(comm, "void PicoAshaComm::handleAudioTracePacket")
        gui_sources = "\n".join(
            path.read_text(encoding="utf-8")
            for path in (ROOT / "gui").glob("*.cpp")
        )

        self.assertIn("AUDIO_STALL_TRACE_RSSI_SAMPLE", parser)
        self.assertIn("getRemoteByHCIHandle", parser)
        self.assertIn("addRssiSample", parser)
        self.assertNotIn("sendCommandPacket", parser)
        self.assertNotIn("gap_read_rssi", gui_sources)
        self.assertNotIn("sample_rssi", gui_sources)

    def test_history_windows_statistics_and_quality_labels_are_present(self):
        remote = (ROOT / "gui" / "remotedevice.cpp").read_text(encoding="utf-8")
        graph = (ROOT / "gui" / "rssihistorygraph.cpp").read_text(encoding="utf-8")
        update = function_body(remote, "void RemoteDevice::updateRssiLabels")
        quality = function_body(remote, "QString RemoteDevice::rssiQuality")

        self.assertIn("rssi_average_window_ms = 10'000", remote)
        self.assertIn("rssi_history_window_ms = 60'000", remote)
        self.assertIn("10 s average", remote)
        self.assertIn("10 s minimum", remote)
        self.assertIn("graph_history_ms = 60'000", graph)
        self.assertIn('"unavailable"', remote)
        self.assertIn("m_rssiAverageLabel", update)
        self.assertIn("m_rssiMinimumLabel", update)
        for label in ("good", "fair", "weak", "very weak"):
            self.assertIn(f'"{label}"', quality)

    def test_history_resets_on_disconnect_and_handle_change(self):
        remote = (ROOT / "gui" / "remotedevice.cpp").read_text(encoding="utf-8")
        comm = (ROOT / "gui" / "picoashacomm.cpp").read_text(encoding="utf-8")
        main_window = (ROOT / "gui" / "picoashamainwindow.cpp").read_text(
            encoding="utf-8"
        )
        set_handle = function_body(remote, "void RemoteDevice::setHCIHandle")
        remove_remote = function_body(
            main_window, "void PicoAshaMainWindow::removeRemote"
        )
        events = function_body(comm, "void PicoAshaComm::handleEventPacket")

        self.assertIn("m_hciHandle != hciHandle", set_handle)
        self.assertLess(
            set_handle.index("resetRssiHistory"),
            set_handle.index("m_hciHandle = hciHandle"),
        )
        self.assertIn("setDefaultValues", remove_remote)
        self.assertIn("resetRssiHistories", events)
        self.assertIn("BLEConnectionState::Disconnected", events)
        self.assertIn("BLEConnectionState::Recovering", events)

    def test_rssi_ui_is_separate_from_connection_status(self):
        remote = (ROOT / "gui" / "remotedevice.cpp").read_text(encoding="utf-8")
        main_window = (ROOT / "gui" / "picoashamainwindow.cpp").read_text(
            encoding="utf-8"
        )

        self.assertIn("BLE RSSI (radio signal only)", remote)
        self.assertIn("does not prove ASHA audio quality", remote)
        self.assertIn("m_bleConnectionStatus", main_window)
        self.assertNotIn("m_bleConnectionStatus", remote)


if __name__ == "__main__":
    unittest.main()
