# SPDX-License-Identifier: GPL-3.0-or-later
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

TOOL = Path(__file__).resolve().parents[1] / "video-packet-loss.py"
spec = importlib.util.spec_from_file_location("video_packet_loss", TOOL)
audit = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = audit
spec.loader.exec_module(audit)


class PacketLossTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.log = Path(cls.temp.name) / "audit.log"
        fixture = os.environ.get("WIVRN_PACKET_AUDIT_FIXTURE")
        if not fixture:
            fixture = str(Path(cls.temp.name) / "audit-fixture")
            root = TOOL.parents[1]
            subprocess.run(["c++", "-std=c++23", "-Wall", "-Wextra", "-Werror",
                            "-I", str(root / "common"), str(root / "common/tests/video_packet_audit.cpp"),
                            "-o", fixture], check=True)
        cls.raw = subprocess.check_output([fixture], text=True)
        cls.log.write_text("".join("log prefix " + line + "\n" for line in cls.raw.splitlines()))
        cls.captures = audit.read_log(cls.log)

    def report(self, stream=0):
        _, tx = audit.select(self.captures, "tx", stream)
        _, rx = audit.select(self.captures, "rx", stream)
        return audit.compare(tx, rx)

    def test_loss_late_duplicates_tcp_failed_send_and_pending_tail(self):
        report = self.report()
        totals = report["totals"]
        self.assertEqual(totals["tx_frames"], 4)
        self.assertEqual(totals["send_failed_or_inactive"], 1)
        self.assertEqual(totals["tcp_accepted"], 1)
        self.assertEqual(totals["udp_accepted"], 30)
        self.assertEqual(totals["udp_finalized_accepted"], 22)
        self.assertEqual(totals["udp_missing_at_retirement"], 10)
        self.assertEqual(totals["udp_received_late_unique"], 1)
        self.assertEqual(totals["udp_not_received_by_capture_end"], 16)
        self.assertEqual(totals["rx_duplicates_all_transports"], 1)
        self.assertEqual(totals["rx_out_of_order_all_transports"], 3)
        self.assertEqual(totals["finalized_frames_not_completed"], 2)
        self.assertEqual(totals["unfinalized_frames"], 1)
        self.assertEqual(totals["max_consecutive_missing_in_frame"], 8)
        self.assertEqual(report["missing_per_finalized_udp_frame"], {0: 1, 2: 1, 8: 1})
        self.assertAlmostEqual(report["udp_missing_at_retirement_percent"], 1000 / 22)

    def test_chunk_boundaries(self):
        report = self.report(1)
        self.assertEqual(report["totals"]["udp_finalized_accepted"], 4)
        self.assertEqual(report["udp_missing_at_retirement_percent"], 0)
        self.assertTrue(all(len(line) < 2048 for line in self.raw.splitlines()))

    def test_history_eviction_and_reset(self):
        report = self.report(2)
        self.assertEqual(report["totals"]["udp_accepted"], 130)
        self.assertEqual(report["totals"]["rx_duplicates_all_transports"], 1)
        self.assertEqual(report["totals"]["udp_received_late_unique"], 0)
        self.assertEqual(report["udp_missing_at_retirement_percent"], 0)

    def test_missing_first_and_terminal_shards(self):
        report = self.report(3)
        self.assertEqual(report["totals"]["udp_accepted"], 8)
        self.assertEqual(report["totals"]["udp_missing_at_retirement"], 2)
        self.assertEqual(report["udp_missing_at_retirement_percent"], 25)

    def test_tcp_only_has_no_udp_loss_rate(self):
        report = self.report(4)
        self.assertEqual(report["totals"]["tcp_accepted"], 1)
        self.assertIsNone(report["udp_missing_at_retirement_percent"])

    def test_no_packets_received(self):
        report = self.report(5)
        self.assertEqual(report["totals"]["udp_not_received_by_capture_end"], 8)
        self.assertEqual(report["totals"]["unfinalized_frames"], 1)
        self.assertIsNone(report["udp_missing_at_retirement_percent"])

    def altered_log(self, lines):
        path = Path(self.temp.name) / "altered.log"
        path.write_text("\n".join(lines) + "\n")
        return path

    def test_missing_record_rejected(self):
        lines = self.raw.splitlines()
        del lines[2]
        with self.assertRaisesRegex(ValueError, "records missing"):
            audit.read_log(self.altered_log(lines))

    def test_truncated_line_rejected(self):
        with self.assertRaises(ValueError):
            audit.read_log(self.altered_log([self.raw.splitlines()[0][:-5]]))

    def test_missing_begin_rejected(self):
        with self.assertRaisesRegex(ValueError, "missing begin"):
            audit.read_log(self.altered_log(self.raw.splitlines()[2:]))

    def test_open_capture_rejected(self):
        captures = audit.read_log(self.altered_log([
            line for line in self.raw.splitlines() if '"event":"end"' not in line]))
        with self.assertRaisesRegex(ValueError, "no end record"):
            audit.select(captures, "rx", 0)

    def test_multiple_captures_require_selection(self):
        captures = dict(self.captures)
        original = next(value for key, value in captures.items() if key[:2] == ("tx", 0))
        captures[("tx", 0, 999)] = original
        with self.assertRaisesRegex(ValueError, "select one"):
            audit.select(captures, "tx", 0)
        self.assertEqual(audit.select(captures, "tx", 0, 999)[0], 999)

    def test_cli(self):
        result = subprocess.run([sys.executable, str(TOOL), str(self.log), str(self.log)],
                                text=True, capture_output=True, check=True)
        self.assertEqual(json.loads(result.stdout)["totals"]["udp_missing_at_retirement"], 10)


if __name__ == "__main__":
    unittest.main()
