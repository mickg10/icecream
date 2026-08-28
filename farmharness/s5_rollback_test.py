#!/usr/bin/env python3
"""Local controls for the S5 rollback/roll-forward acceptance owner."""

from __future__ import annotations

import copy
import json
import shlex
import sys
import tempfile
import unittest
from pathlib import Path

from farmharness.s5_rollback import (
    COUNTERS,
    PHASES,
    RollbackError,
    artifact_manifest,
    make_evidence,
    verify_evidence,
)


def _counter_values() -> dict[str, int]:
    return {name: 0 for name in COUNTERS}


def _hook(value: dict[str, object]) -> str:
    expression = "import json; print(json.dumps(" + repr(value) + "))"
    return f"{shlex.quote(sys.executable)} -c {shlex.quote(expression)}"


def _make_root(path: Path, *, cache: bool) -> None:
    for role, relative in {
        "S": "scheduler/icecc-scheduler",
        "C": "client/icecc",
        "F": "daemon/iceccd",
        "E": "client/icecc-create-env",
        "X": "cache/icecc-cache-service",
    }.items():
        if role == "X" and not cache:
            continue
        target = path / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(f"{role}-artifact\n".encode())
        if role != "E":
            target.chmod(0o755)


def _hooks() -> dict[str, str]:
    counters = _counter_values()
    return {
        "drain": _hook({"declared": True, "in_flight": "stopped-and-reclaimed",
                         "deadline_ns": 123}),
        "stop": _hook({"stopped": True, "sidecar_processes": 0,
                        "sidecar_leases": 0, **{name: 0 for name in COUNTERS
                                                 if name not in {"sidecar_processes", "sidecar_leases"}}}),
        "reclaim": _hook({"reclaimed": True, **counters}),
        "legacy": _hook({"remote_compile": True, "byte_identical": True,
                          "cache_observed": False, "profile_use_count": 0,
                          "selected_profile": None, **counters}),
        "current": _hook({"remote_compile": True, "byte_identical": True,
                           "cache_observed": True, "selected_profile": "ZSTD_TU",
                           "selected_tu_count": 1, **counters}),
    }


class RollbackGateTest(unittest.TestCase):
    def test_local_transition_flips_bind_path_and_verifies_both_profiles(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            current, previous = root / "current", root / "previous"
            _make_root(current, cache=True)
            _make_root(previous, cache=False)
            bind = root / "runtime"
            bind.symlink_to(current, target_is_directory=True)
            evidence = make_evidence(current, previous, bind, _hooks(), 10.0)
            self.assertEqual(evidence["schema"], "icecream-s5-rollback-gate-v1")
            self.assertEqual(evidence["phases"], list(PHASES))
            self.assertTrue(bind.is_symlink())
            self.assertEqual(bind.resolve(), current.resolve())
            self.assertEqual(verify_evidence(evidence)["status"], "PASS")

    def test_corrupt_previous_artifact_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            current, previous = root / "current", root / "previous"
            _make_root(current, cache=True)
            _make_root(previous, cache=False)
            bind = root / "runtime"
            bind.symlink_to(current, target_is_directory=True)
            evidence = make_evidence(current, previous, bind, _hooks(), 10.0)
            (previous / "scheduler/icecc-scheduler").write_bytes(b"corrupt\n")
            with self.assertRaisesRegex(RollbackError, "previous:S:hash-mismatch"):
                verify_evidence(evidence)

    def test_skipped_cleanup_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            current, previous = root / "current", root / "previous"
            _make_root(current, cache=True)
            _make_root(previous, cache=False)
            bind = root / "runtime"
            bind.symlink_to(current, target_is_directory=True)
            evidence = make_evidence(current, previous, bind, _hooks(), 10.0)
            mutant = copy.deepcopy(evidence)
            mutant["cleanup"]["skipped"] = True
            with self.assertRaisesRegex(RollbackError, "cleanup was skipped"):
                verify_evidence(mutant)

    def test_legacy_cache_use_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            current, previous = root / "current", root / "previous"
            _make_root(current, cache=True)
            _make_root(previous, cache=False)
            bind = root / "runtime"
            bind.symlink_to(current, target_is_directory=True)
            evidence = make_evidence(current, previous, bind, _hooks(), 10.0)
            mutant = copy.deepcopy(evidence)
            mutant["legacy"]["profile_use_count"] = 1
            with self.assertRaisesRegex(RollbackError, "legacy compile used"):
                verify_evidence(mutant)

    def test_manifest_requires_current_cache_service(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _make_root(root, cache=False)
            with self.assertRaisesRegex(RollbackError, "X:missing-or-symlink"):
                artifact_manifest(root, require_cache=True)


if __name__ == "__main__":
    unittest.main()
