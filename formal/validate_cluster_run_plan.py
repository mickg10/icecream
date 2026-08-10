#!/usr/bin/env python3
"""Validate an assignment-fence cluster execution plan fail-closed."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path
from typing import Any

SHA1_RE = re.compile(r"^[0-9a-f]{40}$")
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")

TOPOLOGIES = {
    "S'FC",
    "S'FC'",
    "S'F'C",
    "S'F'C'",
    "S'F[F']C[C']",
    "S'F'[CC']",
}

SCENARIOS = {
    "normal-completion",
    "cancel-before-usecs",
    "claim-before-queued-revoke",
    "revoke-before-delayed-claim",
    "submitter-loss-before-begin",
    "submitter-loss-after-begin-worker-completes",
    "submitter-loss-after-begin-worker-lost",
    "scheduler-restart-delayed-claim",
    "fulfillment-relogin-generation",
    "client-reconnect-generation",
    "duplicate-begin",
    "duplicate-terminal",
    "partial-usecs-all-cuts",
    "compaction-delayed-claim",
    "management-under-saturation",
    "connectivity-retry-storm",
}

METRICS = {
    "assignments_per_second",
    "completions_per_second",
    "request_to_usecs_latency",
    "request_to_begin_latency",
    "scheduler_cpu",
    "scheduler_rss",
    "scheduler_fds",
    "management_latency",
    "queue_depth",
    "pending_claim_high_water",
    "tombstone_count",
    "wire_bytes_per_assignment",
    "terminal_uniqueness_failures",
    "identity_uniqueness_failures",
    "accounting_conservation_failures",
}

TIER_MINIMA = {
    "smoke": (2, 4),
    "small": (10, 100),
    "medium": (25, 1_000),
    "high": (50, 5_000),
    "saturation": (50, 10_000),
}

ROLES = {"scheduler", "fulfillment", "client-generator", "observer"}
VERSIONS = {"old", "candidate"}
MODES = {"baseline", "strict", "pipelined"}


class PlanError(ValueError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise PlanError(message)


def require_keys(value: dict[str, Any], keys: set[str], context: str) -> None:
    missing = sorted(keys - value.keys())
    require(not missing, f"{context}: missing keys {missing}")


def validate_hash(value: Any, pattern: re.Pattern[str], context: str) -> None:
    require(isinstance(value, str), f"{context}: string required")
    require(pattern.fullmatch(value) is not None, f"{context}: invalid digest")
    require(set(value) != {"0"}, f"{context}: all-zero placeholder is forbidden")


def validate_binaries(document: dict[str, Any]) -> None:
    binaries = document.get("binaries")
    require(isinstance(binaries, dict), "binaries: object required")
    for version in VERSIONS:
        entry = binaries.get(version)
        require(isinstance(entry, dict), f"binaries.{version}: object required")
        for role in ("scheduler", "daemon", "client"):
            validate_hash(
                entry.get(f"{role}_sha256"),
                SHA256_RE,
                f"binaries.{version}.{role}_sha256",
            )


def validate_hosts(document: dict[str, Any]) -> None:
    hosts = document.get("hosts")
    require(isinstance(hosts, list) and hosts, "hosts: nonempty array required")
    names: set[str] = set()
    scheduler_hosts = 0
    observer_hosts = 0
    fulfillment_versions: set[str] = set()
    client_versions: set[str] = set()
    for index, host in enumerate(hosts):
        context = f"hosts[{index}]"
        require(isinstance(host, dict), f"{context}: object required")
        require_keys(host, {"name", "roles", "versions"}, context)
        name = host["name"]
        require(isinstance(name, str) and name, f"{context}.name: nonempty string")
        require(name not in names, f"{context}.name: duplicate {name!r}")
        names.add(name)
        roles = host["roles"]
        require(isinstance(roles, list) and roles, f"{context}.roles: nonempty array")
        role_set = set(roles)
        require(role_set <= ROLES, f"{context}.roles: unknown {sorted(role_set - ROLES)}")
        versions = host["versions"]
        require(isinstance(versions, list), f"{context}.versions: array required")
        version_set = set(versions)
        require(version_set <= VERSIONS, f"{context}.versions: unknown {sorted(version_set - VERSIONS)}")
        if "scheduler" in role_set:
            scheduler_hosts += 1
            require("candidate" in version_set, f"{context}: scheduler must run candidate")
        if "observer" in role_set:
            observer_hosts += 1
        if "fulfillment" in role_set:
            fulfillment_versions |= version_set
        if "client-generator" in role_set:
            client_versions |= version_set
    require(scheduler_hosts == 1, f"hosts: exactly one scheduler host required, found {scheduler_hosts}")
    require(observer_hosts >= 1, "hosts: at least one observer host required")
    require(fulfillment_versions == VERSIONS, "hosts: old and candidate fulfillment pools required")
    require(client_versions == VERSIONS, "hosts: old and candidate client generators required")


def validate_tiers(document: dict[str, Any]) -> None:
    tiers = document.get("tiers")
    require(isinstance(tiers, list), "tiers: array required")
    by_id: dict[str, dict[str, Any]] = {}
    for index, tier in enumerate(tiers):
        context = f"tiers[{index}]"
        require(isinstance(tier, dict), f"{context}: object required")
        require_keys(
            tier,
            {
                "id",
                "fulfillment_count",
                "client_count",
                "duration_seconds",
                "topologies",
                "scenarios",
                "modes",
                "metrics",
            },
            context,
        )
        tier_id = tier["id"]
        require(isinstance(tier_id, str), f"{context}.id: string required")
        require(tier_id not in by_id, f"{context}.id: duplicate {tier_id}")
        by_id[tier_id] = tier

        for key in ("fulfillment_count", "client_count", "duration_seconds"):
            require(
                isinstance(tier[key], int) and not isinstance(tier[key], bool),
                f"{context}.{key}: integer required",
            )
            require(tier[key] > 0, f"{context}.{key}: positive required")
        require(tier["duration_seconds"] >= 60, f"{context}: duration below 60 seconds")

        topology_set = set(tier["topologies"])
        require(topology_set == TOPOLOGIES, f"{context}: topology set mismatch")
        scenario_set = set(tier["scenarios"])
        require(scenario_set == SCENARIOS, f"{context}: scenario set mismatch")
        mode_set = set(tier["modes"])
        require(mode_set <= MODES, f"{context}: unknown modes {sorted(mode_set - MODES)}")
        require({"baseline", "strict"} <= mode_set, f"{context}: baseline and strict required")
        metric_set = set(tier["metrics"])
        require(metric_set == METRICS, f"{context}: metric set mismatch")

    require(set(by_id) == set(TIER_MINIMA), "tiers: exact smoke/small/medium/high/saturation set required")
    for tier_id, (minimum_f, minimum_c) in TIER_MINIMA.items():
        tier = by_id[tier_id]
        require(tier["fulfillment_count"] >= minimum_f, f"tiers.{tier_id}: fulfillment count below {minimum_f}")
        require(tier["client_count"] >= minimum_c, f"tiers.{tier_id}: client count below {minimum_c}")

    contingency = document.get("enable_pipelined_contingency")
    require(isinstance(contingency, bool), "enable_pipelined_contingency: boolean required")
    any_pipelined = any("pipelined" in set(tier["modes"]) for tier in tiers)
    require(any_pipelined == contingency, "pipelined mode and contingency decision disagree")
    if contingency:
        reason = document.get("pipelined_contingency_reason")
        require(isinstance(reason, str) and len(reason.strip()) >= 20, "pipelined contingency requires a substantive reason")
    else:
        require(not any_pipelined, "pipelined mode forbidden without contingency decision")


def validate_evidence(document: dict[str, Any], repo: Path) -> None:
    evidence = document.get("evidence")
    require(isinstance(evidence, dict), "evidence: object required")
    require_keys(
        evidence,
        {"artifacts_root", "require_clean_checkout", "require_complete_management_replies"},
        "evidence",
    )
    artifacts_root = evidence["artifacts_root"]
    require(isinstance(artifacts_root, str) and artifacts_root, "evidence.artifacts_root: nonempty string")
    root = Path(artifacts_root).expanduser().resolve()
    try:
        root.relative_to(repo.resolve())
    except ValueError:
        pass
    else:
        raise PlanError("evidence.artifacts_root must be outside the checkout")
    require(evidence["require_clean_checkout"] is True, "clean checkout must be required")
    require(
        evidence["require_complete_management_replies"] is True,
        "complete management replies must be required",
    )


def validate(document: Any, repo: Path) -> None:
    require(isinstance(document, dict), "plan: object required")
    require(document.get("schema") == 1, "plan.schema: expected 1")
    validate_hash(document.get("revision"), SHA1_RE, "revision")
    validate_binaries(document)
    validate_hosts(document)
    validate_tiers(document)
    validate_evidence(document, repo)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("plan", type=Path)
    parser.add_argument("--repo", default=".", type=Path)
    args = parser.parse_args()
    document = json.loads(args.plan.read_text(encoding="utf-8"))
    try:
        validate(document, args.repo.resolve())
    except PlanError as error:
        print(f"cluster run plan rejected: {error}")
        return 1
    print("PASS: cluster run plan is complete and fail-closed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
