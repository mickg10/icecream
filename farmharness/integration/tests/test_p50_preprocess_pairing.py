"""Deterministic and fail-closed tests for paired source preprocessing."""

import re
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from tools.p50_regenerate_paired_corpus import (
    archive_match,
    compile_index,
    compiler_identity,
    main,
    preprocess,
    select_candidates,
    validate_source_roots,
    write_manifest,
    write_vfs_overlay,
)


class TestPairedCorpusGenerator(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="p50-pair-generator-test-")
        self.root = Path(self.temp.name)
        self.source = self.root / "source"
        self.build = self.root / "build"
        self.archive = self.root / "archive"
        self.output = self.root / "out"
        for path in (self.source, self.build, self.archive, self.output):
            path.mkdir()

    def tearDown(self):
        self.temp.cleanup()

    def _entry(self, identity):
        source = self.source / "contrib/rocksdb" / (identity + ".cc")
        source.parent.mkdir(parents=True, exist_ok=True)
        source.touch()
        return {
            "directory": str(self.build),
            "file": str(source),
            "output": str(self.build / "obj" / (identity + ".o")),
            "command": "clang++ -c source.cc",
        }

    def _archive(self, identity, size):
        path = self.archive / (identity + ".ii")
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b"x" * size)
        return path

    def test_selection_is_deterministic_and_manifests_preserve_pair_order(self):
        archives = {
            "contrib/rocksdb/a.ii": self._archive("contrib/rocksdb/a", 30),
            "contrib/rocksdb/b.ii": self._archive("contrib/rocksdb/b", 40),
            "contrib/rocksdb/c.ii": self._archive("contrib/rocksdb/c", 40),
        }
        entries = {identity: self._entry(Path(identity).stem) for identity in archives}
        selected, total = select_candidates(
            archives, entries, re.compile(r"/contrib/rocksdb/"), 2, 80
        )
        self.assertEqual(total, 80)
        identities = [identity for _, identity, _ in selected]
        self.assertEqual(identities, ["contrib/rocksdb/b.ii", "contrib/rocksdb/c.ii"])
        write_manifest(self.output, "A", selected)
        write_manifest(self.output, "B", selected)
        a_lines = (self.output / "manifest-A.txt").read_text().splitlines()
        b_lines = (self.output / "manifest-B.txt").read_text().splitlines()
        self.assertEqual(
            [Path(item).relative_to(self.output / "A") for item in a_lines],
            [Path(item).relative_to(self.output / "B") for item in b_lines],
        )

    def test_cap_is_fail_closed(self):
        archives = {"contrib/rocksdb/a.ii": self._archive("contrib/rocksdb/a", 51)}
        entries = {"contrib/rocksdb/a.ii": self._entry("a")}
        with self.assertRaisesRegex(ValueError, "above cap"):
            select_candidates(archives, entries, re.compile(r"/contrib/rocksdb/"), 1, 50)

    def test_missing_compile_mapping_is_not_counted_as_a_pair(self):
        archives = {
            "contrib/rocksdb/a.ii": self._archive("contrib/rocksdb/a", 20),
            "contrib/rocksdb/b.ii": self._archive("contrib/rocksdb/b", 10),
        }
        entries = {"contrib/rocksdb/a.ii": self._entry("a")}
        with self.assertRaisesRegex(ValueError, "only 1 archived TUs"):
            select_candidates(archives, entries, re.compile(r"/contrib/rocksdb/"), 2, 100)

    def test_duplicate_compile_output_identity_fails(self):
        first = self._entry("a")
        second = dict(first)
        second["file"] = str(self.source / "contrib/rocksdb/other.cc")
        Path(second["file"]).touch()
        with self.assertRaisesRegex(ValueError, "duplicate compile output identity"):
            compile_index([first, second], self.source, self.build)

    def test_preprocessor_failure_is_reported_with_exit_and_stderr(self):
        with self.assertRaisesRegex(RuntimeError, r"rc=7[\s\S]*controlled failure"):
            preprocess(
                [sys.executable, "-c", "import sys; print('controlled failure', file=sys.stderr); sys.exit(7)"],
                str(self.root), "A", "contrib/rocksdb/a.ii",
            )

    def test_different_selected_compilers_are_rejected(self):
        compilers = []
        for name in ("compiler-a", "compiler-b"):
            compiler = self.root / name
            compiler.write_text("#!/bin/sh\necho test-" + name + "\n")
            compiler.chmod(0o755)
            compilers.append(compiler)
        entries = [
            {"command": f"{compilers[0]} -c source.cc"},
            {"command": f"{compilers[1]} -c source.cc"},
        ]
        with self.assertRaisesRegex(ValueError, "multiple compiler executables"):
            compiler_identity(entries)

    def test_vfs_overlay_keeps_virtual_name_and_external_name_disabled(self):
        overlay_path = self.root / "overlay.json"
        virtual = self.source / "contrib/rocksdb/a.cc"
        external = self.root / "overlay-copy/a.cc"
        external.parent.mkdir(parents=True)
        external.write_text("// edited source copy\n")
        digest = write_vfs_overlay(overlay_path, virtual, external)
        import json

        overlay = json.loads(overlay_path.read_text())
        root = overlay["roots"][0]
        self.assertEqual(root["name"], str(virtual))
        self.assertEqual(root["external-contents"], str(external))
        self.assertFalse(root["use-external-name"])
        self.assertFalse(overlay["use-external-names"])
        self.assertEqual(len(digest), 64)

    def test_separate_source_trees_are_rejected_to_prevent_include_path_mixing(self):
        other = self.root / "other-source"
        other.mkdir()
        with self.assertRaisesRegex(ValueError, "requires A and B source roots"):
            validate_source_roots(self.source, self.source, other)

    def test_archive_mismatch_is_recorded_not_mixed_into_a_manifest(self):
        old = self._archive("contrib/rocksdb/a", 4)
        regenerated = self.root / "regenerated.ii"
        regenerated.write_bytes(b"new bytes")
        self.assertFalse(archive_match(regenerated, old))
        selected = [(old.stat().st_size, "contrib/rocksdb/a.ii", self._entry("a"))]
        write_manifest(self.output, "A", selected)
        write_manifest(self.output, "B", selected)
        a_identity = Path((self.output / "manifest-A.txt").read_text().strip()).relative_to(
            self.output / "A"
        )
        b_identity = Path((self.output / "manifest-B.txt").read_text().strip()).relative_to(
            self.output / "B"
        )
        self.assertEqual(a_identity, b_identity)

    def _prepare_cli_fixture(self, edited_bytes):
        source_root = self.root / "cli-source"
        build_root = self.root / "cli-build"
        archive_root = self.root / "cli-archive"
        output_dir = self.root / "cli-output"
        for directory in (source_root, build_root, archive_root):
            directory.mkdir()
        source = source_root / "contrib/rocksdb/sample.cc"
        source.parent.mkdir(parents=True)
        source.write_bytes(b"alpha")
        overlay_root = self.root / "cli-overlay"
        overlay_source = overlay_root / "contrib/rocksdb/sample.cc"
        overlay_source.parent.mkdir(parents=True)
        overlay_source.write_bytes(edited_bytes)

        subprocess.run(["git", "init", "-q", str(source_root)], check=True)
        subprocess.run(["git", "-C", str(source_root), "config", "user.name", "QA"], check=True)
        subprocess.run(["git", "-C", str(source_root), "config", "user.email", "qa@example.invalid"], check=True)
        subprocess.run(["git", "-C", str(source_root), "add", "contrib/rocksdb/sample.cc"], check=True)
        subprocess.run(["git", "-C", str(source_root), "commit", "-qm", "fixture"], check=True)

        fake_compiler = self.root / "fake-preprocessor"
        fake_compiler.write_text(
            f"#!{sys.executable}\n"
            "import json, pathlib, sys\n"
            "if '--version' in sys.argv:\n"
            " print('fake-preprocessor 1'); raise SystemExit(0)\n"
            "args=sys.argv[1:]\n"
            "out=pathlib.Path(args[args.index('-o')+1])\n"
            "source=pathlib.Path(next(arg for arg in args if arg.endswith('.cc')))\n"
            "data=source.read_bytes()\n"
            "if '-ivfsoverlay' in args:\n"
            " overlay=json.loads(pathlib.Path(args[args.index('-ivfsoverlay')+1]).read_text())\n"
            " data=pathlib.Path(overlay['roots'][0]['external-contents']).read_bytes()\n"
            "out.write_bytes(data)\n"
        )
        fake_compiler.chmod(0o755)

        compile_path = self.root / "compile_commands.json"
        compile_path.write_text(json.dumps([{
            "directory": str(build_root),
            "file": str(source),
            "output": str(build_root / "contrib/rocksdb/sample.o"),
            "command": f"{fake_compiler} -c {source}",
        }]))
        archived = archive_root / "contrib/rocksdb/sample.ii"
        archived.parent.mkdir(parents=True)
        archived.write_bytes(source.read_bytes())
        manifest = archive_root / "manifest.txt"
        manifest.write_text(str(archived) + "\n")
        argv = [
            "p50_regenerate_paired_corpus.py",
            "--compile-commands", str(compile_path),
            "--original-source-root", str(source_root),
            "--build-root", str(build_root),
            "--archive-root", str(archive_root),
            "--archive-manifest", str(manifest),
            "--source-root-a", str(source_root),
            "--source-root-b", str(source_root),
            "--overlay-b-root", str(overlay_root),
            "--output-dir", str(output_dir),
            "--count", "1",
            "--max-bytes", "5",
        ]
        return argv, output_dir, overlay_root

    def test_cli_publishes_both_manifests_only_after_successful_pair(self):
        argv, output_dir, _ = self._prepare_cli_fixture(b"omega")
        with patch.object(sys, "argv", argv):
            self.assertEqual(main(), 0)
        self.assertTrue((output_dir / "manifest-A.txt").is_file())
        self.assertTrue((output_dir / "manifest-B.txt").is_file())
        self.assertEqual((output_dir / "A/contrib/rocksdb/sample.ii").read_bytes(), b"alpha")
        self.assertEqual((output_dir / "B/contrib/rocksdb/sample.ii").read_bytes(), b"omega")
        with patch.object(sys, "argv", argv), self.assertRaisesRegex(ValueError, "refusing overwrite"):
            main()

    def test_cli_b_cap_failure_publishes_neither_manifest(self):
        argv, output_dir, _ = self._prepare_cli_fixture(b"longer")
        with patch.object(sys, "argv", argv), self.assertRaisesRegex(ValueError, "exceeded cap"):
            main()
        self.assertFalse((output_dir / "manifest-A.txt").exists())
        self.assertFalse((output_dir / "manifest-B.txt").exists())

    def test_cli_rejects_header_edits_outside_selected_tu_overlay(self):
        argv, _, overlay_root = self._prepare_cli_fixture(b"omega")
        header = overlay_root / "contrib/rocksdb/sample.h"
        header.write_text("// unsupported header edit\n")
        with patch.object(sys, "argv", argv), self.assertRaisesRegex(
            ValueError, "unsupported header edit"
        ):
            main()

    def test_cli_rejects_missing_overlay_root(self):
        argv, _, _ = self._prepare_cli_fixture(b"omega")
        argv[argv.index("--overlay-b-root") + 1] = str(self.root / "missing-overlay")
        with patch.object(sys, "argv", argv), self.assertRaisesRegex(
            ValueError, "B overlay root does not exist"
        ):
            main()


if __name__ == "__main__":
    unittest.main()
