#!/usr/bin/env python3
"""Unit tests for run_mode_refinement_checks.py."""

from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import run_mode_refinement_checks as launcher

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
SCRIPT = HERE / "run_mode_refinement_checks.py"
MANIFEST = HERE / "mode-refinement-checks-v1.json"


class ModeRefinementLauncherTests(unittest.TestCase):
    def arguments(self, manifest: Path, artifacts: Path) -> list[str]:
        return [
            sys.executable,
            str(SCRIPT),
            "--repo",
            str(REPO),
            "--manifest",
            str(manifest),
            "--artifacts",
            str(artifacts),
            "--expected-git-sha",
            "1" * 40,
            "--stable-jar",
            "/tmp/stable.jar",
            "--stable-sha256",
            "a" * 64,
            "--differential-jar",
            "/tmp/differential.jar",
            "--differential-sha256",
            "b" * 64,
        ]

    def test_substituted_manifest_is_rejected_before_runner_execution(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            substitute = root / MANIFEST.name
            substitute.write_bytes(MANIFEST.read_bytes())
            result = subprocess.run(
                self.arguments(substitute, root / "artifacts"),
                cwd=REPO,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
            )
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn("only", result.stdout.lower())
        self.assertIn("authoritative", result.stdout.lower())

    def test_in_checkout_artifact_directory_is_rejected(self) -> None:
        result = subprocess.run(
            self.arguments(MANIFEST, REPO / "formal" / "artifacts"),
            cwd=REPO,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn("outside the checkout", result.stdout.lower())

    def test_help_option_discovery(self) -> None:
        help_text = """usage: runner [-h] --manifest MANIFEST --repo REPO
              --artifacts ARTIFACTS --expected-git-sha SHA
              --stable-jar JAR --stable-sha256 SHA
              --differential-jar JAR --differential-sha256 SHA

options:
  --manifest MANIFEST
  --static-checker PATH
  --static-checker-arg ARG
  --domain NAME
"""
        available = launcher.available_options(help_text)
        self.assertIn("--manifest", available)
        self.assertIn("--static-checker-arg", available)
        required = launcher.required_options(help_text)
        self.assertIn("--manifest", required)
        self.assertIn("--repo", required)
        self.assertNotIn("--static-checker", required)

    def test_first_supported_alias_is_selected(self) -> None:
        command = ["runner"]
        selected = launcher.add_supported(
            command,
            {"--artifact-dir", "--artifacts-dir"},
            ("--artifacts", "--artifact-dir", "--artifacts-dir"),
            "/tmp/evidence",
        )
        self.assertEqual(selected, "--artifact-dir")
        self.assertEqual(command, ["runner", "--artifact-dir", "/tmp/evidence"])

    def test_missing_required_alias_is_fail_closed(self) -> None:
        with self.assertRaises(SystemExit):
            launcher.require_supported(
                ["runner"],
                set(),
                ("--manifest",),
                str(MANIFEST),
                "manifest",
            )


if __name__ == "__main__":
    unittest.main()
