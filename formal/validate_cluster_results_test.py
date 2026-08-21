#!/usr/bin/env python3
"""Mutation tests for validate_cluster_results.py."""

from __future__ import annotations

import copy
import json
import tempfile
import unittest
from pathlib import Path

import validate_cluster_results as results_validator
from validate_cluster_run_plan_test import valid_plan

HERE = Path(__file__).resolve().parent


def valid_policy() -> dict:
    limits = {
        "minimum_throughput_ratio": 0.95,
        "maximum_usecs_p99_ratio": 1.10,
        "maximum_begin_p99_ratio": 1.10,
        "maximum_cpu_ratio": 1.10,
        "maximum_rss_ratio": 1.10,
        "maximum_management_p99_ms": 100.0,
    }
    return {
        "schema": 1,
        "repetitions_per_case": 3,
        "warmup_seconds": 60,
        "measurement_seconds": 300,
        "strict": copy.deepcopy(limits),
        "pipelined": copy.deepcopy(limits),
        "allow_pipelined_only_when_strict_misses_performance": True,
        "require_zero_correctness_failures": True,
    }


def aggregate(tier: str, mode: str, repetitions: int) -> dict:
    factor = 1.0
    if mode == "strict":
        factor = 0.98
    elif mode == "pipelined":
        factor = 0.99
    return {
        "tier": tier,
        "mode": mode,
        "repetitions": repetitions,
        "assignments_per_second": 1000.0 * factor,
        "completions_per_second": 990.0 * factor,
        "request_to_usecs_p99_ms": 10.0 * (1.02 if mode != "baseline" else 1.0),
        "request_to_begin_p99_ms": 20.0 * (1.02 if mode != "baseline" else 1.0),
        "scheduler_cpu_percent": 50.0 * (1.02 if mode != "baseline" else 1.0),
        "scheduler_rss_bytes": 1_000_000_000.0 * (1.02 if mode != "baseline" else 1.0),
        "scheduler_fd_max": 500.0,
        "management_p99_ms": 50.0,
        "queue_depth_max": 100.0,
        "pending_claim_high_water": 0.0 if mode != "pipelined" else 2.0,
        "tombstone_count_max": 100.0,
        "wire_bytes_per_assignment": 128.0 if mode == "baseline" else 160.0,
    }


def build_results(
    plan: dict,
    policy: dict,
    plan_path: Path,
    policy_path: Path,
    *,
    selected_mode: str = "strict",
) -> dict:
    repetitions = policy["repetitions_per_case"]
    cases = []
    zero_correctness = {
        "terminal_uniqueness_failures": 0,
        "identity_uniqueness_failures": 0,
        "accounting_conservation_failures": 0,
        "unauthorized_start_failures": 0,
        "management_reply_failures": 0,
        "unexpected_process_exits": 0,
    }
    for tier in plan["tiers"]:
        for mode in tier["modes"]:
            for repetition in range(1, repetitions + 1):
                for topology in tier["topologies"]:
                    for scenario in tier["scenarios"]:
                        cases.append(
                            {
                                "tier": tier["id"],
                                "mode": mode,
                                "repetition": repetition,
                                "topology": topology,
                                "scenario": scenario,
                                "status": "PASS",
                                "barriers_complete": True,
                                "management_complete": True,
                                "process_exits_expected": True,
                                "correctness": copy.deepcopy(zero_correctness),
                            }
                        )
    aggregates = [
        aggregate(tier["id"], mode, repetitions)
        for tier in plan["tiers"]
        for mode in tier["modes"]
    ]
    return {
        "schema": 1,
        "plan_sha256": results_validator.digest(plan_path),
        "policy_sha256": results_validator.digest(policy_path),
        "revision": plan["revision"],
        "binaries": copy.deepcopy(plan["binaries"]),
        "cases": cases,
        "aggregates": aggregates,
        "verdict": "GREEN",
        "selected_mode": selected_mode,
    }


