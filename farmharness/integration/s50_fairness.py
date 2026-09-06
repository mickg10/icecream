"""Offline, fail-closed scoring for the matched S50 all-legacy control."""

from __future__ import annotations

from collections.abc import Mapping, Sequence
from typing import Any


class S50FairnessError(ValueError):
    """S50 fairness evidence is absent, malformed, or incomparable."""


def _scenario_structure(scenario: Mapping[str, Any]) -> dict[str, Any]:
    instances = {}
    for item in scenario.get("instances", []):
        if not isinstance(item, Mapping):
            raise S50FairnessError("scenario instance is not an object")
        name = item.get("name")
        if not isinstance(name, str) or name in instances:
            raise S50FairnessError("scenario instance names are not unique")
        instances[name] = {
            "client_environment": item.get("client_environment"),
            "host": item.get("host"),
            "role": item.get("role"),
            "slots": item.get("slots"),
        }
    workload = scenario.get("workload")
    if not isinstance(workload, Mapping):
        raise S50FairnessError("workload is absent")
    return {
        "instances": instances,
        "network": scenario.get("network"),
        "timeline": scenario.get("timeline"),
        "timeouts": scenario.get("timeouts"),
        "workload": {
            key: workload.get(key)
            for key in ("clients", "corpus", "jobs", "oracle", "repeat", "turns")
        },
    }


def _bundle_timing(
    bundle: Mapping[str, Any], client: str, expected_turns: Sequence[str]
) -> dict[str, Any]:
    observations = bundle.get("observations")
    if not isinstance(observations, Mapping):
        raise S50FairnessError("bundle observations are absent")
    client_turns = observations.get("client_turns")
    if not isinstance(client_turns, Mapping):
        raise S50FairnessError("per-client turn timing evidence is absent")
    turns = observations.get("turns")
    if not isinstance(turns, Mapping) or not turns:
        raise S50FairnessError("turn timing evidence is absent")
    if set(client_turns) != set(expected_turns) or set(turns) != set(expected_turns):
        raise S50FairnessError("per-client and aggregate turn sets differ")
    selected: list[Mapping[str, Any]] = []
    for turn in expected_turns:
        clients = client_turns[turn]
        if not isinstance(clients, Mapping) or client not in clients:
            raise S50FairnessError(f"missing {client} timing for turn {turn!r}")
        timing = clients[client]
        if not isinstance(timing, Mapping):
            raise S50FairnessError(f"malformed {client} timing for turn {turn!r}")
        selected.append(timing)
    wall_values = [item.get("wall_ms") for item in selected]
    if any(type(value) is not int or value <= 0 for value in wall_values):
        raise S50FairnessError(f"{client} timing contains no positive wall_ms")
    jobs = [item.get("jobs") for item in selected]
    objects = [item.get("exact_objects") for item in selected]
    if any(type(value) is not int or value <= 0 for value in jobs + objects):
        raise S50FairnessError(f"{client} timing lacks exact job/object counts")
    return {
        "exact_objects": sum(objects),
        "jobs": sum(jobs),
        "wall_ms": sum(wall_values),
    }


def _validate_bundle(bundle: Mapping[str, Any], scenario: Mapping[str, Any]) -> None:
    rows = bundle.get("rows")
    workload = scenario.get("workload", {})
    if not isinstance(rows, list) or not isinstance(workload, Mapping):
        raise S50FairnessError("bundle rows/workload are malformed")
    clients = workload.get("clients")
    turns = workload.get("turns")
    if (
        not isinstance(clients, list)
        or not clients
        or not isinstance(turns, list)
        or not turns
        or any(not isinstance(value, str) or not value for value in clients + turns)
    ):
        raise S50FairnessError("workload counts are malformed")
    if not rows or any(
        not isinstance(row, Mapping) or row.get("exact") is not True for row in rows
    ):
        raise S50FairnessError("exact row/object evidence is absent or malformed")
    row_counts = {client: 0 for client in clients}
    for row in rows:
        client = row.get("client_instance")
        if client not in row_counts:
            raise S50FairnessError(f"row names unexpected client {client!r}")
        row_counts[client] += 1
    for client in clients:
        timing = _bundle_timing(bundle, client, turns)
        expected = row_counts[client]
        if (
            expected <= 0
            or timing["jobs"] != expected
            or timing["exact_objects"] != expected
        ):
            raise S50FairnessError(
                f"{client}: exact per-client count mismatch; "
                f"rows {expected}, got {timing['jobs']}/{timing['exact_objects']}"
            )


