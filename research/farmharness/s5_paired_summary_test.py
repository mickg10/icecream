#!/usr/bin/env python3
"""Focused tests for the current-run S5 paired summary adapter."""

from __future__ import annotations

import hashlib
import json
import tempfile
import unittest
from pathlib import Path

from research.farmharness.s5_paired_summary import (
    EvidenceError,
    RUN_SCHEMA,
    summarize_experiments,
)


ROLE_HASHES = {role: hashlib.sha256(role.encode()).hexdigest()
               for role in ("S", "C", "F", "E", "X")}
ROOT_COMMIT = "1" * 40


def _bytes(value):
    return (json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n").encode()


def _write(path: Path, value) -> str:
    data = _bytes(value)
    path.write_bytes(data)
    return hashlib.sha256(data).hexdigest()


def _workload():
    return {
        "schema": "icecream-retained-fmt-workload-v1",
        "source_root": "/retained/fmt",
        "manifest": "/retained/fmt/compile_commands.json",
        "manifest_sha256": "2" * 64,
        "tus": [
            {
                "tu_id": f"fmt-{index:04d}-unit", "source": f"src/unit-{index}.cc",
                "source_sha256": hashlib.sha256(f"source-{index}".encode()).hexdigest(),
                "compiler": "g++", "flags": ["-O2"],
            }
            for index in range(31)
        ],
    }


def _row(root: Path, regime: str, order: str, mode: str, seconds: float,
         workload, identity_number: int):
    block_id = f"{regime}-01"
    start = 1_000_000_000
    end = start + int(seconds * 1e9)
    ledger = []
    for index, tu in enumerate(workload["tus"]):
        digest = hashlib.sha256(f"{root.name}:{mode}:{index}".encode()).hexdigest()
        ledger.append({
            "tu_id": tu["tu_id"], "source": tu["source"],
            "remote_sha256": digest, "reference_sha256": digest,
            "remote_bytes": 100 + index, "reference_bytes": 100 + index,
            "byte_identical": True,
        })
    cache = mode == "cache"
    warm = regime == "warm"
    prewarm = warm_identity = None
    if warm and cache:
        c_guid = f"{identity_number:032x}"
        prewarm = {
            "mode": "cache", "state_digest": "3" * 64,
            "trace": "/tmp/lifecycle.trace",
            "snapshot_identity": "/tmp/lifecycle.trace:" + "3" * 64,
            "c_guid": c_guid, "f_store_generation": "1",
            "scheduler_epoch": str(1000 + identity_number), "tu_seq_count": "31",
            "tu_seq_digest": "4" * 64,
        }
        warm_identity = {"c_guid": c_guid, "f_store_generation": "1",
                         "scheduler_epoch": str(1000 + identity_number)}
    elif warm:
        prewarm = {"mode": "legacy", "manifest_count": "31", "output_count": "31"}
    stage = f"/tmp/s5-p50-build.{root.name}"
    row = {
        "kind": "build", "schema": RUN_SCHEMA,
        "build_id": f"{block_id}-{mode}-full", "block_id": block_id,
        "regime": regime, "order": order, "mode": mode, "tu_count": 31,
        "tu_id": None, "source": None, "source_sha256": None,
        "measurement_boundary": "measured-full-build",
        "start_ns": start, "end_ns": end, "wall_seconds": (end - start) / 1e9,
        "aggregate_wall_seconds": (end - start) / 1e9,
        "cache_expected": cache, "cache_observed": cache,
        "legacy_observed": not cache, "remote_compile": True,
        "selected_profile": "ZSTD_TU" if cache else None,
        "selected_tu_count": (62 if warm else 31) if cache else 0,
        "selected_route_count": 0, "byte_identical": True, "tu_ledger": ledger,
        "prewarm": prewarm, "warm_identity": warm_identity,
        "cleanup": "bounded forced=0 seconds=17", "status": "PASS",
        "reason": "remote-byte-identical",
        "namespace": {"namespace_id": f"s5/{regime}/{mode}/{block_id}",
                      "mode_private": True,
                      "reset": "same-lifecycle-prewarm" if warm else "fresh-per-build"},
        "returncode": 0,
        "command": ["ssh", "host", "bash", "-s", "--", stage, stage, stage,
                    "s50-c50-f50", "1" if cache else "0", "-", "c1f1",
                    "1" if warm else "0"],
        "evidence": str(root / "builds" / f"{block_id}-{mode}-full"),
        "runner_elapsed_ns": 20_000_000_000,
    }
    return row


def make_run(parent: Path, name: str, regime: str, order: str,
             cache_seconds: float = 102.0, legacy_seconds: float = 100.0,
             identity_number: int = 1) -> Path:
    root = parent / name
    root.mkdir()
    workload = _workload()
    workload_digest = _write(root / "workload_manifest.json", workload)
    role_manifest = {
        "root": "/p50", "source_sha256": ROOT_COMMIT,
        "roles": {
            role: {"role": role, "path": f"/p50/{role}", "exists": True,
                   "symlink": False, "bytes": 10, "sha256": digest,
                   "executable": True}
            for role, digest in ROLE_HASHES.items()
        },
    }
    experiment = {
        "schema": RUN_SCHEMA, "immutable": True, "root_commit": ROOT_COMMIT,
        "source_sha256": ROOT_COMMIT,
        "source_identity": {"status": "ok", "commit": "5" * 40,
                            "root_commit": ROOT_COMMIT,
                            "product_tree_matches_root": "true"},
        "role_manifest": role_manifest, "p50_binary_hashes": ROLE_HASHES,
        "workload_manifest_digest": workload_digest,
        "modes": ["cache", "legacy"], "regimes": ["cold", "warm"],
        "bootstrap_unit": "whole_block",
        "block_plan": [{"block_id": f"{regime}-01", "regime": regime,
                        "order": order, "sequence": 0}],
        "cold_reset": "fresh", "warm_reset": "mode-private prewarm",
        "measurement_boundary": "full remote build",
        "smoke_scope": {"blocks": 1 if regime == "cold" else 0,
                        "tu_count": 31, "statistic_claim": False},
    }
    _write(root / "experiment_manifest.json", experiment)
    _write(root / "preflight.json", {"status": "READY",
                                      "reason": "target-uncontaminated",
                                      "target": {"status": "READY"}})
    by_mode = {
        "cache": _row(root, regime, order, "cache", cache_seconds,
                      workload, identity_number),
        "legacy": _row(root, regime, order, "legacy", legacy_seconds,
                       workload, identity_number),
    }
    modes = ("cache", "legacy") if order == "AB" else ("legacy", "cache")
    rows = [by_mode[mode] for mode in modes]
    for row in rows:
        evidence = Path(row["evidence"])
        evidence.mkdir(parents=True)
        _write(evidence / "record.json", row)
    (root / "results.jsonl").write_bytes(b"".join(_bytes(row) for row in rows))
    return root


class PairedSummaryTest(unittest.TestCase):
    def test_two_warm_blocks_are_valid_but_incomplete(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            runs = [make_run(root, "warm-ab", "warm", "AB", identity_number=1),
                    make_run(root, "warm-ba", "warm", "BA", identity_number=2)]
            rows = summarize_experiments(runs, bootstrap_repetitions=200)
        summary = rows[-1]
        self.assertEqual(summary["status"], "INCOMPLETE")
        self.assertEqual(summary["decision"], "INCONCLUSIVE")
        self.assertEqual(summary["counts"], {"cold": 0, "warm": 2})
        self.assertIsNone(summary["statistics"][1]["whole_block_bootstrap"]
                          ["one_sided_upper_95"])
        self.assertEqual(len(set(summary["global_block_ids"])), 2)

    def test_complete_four_by_four_matrix_is_green(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            runs = []
            for regime_index, regime in enumerate(("cold", "warm")):
                for index, order in enumerate(("AB", "BA", "AB", "BA")):
                    runs.append(make_run(root, f"{regime}-{index}", regime, order,
                                         cache_seconds=102.0 + index / 10,
                                         legacy_seconds=100.0 + index / 10,
                                         identity_number=100 * regime_index + index + 1))
            rows = summarize_experiments(runs, bootstrap_repetitions=500)
        summary = rows[-1]
        self.assertEqual(summary["status"], "COMPLETE")
        self.assertEqual(summary["decision"], "GREEN")
        self.assertEqual(summary["counts"], {"cold": 4, "warm": 4})
        self.assertTrue(all(statistic["whole_block_bootstrap"]["one_sided_upper_95"] < 1.10
                            for statistic in summary["statistics"]))

    def test_ledger_omission_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            run = make_run(Path(directory), "bad-ledger", "cold", "AB")
            rows = [json.loads(line) for line in
                    (run / "results.jsonl").read_text().splitlines()]
            rows[0]["tu_ledger"].pop()
            _write(Path(rows[0]["evidence"]) / "record.json", rows[0])
            (run / "results.jsonl").write_bytes(b"".join(_bytes(row) for row in rows))
            with self.assertRaisesRegex(EvidenceError, "incomplete TU ledger"):
                summarize_experiments([run], bootstrap_repetitions=200)

    def test_cross_experiment_workload_mismatch_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            first = make_run(root, "first", "cold", "AB")
            second = make_run(root, "second", "cold", "BA")
            workload = json.loads((second / "workload_manifest.json").read_text())
            workload["source_root"] = "/different/fmt"
            digest = _write(second / "workload_manifest.json", workload)
            experiment = json.loads((second / "experiment_manifest.json").read_text())
            experiment["workload_manifest_digest"] = digest
            _write(second / "experiment_manifest.json", experiment)
            with self.assertRaisesRegex(EvidenceError, "cross-experiment workload_identity"):
                summarize_experiments([first, second], bootstrap_repetitions=200)

    def test_non_alternating_orders_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            runs = [make_run(root, "one", "cold", "AB"),
                    make_run(root, "two", "cold", "AB")]
            with self.assertRaisesRegex(EvidenceError, "not ordered AB/BA"):
                summarize_experiments(runs, bootstrap_repetitions=200)


if __name__ == "__main__":
    unittest.main()
