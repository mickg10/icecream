#!/usr/bin/env python3
"""Validate complete cluster evidence and select strict versus pipelined mode.

The validator trusts neither a producer-supplied verdict nor summary prose. It
reconstructs the required case cross product from the validated run plan,
requires complete correctness evidence for every case, validates aggregate
sample counts, and applies an explicit acceptance policy.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import validate_cluster_run_plan as plan_validator


class ResultError(ValueError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ResultError(message)


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def number(value: Any, context: str, *, positive: bool = False) -> float:
    require(
        isinstance(value, (int, float)) and not isinstance(value, bool),
        f"{context}: number required",
    )
    result = float(value)
    require(math.isfinite(result), f"{context}: finite value required")
    require(result >= 0.0, f"{context}: nonnegative value required")
    if positive:
        require(result > 0.0, f"{context}: positive value required")
    return result


def integer(value: Any, context: str, *, minimum: int = 0) -> int:
    require(
        isinstance(value, int) and not isinstance(value, bool),
        f"{context}: integer required",
    )
    require(value >= minimum, f"{context}: must be at least {minimum}")
    return value


@dataclass(frozen=True)
class Thresholds:
    minimum_throughput_ratio: float
    maximum_usecs_p99_ratio: float
    maximum_begin_p99_ratio: float
    maximum_cpu_ratio: float
    maximum_rss_ratio: float
    maximum_management_p99_ms: float


def thresholds(value: Any, context: str) -> Thresholds:
    require(isinstance(value, dict), f"{context}: object required")
    required = {
        "minimum_throughput_ratio",
        "maximum_usecs_p99_ratio",
        "maximum_begin_p99_ratio",
        "maximum_cpu_ratio",
        "maximum_rss_ratio",
        "maximum_management_p99_ms",
    }
    require(set(value) == required, f"{context}: exact threshold keys required")
    minimum_throughput_ratio = number(
        value["minimum_throughput_ratio"],
        f"{context}.minimum_throughput_ratio",
        positive=True,
    )
    require(
        minimum_throughput_ratio <= 1.0,
        f"{context}.minimum_throughput_ratio: cannot exceed 1",
    )
    ratio_values = {}
    for key in (
        "maximum_usecs_p99_ratio",
        "maximum_begin_p99_ratio",
        "maximum_cpu_ratio",
        "maximum_rss_ratio",
    ):
        ratio_values[key] = number(value[key], f"{context}.{key}", positive=True)
        require(ratio_values[key] >= 1.0, f"{context}.{key}: must be at least 1")
    management = number(
        value["maximum_management_p99_ms"],
        f"{context}.maximum_management_p99_ms",
        positive=True,
    )
    return Thresholds(
        minimum_throughput_ratio=minimum_throughput_ratio,
        maximum_usecs_p99_ratio=ratio_values["maximum_usecs_p99_ratio"],
        maximum_begin_p99_ratio=ratio_values["maximum_begin_p99_ratio"],
        maximum_cpu_ratio=ratio_values["maximum_cpu_ratio"],
        maximum_rss_ratio=ratio_values["maximum_rss_ratio"],
        maximum_management_p99_ms=management,
    )


def validate_policy(document: Any) -> dict[str, Any]:
    require(isinstance(document, dict), "policy: object required")
    require(document.get("schema") == 1, "policy.schema: expected 1")
    repetitions = integer(
        document.get("repetitions_per_case"),
        "policy.repetitions_per_case",
        minimum=3,
    )
    warmup = integer(
        document.get("warmup_seconds"),
        "policy.warmup_seconds",
        minimum=60,
    )
    measurement = integer(
        document.get("measurement_seconds"),
        "policy.measurement_seconds",
        minimum=300,
    )
    strict = thresholds(document.get("strict"), "policy.strict")
    pipelined = thresholds(document.get("pipelined"), "policy.pipelined")
    require(
        document.get("allow_pipelined_only_when_strict_misses_performance") is True,
        "policy must require strict-first mode selection",
    )
    require(
        document.get("require_zero_correctness_failures") is True,
        "policy must require zero correctness failures",
    )
    return {
        "repetitions": repetitions,
        "warmup": warmup,
        "measurement": measurement,
        "strict": strict,
        "pipelined": pipelined,
    }


def expected_cases(plan: dict[str, Any], repetitions: int) -> set[tuple[str, str, int, str, str]]:
    result: set[tuple[str, str, int, str, str]] = set()
    for tier in plan["tiers"]:
        for mode in tier["modes"]:
            for repetition in range(1, repetitions + 1):
                for topology in tier["topologies"]:
                    for scenario in tier["scenarios"]:
                        result.add((tier["id"], mode, repetition, topology, scenario))
    return result


def validate_case(case: Any, index: int) -> tuple[str, str, int, str, str]:
    context = f"cases[{index}]"
    require(isinstance(case, dict), f"{context}: object required")
    required = {
        "tier",
        "mode",
        "repetition",
        "topology",
        "scenario",
        "status",
        "barriers_complete",
        "management_complete",
        "process_exits_expected",
        "correctness",
    }
    require(set(case) == required, f"{context}: exact keys required")
    for key in ("tier", "mode", "topology", "scenario"):
        require(
            isinstance(case[key], str) and case[key],
            f"{context}.{key}: nonempty string required",
        )
    repetition = integer(case["repetition"], f"{context}.repetition", minimum=1)
    require(case["status"] == "PASS", f"{context}: status must be PASS")
    require(case["barriers_complete"] is True, f"{context}: barriers incomplete")
    require(case["management_complete"] is True, f"{context}: management replies incomplete")
    require(case["process_exits_expected"] is True, f"{context}: process exit mismatch")

    correctness = case["correctness"]
    require(isinstance(correctness, dict), f"{context}.correctness: object required")
    required_counters = {
        "terminal_uniqueness_failures",
        "identity_uniqueness_failures",
        "accounting_conservation_failures",
        "unauthorized_start_failures",
        "management_reply_failures",
        "unexpected_process_exits",
    }
    require(
        set(correctness) == required_counters,
        f"{context}.correctness: exact counters required",
    )
    for name, value in correctness.items():
        require(
            integer(value, f"{context}.correctness.{name}") == 0,
            f"{context}.correctness.{name}: must be zero",
        )
    return (
        case["tier"],
        case["mode"],
        repetition,
        case["topology"],
        case["scenario"],
    )


AGGREGATE_KEYS = {
    "tier",
    "mode",
    "repetitions",
    "assignments_per_second",
    "completions_per_second",
    "request_to_usecs_p99_ms",
    "request_to_begin_p99_ms",
    "scheduler_cpu_percent",
    "scheduler_rss_bytes",
    "scheduler_fd_max",
    "management_p99_ms",
    "queue_depth_max",
    "pending_claim_high_water",
    "tombstone_count_max",
    "wire_bytes_per_assignment",
}


def validate_aggregate(
    aggregate: Any,
    index: int,
    repetitions: int,
) -> tuple[tuple[str, str], dict[str, float]]:
    context = f"aggregates[{index}]"
    require(isinstance(aggregate, dict), f"{context}: object required")
    require(set(aggregate) == AGGREGATE_KEYS, f"{context}: exact keys required")
    tier = aggregate["tier"]
    mode = aggregate["mode"]
    require(isinstance(tier, str) and tier, f"{context}.tier: string required")
    require(isinstance(mode, str) and mode, f"{context}.mode: string required")
    require(
        integer(aggregate["repetitions"], f"{context}.repetitions") == repetitions,
        f"{context}.repetitions: policy count mismatch",
    )

    positive_metrics = {
        "assignments_per_second",
        "completions_per_second",
        "scheduler_rss_bytes",
    }
    values: dict[str, float] = {}
    for key in AGGREGATE_KEYS - {"tier", "mode", "repetitions"}:
        values[key] = number(
            aggregate[key],
            f"{context}.{key}",
            positive=key in positive_metrics,
        )
    return (tier, mode), values


def compare_mode(
    baseline: dict[str, float],
    candidate: dict[str, float],
    limits: Thresholds,
) -> list[str]:
    failures: list[str] = []

    def ratio(numerator: str, denominator: str) -> float:
        require(
            baseline[denominator] > 0.0,
            f"baseline {denominator} must be positive",
        )
        return candidate[numerator] / baseline[denominator]

    throughput_ratio = ratio("assignments_per_second", "assignments_per_second")
    if throughput_ratio < limits.minimum_throughput_ratio:
        failures.append(
            f"assignment throughput ratio {throughput_ratio:.6f} < "
            f"{limits.minimum_throughput_ratio:.6f}"
        )
    completion_ratio = ratio("completions_per_second", "completions_per_second")
    if completion_ratio < limits.minimum_throughput_ratio:
        failures.append(
            f"completion throughput ratio {completion_ratio:.6f} < "
            f"{limits.minimum_throughput_ratio:.6f}"
        )

    comparisons = (
        ("request_to_usecs_p99_ms", limits.maximum_usecs_p99_ratio),
        ("request_to_begin_p99_ms", limits.maximum_begin_p99_ratio),
        ("scheduler_cpu_percent", limits.maximum_cpu_ratio),
        ("scheduler_rss_bytes", limits.maximum_rss_ratio),
    )
    for metric, maximum in comparisons:
        if baseline[metric] == 0.0:
            require(candidate[metric] == 0.0, f"{metric}: nonzero candidate over zero baseline")
            continue
        observed = candidate[metric] / baseline[metric]
        if observed > maximum:
            failures.append(f"{metric} ratio {observed:.6f} > {maximum:.6f}")

    if candidate["management_p99_ms"] > limits.maximum_management_p99_ms:
        failures.append(
            f"management_p99_ms {candidate['management_p99_ms']:.6f} > "
            f"{limits.maximum_management_p99_ms:.6f}"
        )
    return failures


def validate_results(
    results: Any,
    plan: dict[str, Any],
    policy: dict[str, Any],
    plan_path: Path,
    policy_path: Path,
) -> dict[str, Any]:
    require(isinstance(results, dict), "results: object required")
    require(results.get("schema") == 1, "results.schema: expected 1")
    require(results.get("plan_sha256") == digest(plan_path), "results.plan_sha256 mismatch")
    require(results.get("policy_sha256") == digest(policy_path), "results.policy_sha256 mismatch")
    require(results.get("revision") == plan["revision"], "results.revision mismatch")
    require(results.get("binaries") == plan["binaries"], "results.binaries mismatch")

    cases = results.get("cases")
    require(isinstance(cases, list), "results.cases: array required")
    actual_cases: set[tuple[str, str, int, str, str]] = set()
    for index, case in enumerate(cases):
        key = validate_case(case, index)
        require(key not in actual_cases, f"duplicate case {key}")
        actual_cases.add(key)
    expected = expected_cases(plan, policy["repetitions"])
    require(
        actual_cases == expected,
        f"case matrix mismatch; missing={len(expected-actual_cases)}, "
        f"extra={len(actual_cases-expected)}",
    )

    aggregates = results.get("aggregates")
    require(isinstance(aggregates, list), "results.aggregates: array required")
    aggregate_map: dict[tuple[str, str], dict[str, float]] = {}
    for index, aggregate in enumerate(aggregates):
        key, values = validate_aggregate(
            aggregate, index, policy["repetitions"]
        )
        require(key not in aggregate_map, f"duplicate aggregate {key}")
        aggregate_map[key] = values

    expected_aggregates = {
        (tier["id"], mode)
        for tier in plan["tiers"]
        for mode in tier["modes"]
    }
    require(
        set(aggregate_map) == expected_aggregates,
        f"aggregate matrix mismatch; missing={sorted(expected_aggregates-set(aggregate_map))}, "
        f"extra={sorted(set(aggregate_map)-expected_aggregates)}",
    )

    strict_failures: dict[str, list[str]] = {}
    pipelined_failures: dict[str, list[str]] = {}
    for tier in plan["tiers"]:
        tier_id = tier["id"]
        baseline = aggregate_map[(tier_id, "baseline")]
        strict = aggregate_map[(tier_id, "strict")]
        strict_failures[tier_id] = compare_mode(
            baseline, strict, policy["strict"]
        )
        if "pipelined" in tier["modes"]:
            pipelined_failures[tier_id] = compare_mode(
                baseline,
                aggregate_map[(tier_id, "pipelined")],
                policy["pipelined"],
            )

    strict_missed = any(strict_failures.values())
    contingency = plan["enable_pipelined_contingency"]
    if strict_missed:
        require(
            contingency,
            "strict mode misses performance policy but pipelined contingency is disabled",
        )
        require(
            set(pipelined_failures) == {tier["id"] for tier in plan["tiers"]},
            "pipelined contingency must cover every tier",
        )
        require(
            not any(pipelined_failures.values()),
            f"pipelined mode also misses performance policy: {pipelined_failures}",
        )
        selected_mode = "pipelined"
    else:
        require(
            not contingency,
            "strict mode meets performance policy; pipelined contingency is unnecessary",
        )
        selected_mode = "strict"

    producer_verdict = results.get("verdict")
    require(producer_verdict == "GREEN", "producer verdict must be GREEN")
    require(
        results.get("selected_mode") == selected_mode,
        "producer selected_mode disagrees with reconstructed decision",
    )
    return {
        "schema": 1,
        "verdict": "GREEN",
        "selected_mode": selected_mode,
        "case_count": len(actual_cases),
        "aggregate_count": len(aggregate_map),
        "strict_failures": strict_failures,
        "pipelined_failures": pipelined_failures,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--plan", required=True, type=Path)
    parser.add_argument("--policy", required=True, type=Path)
    parser.add_argument("--results", required=True, type=Path)
    parser.add_argument("--repo", default=".", type=Path)
    args = parser.parse_args()

    try:
        plan = json.loads(args.plan.read_text(encoding="utf-8"))
        plan_validator.validate(plan, args.repo.resolve())
        policy_document = json.loads(args.policy.read_text(encoding="utf-8"))
        policy = validate_policy(policy_document)
        results = json.loads(args.results.read_text(encoding="utf-8"))
        report = validate_results(
            results,
            plan,
            policy,
            args.plan,
            args.policy,
        )
    except (
        ResultError,
        plan_validator.PlanError,
        OSError,
        json.JSONDecodeError,
    ) as error:
        print(f"cluster results rejected: {error}")
        return 1
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
