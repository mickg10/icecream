#!/usr/bin/env python3
"""Self-tests for run_formal_checks_v4.py; no external packages required."""

from __future__ import annotations

import importlib.util
import os
import struct
import subprocess
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

LINT_PATH = Path(__file__).with_name("tla_primed_assignment_lint.py")
LINT_SPEC = importlib.util.spec_from_file_location(
    "tla_primed_assignment_lint", LINT_PATH
)
assert LINT_SPEC and LINT_SPEC.loader
assignment_lint = importlib.util.module_from_spec(LINT_SPEC)
sys.modules[LINT_SPEC.name] = assignment_lint
LINT_SPEC.loader.exec_module(assignment_lint)

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
        build_id = "4be712aa86094ac6fa52cf659407485a31cbfbd7"
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
            2,
            62,
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
            4,
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

    def test_primed_assignment_lint_rejects_unparenthesized_boolean_rhs(self) -> None:
        bad = """
A ==
    /\\ seen' = seen
          \\/ event
B ==
    /\\ owned' = owned /\\ reserved
"""
        violations = assignment_lint.lint_text(self.root / "bad.tla", bad)
        self.assertEqual([item.variable for item in violations], ["seen", "owned"])
        self.assertEqual([item.operator for item in violations], ["\\/", "/\\"])

    def test_primed_assignment_lint_accepts_parenthesized_and_structured_rhs(self) -> None:
        good = """
A ==
    /\\ seen' = (seen \\/ event)
B ==
    /\\ owned' =
          (owned /\\ reserved)
C ==
    /\\ value' = IF ready /\\ live THEN next ELSE value
"""
        self.assertEqual(
            assignment_lint.lint_text(self.root / "good.tla", good),
            [],
        )

    def test_all_repository_primed_assignments_have_unambiguous_rhs(self) -> None:
        formal_dir = Path(__file__).resolve().parent
        violations = assignment_lint.lint_directory(formal_dir)
        self.assertEqual(
            violations,
            [],
            "\n".join(
                f"{item.path}:{item.line}: {item.variable}' {item.operator}"
                for item in violations
            ),
        )

    def test_tlaps_generated_files_are_isolated_from_source_checkout(self) -> None:
        repo = self.root / "repo"
        formal = repo / "formal"
        formal.mkdir(parents=True)
        (formal / "AssignmentFenceCore.tla").write_text(
            "---- MODULE AssignmentFenceCore ----\nX == TRUE\n====\n",
            encoding="utf-8",
        )
        (formal / "AssignmentFenceCoreProof.tla").write_text(
            "---- MODULE AssignmentFenceCoreProof ----\n"
            "EXTENDS AssignmentFenceCore\n"
            "THEOREM T == X\nBY DEF X\n"
            "====\n",
            encoding="utf-8",
        )
        subprocess.run(["git", "init", "-q"], cwd=repo, check=True)
        subprocess.run(
            ["git", "config", "user.email", "formal-test@example.invalid"],
            cwd=repo,
            check=True,
        )
        subprocess.run(
            ["git", "config", "user.name", "Formal Test"],
            cwd=repo,
            check=True,
        )
        subprocess.run(["git", "add", "formal"], cwd=repo, check=True)
        subprocess.run(
            ["git", "commit", "-q", "-m", "fixture"],
            cwd=repo,
            check=True,
        )

        fake_tlapm = self.root / "fake-tlapm"
        fake_tlapm.write_text(
            "#!/bin/sh\n"
            "mkdir -p .tlacache\n"
            "echo cache > .tlacache/cache\n"
            "echo trace > AssignmentFenceCore_TTrace_1.tla\n"
            "echo 'All obligations proved'\n",
            encoding="utf-8",
        )
        fake_tlapm.chmod(0o755)
        artifacts = self.root / "artifacts"
        record = module.run_tlaps_proof(
            proof={
                "id": "core-tlaps-proof",
                "file": "AssignmentFenceCoreProof.tla",
                "timeout_seconds": 30,
            },
            tlapm=fake_tlapm,
            formal_dir=formal,
            artifacts=artifacts,
            time_bin=Path("/usr/bin/time"),
            proof_env=dict(os.environ),
        )

        self.assertFalse((formal / ".tlacache").exists())
        self.assertFalse((formal / "AssignmentFenceCore_TTrace_1.tla").exists())
        work = Path(record["isolated_work_dir"])
        self.assertTrue((work / ".tlacache" / "cache").is_file())
        self.assertTrue((work / "AssignmentFenceCore_TTrace_1.tla").is_file())
        status = subprocess.run(
            ["git", "status", "--porcelain", "--untracked-files=all"],
            cwd=repo,
            text=True,
            stdout=subprocess.PIPE,
            check=True,
        )
        self.assertEqual(status.stdout, "")


if __name__ == "__main__":
    unittest.main()
