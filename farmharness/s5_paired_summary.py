#!/usr/bin/env python3
"""Validate independent S5 paired runs and compute the product decision.

This is the deliberately small adapter between ``s5_paired_build.py`` output
and the S5 paired-block rule.  It validates the bytes the current farm runner
actually retains, treats each experiment directory as one independent block,
and reports a transparent whole-block bootstrap.  It does not claim acceptance
by the older preregistration verifier.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import random
import re
import sys
from pathlib import Path
from typing import Any, Iterable, Sequence


RUN_SCHEMA = "icecream-s5-zstd-tu-paired-run-v1"
SUMMARY_SCHEMA = "icecream-s5-paired-product-summary-v1"
BLOCK_SCHEMA = "icecream-s5-paired-product-block-v1"
MODES = ("cache", "legacy")
REGIMES = ("cold", "warm")
ORDERS = ("AB", "BA")
EXPECTED_TUS = 31
MIN_BLOCKS_PER_REGIME = 4
REGRESSION_LIMIT = 1.10
DEFAULT_BOOTSTRAP_REPS = 10_000
DEFAULT_BOOTSTRAP_SEED = 20_260_828
HEX40 = re.compile(r"^[0-9a-f]{40}$")
HEX64 = re.compile(r"^[0-9a-f]{64}$")
HEX32 = re.compile(r"^[0-9a-f]{32}$")
CLEANUP = re.compile(r"^bounded forced=0 seconds=[0-9]+$")


class EvidenceError(ValueError):
    """Raised when retained paired-run evidence is incomplete or inconsistent."""


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise EvidenceError(message)


def _no_duplicate_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise EvidenceError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def _read_json(path: Path) -> dict[str, Any]:
    _require(path.is_file() and not path.is_symlink(), f"missing regular file: {path}")
    try:
        value = json.loads(path.read_text(encoding="utf-8"),
                           object_pairs_hook=_no_duplicate_object,
                           parse_constant=lambda token: (_ for _ in ()).throw(
                               EvidenceError(f"non-finite JSON constant: {token}")))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise EvidenceError(f"cannot parse {path}: {exc}") from exc
    _require(isinstance(value, dict), f"JSON root is not an object: {path}")
    return value


def _read_jsonl(path: Path) -> list[dict[str, Any]]:
    _require(path.is_file() and not path.is_symlink(), f"missing regular file: {path}")
    rows: list[dict[str, Any]] = []
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeError) as exc:
        raise EvidenceError(f"cannot read {path}: {exc}") from exc
    _require(lines and all(line.strip() for line in lines),
             f"empty or unterminated logical row in {path}")
    for number, line in enumerate(lines, 1):
        try:
            row = json.loads(line, object_pairs_hook=_no_duplicate_object,
                             parse_constant=lambda token: (_ for _ in ()).throw(
                                 EvidenceError(f"non-finite JSON constant: {token}")))
        except (json.JSONDecodeError, EvidenceError) as exc:
            raise EvidenceError(f"cannot parse {path}:{number}: {exc}") from exc
        _require(isinstance(row, dict), f"JSONL row is not an object: {path}:{number}")
        rows.append(row)
    return rows


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _canonical_bytes(value: Any) -> bytes:
    return (json.dumps(value, sort_keys=True, separators=(",", ":"),
                       ensure_ascii=True, allow_nan=False) + "\n").encode()


def _canonical_digest(value: Any) -> str:
    return hashlib.sha256(_canonical_bytes(value)).hexdigest()


def _finite_positive(value: Any, label: str) -> float:
    _require(isinstance(value, (int, float)) and not isinstance(value, bool),
             f"{label} is not numeric")
    number = float(value)
    _require(math.isfinite(number) and number > 0, f"{label} is not finite and positive")
    return number


def _digits(value: Any, label: str) -> int:
    _require(isinstance(value, str) and value.isdigit(), f"{label} is not decimal text")
    return int(value)


def _inside(path: Path, root: Path) -> bool:
    try:
        path.relative_to(root)
        return True
    except ValueError:
        return False


def _experiment_dir(value: Path) -> Path:
    path = value.resolve()
    if path.name == "results.jsonl":
        path = path.parent
    _require(path.is_dir() and not path.is_symlink(), f"not an experiment directory: {value}")
    return path


def _validate_workload(workload: dict[str, Any], label: str) -> list[dict[str, Any]]:
    _require(workload.get("schema") == "icecream-retained-fmt-workload-v1",
             f"{label}: wrong workload schema")
    _require(isinstance(workload.get("source_root"), str) and workload["source_root"],
             f"{label}: missing source root")
    _require(HEX64.fullmatch(str(workload.get("manifest_sha256", ""))) is not None,
             f"{label}: invalid compile manifest digest")
    tus = workload.get("tus")
    _require(isinstance(tus, list) and len(tus) == EXPECTED_TUS,
             f"{label}: retained workload must contain exactly {EXPECTED_TUS} TUs")
    seen: set[str] = set()
    for index, tu in enumerate(tus):
        _require(isinstance(tu, dict), f"{label}: TU {index} is not an object")
        tu_id, source = tu.get("tu_id"), tu.get("source")
        _require(isinstance(tu_id, str) and tu_id and tu_id not in seen,
                 f"{label}: duplicate or missing TU id at {index}")
        seen.add(tu_id)
        _require(isinstance(source, str) and source and not source.startswith("/"),
                 f"{label}: invalid TU source at {index}")
        _require(HEX64.fullmatch(str(tu.get("source_sha256", ""))) is not None,
                 f"{label}: invalid TU source digest at {index}")
        _require(tu.get("compiler") in ("gcc", "g++"),
                 f"{label}: invalid TU compiler at {index}")
        _require(isinstance(tu.get("flags"), list) and
                 all(isinstance(flag, str) for flag in tu["flags"]),
                 f"{label}: invalid TU flags at {index}")
    return tus


def _validate_roles(experiment: dict[str, Any], label: str) -> str:
    hashes = experiment.get("p50_binary_hashes")
    _require(isinstance(hashes, dict) and set(hashes) == {"S", "C", "F", "E", "X"},
             f"{label}: incomplete role hashes")
    _require(all(HEX64.fullmatch(str(value)) for value in hashes.values()),
             f"{label}: invalid role hash")
    manifest = experiment.get("role_manifest")
    _require(isinstance(manifest, dict) and isinstance(manifest.get("roles"), dict),
             f"{label}: missing role manifest")
    roles = manifest["roles"]
    _require(set(roles) == set(hashes), f"{label}: role manifest set differs")
    for role, expected in hashes.items():
        item = roles[role]
        _require(isinstance(item, dict) and item.get("role") == role,
                 f"{label}: invalid {role} role record")
        _require(item.get("sha256") == expected and item.get("exists") is True and
                 item.get("symlink") is False and item.get("executable") is True,
                 f"{label}: unusable or mismatched {role} artifact")
    return _canonical_digest({"hashes": hashes, "manifest": manifest})


def _validate_ledger(row: dict[str, Any], tus: list[dict[str, Any]], label: str) -> None:
    ledger = row.get("tu_ledger")
    _require(isinstance(ledger, list) and len(ledger) == len(tus),
             f"{label}: incomplete TU ledger")
    for index, (item, tu) in enumerate(zip(ledger, tus)):
        _require(isinstance(item, dict), f"{label}: ledger {index} is not an object")
        _require(item.get("tu_id") == tu["tu_id"] and item.get("source") == tu["source"],
                 f"{label}: ledger order/source mismatch at {index}")
        remote, reference = item.get("remote_sha256"), item.get("reference_sha256")
        _require(HEX64.fullmatch(str(remote or "")) is not None and remote == reference,
                 f"{label}: object digest mismatch at {index}")
        remote_bytes, reference_bytes = item.get("remote_bytes"), item.get("reference_bytes")
        _require(isinstance(remote_bytes, int) and not isinstance(remote_bytes, bool) and
                 remote_bytes > 0 and remote_bytes == reference_bytes,
                 f"{label}: object length mismatch at {index}")
        _require(item.get("byte_identical") is True,
                 f"{label}: non-identical object at {index}")


def _validate_command(row: dict[str, Any], label: str) -> str:
    command = row.get("command")
    _require(isinstance(command, list) and all(isinstance(token, str) for token in command),
             f"{label}: missing command")
    try:
        marker = command.index("--")
    except ValueError as exc:
        raise EvidenceError(f"{label}: command has no remote argument boundary") from exc
    args = command[marker + 1:]
    _require(len(args) == 8 and args[0] == args[1] == args[2],
             f"{label}: unexpected remote argument shape")
    _require(Path(args[0]).name.startswith("s5-p50-build."),
             f"{label}: remote role root is not private")
    _require(args[3] == "s50-c50-f50" and args[5] == "-" and args[6] == "c1f1",
             f"{label}: unexpected product cell")
    expected_cache = "1" if row["mode"] == "cache" else "0"
    expected_warm = "1" if row["regime"] == "warm" else "0"
    _require(args[4] == expected_cache and args[7] == expected_warm,
             f"{label}: command mode/regime mismatch")
    return args[0]


def _validate_warm(row: dict[str, Any], tu_count: int, label: str) -> str | None:
    prewarm, identity = row.get("prewarm"), row.get("warm_identity")
    if row["regime"] == "cold":
        _require(prewarm is None and identity is None,
                 f"{label}: cold row contains warm state")
        return None
    _require(isinstance(prewarm, dict) and prewarm.get("mode") == row["mode"],
             f"{label}: missing mode-private prewarm")
    if row["mode"] == "legacy":
        _require(_digits(prewarm.get("manifest_count"), f"{label} manifest_count") == tu_count and
                 _digits(prewarm.get("output_count"), f"{label} output_count") == tu_count,
                 f"{label}: incomplete legacy prewarm")
        _require(identity is None, f"{label}: legacy row claims cache identity")
        return None
    for field in ("state_digest", "tu_seq_digest"):
        _require(HEX64.fullmatch(str(prewarm.get(field, ""))) is not None,
                 f"{label}: invalid prewarm {field}")
    _require(HEX32.fullmatch(str(prewarm.get("c_guid", ""))) is not None,
             f"{label}: invalid prewarm C GUID")
    _require(_digits(prewarm.get("f_store_generation"), f"{label} F generation") > 0 and
             _digits(prewarm.get("scheduler_epoch"), f"{label} scheduler epoch") > 0 and
             _digits(prewarm.get("tu_seq_count"), f"{label} TU_SEQ count") >= tu_count,
             f"{label}: incomplete cache prewarm identity")
    _require(isinstance(prewarm.get("snapshot_identity"), str) and
             prewarm["snapshot_identity"].endswith(":" + prewarm["state_digest"]),
             f"{label}: prewarm snapshot is not digest-bound")
    _require(isinstance(identity, dict) and
             identity.get("c_guid") == prewarm["c_guid"] and
             identity.get("f_store_generation") == prewarm["f_store_generation"] and
             identity.get("scheduler_epoch") == prewarm["scheduler_epoch"],
             f"{label}: warm cache identity changed")
    return ":".join((identity["c_guid"], identity["f_store_generation"],
                     identity["scheduler_epoch"]))


def _validate_row(row: dict[str, Any], experiment_dir: Path,
                  block: dict[str, Any], tus: list[dict[str, Any]],
                  expected_mode: str) -> tuple[float, str, str | None, str]:
    label = f"{experiment_dir.name}/{expected_mode}"
    _require(row.get("kind") == "build" and row.get("schema") == RUN_SCHEMA,
             f"{label}: wrong result schema")
    for key in ("block_id", "regime", "order"):
        _require(row.get(key) == block[key], f"{label}: {key} differs from plan")
    _require(row.get("mode") == expected_mode,
             f"{label}: mode order differs from plan")
    _require(row.get("build_id") == f"{block['block_id']}-{expected_mode}-full",
             f"{label}: build id differs")
    _require(row.get("status") == "PASS" and row.get("reason") == "remote-byte-identical",
             f"{label}: build did not pass byte-exactly")
    _require(row.get("measurement_boundary") == "measured-full-build",
             f"{label}: wrong measurement boundary")
    start, end = row.get("start_ns"), row.get("end_ns")
    _require(isinstance(start, int) and not isinstance(start, bool) and start > 0 and
             isinstance(end, int) and not isinstance(end, bool) and end > start,
             f"{label}: invalid measurement interval")
    derived = (end - start) / 1e9
    wall = _finite_positive(row.get("wall_seconds"), f"{label} wall_seconds")
    aggregate = _finite_positive(row.get("aggregate_wall_seconds"),
                                 f"{label} aggregate_wall_seconds")
    _require(math.isclose(wall, derived, rel_tol=1e-12, abs_tol=1e-9) and
             math.isclose(aggregate, derived, rel_tol=1e-12, abs_tol=1e-9),
             f"{label}: wall time differs from timestamps")
    _require(row.get("tu_count") == len(tus) and row.get("tu_id") is None and
             row.get("source") is None and row.get("source_sha256") is None,
             f"{label}: result is not a complete full-manifest build")
    _require(row.get("remote_compile") is True and row.get("byte_identical") is True and
             row.get("returncode") == 0,
             f"{label}: remote/byte/return status differs")
    _validate_ledger(row, tus, label)
    namespace = row.get("namespace")
    expected_reset = "fresh-per-build" if block["regime"] == "cold" else "same-lifecycle-prewarm"
    _require(isinstance(namespace, dict) and namespace.get("mode_private") is True and
             namespace.get("namespace_id") ==
             f"s5/{block['regime']}/{expected_mode}/{block['block_id']}" and
             namespace.get("reset") == expected_reset,
             f"{label}: namespace/reset differs")
    _require(isinstance(row.get("cleanup"), str) and CLEANUP.fullmatch(row["cleanup"]),
             f"{label}: cleanup was not bounded and clean")
    _require(isinstance(row.get("runner_elapsed_ns"), int) and row["runner_elapsed_ns"] > 0,
             f"{label}: invalid runner duration")
    if expected_mode == "cache":
        expected_count = len(tus) * (2 if block["regime"] == "warm" else 1)
        _require(row.get("cache_expected") is True and row.get("cache_observed") is True and
                 row.get("legacy_observed") is False and
                 row.get("selected_profile") == "ZSTD_TU" and
                 row.get("selected_tu_count") == expected_count and
                 row.get("selected_route_count") == 0,
                 f"{label}: cache profile observation differs")
    else:
        _require(row.get("cache_expected") is False and row.get("cache_observed") is False and
                 row.get("legacy_observed") is True and
                 row.get("selected_profile") is None and
                 row.get("selected_tu_count") == 0 and
                 row.get("selected_route_count") == 0,
                 f"{label}: legacy observation differs")
    warm_identity = _validate_warm(row, len(tus), label)
    staged_root = _validate_command(row, label)
    evidence = Path(str(row.get("evidence", ""))).resolve()
    _require(_inside(evidence, experiment_dir) and evidence.is_dir(),
             f"{label}: build evidence escapes experiment directory")
    record_path = evidence / "record.json"
    _require(_read_json(record_path) == row, f"{label}: record.json differs from results row")
    return wall, staged_root, warm_identity, _sha256_file(record_path)


def _validate_experiment(path: Path, ordinal: int) -> dict[str, Any]:
    run = _experiment_dir(path)
    label = run.name
    files = {name: run / name for name in
             ("experiment_manifest.json", "workload_manifest.json", "preflight.json",
              "results.jsonl")}
    experiment = _read_json(files["experiment_manifest.json"])
    workload = _read_json(files["workload_manifest.json"])
    preflight = _read_json(files["preflight.json"])
    rows = _read_jsonl(files["results.jsonl"])
    _require(experiment.get("schema") == RUN_SCHEMA and experiment.get("immutable") is True,
             f"{label}: wrong experiment schema")
    root_commit = str(experiment.get("root_commit", ""))
    _require(HEX40.fullmatch(root_commit) is not None and
             experiment.get("source_sha256") == root_commit,
             f"{label}: invalid source checkpoint")
    source_identity = experiment.get("source_identity")
    _require(isinstance(source_identity, dict) and source_identity.get("status") == "ok" and
             source_identity.get("root_commit") == root_commit and
             source_identity.get("product_tree_matches_root") == "true",
             f"{label}: product tree is not checkpoint-exact")
    _require(experiment.get("modes") == list(MODES) and
             experiment.get("regimes") == list(REGIMES) and
             experiment.get("bootstrap_unit") == "whole_block",
             f"{label}: experiment dimensions differ")
    _require(isinstance(experiment.get("cold_reset"), str) and experiment["cold_reset"] and
             isinstance(experiment.get("warm_reset"), str) and experiment["warm_reset"] and
             isinstance(experiment.get("measurement_boundary"), str) and
             experiment["measurement_boundary"],
             f"{label}: reset/measurement declaration missing")
    role_identity = _validate_roles(experiment, label)
    tus = _validate_workload(workload, label)
    workload_hash = _sha256_file(files["workload_manifest.json"])
    _require(experiment.get("workload_manifest_digest") == workload_hash,
             f"{label}: workload file digest differs from experiment")
    scope = experiment.get("smoke_scope")
    _require(isinstance(scope, dict) and scope.get("tu_count") == len(tus) and
             scope.get("statistic_claim") is False,
             f"{label}: runner overclaimed or changed workload scope")
    _require(preflight.get("status") == "READY" and preflight.get("reason") ==
             "target-uncontaminated" and isinstance(preflight.get("target"), dict) and
             preflight["target"].get("status") == "READY",
             f"{label}: preflight was not ready")
    plan = experiment.get("block_plan")
    _require(isinstance(plan, list) and len(plan) == 1 and isinstance(plan[0], dict),
             f"{label}: each independent experiment must contain exactly one block")
    block = plan[0]
    _require(block.get("regime") in REGIMES and block.get("order") in ORDERS and
             isinstance(block.get("block_id"), str) and block["block_id"] and
             block.get("sequence") == 0,
             f"{label}: invalid block plan")
    expected_modes = MODES if block["order"] == "AB" else tuple(reversed(MODES))
    _require(len(rows) == 2, f"{label}: paired experiment does not contain two build rows")
    values: dict[str, float] = {}
    roots: set[str] = set()
    warm_identity: str | None = None
    record_hashes: list[str] = []
    for row, mode in zip(rows, expected_modes):
        wall, staged_root, identity, record_hash = _validate_row(
            row, run, block, tus, mode)
        values[mode] = wall
        roots.add(staged_root)
        if identity is not None:
            warm_identity = identity
        record_hashes.append(record_hash)
    _require(len(roots) == 1, f"{label}: paired arms used different staged role roots")
    _require(rows[0]["namespace"]["namespace_id"] != rows[1]["namespace"]["namespace_id"],
             f"{label}: cache and legacy shared a namespace")
    ratio = values["cache"] / values["legacy"]
    global_id = f"{ordinal:04d}:{label}:{block['block_id']}"
    hashes = {name: _sha256_file(file) for name, file in files.items()}
    return {
        "kind": "paired_block", "schema": BLOCK_SCHEMA, "status": "PASS",
        "global_block_id": global_id, "experiment_dir": str(run),
        "experiment_name": label, "block_id": block["block_id"],
        "regime": block["regime"], "order": block["order"],
        "cache_seconds": values["cache"], "legacy_seconds": values["legacy"],
        "ratio_cache_to_legacy": ratio,
        "slowdown_percent": 100.0 * (ratio - 1.0),
        "tu_count": len(tus), "root_commit": root_commit,
        "workload_identity": _canonical_digest(workload),
        "workload_file_sha256": workload_hash, "role_identity": role_identity,
        "source_root": workload["source_root"], "staged_role_root": next(iter(roots)),
        "warm_identity": warm_identity, "file_sha256": hashes,
        "record_sha256": record_hashes,
        "evidence_digest": _canonical_digest({"files": hashes, "records": record_hashes}),
    }


def _geometric_mean(values: Sequence[float]) -> float:
    return math.exp(math.fsum(math.log(value) for value in values) / len(values))


def _quantile(sorted_values: Sequence[float], fraction: float) -> float:
    index = int(math.floor((len(sorted_values) - 1) * fraction))
    return sorted_values[index]


def _regime_statistics(regime: str, blocks: list[dict[str, Any]],
                       repetitions: int, seed: int) -> dict[str, Any]:
    ratios = [float(block["ratio_cache_to_legacy"]) for block in blocks]
    orders = [str(block["order"]) for block in blocks]
    expected_orders = [ORDERS[index % len(ORDERS)] for index in range(len(orders))]
    _require(orders == expected_orders,
             f"{regime}: blocks are not ordered AB/BA alternately from AB")
    complete = (len(blocks) >= MIN_BLOCKS_PER_REGIME and len(blocks) % 2 == 0 and
                orders.count("AB") == orders.count("BA"))
    estimate = _geometric_mean(ratios) if ratios else None
    lower = upper = None
    regime_seed = seed ^ int(hashlib.sha256(regime.encode()).hexdigest()[:16], 16)
    if complete:
        generator = random.Random(regime_seed)
        samples = sorted(_geometric_mean([ratios[generator.randrange(len(ratios))]
                                         for _ in ratios])
                         for _ in range(repetitions))
        lower, upper = _quantile(samples, 0.05), _quantile(samples, 0.95)
    decision = "INCONCLUSIVE"
    if complete and lower is not None and lower > REGRESSION_LIMIT:
        decision = "RED"
    elif complete and upper is not None and upper <= REGRESSION_LIMIT:
        decision = "GREEN"
    return {
        "regime": regime, "block_count": len(blocks), "required_blocks": MIN_BLOCKS_PER_REGIME,
        "orders": orders, "ratios": ratios, "complete": complete,
        "geometric_mean_ratio": estimate,
        "slowdown_percent": None if estimate is None else 100.0 * (estimate - 1.0),
        "whole_block_bootstrap": {
            "repetitions": repetitions, "seed": regime_seed,
            "one_sided_lower_95": lower, "one_sided_upper_95": upper,
        },
        "regression_limit_ratio": REGRESSION_LIMIT, "decision": decision,
    }


def summarize_experiments(paths: Sequence[Path], *,
                          bootstrap_repetitions: int = DEFAULT_BOOTSTRAP_REPS,
                          bootstrap_seed: int = DEFAULT_BOOTSTRAP_SEED) -> list[dict[str, Any]]:
    _require(paths, "at least one experiment is required")
    _require(isinstance(bootstrap_repetitions, int) and bootstrap_repetitions >= 100,
             "bootstrap repetitions must be at least 100")
    _require(isinstance(bootstrap_seed, int) and bootstrap_seed >= 0,
             "bootstrap seed must be nonnegative")
    blocks = [_validate_experiment(Path(path), index)
              for index, path in enumerate(paths, 1)]
    resolved = [block["experiment_dir"] for block in blocks]
    _require(len(set(resolved)) == len(resolved), "an experiment directory was supplied twice")
    for key in ("root_commit", "workload_identity", "role_identity", "source_root"):
        _require(len({block[key] for block in blocks}) == 1,
                 f"cross-experiment {key} mismatch")
    _require(len({block["staged_role_root"] for block in blocks}) == len(blocks),
             "independent experiments reused a staged role root")
    warm_identities = [block["warm_identity"] for block in blocks
                       if block["regime"] == "warm"]
    _require(all(warm_identities) and len(set(warm_identities)) == len(warm_identities)
             if warm_identities else True,
             "independent warm experiments reused or omitted cache identity")
    by_regime = {regime: [block for block in blocks if block["regime"] == regime]
                 for regime in REGIMES}
    statistics = [_regime_statistics(regime, by_regime[regime],
                                     bootstrap_repetitions, bootstrap_seed)
                  for regime in REGIMES]
    complete = all(item["complete"] for item in statistics)
    if any(item["decision"] == "RED" for item in statistics):
        decision = "RED"
    elif complete and all(item["decision"] == "GREEN" for item in statistics):
        decision = "GREEN"
    else:
        decision = "INCONCLUSIVE"
    evidence_set = [block["evidence_digest"] for block in blocks]
    summary = {
        "kind": "summary", "schema": SUMMARY_SCHEMA,
        "scope": "reasonable-product-paired-run-decision",
        "official_preregistered_verifier_claim": False,
        "status": "COMPLETE" if complete else "INCOMPLETE",
        "decision": decision, "block_count": len(blocks),
        "counts": {regime: len(by_regime[regime]) for regime in REGIMES},
        "required_counts": {regime: MIN_BLOCKS_PER_REGIME for regime in REGIMES},
        "statistics": statistics,
        "input_experiments": resolved,
        "global_block_ids": [block["global_block_id"] for block in blocks],
        "root_commit": blocks[0]["root_commit"],
        "workload_identity": blocks[0]["workload_identity"],
        "role_identity": blocks[0]["role_identity"],
        "evidence_set_sha256": _canonical_digest(evidence_set),
    }
    return [*blocks, summary]


def _write_jsonl(path: Path | None, rows: Iterable[dict[str, Any]]) -> None:
    data = b"".join(_canonical_bytes(row) for row in rows)
    if path is None:
        sys.stdout.buffer.write(data)
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("xb") as stream:
        stream.write(data)
        stream.flush()
        os.fsync(stream.fileno())


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("experiment", nargs="+", type=Path,
                        help="ordered paired-run directory or its results.jsonl")
    parser.add_argument("--output", type=Path,
                        help="exclusive-create JSONL output (default: stdout)")
    parser.add_argument("--bootstrap-repetitions", type=int,
                        default=DEFAULT_BOOTSTRAP_REPS)
    parser.add_argument("--bootstrap-seed", type=int, default=DEFAULT_BOOTSTRAP_SEED)
    args = parser.parse_args(argv)
    try:
        rows = summarize_experiments(
            args.experiment, bootstrap_repetitions=args.bootstrap_repetitions,
            bootstrap_seed=args.bootstrap_seed)
        _write_jsonl(args.output, rows)
    except (EvidenceError, OSError) as exc:
        print(f"S5_SUMMARY_ERROR={exc}", file=sys.stderr)
        return 2
    summary = rows[-1]
    print(f"S5_SUMMARY_STATUS={summary['status']} decision={summary['decision']}",
          file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
