#!/usr/bin/env python3
"""Fail-closed salvage of an interrupted two-pass S8 live package.

This command only reads the failed package and predictive artifacts.  It never
reuses the failed directory as an output location and never opens retained
object files.  The full-2 curve is reconstructed from authenticated timing and
action-trace facts; the current predictive/live normalizer authenticates the
records emitted into the new sibling package.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import stat
import sys
import math
import shutil
from pathlib import Path
from typing import Any

try:
    from . import s8_predictive_live_normalizer as normalizer
except ImportError:  # direct invocation from farmharness/
    import s8_predictive_live_normalizer as normalizer


EXPECTED_COMMIT = "6c72c2dad8aa718f64facb678bac02ed9b18a7cb"
EXPECTED_TREE = "bb0dce90824dae4391c08b5639b0ee2582b06e0c"
EXPECTED_CELL = ("RocksDB", "ZSTD_TU", "cold")
EXPECTED_COUNT = 622
HEX64 = re.compile(r"^[0-9a-f]{64}$")


class SalvageError(ValueError):
    """An input failed an immutable salvage gate."""


def _fail(message: str) -> None:
    raise SalvageError(message)


def _read(path: Path, label: str, limit: int = 128 * 1024 * 1024) -> tuple[bytes, dict[str, Any]]:
    try:
        info = path.lstat()
    except OSError as exc:
        _fail(f"{label}:unavailable:{path}")
        raise AssertionError from exc
    if stat.S_ISLNK(info.st_mode) or not stat.S_ISREG(info.st_mode) or info.st_nlink != 1:
        _fail(f"{label}:not_private_regular_file:{path}")
    flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
    try:
        fd = os.open(path, flags)
    except OSError as exc:
        _fail(f"{label}:cannot_open:{path}")
        raise AssertionError from exc
    try:
        before = os.fstat(fd)
        digest = hashlib.sha256()
        chunks: list[bytes] = []
        total = 0
        while True:
            block = os.read(fd, 1 << 20)
            if not block:
                break
            total += len(block)
            if total > limit:
                _fail(f"{label}:too_large")
            digest.update(block)
            chunks.append(block)
        after = os.fstat(fd)
        if any(getattr(before, x) != getattr(after, x)
               for x in ("st_dev", "st_ino", "st_size", "st_mtime_ns", "st_ctime_ns")):
            _fail(f"{label}:changed_while_reading")
        return b"".join(chunks), {"bytes": total, "sha256": digest.hexdigest()}
    finally:
        os.close(fd)


def _json(raw: bytes, label: str) -> Any:
    return normalizer.parse_json(raw, label)


def _jsonl(raw: bytes, label: str) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for n, line in enumerate(raw.splitlines(), 1):
        if not line.strip():
            _fail(f"{label}:blank:{n}")
        value = _json(line, f"{label}:{n}")
        if not isinstance(value, dict):
            _fail(f"{label}:{n}:not_object")
        rows.append(value)
    return rows


def _sha(value: Any, label: str) -> str:
    if not isinstance(value, str) or HEX64.fullmatch(value.lower()) is None or int(value, 16) == 0:
        _fail(f"{label}:invalid_sha256")
    return value.lower()


def _int(value: Any, label: str, positive: bool = False) -> int:
    if type(value) is not int or (positive and value <= 0):
        _fail(f"{label}:invalid_integer")
    return value


def _token_int(value: Any, label: str) -> int:
    try:
        return int(value)
    except (TypeError, ValueError) as exc:
        _fail(f"{label}:invalid_integer")
        raise AssertionError from exc


def _kv(line: str, prefix: str) -> dict[str, str]:
    if not line.startswith(prefix + " "):
        _fail(f"log:bad_prefix:{prefix}")
    result: dict[str, str] = {}
    for token in line[len(prefix) + 1:].split():
        if "=" not in token:
            _fail(f"log:unkeyed_token:{prefix}")
        key, value = token.split("=", 1)
        if not key or key in result:
            _fail(f"log:duplicate_key:{prefix}:{key}")
        result[key] = value
    return result


def _product_log(root: Path) -> tuple[dict[str, list[dict[str, Any]]], dict[str, Any]]:
    path = root / "product-evidence" / "product-output.log"
    raw, facts = _read(path, "product_log")
    lines = raw.decode("utf-8").splitlines()
    records: dict[str, list[dict[str, Any]]] = {"full-1": [], "full-2": []}
    windows: dict[str, dict[str, int]] = {}
    complete: dict[str, int] = {}
    for line in lines:
        if line.startswith("S8_BATCH_WINDOW "):
            v = _kv(line, "S8_BATCH_WINDOW")
            run = v.get("run")
            if run not in records or run in windows:
                _fail("product_log:window_invalid")
            windows[run] = {k: _int(_token_int(v.get(k), f"window.{k}"), f"window.{k}", True)
                            for k in ("start_ns", "end_ns")}
        elif line.startswith("S8_BATCH_COMPLETE "):
            v = _kv(line, "S8_BATCH_COMPLETE")
            run = v.get("run")
            if run not in records or run in complete or v.get("count") is None:
                _fail("product_log:complete_invalid")
            complete[run] = int(v["count"])
        elif line.startswith("S8_BATCH_TU "):
            v = _kv(line, "S8_BATCH_TU")
            run = v.get("run")
            if run not in records:
                _fail("product_log:unknown_run")
            required = ("ordinal", "tu_id", "source_sha256",
                        "preprocessed_sha256", "preprocessed_bytes",
                        "remote_sha256", "remote_bytes", "local_sha256", "local_bytes",
                        "admission_start_ns", "input_ready_ns", "compile_start_ns", "compile_end_ns",
                        "witness_end_ns", "wait_for_cs_ns", "planned_assignment_ordinal",
                        "planned_relationship", "planned_admission_lane", "observed_scheduler_job_id",
                        "observed_f_service_identity", "observed_source_tu_seq")
            if any(k not in v for k in required):
                _fail("product_log:record_fields_missing")
            item: dict[str, Any] = {"run": run, "tu_id": v["tu_id"]}
            item["observed_f_service_identity"] = v["observed_f_service_identity"]
            for k in ("source_sha256", "preprocessed_sha256", "remote_sha256", "local_sha256"):
                item[k] = _sha(v[k], f"product.{k}")
            for k in ("ordinal", "preprocessed_bytes", "remote_bytes", "local_bytes",
                      "admission_start_ns", "input_ready_ns", "compile_start_ns", "compile_end_ns",
                      "witness_end_ns", "wait_for_cs_ns", "planned_assignment_ordinal",
                      "planned_relationship", "planned_admission_lane", "observed_source_tu_seq",
                      "observed_scheduler_job_id"):
                item[k] = _token_int(v[k], f"product.{k}")
            if any(item[k] <= 0 for k in ("preprocessed_bytes", "remote_bytes", "local_bytes")):
                _fail("product_log:nonpositive_artifact")
            if item["remote_sha256"] != item["local_sha256"] or item["remote_bytes"] != item["local_bytes"]:
                _fail("product_log:remote_local_mismatch")
            records[run].append(item)
    if set(windows) != set(records) or set(complete) != set(records):
        _fail("product_log:batch_window_or_complete_missing")
    for run, values in records.items():
        if len(values) != EXPECTED_COUNT or complete[run] != EXPECTED_COUNT:
            _fail(f"product_log:{run}:count_mismatch")
        if [x["ordinal"] for x in values] != list(range(EXPECTED_COUNT)):
            _fail(f"product_log:{run}:ordinal_sequence")
        if any(x["observed_source_tu_seq"] != x["ordinal"] + (EXPECTED_COUNT if run == "full-2" else 0)
               for x in values):
            _fail(f"product_log:{run}:source_sequence")
        if windows[run]["end_ns"] <= windows[run]["start_ns"]:
            _fail(f"product_log:{run}:window_order")
    if not any(line == "PASS: all-P50 C1F1 ZSTD_TU compile is remote and byte-identical" for line in lines):
        _fail("product_log:pass_marker_missing")
    return records, {"path": str(path.resolve()), **facts, "windows": windows,
                     "pass_marker": True}


def _load_timing(root: Path, run: str) -> tuple[list[dict[str, Any]], dict[str, Any]]:
    path = root / f"timing_{run}.jsonl"
    raw, facts = _read(path, f"timing_{run}")
    rows = _jsonl(raw, f"timing_{run}")
    if len(rows) != EXPECTED_COUNT:
        _fail(f"timing_{run}:count_mismatch")
    expected_source = 0 if run == "full-1" else EXPECTED_COUNT
    for ordinal, row in enumerate(rows):
        if row.get("run") != run or row.get("ordinal") != ordinal or row.get("phase") != "measured":
            _fail(f"timing_{run}:identity:{ordinal}")
        if row.get("observed_source_tu_seq") != expected_source + ordinal or row.get("tu_seq") != expected_source + ordinal:
            _fail(f"timing_{run}:sequence:{ordinal}")
        if row.get("cell") != "/".join(EXPECTED_CELL) or row.get("remote_compile") is not True:
            _fail(f"timing_{run}:cell_or_remote:{ordinal}")
        for k in ("C_TO_F_bytes", "F_TO_C_bytes", "channel_bytes", "returned_object_bytes", "elapsed_ns"):
            _int(row.get(k), f"timing.{k}", True)
        _sha(row.get("object_sha256"), "timing.object_sha256")
    return rows, {"path": str(path.resolve()), **facts}


def _validate_trace(root: Path, timings: dict[str, list[dict[str, Any]]]) -> dict[str, Any]:
    evidence = root / "product-evidence"
    paths = {
        "c_action": evidence / "s7-measured-c-action-trace.jsonl",
        "f_action": evidence / "s7-measured-f-action-trace.jsonl",
    }
    trace_facts: dict[str, Any] = {}
    tx: dict[str, dict[tuple[int, int], dict[str, Any]]] = {"c_action": {}, "f_action": {}}
    commits: dict[str, dict[tuple[int, int], dict[str, Any]]] = {"c_action": {}, "f_action": {}}
    for key, path in paths.items():
        raw, facts = _read(path, key, 64 * 1024 * 1024)
        rows = _jsonl(raw, key)
        trace_facts[key] = {"path": str(path.resolve()), **facts, "lines": len(rows)}
        for row in rows:
            if row.get("action") == "TX_BEGIN":
                ident = (row.get("tu_seq"), row.get("rel_seq"))
                if not all(type(x) is int for x in ident) or ident in tx[key]:
                    _fail(f"{key}:tx_identity")
                tx[key][ident] = row
            commit_action = "COMMIT_ACCEPTED" if key == "c_action" else "INPUT_COMMITTED"
            if row.get("action") == commit_action:
                ident = (row.get("tu_seq"), row.get("rel_seq"))
                if not all(type(x) is int for x in ident) or ident in commits[key]:
                    _fail(f"{key}:commit_identity")
                commits[key][ident] = row
        expected = 1244 if key == "c_action" else 1244
        if len(tx[key]) != expected or len(commits[key]) != expected:
            _fail(f"{key}:tx_count")
        trace_facts[key]["tx_begin_count"] = len(tx[key])
        trace_facts[key]["commit_count"] = len(commits[key])
    for run, rows in timings.items():
        for row in rows:
            ident = (row["tu_seq"], row["rel_seq"])
            c = tx["c_action"].get(ident)
            f = tx["f_action"].get(ident)
            if c is None or f is None:
                _fail(f"trace:missing_tx:{run}:{row['ordinal']}")
            for item, prefix in ((c, "c"), (f, "f")):
                if item.get("transaction_digest") != row["transaction_digest"]:
                    _fail(f"trace:{prefix}:transaction_digest:{run}:{row['ordinal']}")
                raw_field = "raw_digest" if prefix == "c" else "f_raw_digest"
                state_field = "state_digest" if prefix == "c" else "f_state_digest"
                if item.get("raw_digest") != row[raw_field] or item.get("state_digest") != row[state_field]:
                    _fail(f"trace:{prefix}:digest:{run}:{row['ordinal']}")
                if item.get("c_store_guid") != row["c_store_guid"] or item.get("f_store_guid") != row["f_store_guid"]:
                    _fail(f"trace:{prefix}:store_guid:{run}:{row['ordinal']}")
                if item.get("history_nonce") != row["history_nonce"] or item.get("rel_seq") != row["rel_seq"] or item.get("tu_seq") != row["tu_seq"]:
                    _fail(f"trace:{prefix}:state_sequence:{run}:{row['ordinal']}")
            if c["stage_bytes"] != row["C_TO_F_bytes"] or f["stage_bytes"] != row["C_TO_F_bytes"]:
                _fail(f"trace:stage_bytes:{run}:{row['ordinal']}")
            for commit, begin, prefix in ((commits["c_action"][ident], c, "c"),
                                           (commits["f_action"][ident], f, "f")):
                for field in ("c_store_guid", "f_store_guid", "transaction_digest", "raw_digest",
                              "history_nonce", "rel_seq", "tu_seq"):
                    if commit.get(field) != begin.get(field):
                        _fail(f"trace:{prefix}:commit_mismatch:{run}:{row['ordinal']}")
                if commit.get("stage_bytes") != 0:
                    _fail(f"trace:{prefix}:commit_stage_bytes:{run}:{row['ordinal']}")
            if commits["c_action"][ident]["state_digest"] != commits["f_action"][ident]["state_digest"]:
                _fail(f"trace:commit_state_mismatch:{run}:{row['ordinal']}")
    # The repeat must begin from the terminal accepted state, preserving both
    # service identities and the carried history nonce; only TU/relationship
    # sequence and transaction payload change for the next input.
    for key in ("c_action", "f_action"):
        last = commits[key][(EXPECTED_COUNT - 1, EXPECTED_COUNT - 1)]
        first = tx[key][(EXPECTED_COUNT, EXPECTED_COUNT)]
        if (last.get("c_store_guid") != first.get("c_store_guid") or
                last.get("f_store_guid") != first.get("f_store_guid") or
                last.get("history_nonce") != first.get("history_nonce") or
                last.get("state_digest") != first.get("state_digest") or
                first.get("tu_seq") != last.get("tu_seq") + 1 or
                first.get("rel_seq") != last.get("rel_seq") + 1):
            _fail(f"trace:{key}:full2_boundary")
    return trace_facts


def _validate_logs(root: Path) -> dict[str, Any]:
    out: dict[str, Any] = {}
    for name, pattern, expected in (
        ("scheduler", re.compile(r"END \d+ status=(\d+)"), 1246),
        ("f", re.compile(r"Remote compilation completed with exit code (\d+)"), 1246),
    ):
        path = root / "product-evidence" / f"{name}.log"
        raw, facts = _read(path, name, 128 * 1024 * 1024)
        values = [int(x) for x in pattern.findall(raw.decode("utf-8"))]
        if len(values) != expected or any(x != 0 for x in values):
            _fail(f"{name}:completion_status")
        out[name] = {"path": str(path.resolve()), **facts, "completion_count": len(values), "all_zero": True}
    return out


def _descriptor(path: Path, label: str) -> dict[str, Any]:
    raw, facts = _read(path, label)
    return {"path": str(path.resolve()), **facts}


def _auth_descriptor(root: Path, value: Any, label: str,
                     aliases: tuple[Path, ...] = ()) -> dict[str, Any]:
    if not isinstance(value, dict) or set(value) != {"path", "sha256", "bytes"}:
        _fail(f"{label}:descriptor")
    relative = value["path"]
    candidate = _relative_output_path(root, relative, label)
    raw, facts = _read(candidate, label)
    if facts["sha256"] != _sha(value["sha256"], f"{label}.sha256") or facts["bytes"] != value["bytes"]:
        _fail(f"{label}:descriptor_mismatch")
    for alias in aliases:
        alias_raw, alias_facts = _read(alias, f"{label}.alias")
        if alias_raw != raw or alias_facts != facts:
            _fail(f"{label}:alias_mismatch")
    return {"path": str(candidate.resolve()), **facts}


def _join_product_record(product: dict[str, Any], timing: dict[str, Any], label: str) -> None:
    fields = (
        "tu_id", "observed_source_tu_seq", "observed_scheduler_job_id",
        "admission_start_ns", "input_ready_ns", "compile_start_ns",
        "compile_end_ns", "witness_end_ns", "wait_for_cs_ns",
        "planned_assignment_ordinal", "planned_relationship", "planned_admission_lane",
        "observed_f_service_identity",
    )
    if any(product.get(field) != timing.get(field) for field in fields):
        _fail(f"product_timing_join:{label}:identity_or_timing")


def _validate_batch_authorities(root: Path, products: dict[str, list[dict[str, Any]]],
                                plans: list[dict[str, Any]]) -> dict[str, Any]:
    evidence = root / "product-evidence"
    batch_raw, batch_facts = _read(evidence / "batch-manifest.jsonl", "batch_manifest", 128 * 1024 * 1024)
    batch = _jsonl(batch_raw, "batch_manifest")
    input_raw, input_facts = _read(evidence / "input-descriptors.json", "input_descriptors", 16 * 1024 * 1024)
    inputs_value = _json(input_raw, "input_descriptors")
    inputs = inputs_value.get("inputs") if isinstance(inputs_value, dict) else None
    if not isinstance(inputs, list) or len(inputs) != EXPECTED_COUNT or len(batch) != EXPECTED_COUNT:
        _fail("input_authorities:count")
    for ordinal, (batch_row, descriptor) in enumerate(zip(batch, inputs, strict=True)):
        if batch_row.get("predictive_input", {}).get("ordinal") != ordinal or descriptor.get("ordinal") != ordinal:
            _fail(f"input_authorities:ordinal:{ordinal}")
        if batch_row.get("predictive_input") != descriptor:
            _fail(f"input_authorities:descriptor_join:{ordinal}")
        for plan in plans:
            plan_input = plan["inputs"][ordinal]
            if (plan_input.get("ordinal") != ordinal or plan_input.get("sha256") != descriptor.get("sha256") or
                    plan_input.get("bytes") != descriptor.get("bytes") or
                    plan_input.get("source_relative") != descriptor.get("source_relative")):
                _fail(f"input_authorities:plan_join:{ordinal}")
        for run in ("full-1", "full-2"):
            product = products[run][ordinal]
            if (product.get("ordinal") != ordinal or product.get("source_sha256") != batch_row.get("sha256") or
                    product.get("preprocessed_sha256") != descriptor.get("sha256") or
                    product.get("preprocessed_bytes") != descriptor.get("bytes") or
                    product.get("tu_id") != batch_row.get("tu_id")):
                _fail(f"input_authorities:product_join:{run}:{ordinal}")
    return {"batch_manifest": {"path": str((evidence / "batch-manifest.jsonl").resolve()), **batch_facts},
            "input_descriptors": {"path": str((evidence / "input-descriptors.json").resolve()), **input_facts}}


def _producer_descriptor(base: Path, value: Any, label: str) -> dict[str, Any]:
    if (not isinstance(value, dict) or
            not {"path", "sha256", "bytes"}.issubset(value)):
        _fail(f"{label}:descriptor")
    path = Path(value["path"])
    if not path.is_absolute():
        path = base / path
    raw, facts = _read(path, label)
    if facts["sha256"] != _sha(value["sha256"], f"{label}.sha256") or facts["bytes"] != value["bytes"]:
        _fail(f"{label}:descriptor_mismatch")
    return {"path": str(path.resolve()), **facts}


def _validate_predictive_producer(manifest: Path, plan_path: Path, run: str,
                                 evidence: dict[str, Any]) -> dict[str, Any]:
    manifest = manifest.absolute()
    producer_path = manifest.parent / "producer_manifest.json"
    producer_raw, producer_facts = _read(producer_path, f"producer_manifest.{run}", 8 * 1024 * 1024)
    producer = _json(producer_raw, f"producer_manifest.{run}")
    if not isinstance(producer, dict) or producer.get("schema") != "icecream-s8-multitu-predictive-producer-v1":
        _fail(f"producer_manifest.{run}:schema")
    identity = producer.get("identity")
    if (not isinstance(identity, dict) or identity.get("source_commit") != EXPECTED_COMMIT or
            identity.get("source_tree") != EXPECTED_TREE):
        _fail(f"producer_manifest.{run}:identity")
    predictive = normalizer._load_manifest(manifest, "predictive_sim")
    if (producer.get("cell") != dict(zip(("corpus", "profile", "regime"), EXPECTED_CELL)) or
            identity.get("input_digest") != predictive["identity"]["input_digest"] or
            identity.get("topology_digest") != predictive["identity"]["topology_digest"]):
        _fail(f"producer_manifest.{run}:cell_binding")
    expected_plan = evidence["predictive_plans"][run]
    plan_ref = _producer_descriptor(manifest.parent, producer.get("plan"), f"producer.{run}.plan")
    _, expected_plan_facts = _read(plan_path, f"plan.{run}")
    if plan_ref["sha256"] != expected_plan_facts["sha256"] or plan_ref["bytes"] != expected_plan_facts["bytes"] or \
            plan_ref["sha256"] != expected_plan.get("sha256") or plan_ref["bytes"] != expected_plan.get("bytes"):
        _fail(f"producer_manifest.{run}:plan_binding")
    outputs = producer.get("outputs")
    if not isinstance(outputs, dict):
        _fail(f"producer_manifest.{run}:outputs")
    manifest_ref = _producer_descriptor(manifest.parent, outputs.get("predictive_manifest"), f"producer.{run}.manifest")
    if manifest_ref["path"] != str(manifest.resolve()):
        _fail(f"producer_manifest.{run}:manifest_not_adjacent")
    curve_ref = _producer_descriptor(manifest.parent, outputs.get("predictive_sim"), f"producer.{run}.curve")
    if (curve_ref["path"] != str(Path(predictive["curve_path"]).resolve()) or
            curve_ref["sha256"] != predictive["curve_sha256"] or curve_ref["bytes"] != Path(predictive["curve_path"]).stat().st_size):
        _fail(f"producer_manifest.{run}:curve_binding")
    batch_binding = producer.get("batch_binding")
    if (not isinstance(batch_binding, dict) or batch_binding.get("source_commit") != EXPECTED_COMMIT or
            batch_binding.get("source_tree") != EXPECTED_TREE or
            not isinstance(batch_binding.get("input_manifest_sha256"), str) or
            not isinstance(batch_binding.get("assignment_sha256"), str) or
            not isinstance(batch_binding.get("simulator_sha256"), str)):
        _fail(f"producer_manifest.{run}:batch_binding")
    codec = producer.get("codec")
    simulator_ref = codec.get("simulator") if isinstance(codec, dict) else None
    simulator = _producer_descriptor(manifest.parent, simulator_ref, f"producer.{run}.simulator")
    if simulator["sha256"] != batch_binding.get("simulator_sha256"):
        _fail(f"producer_manifest.{run}:simulator_binding")
    build = producer.get("product_build")
    receipt = _producer_descriptor(manifest.parent, build.get("receipt") if isinstance(build, dict) else None,
                                   f"producer.{run}.build_receipt")
    return {"path": str(producer_path.resolve()), **producer_facts,
            "manifest": manifest_ref, "curve": curve_ref, "plan": plan_ref,
            "simulator": simulator, "build_receipt": receipt}


def _validate_inputs(root: Path, predictive: tuple[Path, Path]) -> tuple[dict[str, Any], dict[str, Any], dict[str, Any]]:
    evidence_raw, evidence_facts = _read(root / "evidence.json", "evidence", 4 * 1024 * 1024)
    evidence = _json(evidence_raw, "evidence")
    if not isinstance(evidence, dict) or evidence.get("source_commit") != EXPECTED_COMMIT or evidence.get("source_tree") != EXPECTED_TREE:
        _fail("evidence:source_identity")
    if evidence.get("cell") != "/".join(EXPECTED_CELL) or evidence.get("state_carrying_repeat") is not True:
        _fail("evidence:cell_or_repeat")
    claimed = evidence.get("evidence_sha256")
    if not isinstance(claimed, str):
        _fail("evidence:self_hash_missing")
    without = dict(evidence)
    without.pop("evidence_sha256", None)
    if hashlib.sha256(normalizer.canonical_bytes(without)).hexdigest() != claimed:
        _fail("evidence:self_hash_mismatch")
    evidence_files: dict[str, Any] = {}
    evidence_files["results"] = _auth_descriptor(root, evidence.get("evidence", {}).get("results"),
                                                  "evidence.results")
    evidence_files["timing"] = _auth_descriptor(
        root, evidence.get("evidence", {}).get("timing"), "evidence.timing",
        aliases=(root / "timing_full-1.jsonl",))
    predictive_plans = evidence.get("predictive_plans")
    if (not isinstance(predictive_plans, dict) or
            not all(isinstance(predictive_plans.get(run), dict) for run in ("full-1", "full-2"))):
        _fail("evidence:predictive_plans")
    plans: list[dict[str, Any]] = []
    plan_paths = (root / "product-evidence" / "predictive-plan.json",
                  root / "product-evidence" / "predictive-plan-full-2.json")
    for run, path in zip(("full-1", "full-2"), plan_paths, strict=True):
        plan_descriptor = predictive_plans[run]
        expected_sha = plan_descriptor.get("sha256")
        raw, facts = _read(path, "predictive_plan", 8 * 1024 * 1024)
        if facts["sha256"] != _sha(expected_sha, f"evidence.predictive_plans.{run}.sha256") or facts["bytes"] != plan_descriptor.get("bytes"):
            _fail(f"predictive_plan:sha256:{path}")
        value = _json(raw, "predictive_plan")
        if not isinstance(value, dict) or value.get("cell") != dict(zip(("corpus", "profile", "regime"), EXPECTED_CELL)):
            _fail("predictive_plan:cell")
        if len(value.get("inputs", [])) != EXPECTED_COUNT or value.get("scheduling", {}).get("topology") != "C1F1":
            _fail("predictive_plan:count_or_topology")
        plans.append(value)
    witness = evidence.get("witness")
    if not isinstance(witness, dict):
        _fail("evidence:witness")
    evidence_files["timing_full_2"] = _auth_descriptor(root, witness.get("timing_full_2"), "evidence.timing_full_2")
    evidence_files["c_action"] = _auth_descriptor(root, witness.get("c_action"), "evidence.c_action")
    evidence_files["f_action"] = _auth_descriptor(root, witness.get("f_action"), "evidence.f_action")
    topology_path = root / "product-evidence" / "topology.json"
    topology_raw, topology_facts = _read(topology_path, "topology", 8 * 1024 * 1024)
    topology = {"path": str(topology_path.resolve()), **topology_facts}
    if topology["sha256"] != evidence["topology_sha256"]:
        _fail("topology:sha256")
    topology_value = _json(topology_raw, "topology")
    assignments = topology_value.get("assignments") if isinstance(topology_value, dict) else None
    if not isinstance(assignments, list) or len(assignments) != EXPECTED_COUNT:
        _fail("topology:assignments")
    for ordinal, assignment in enumerate(assignments):
        if not isinstance(assignment, dict) or assignment.get("ordinal") != ordinal:
            _fail("topology:ordinal")
        for plan in plans:
            plan_assignment = plan["scheduling"]["assignments"][ordinal]
            if (assignment.get("relationship") != plan_assignment.get("f_relationship") or
                    assignment.get("f_slot") != plan_assignment.get("per_f_slot") or
                    assignment.get("global_slot") not in (None, plan_assignment.get("global_slot"))):
                _fail(f"topology:plan_binding:{ordinal}")
    for key, descriptor in (("input_manifest", evidence.get("input_manifest")),
                            ("input_descriptors", evidence.get("input_descriptors"))):
        if not isinstance(descriptor, dict):
            _fail(f"evidence:{key}:descriptor")
        source = root / descriptor["path"]
        observed = _descriptor(source, f"evidence_{key}")
        if observed["sha256"] != descriptor.get("sha256") or observed["bytes"] != descriptor.get("bytes"):
            _fail(f"evidence:{key}:sha256")
    return evidence, {"path": str((root / "evidence.json").resolve()), **evidence_facts,
                      "authenticated_descriptors": evidence_files}, {"plans": plans, "topology": topology}


def _curve_rows(timing: list[dict[str, Any]], start_ns: int) -> bytes:
    c_total = 0
    f_total = 0
    prefix_end = start_ns
    output: list[dict[str, Any]] = []
    for step, row in enumerate(timing):
        c_total += row["C_TO_F_bytes"]
        f_total += row["F_TO_C_bytes"]
        prefix_end = max(prefix_end, row["compile_end_ns"])
        channel = c_total + f_total
        elapsed = prefix_end - start_ns
        output.append({"cell": {"corpus": EXPECTED_CELL[0], "profile": EXPECTED_CELL[1], "regime": EXPECTED_CELL[2]},
                       "cumulative": {"C_TO_F_bytes": c_total, "F_TO_C_bytes": f_total,
                                       "channel_bytes": channel, "elapsed_ns": elapsed,
                                       "throughput_bytes_per_s": channel / elapsed * 1_000_000_000 if elapsed else 0.0},
                       "step": step, "tu_id": row["tu_id"]})
    return b"".join(normalizer.canonical_bytes(row) + b"\n" for row in output)


def _write(path: Path, raw: bytes) -> None:
    if path.exists() or path.is_symlink():
        _fail(f"output_exists:{path}")
    with path.open("xb") as stream:
        stream.write(raw)
        stream.flush()
        os.fsync(stream.fileno())


def _relative_output_path(root: Path, relative: str, label: str) -> Path:
    candidate = Path(relative)
    if candidate.is_absolute() or any(part in ("", ".", "..") for part in candidate.parts):
        _fail(f"{label}:path_not_relative")
    path = root / candidate
    try:
        path.resolve().relative_to(root.resolve())
    except ValueError as exc:
        raise SalvageError(f"{label}:path_escapes_output") from exc
    return path


def _post_validate_output(out: Path, experiment: dict[str, Any], witness: dict[str, Any],
                          environment_preparation: Any) -> None:
    for run, relative in experiment["curve_manifests"].items():
        path = _relative_output_path(out, relative, f"output.curve_manifest.{run}")
        if not path.is_file():
            _fail(f"output.curve_manifest.{run}:missing")
        normalizer._load_manifest(path, "live")
    witness_path = _relative_output_path(out, experiment["assignment_witness"], "output.assignment_witness")
    _, facts = _read(witness_path, "output.assignment_witness")
    if facts["sha256"] != witness["sha256"] or facts["bytes"] != witness["bytes"]:
        _fail("output.assignment_witness:descriptor_mismatch")
    evidence_path = _relative_output_path(out, experiment["evidence"], "output.evidence")
    if not evidence_path.is_file():
        _fail("output.evidence:missing")
    for name in ("records.jsonl", "live_curve_full-1.jsonl", "live_curve_full-2.jsonl",
                 "live_curve_manifest.json", "live_curve.jsonl"):
        if not (out / name).is_file():
            _fail(f"output:{name}:missing")
    if experiment["environment_preparation"] != environment_preparation:
        _fail("output.environment_preparation:mismatch")


def _curve_matches(expected_raw: bytes, actual_raw: bytes) -> bool:
    expected = _jsonl(expected_raw, "recomputed_curve")
    actual = _jsonl(actual_raw, "retained_curve")
    if len(expected) != len(actual):
        return False
    for left, right in zip(expected, actual, strict=True):
        if left.get("step") != right.get("step") or left.get("tu_id") != right.get("tu_id") or left.get("cell") != right.get("cell"):
            return False
        lc, rc = left.get("cumulative"), right.get("cumulative")
        if not isinstance(lc, dict) or not isinstance(rc, dict):
            return False
        for key in ("C_TO_F_bytes", "F_TO_C_bytes", "channel_bytes", "elapsed_ns"):
            if lc.get(key) != rc.get(key):
                return False
        if not math.isclose(float(lc.get("throughput_bytes_per_s")), float(rc.get("throughput_bytes_per_s")), rel_tol=1e-14, abs_tol=1e-12):
            return False
    return True


def _salvage(live_root: Path, predictive_full_1: Path, predictive_full_2: Path, out: Path) -> Path:
    live_root = live_root.absolute()
    out = out.absolute()
    if not live_root.is_dir() or out.parent.resolve() != live_root.parent.resolve() or out == live_root:
        _fail("output:must_be_new_sibling")
    if out.exists() or out.is_symlink():
        _fail("output:already_exists")
    evidence, evidence_desc, plan_facts = _validate_inputs(live_root, (predictive_full_1.absolute(), predictive_full_2.absolute()))
    products, product_desc = _product_log(live_root)
    input_authorities = _validate_batch_authorities(live_root, products, plan_facts["plans"])
    for key, observed_key in (("input_manifest", "batch_manifest"), ("input_descriptors", "input_descriptors")):
        ref = evidence.get(key)
        observed = input_authorities[observed_key]
        declared = _auth_descriptor(live_root, ref, f"evidence.{key}")
        if declared["sha256"] != observed["sha256"] or declared["bytes"] != observed["bytes"]:
            _fail(f"evidence:{key}:descriptor_mismatch")
    timings: dict[str, list[dict[str, Any]]] = {}
    timing_desc: dict[str, Any] = {}
    for run in ("full-1", "full-2"):
        timings[run], timing_desc[run] = _load_timing(live_root, run)
        for product, timing in zip(products[run], timings[run], strict=True):
            checks = (("tu_id", "tu_id"), ("observed_source_tu_seq", "observed_source_tu_seq"),
                      ("remote_sha256", "object_sha256"), ("remote_bytes", "returned_object_bytes"))
            if any(product[a] != timing[b] for a, b in checks):
                _fail(f"product_timing_join:{run}:{product['ordinal']}")
            _join_product_record(product, timing, f"{run}:{product['ordinal']}")
    trace_desc = _validate_trace(live_root, timings)
    log_desc = _validate_logs(live_root)

    live1_manifest = live_root / "live_curve_manifest_full-1.json"
    live1 = normalizer._load_manifest(live1_manifest, "live")
    if live1["identity"]["source_commit"] != EXPECTED_COMMIT or live1["identity"]["source_tree"] != EXPECTED_TREE:
        _fail("live_manifest:source_identity")
    expected1 = _curve_rows(timings["full-1"], product_desc["windows"]["full-1"]["start_ns"])
    if not _curve_matches(expected1, _read(live_root / "live_curve_full-1.jsonl", "live_curve_full_1")[0]):
        _fail("live_curve_full_1:recompute_mismatch")
    producer_desc: dict[str, Any] = {}
    for run, manifest, expected_plan in (("full-1", predictive_full_1, plan_facts["plans"][0]), ("full-2", predictive_full_2, plan_facts["plans"][1])):
        loaded = normalizer._load_manifest(manifest.absolute(), "predictive_sim")
        if loaded["identity"]["source_commit"] != EXPECTED_COMMIT or loaded["identity"]["source_tree"] != EXPECTED_TREE:
            _fail("predictive_manifest:source_identity")
        expected_comparison = normalizer.comparison_descriptor(
            plan_facts["plans"][0 if run == "full-1" else 1].get("result", {}).get("plan_sha256", evidence["comparisons"][run]["plan_sha256"]),
            expected_plan["scheduling"])
        if loaded.get("comparison") != evidence["comparisons"][run] or loaded.get("comparison") != expected_comparison:
            _fail(f"predictive_manifest:comparison:{run}")
        producer_desc[run] = _validate_predictive_producer(
            manifest, live_root / "product-evidence" / ("predictive-plan.json" if run == "full-1" else "predictive-plan-full-2.json"),
            run, evidence)

    full1_value = _json(_read(live1_manifest, "live_manifest")[0], "live_manifest")
    if not isinstance(full1_value, dict):
        _fail("live_manifest:not_object")
    full2_curve = _curve_rows(timings["full-2"], product_desc["windows"]["full-2"]["start_ns"])
    full2_value = dict(full1_value)
    full2_value["identity"] = dict(full1_value["identity"])
    full2_value["identity"]["run_id"] = "full-2"
    full2_value["comparison"] = evidence["comparisons"]["full-2"]
    full2_value["curve"] = {"path": "live_curve_full-2.jsonl", "sha256": hashlib.sha256(full2_curve).hexdigest(), "bytes": len(full2_curve)}
    full2_manifest = normalizer.canonical_bytes(full2_value) + b"\n"

    assignment_ref = evidence.get("witness", {}).get("assignment") if isinstance(evidence.get("witness"), dict) else None
    if (not isinstance(assignment_ref, dict) or assignment_ref.get("path") != "product-evidence/assignment-witness.json" or
            assignment_ref.get("sha256") is None or assignment_ref.get("bytes") is None):
        _fail("evidence:assignment_witness_descriptor")
    assignment_source = live_root / assignment_ref["path"]
    assignment_desc = _descriptor(assignment_source, "assignment_witness")
    if assignment_desc["sha256"] != assignment_ref["sha256"] or assignment_desc["bytes"] != assignment_ref["bytes"]:
        _fail("evidence:assignment_witness_sha256")

    out.mkdir()
    (out / "product-evidence").mkdir()
    _write(out / "product-evidence" / "assignment-witness.json", _read(assignment_source, "assignment_witness")[0])
    for name in ("results.jsonl", "timing_full-1.jsonl", "timing_full-2.jsonl", "timing.jsonl"):
        _write(out / name, _read(live_root / name, name)[0])
    full1_curve = _read(live_root / "live_curve_full-1.jsonl", "live_curve_full_1")[0]
    full1_manifest = _read(live1_manifest, "live_manifest")[0]
    _write(out / "live_curve_full-1.jsonl", full1_curve)
    _write(out / "live_curve_manifest_full-1.json", full1_manifest)
    _write(out / "live_curve_full-2.jsonl", full2_curve)
    _write(out / "live_curve_manifest_full-2.json", full2_manifest)
    _write(out / "live_curve.jsonl", full1_curve)
    _write(out / "live_curve_manifest.json", full1_manifest)

    # Normalizer reads only the two authenticated curves and writes fresh records.
    normalizer.normalize(predictive_full_1.absolute(), out / "live_curve_manifest_full-1.json", out / "records-full-1.jsonl")
    normalizer.normalize(predictive_full_2.absolute(), out / "live_curve_manifest_full-2.json", out / "records-full-2.jsonl")
    live_artifacts = [normalizer._load_manifest(out / f"live_curve_manifest_{run}.json", "live") for run in ("full-1", "full-2")]
    _write(out / "records.jsonl", b"".join(normalizer.canonical_bytes(normalizer._normalized_record("live", item)) + b"\n" for item in live_artifacts))

    source_files = {
        "product_log": product_desc,
        "evidence": evidence_desc,
        "timing": timing_desc,
        "traces": trace_desc,
        "logs": log_desc,
        "input_authorities": input_authorities,
        "predictive_producers": producer_desc,
        "topology": plan_facts["topology"],
        "assignment_witness": assignment_desc,
        "plans": {run: _descriptor(path, f"predictive_{run}") for run, path in zip(("full-1", "full-2"),
                                                                                      (live_root / "product-evidence" / "predictive-plan.json",
                                                                                       live_root / "product-evidence" / "predictive-plan-full-2.json"), strict=True)},
    }
    retained = sorted(str(p.relative_to(live_root / "product-evidence")) for p in (live_root / "product-evidence").iterdir() if p.suffix in {".o", ".ii"})
    if len(retained) != 12:
        _fail(f"retained_sample:expected_12:{len(retained)}")
    authority = {"schema": "icecream-s8-live-salvage-v1", "status": "PASS",
                 "source_live_root": str(live_root), "source_run_id": evidence["run_id"],
                 "source_commit": EXPECTED_COMMIT, "source_tree": EXPECTED_TREE,
                 "raw_product_returncode": 0, "raw_product_returncode_basis": "pass_marker_and_zero_scheduler_daemon_completions",
                 "raw_product_log": product_desc, "source_files": source_files,
                 "retained_object_artifacts": {"mode": "sample", "count": len(retained), "paths": retained},
                 "replayed_outputs": ["live_curve_full-2.jsonl", "live_curve_manifest_full-2.json", "live_curve.jsonl", "live_curve_manifest.json", "records.jsonl", "experiment_manifest.json"],
                 "no_live_compile": True, "no_held_out_reads": True}
    _write(out / "salvage_manifest.json", normalizer.canonical_bytes(authority) + b"\n")
    experiment = {"schema": "icecream-s8-real-c1f1-live-runner-v2", "status": "PASS",
                  "cell": dict(zip(("corpus", "profile", "regime"), EXPECTED_CELL)),
                  "split": "calibration", "topology": "C1F1/100000", "suite": "C1F1/100000",
                  "depth": "full", "declared_count": EXPECTED_COUNT,
                  "runs": ["full-1", "full-2"], "same_service_state": True, "source_run_id": evidence["run_id"],
                  "source_commit": EXPECTED_COMMIT, "source_tree": EXPECTED_TREE,
                  "input_digest": evidence["input_digest"], "input_manifest_sha256": evidence["input_manifest_sha256"],
                  "topology_sha256": evidence["topology_sha256"],
                  "predictive_plan_sha256": evidence["predictive_plans"]["full-1"]["sha256"],
                  "predictive_plan_sha256_by_run": {run: evidence["predictive_plans"][run]["sha256"] for run in ("full-1", "full-2")},
                  "binary_sha256": evidence["binary_sha256"], "runner_sha256": evidence["runner"]["sha256"],
                  "execution_environment": evidence["execution_environment"], "runtime_image": evidence["runtime_image"],
                  "launch_identity": {"source_commit": EXPECTED_COMMIT, "source_tree": EXPECTED_TREE,
                                      "binary_sha256": evidence["binary_sha256"], "runner_sha256": evidence["runner"]["sha256"],
                                      "runtime_image": evidence["runtime_image"]},
                  "environment_preparation": evidence["environment_preparation"],
                  "execution_limits": evidence["execution_limits"], "diagnostic_policy": evidence["diagnostic_policy"],
                  "remote_compile_required": True, "replay": "immutable_salvage_from_retained_evidence", "raw_product_returncode": 0,
                  "raw_product_log_sha256": product_desc["sha256"], "curve_manifests": {"full-1": "live_curve_manifest_full-1.json", "full-2": "live_curve_manifest_full-2.json"},
                  "batch_windows": product_desc["windows"], "artifact_retention": {"mode": "sample", "sample_tus_per_run": 2},
                  "scheduling": {"mode": "relationship_ordered", "execution_slots": 1,
                                  "planned_admission_lanes_per_relationship": 1,
                                  "admission_lane_field": "planned_admission_lane",
                                  "physical_slot_observed": False,
                                  "source_admission": "per_relationship_source_commit_gate",
                                  "observed_batch_concurrency": product_desc["windows"]},
                  "relationship_count": 1, "slots_per_f": 1,
                  "assignment_witness": "product-evidence/assignment-witness.json", "evidence": "salvage_manifest.json",
                  "prewarm": False, "prewarm_evidence": None}
    _write(out / "experiment_manifest.json", normalizer.canonical_bytes(experiment) + b"\n")
    _post_validate_output(out, experiment, assignment_desc, evidence["environment_preparation"])
    return out


def salvage(live_root: Path, predictive_full_1: Path, predictive_full_2: Path, out: Path) -> Path:
    """Publish a complete sibling or remove the newly-created one on failure."""
    output = out.absolute()
    existed = output.exists() or output.is_symlink()
    try:
        return _salvage(live_root, predictive_full_1, predictive_full_2, output)
    except Exception:
        if not existed and output.is_dir() and not output.is_symlink():
            shutil.rmtree(output)
        raise


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--live-root", type=Path, required=True)
    parser.add_argument("--predictive-full-1", type=Path, required=True)
    parser.add_argument("--predictive-full-2", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        salvage(args.live_root, args.predictive_full_1, args.predictive_full_2, args.out)
    except (SalvageError, normalizer.NormalizationError) as exc:
        print(f"s8_live_salvage: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
