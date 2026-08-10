#!/usr/bin/env python3
"""Unit tests for the fail-closed cluster run-plan validator."""

from __future__ import annotations

import copy
import json
import tempfile
import unittest
from pathlib import Path

import validate_cluster_run_plan as validator

HERE = Path(__file__).resolve().parent


def valid_plan() -> dict:
    topology = sorted(validator.TOPOLOGIES)
    scenarios = sorted(validator.SCENARIOS)
    metrics = sorted(validator.METRICS)
    minima = validator.TIER_MINIMA
    tiers = []
    for tier_id in ("smoke", "small", "medium", "high", "saturation"):
        f_count, c_count = minima[tier_id]
        tiers.append(
            {
                "id": tier_id,
                "fulfillment_count": f_count,
                "client_count": c_count,
                "duration_seconds": 120,
                "topologies": topology,
                "scenarios": scenarios,
                "modes": ["baseline", "strict"],
                "metrics": metrics,
            }
        )
    return {
        "schema": 1,
        "revision": "1" * 40,
        "binaries": {
            "old": {
                "scheduler_sha256": "a" * 64,
                "daemon_sha256": "b" * 64,
                "client_sha256": "c" * 64,
            },
            "candidate": {
                "scheduler_sha256": "d" * 64,
                "daemon_sha256": "e" * 64,
                "client_sha256": "f" * 64,
            },
        },
        "hosts": [
            {
                "name": "scheduler",
                "roles": ["scheduler"],
                "versions": ["candidate"],
            },
            {
                "name": "old-f",
                "roles": ["fulfillment"],
                "versions": ["old"],
            },
            {
                "name": "new-f",
                "roles": ["fulfillment"],
                "versions": ["candidate"],
            },
            {
                "name": "old-c",
                "roles": ["client-generator"],
                "versions": ["old"],
            },
            {
                "name": "new-c",
                "roles": ["client-generator"],
                "versions": ["candidate"],
            },
            {
                "name": "observer",
                "roles": ["observer"],
                "versions": [],
            },
        ],
        "enable_pipelined_contingency": False,
        "pipelined_contingency_reason": "",
        "tiers": tiers,
        "evidence": {
            "artifacts_root": "/tmp/icecream-cluster-evidence",
            "require_clean_checkout": True,
            "require_complete_management_replies": True,
        },
    }


class ClusterPlanTests(unittest.TestCase):
    def assert_rejected(self, document: dict) -> None:
        with self.assertRaises(validator.PlanError):
            validator.validate(document, HERE.parent)

    def test_complete_plan_passes(self) -> None:
        validator.validate(valid_plan(), HERE.parent)

    def test_template_is_not_fake_evidence(self) -> None:
        template = json.loads(
            (HERE / "cluster-run-plan.template.json").read_text(encoding="utf-8")
        )
        self.assert_rejected(template)

    def test_missing_topology_is_rejected(self) -> None:
        document = valid_plan()
        document["tiers"][0]["topologies"].pop()
        self.assert_rejected(document)

    def test_missing_scenario_is_rejected(self) -> None:
        document = valid_plan()
        document["tiers"][2]["scenarios"].pop()
        self.assert_rejected(document)

    def test_high_tier_below_minimum_is_rejected(self) -> None:
        document = valid_plan()
        high = next(tier for tier in document["tiers"] if tier["id"] == "high")
        high["fulfillment_count"] = 49
        self.assert_rejected(document)

    def test_all_zero_binary_hash_is_rejected(self) -> None:
        document = valid_plan()
        document["binaries"]["old"]["daemon_sha256"] = "0" * 64
        self.assert_rejected(document)

    def test_duplicate_host_is_rejected(self) -> None:
        document = valid_plan()
        document["hosts"][1]["name"] = document["hosts"][0]["name"]
        self.assert_rejected(document)

    def test_missing_old_client_pool_is_rejected(self) -> None:
        document = valid_plan()
        document["hosts"] = [
            host for host in document["hosts"] if host["name"] != "old-c"
        ]
        self.assert_rejected(document)

    def test_pipelined_without_contingency_decision_is_rejected(self) -> None:
        document = valid_plan()
        document["tiers"][0]["modes"].append("pipelined")
        self.assert_rejected(document)

    def test_contingency_requires_pipelined_rows_and_reason(self) -> None:
        document = valid_plan()
        document["enable_pipelined_contingency"] = True
        self.assert_rejected(document)

        document = valid_plan()
        document["enable_pipelined_contingency"] = True
        document["pipelined_contingency_reason"] = "strict missed the accepted latency budget"
        for tier in document["tiers"]:
            tier["modes"].append("pipelined")
        validator.validate(document, HERE.parent)

    def test_evidence_inside_checkout_is_rejected(self) -> None:
        document = valid_plan()
        document["evidence"]["artifacts_root"] = str(HERE / "artifacts")
        self.assert_rejected(document)

    def test_incomplete_management_replies_are_forbidden(self) -> None:
        document = valid_plan()
        document["evidence"]["require_complete_management_replies"] = False
        self.assert_rejected(document)


if __name__ == "__main__":
    unittest.main()
