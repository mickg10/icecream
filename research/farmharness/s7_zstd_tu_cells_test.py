import json
import tempfile
import unittest
from pathlib import Path

from s7_zstd_tu_cells import compile_argv_from_database, source_from_manifest


class S7InputSeamsTest(unittest.TestCase):
    def test_manifest_rejects_escape_and_accepts_relative_file(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "ok.cc").write_text("int x;", encoding="utf-8")
            manifest = root / "manifest"
            manifest.write_text("../ok.cc\n", encoding="utf-8")
            self.assertIsNone(source_from_manifest(root, manifest))
            manifest.write_text("ok.cc\n", encoding="utf-8")
            self.assertEqual(source_from_manifest(root, manifest)[1], "ok.cc")

    def test_compile_database_preserves_authoritative_argv_order(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            database = root / "compile_commands.json"
            source = str(root / "db.cc")
            database.write_text(json.dumps([{
                "file": source,
                "command": "/usr/bin/c++ -DSECOND=2 -Ifirst -std=gnu++20 -c " + source,
            }]), encoding="utf-8")
            argv = compile_argv_from_database(database, source)
            self.assertEqual(argv, ["-DSECOND=2", "-Ifirst", "-std=gnu++20", "-c", source])
            self.assertIsNone(compile_argv_from_database(database, str(root / "other.cc")))

    def test_compile_database_keeps_debug_and_normalizes_producer_notes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            database = root / "compile_commands.json"
            source = str(root / "db.cc")
            database.write_text(json.dumps([{
                "file": source,
                "command": f"/usr/bin/c++ -g -O2 -c {source}",
            }]), encoding="utf-8")
            argv = compile_argv_from_database(database, source)
            self.assertIn("-g", argv)
            self.assertEqual(argv[-1], "-gno-record-gcc-switches")

            database.write_text(json.dumps([{
                "file": source,
                "command": f"/usr/bin/c++ -g -grecord-gcc-switches -c {source}",
            }]), encoding="utf-8")
            explicit = compile_argv_from_database(database, source)
            self.assertEqual(explicit.count("-grecord-gcc-switches"), 1)
            self.assertNotIn("-gno-record-gcc-switches", explicit)


if __name__ == "__main__":
    unittest.main()