class ClusterResultsTests(unittest.TestCase):
    def make_bundle(
        self,
        temporary: str,
        *,
        contingency: bool = False,
    ) -> tuple[dict, dict, dict, Path, Path]:
        plan = valid_plan()
        if contingency:
            plan["enable_pipelined_contingency"] = True
            plan["pipelined_contingency_reason"] = (
                "Strict mode missed an explicitly accepted performance budget."
            )
            for tier in plan["tiers"]:
                tier["modes"].append("pipelined")
        policy = valid_policy()
        root = Path(temporary)
        plan_path = root / "plan.json"
        policy_path = root / "policy.json"
        plan_path.write_text(json.dumps(plan, indent=2) + "\n", encoding="utf-8")
        policy_path.write_text(json.dumps(policy, indent=2) + "\n", encoding="utf-8")
        results = build_results(
            plan,
            policy,
            plan_path,
            policy_path,
            selected_mode="pipelined" if contingency else "strict",
        )
        return plan, policy, results, plan_path, policy_path

    def validate(
        self,
        results: dict,
        plan: dict,
        policy: dict,
        plan_path: Path,
        policy_path: Path,
    ) -> dict:
        validated_policy = results_validator.validate_policy(policy)
        return results_validator.validate_results(
            results,
            plan,
            validated_policy,
            plan_path,
            policy_path,
        )

    def assert_rejected(
        self,
        results: dict,
        plan: dict,
        policy: dict,
        plan_path: Path,
        policy_path: Path,
        fragment: str,
    ) -> None:
        with self.assertRaises(results_validator.ResultError) as raised:
            self.validate(results, plan, policy, plan_path, policy_path)
        self.assertIn(fragment.lower(), str(raised.exception).lower())

    def test_complete_strict_result_selects_strict(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            plan, policy, results, plan_path, policy_path = self.make_bundle(temporary)
            report = self.validate(results, plan, policy, plan_path, policy_path)
        self.assertEqual(report["verdict"], "GREEN")
        self.assertEqual(report["selected_mode"], "strict")
        self.assertEqual(report["case_count"], 5 * 2 * 3 * 6 * 16)
        self.assertEqual(report["aggregate_count"], 10)

    def test_missing_case_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            plan, policy, results, plan_path, policy_path = self.make_bundle(temporary)
            results["cases"].pop()
            self.assert_rejected(
                results, plan, policy, plan_path, policy_path, "case matrix mismatch"
            )

    def test_duplicate_case_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            plan, policy, results, plan_path, policy_path = self.make_bundle(temporary)
            results["cases"].append(copy.deepcopy(results["cases"][0]))
            self.assert_rejected(
                results, plan, policy, plan_path, policy_path, "duplicate case"
            )

    def test_nonzero_correctness_counter_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            plan, policy, results, plan_path, policy_path = self.make_bundle(temporary)
            results["cases"][0]["correctness"]["unauthorized_start_failures"] = 1
            self.assert_rejected(
                results, plan, policy, plan_path, policy_path, "must be zero"
            )

    def test_incomplete_management_reply_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            plan, policy, results, plan_path, policy_path = self.make_bundle(temporary)
            results["cases"][0]["management_complete"] = False
            self.assert_rejected(
                results, plan, policy, plan_path, policy_path, "management replies incomplete"
            )

    def test_unexpected_process_exit_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            plan, policy, results, plan_path, policy_path = self.make_bundle(temporary)
            results["cases"][0]["process_exits_expected"] = False
            self.assert_rejected(
                results, plan, policy, plan_path, policy_path, "process exit mismatch"
            )

    def test_missing_aggregate_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            plan, policy, results, plan_path, policy_path = self.make_bundle(temporary)
            results["aggregates"].pop()
            self.assert_rejected(
                results, plan, policy, plan_path, policy_path, "aggregate matrix mismatch"
            )

    def test_plan_hash_mismatch_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            plan, policy, results, plan_path, policy_path = self.make_bundle(temporary)
            results["plan_sha256"] = "f" * 64
            self.assert_rejected(
                results, plan, policy, plan_path, policy_path, "plan_sha256 mismatch"
            )

    def test_strict_regression_without_contingency_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            plan, policy, results, plan_path, policy_path = self.make_bundle(temporary)
            for row in results["aggregates"]:
                if row["mode"] == "strict":
                    row["assignments_per_second"] = 800.0
            self.assert_rejected(
                results,
                plan,
                policy,
                plan_path,
                policy_path,
                "pipelined contingency is disabled",
            )

    def test_strict_miss_with_green_pipelined_selects_pipelined(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            plan, policy, results, plan_path, policy_path = self.make_bundle(
                temporary, contingency=True
            )
            for row in results["aggregates"]:
                if row["mode"] == "strict":
                    row["assignments_per_second"] = 800.0
            report = self.validate(results, plan, policy, plan_path, policy_path)
        self.assertEqual(report["selected_mode"], "pipelined")
        self.assertEqual(report["case_count"], 5 * 3 * 3 * 6 * 16)

    def test_pipelined_is_rejected_when_strict_meets_budget(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            plan, policy, results, plan_path, policy_path = self.make_bundle(
                temporary, contingency=True
            )
            self.assert_rejected(
                results,
                plan,
                policy,
                plan_path,
                policy_path,
                "pipelined contingency is unnecessary",
            )

    def test_pipelined_miss_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            plan, policy, results, plan_path, policy_path = self.make_bundle(
                temporary, contingency=True
            )
            for row in results["aggregates"]:
                if row["mode"] == "strict":
                    row["assignments_per_second"] = 800.0
                if row["mode"] == "pipelined":
                    row["management_p99_ms"] = 1000.0
            self.assert_rejected(
                results,
                plan,
                policy,
                plan_path,
                policy_path,
                "pipelined mode also misses",
            )

    def test_producer_selected_mode_mismatch_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            plan, policy, results, plan_path, policy_path = self.make_bundle(temporary)
            results["selected_mode"] = "pipelined"
            self.assert_rejected(
                results,
                plan,
                policy,
                plan_path,
                policy_path,
                "selected_mode disagrees",
            )

    def test_policy_template_cannot_masquerade_as_policy(self) -> None:
        template = json.loads(
            (HERE / "cluster-acceptance-policy.template.json").read_text(
                encoding="utf-8"
            )
        )
        with self.assertRaises(results_validator.ResultError):
            results_validator.validate_policy(template)


if __name__ == "__main__":
    unittest.main()
