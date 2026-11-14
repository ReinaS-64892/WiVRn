# SPDX-License-Identifier: GPL-3.0-or-later
import importlib.util
from pathlib import Path
import tempfile
import unittest


spec = importlib.util.spec_from_file_location(
    "prepare_adapter", Path(__file__).resolve().parents[1] / "prepare_adapter.py")
adapter = importlib.util.module_from_spec(spec)
spec.loader.exec_module(adapter)


class PrepareProviderTest(unittest.TestCase):
    prefix = (
        "    // Until it's locked (or is the origin) it shows as searching (a blinking icon): its\n"
        "    // place can still move. Learned but not locked yet: calibrating.\n")
    old = "    const bool settled = b.known && (b.origin || b.locked);"
    current = (
        "    const bool settled = b.known && (b.origin || b.locked || b.learned);"
        "  // learned: placed well enough to track with")

    def test_supported_versions_preserve_source_and_visibility(self):
        for declaration in (self.old, self.current):
            with self.subTest(declaration=declaration), tempfile.TemporaryDirectory() as directory:
                source = Path(directory) / "driver.cpp"
                destination = Path(directory) / "provider" / "driver.cpp"
                original = self.prefix + declaration + "\n    p.poseIsValid = settled && b.seen;\n"
                source.write_text(original)
                adapter.prepare_provider(source, destination)
                result = destination.read_text()
                self.assertIn("const bool settled = b.known && b.learned;", result)
                self.assertIn("p.poseIsValid = settled && b.seen;", result)
                self.assertNotIn("b.origin", result)
                self.assertNotIn("b.locked", result)
                self.assertEqual(source.read_text(), original)

    def test_unknown_or_duplicate_blocks_fail_without_writing(self):
        for original in ("unknown provider", (self.prefix + self.current + "\n") * 2,
                         self.prefix + self.old + "\n" + self.prefix + self.current):
            with self.subTest(original=original), tempfile.TemporaryDirectory() as directory:
                source = Path(directory) / "driver.cpp"
                destination = Path(directory) / "provider.cpp"
                source.write_text(original)
                with self.assertRaisesRegex(RuntimeError, "Openhouse provider changed"):
                    adapter.prepare_provider(source, destination)
                self.assertFalse(destination.exists())


if __name__ == "__main__":
    unittest.main()
