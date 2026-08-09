#!/usr/bin/env python3
"""Self-tests for run_formal_checks_v4.py; no external packages required."""

from __future__ import annotations

import importlib.util
import os
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.dont_write_bytecode = True

MODULE_PATH = Path(__file__).with_name("run_formal_checks_v4.py")
SPEC = importlib.util.spec_from_file_location("run_formal_checks_v4", MODULE_PATH)
assert SPEC and SPEC.loader
module = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = module
SPEC.loader.exec_module(module)

v2 = module.v2


class BackendProbeTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def write_probe(
        self,
        name: str,
        expected_argument: str,
        output: str,
        *,
        returncode: int = 0,
    ) -> Path:
        path = self.root / name
        path.write_text(
            "#!/bin/sh\n"
            f"if [ \"$1\" = \"{expected_argument}\" ]; then\n"
            f"  echo '{output}'\n"
            f"  exit {returncode}\n"
            "fi\n"
            "echo 'unexpected probe' >&2\n"
            "exit 99\n",
            encoding="utf-8",
        )
        path.chmod(0o755)
        return path

    def write_ls4_elf(self, *, include_marker: bool = True) -> tuple[Path, str]:
        build_id = "4be712aa86094ac6fa52cf659407485a31cbfbd7b"
        descriptor = bytes.fromhex(build_id)
        note = struct.pack("<III", 4, len(descriptor), 3)
        note += b"GNU\0"
        note += descriptor
        while len(note) % 4:
            note += b"\0"

        elf_header_size = 64
        program_header_size = 56
        note_offset = elf_header_size + program_header_size
        ident = b"\x7fELF" + bytes([2, 1, 1, 0, 0]) + b"\0" * 7
        header = struct.pack(
            "<HHIQQQIHHHHHH",
            2,       # ET_EXEC
            62,      # x86-64
            1,
            0,
            elf_header_size,
            0,
            0,
            elf_header_size,
            program_header_size,
            1,
            0,
            0,
            0,
        )
        program = struct.pack(
            "<IIQQQQQQ",
            4,       # PT_NOTE
            4,
            note_offset,
            0,
            0,
            len(note),
            len(note),
            4,
        )
        marker = (
            b"\0/build/tlaps/ls4-1.0/src/ls4\0"
            if include_marker
            else b"\0/build/tlaps/unknown/src/ls4\0"
        )
        path = self.root / "ls4"
        path.write_bytes(ident + header + program + note + marker)
        path.chmod(0o755)
        return path, build_id

    def test_backend_specific_successful_probes(self) -> None:
        isabelle = self.write_probe(
            "isabelle", "version", "Isabelle2011-1: October 2011"
        )
        zenon = self.write_probe(
            "zenon", "-v", "zenon version 0.8.4 [a268] 2017-11-14"
        )
        z3 = self.write_probe("z3", "--version", "Z3 version 4.8.9")

        command, returncode, output = module.backend_version(
            "isabelle", isabelle, self.root
        )
        self.assertEqual(returncode, 0)
        self.assertEqual(command[-1], "version")
        self.assertIn("Isabelle2011-1", output)

        command, returncode, output = module.backend_version(
            "zenon", zenon, self.root
        )
        self.assertEqual(returncode, 0)
        self.assertEqual(command[-1], "-v")
        self.assertIn("zenon version 0.8.4", output)

        command, returncode, output = module.backend_version("z3", z3, self.root)
        self.assertEqual(returncode, 0)
        self.assertEqual(command[-1], "--version")
        self.assertIn("4.8.9", output)

    def test_nonzero_usage_output_is_not_version_evidence(self) -> None:
        zenon = self.write_probe(
            "bad-zenon",
            "-v",
            "unknown option '-v'; usage: zenon ...",
            returncode=2,
        )
        with self.assertRaisesRegex(v2.FormalRunError, "exited 2"):
            module.backend_version("zenon", zenon, self.root)

    def test_ls4_identity_uses_embedded_package_and_elf_build_id(self) -> None:
        ls4, build_id = self.write_ls4_elf()
        command, returncode, output = module.backend_version(
            "ls4", ls4, self.root
        )
        self.assertIsNone(command)
        self.assertIsNone(returncode)
        self.assertIn("package=ls4-1.0", output)
        self.assertIn(build_id, output)

    def test_ls4_missing_package_marker_is_rejected(self) -> None:
        ls4, _ = self.write_ls4_elf(include_marker=False)
        with self.assertRaisesRegex(v2.FormalRunError, "package/build marker"):
            module.backend_version("ls4", ls4, self.root)

    def test_full_backend_pin_set(self) -> None:
        paths = {
            "isabelle": self.write_probe(
                "isabelle", "version", "Isabelle2011-1: October 2011"
            ),
            "zenon": self.write_probe(
                "zenon", "-v", "zenon version 0.8.4 [a268] 2017-11-14"
            ),
            "z3": self.write_probe("z3", "--version", "Z3 version 4.8.9"),
        }
        paths["ls4"], build_id = self.write_ls4_elf()
        specs = [
            f"{name}={path}={v2.sha256_file(path)}"
            for name, path in paths.items()
        ]
        backends, environment = module.pin_backends(specs, self.root)
        self.assertEqual(set(backends), {"isabelle", "zenon", "z3", "ls4"})
        self.assertEqual(backends["isabelle"].version_returncode, 0)
        self.assertEqual(backends["zenon"].version_returncode, 0)
        self.assertEqual(backends["z3"].version_returncode, 0)
        self.assertIsNone(backends["ls4"].version_returncode)
        self.assertIn(build_id, backends["ls4"].version_output)
        self.assertTrue(environment["PATH"].startswith(str(self.root)))

    def test_artifact_directory_relation(self) -> None:
        repo = self.root / "repo"
        repo.mkdir()
        self.assertTrue(module._is_inside(repo / "artifacts", repo))
        self.assertFalse(module._is_inside(self.root / "outside", repo))


if __name__ == "__main__":
    unittest.main()
