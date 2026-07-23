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


class GuiReconnectOwnershipTest(unittest.TestCase):
    def test_disconnect_uses_firmware_recovery_without_gui_restart(self):
        gui = (ROOT / "gui" / "picoashacomm.cpp").read_text(encoding="utf-8")
        events = function_body(gui, "void PicoAshaComm::handleEventPacket")
        disconnected = events.split("case EventType::RemoteDisconnected:", 1)[1]
        disconnected = disconnected.split("case EventType::BLEConnectionState:", 1)[0]

        self.assertNotIn(".cmd = Command::Restart", disconnected)
        self.assertNotIn("sendCommandPacket", disconnected)
        self.assertIn("BLEConnectionState::Recovering", disconnected)

        manual = function_body(gui, "void PicoAshaComm::onCmdRestartBtnClicked")
        self.assertIn("Command::Restart", manual)
        self.assertIn("m_manualRestartPending", manual)
        self.assertEqual(1, gui.count(".cmd = Command::Restart"))

        firmware = (ROOT / "src" / "hearing_aid.cpp").read_text(encoding="utf-8")
        on_disconnect = function_body(firmware, "void HearingAid::on_disconnected")
        self.assertIn("BLEConnectionState::Recovering", on_disconnect)
        self.assertIn("schedule_reconnect(ha, status, reason)", on_disconnect)
        self.assertNotIn("start_scan();", on_disconnect)
        reconnect_loop = function_body(firmware, "void HearingAid::process_reconnect")
        self.assertIn("start_scan();", reconnect_loop)
        self.assertIn("ha->reset();", on_disconnect)

    def test_gui_timers_only_manage_serial_discovery_and_intro_timeout(self):
        gui = (ROOT / "gui" / "picoashacomm.cpp").read_text(encoding="utf-8")
        connect_timer = function_body(gui, "void PicoAshaComm::onConnectTimer")
        intro_timer = function_body(gui, "void PicoAshaComm::onIntroTimer")
        self.assertNotIn("Command::Restart", connect_timer)
        self.assertNotIn("Command::Restart", intro_timer)
        self.assertNotIn("RemoteDisconnected", connect_timer)

    def test_capability_and_all_recovery_states_are_exposed(self):
        protocol = (ROOT / "include" / "asha_comms.hpp").read_text(encoding="utf-8")
        firmware = (ROOT / "src" / "hearing_aid.cpp").read_text(encoding="utf-8")
        window = (ROOT / "gui" / "picoashamainwindow.cpp").read_text(encoding="utf-8")

        self.assertIn("firmware_managed_reconnect", protocol)
        self.assertIn("IntroFlags::firmware_managed_reconnect", firmware)
        for state in ("disconnected", "recovering", "scanning", "connecting", "connected"):
            self.assertIn(f'BLE: {state}', window)


if __name__ == "__main__":
    unittest.main()