def score_s50_fairness(
    control: Mapping[str, Any],
    mixed: Sequence[Mapping[str, Any]],
    *,
    ratio_limit: float = 1.05,
) -> dict[str, Any]:
    """Score one control against exactly three uniquely identified mixed cells."""

    if len(mixed) != 3:
        raise S50FairnessError("S50 fairness requires exactly three mixed cells")
    control_scenario = control.get("scenario_data")
    if not isinstance(control_scenario, Mapping):
        raise S50FairnessError("control scenario is absent")
    if control.get("status") != "PASS":
        raise S50FairnessError("control scenario verdict is not PASS")
    control_run = control.get("run_id")
    if not isinstance(control_run, str) or not control_run:
        raise S50FairnessError("control run id is absent")
    mixed_ids = [item.get("scenario") for item in mixed]
    mixed_runs = [item.get("run_id") for item in mixed]
    if any(not isinstance(value, str) for value in mixed_ids + mixed_runs):
        raise S50FairnessError("mixed scenario/run identity is absent")
    if len(set(mixed_ids)) != 3 or len(set(mixed_runs + [control_run])) != 4:
        raise S50FairnessError("S50 scenario or run ids are not unique")
    base_structure = _scenario_structure(control_scenario)
    control_bundle = control.get("bundle_data")
    if not isinstance(control_bundle, Mapping):
        raise S50FairnessError("control bundle is absent")
    _validate_bundle(control_bundle, control_scenario)
    control_turns = control_scenario["workload"]["turns"]
    control_timing = _bundle_timing(control_bundle, "C1", control_turns)
    results = []
    for item in mixed:
        scenario = item.get("scenario_data")
        bundle = item.get("bundle_data")
        if not isinstance(scenario, Mapping) or not isinstance(bundle, Mapping):
            raise S50FairnessError("mixed scenario/bundle is absent")
        if item.get("status") != "PASS":
            raise S50FairnessError(f"mixed scenario {item.get('scenario')!r} is not PASS")
        if _scenario_structure(scenario) != base_structure:
            raise S50FairnessError("control and mixed scenario structures differ")
        _validate_bundle(bundle, scenario)
        mixed_timing = _bundle_timing(bundle, "C1", scenario["workload"]["turns"])
        if mixed_timing["jobs"] != control_timing["jobs"] or mixed_timing["exact_objects"] != control_timing["exact_objects"]:
            raise S50FairnessError("control/mixed C1 job or object counts differ")
        ratio = mixed_timing["wall_ms"] / control_timing["wall_ms"]
        if ratio > ratio_limit:
            raise S50FairnessError(
                f"{item['scenario']}: C1 wall ratio {ratio:.6f} exceeds {ratio_limit:.6f}"
            )
        results.append({
            "c1_mixed_wall_ms": mixed_timing["wall_ms"],
            "c1_control_wall_ms": control_timing["wall_ms"],
            "ratio": ratio,
            "scenario": item["scenario"],
        })
    return {
        "control": control.get("scenario"),
        "mixed": results,
        "ratio_limit": ratio_limit,
        "schema": "icefarm-s50-fairness-v1",
        "status": "PASS",
    }


def render_fairness_report(result: Mapping[str, Any]) -> str:
    lines = [
        "# S50 matched fairness",
        "",
        f"Status: **{result.get('status', 'FAIL')}**",
        f"Control: `{result.get('control', '')}`",
        f"Ratio limit: `{result.get('ratio_limit', '')}`",
        "",
        "| Mixed cell | Control C1 wall (ms) | Mixed C1 wall (ms) | Ratio |",
        "|---|---:|---:|---:|",
    ]
    for item in result.get("mixed", []):
        lines.append(
            f"| `{item['scenario']}` | {item['c1_control_wall_ms']} | "
            f"{item['c1_mixed_wall_ms']} | {item['ratio']:.6f} |"
        )
    if result.get("error"):
        lines.extend(["", f"Failure: {result['error']}"])
    return "\n".join(lines) + "\n"
