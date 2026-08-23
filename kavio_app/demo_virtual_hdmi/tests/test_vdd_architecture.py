from __future__ import annotations

import json
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class VddArchitectureTests(unittest.TestCase):
    def test_uses_virtualdrivers_vdd_and_dxgi_capture(self) -> None:
        self.assertFalse((ROOT / "host" / "driver").exists())
        self.assertFalse((ROOT / "common" / "VirtualHdmiSharedFrame.h").exists())

        host_component = (ROOT / "host" / "HostComponent.cpp").read_text()
        display_control = (ROOT / "host" / "VirtualDisplayControl.cpp").read_text()
        capture = (ROOT / "host" / "VirtualDisplayCapture.cpp").read_text()
        self.assertIn('Root\\\\MttVDD', display_control)
        self.assertIn("DuplicateOutput", capture)
        self.assertNotIn("KavioVirtualHdmi720Frame", host_component)
        self.assertNotIn("VirtualHdmiSharedFrame", host_component)

    def test_vdd_configuration_is_fixed_to_one_720p_square_monitor(self) -> None:
        config = ROOT / "host" / "vdd_package" / "vdd_settings.xml"
        root = ET.parse(config).getroot()
        self.assertEqual(root.findtext("./monitors/count"), "1")

        resolutions = root.findall("./resolutions/resolution")
        self.assertEqual(len(resolutions), 1)
        self.assertEqual(resolutions[0].findtext("width"), "720")
        self.assertEqual(resolutions[0].findtext("height"), "720")
        self.assertEqual(resolutions[0].findtext("refresh_rate"), "60")

        self.assertEqual(root.findtext("./options/HardwareCursor"), "false")
        self.assertEqual(root.findtext("./options/HDRPlus"), "false")
        self.assertIsNone(root.find("./cursor"))

    def test_host_solution_contains_host_component_and_vdd_control(self) -> None:
        solution = (ROOT / "host" / "VirtualHdmiStream.sln").read_text(errors="ignore")
        self.assertNotIn("KavioVirtualHdmiDriver", solution)
        self.assertIn("VirtualHdmiStreamHost", solution)
        self.assertNotIn("VirtualDisplayCtl", solution)
        self.assertFalse((ROOT / "host" / "tools" / "VirtualDisplayCtl.cpp").exists())
        self.assertFalse((ROOT / "host" / "tools" / "VirtualDisplayCtl.vcxproj").exists())
        self.assertFalse((ROOT / "host" / "plugin").exists())
        self.assertFalse((ROOT / "host" / "build.cmd").exists())

        host_project = (ROOT / "host" / "VirtualHdmiStreamHost.vcxproj").read_text(errors="ignore")
        self.assertIn("HostComponent.cpp", host_project)
        self.assertIn("VirtualDisplayControl.cpp", host_project)
        self.assertNotIn("output\\build", host_project)
        self.assertNotIn("<OutDir>", host_project)
        self.assertNotIn("<IntDir>", host_project)
        self.assertIn("stage_vdd_package.cmd", host_project)

        host_component = (ROOT / "host" / "HostComponent.cpp").read_text(errors="ignore")
        display_control = (ROOT / "host" / "VirtualDisplayControl.cpp").read_text(errors="ignore")
        self.assertIn("KavioVirtualDisplayAdmin", host_component)
        self.assertIn("rundll32.exe", host_component)
        self.assertIn("KavioVirtualDisplayAdmin", display_control)
        self.assertIn("LPSTR command_line", display_control)
        self.assertNotIn("LPWSTR command_line", display_control)
        self.assertNotIn("VirtualDisplayCtl.exe", host_component)


    def test_host_build_stages_complete_vdd_package(self) -> None:
        stage_script = (ROOT / "host" / "stage_vdd_package.cmd").read_text(errors="ignore")
        for name in ("MttVDD.inf", "MttVDD.dll", "mttvdd.cat", "vdd_settings.xml", "VDD_LICENSE.txt", "VDD_SHA256.txt"):
            self.assertIn(name, stage_script)
        self.assertIn("output\\host", stage_script)
        self.assertIn("\\vdd", stage_script)


    def test_packaged_vdd_protocol_and_mode_activation(self) -> None:
        display_control = (ROOT / "host" / "VirtualDisplayControl.cpp").read_text(errors="ignore")
        capture = (ROOT / "host" / "VirtualDisplayCapture.cpp").read_text(errors="ignore")
        host_component = (ROOT / "host" / "HostComponent.cpp").read_text(errors="ignore")

        # Prefer the command used by the packaged 25.7.23 driver and keep the
        # alternate VirtualDrivers command available when the pipe rejects it.
        self.assertIn('primary_command << L"SETCOUNT "', display_control)
        self.assertIn('alternate_command << L"SETDISPLAYCOUNT "', display_control)
        self.assertLess(
            display_control.index('primary_command << L"SETCOUNT "'),
            display_control.index('alternate_command << L"SETDISPLAYCOUNT "'),
        )

        # Force the driver to read the staged XML from the path managed by Kavio,
        # independent of any existing VDDPATH value.
        self.assertIn('kUpstreamRegistryPath', display_control)
        self.assertIn('L"VDDPATH"', display_control)

        # Identification must not depend only on a friendly display name.
        self.assertIn('MTT1337', display_control)
        self.assertIn('MTT1337', capture)

        # Do not switch to 720x720 until the asynchronous VDD mode-list reload
        # has actually published that mode, and expose the real active mode in UI.
        self.assertIn('VddModeIsAvailable', display_control)
        self.assertIn('for (int retry = 0; retry < 80; ++retry)', display_control)
        self.assertIn('GetCurrentDisplayMode', host_component)
        self.assertIn('VDD desktop mode:', host_component)

    def test_streaming_ip_protocol_uses_resync_and_sliding_window(self) -> None:
        protocol = (ROOT / "common" / "VirtualHdmiProtocol.h").read_text(errors="ignore")
        host = (ROOT / "host" / "HostComponent.cpp").read_text(errors="ignore")
        device = (ROOT / "device" / "main.cpp").read_text(errors="ignore")
        receiver = (ROOT / "device" / "FrameReceiver.cpp").read_text(errors="ignore")

        self.assertIn("kProtocolVersion = 1", protocol)
        self.assertIn("KeyFrameRequest = 6", protocol)
        self.assertIn("base_frame_id", protocol)
        self.assertIn("FrameType::IFrame", host)
        self.assertIn("FrameType::PFrame", host)
        self.assertIn("kPeriodicKeyFrameInterval = 90", host)
        self.assertIn("kMaxInflightFrames = 2", host)
        self.assertIn("in_flight_frames", host)
        self.assertIn("window_skipped_capture_frames", host)
        self.assertIn("HandleKeyFrameRequest", host)
        self.assertNotIn("pending_frame_id", host)
        self.assertNotIn("kAckTimeoutMs", host)
        self.assertIn("TxFramePublisher", device)
        self.assertIn("kKeyFrameRequestRetry", device)
        self.assertIn("receiver.NeedsKeyFrame()", device)
        self.assertIn("message.base_frame_id != last_good_frame_id_", receiver)
        self.assertIn("kDefaultMaxFps = 60", host)
        self.assertIn("std::min<std::uint32_t>(60", host)
        self.assertIn("kAdvertisedMaxFps = 60", device)


    def test_readme_documents_tiled_sliding_window_pipeline(self) -> None:
        readme = (ROOT / "README.md").read_text(errors="ignore")
        self.assertIn("```mermaid", readme)
        self.assertIn("64x64", readme)
        self.assertIn("2-frame sliding window", readme)
        self.assertIn("KEYFRAME_REQUEST", readme)

    def test_device_runtime_is_native_cpp(self) -> None:
        package = json.loads((ROOT / "package" / "kavio_package.json").read_text())
        self.assertEqual(package["device"]["runtime"], "native")
        self.assertEqual(package["device"]["entry"], "demo_virtual_hdmi")

        device = ROOT / "device"
        self.assertTrue((device / "main.cpp").exists())
        self.assertTrue((device / "FrameReceiver.cpp").exists())
        self.assertTrue((device / "CMakeLists.txt").exists())
        self.assertFalse((device / "main.py").exists())
        self.assertFalse((device / "protocol.py").exists())
        self.assertFalse((device / "frame_receiver.py").exists())
        self.assertFalse((device / "rle.py").exists())
        self.assertFalse((device / "requirements.txt").exists())


if __name__ == "__main__":
    unittest.main()
