#!/usr/bin/env python3
"""Produce the trace-free RAW_II control curve from explicit wire inputs.

RAW_II is the whole-legacy control arm.  This producer does not call the
compressed predictive engine and does not alias P29.  The legacy-wire witness
supplies the exact C-to-F frame accounting (CompileFile + FileChunk + End); a
separately scoped control engine supplies its F-to-C and elapsed estimates.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import stat
import subprocess
import sys
from pathlib import Path
from typing import Any

try:
    from . import s8_depth_runner as depth_runner
    from . import s8_predictive_live_normalizer as normalizer
    from .s8_schema import CORPORA, CONTROL_PROFILES, CURRENT_SEMANTICS, REGIMES, SPLITS
except ImportError:  # pragma: no cover
    import s8_depth_runner as depth_runner
    import s8_predictive_live_normalizer as normalizer
    from s8_schema import CORPORA, CONTROL_PROFILES, CURRENT_SEMANTICS, REGIMES, SPLITS

DepthPlanError = depth_runner.DepthPlanError
build_schedule = depth_runner.build_schedule


SCHEMA = "icecream-s8-raw-ii-predictive-producer-v1"
ENGINE_SCHEMA = "icecream-s8-raw-ii-control-engine-v1"
WITNESS_SCHEMA = "icecream-s8-raw-ii-legacy-wire-witness-v1"
FORMULA = {
    "name": "legacy-filechunk-wire-v1",
    "c_to_f": "compile_file_bytes+file_chunk_bytes+end_bytes",
}
ENGINE_SCOPE = "raw_ii_control_engine"
MAX_BYTES = 64 * 1024 * 1024
HEX64 = re.compile(r"^[0-9a-fA-F]{64}$")
HEX40 = re.compile(r"^[0-9a-fA-F]{40}$")


class RawIIError(ValueError):
    """A RAW_II control input is absent, ambiguous, or malformed."""


def _canonical(value: object) -> bytes:
    return (json.dumps(value, sort_keys=True, separators=(",", ":"),
                       ensure_ascii=True, allow_nan=False) + "\n").encode("ascii")


def _read(path: Path, label: str) -> tuple[dict[str, Any], dict[str, object]]:
    try:
        info = path.lstat()
        if (stat.S_ISLNK(info.st_mode) or not stat.S_ISREG(info.st_mode) or
                info.st_nlink != 1):
            raise RawIIError(f"{label}:not_private_regular_file")
        raw = path.read_bytes()
    except OSError as exc:
        raise RawIIError(f"{label}:unavailable:{path}") from exc
    if len(raw) > MAX_BYTES:
        raise RawIIError(f"{label}:too_large")
    try:
        value = json.loads(
            raw.decode("utf-8"), object_pairs_hook=_unique_keys,
            parse_constant=lambda token: (_ for _ in ()).throw(
                RawIIError(f"{label}:non_finite_json")))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise RawIIError(f"{label}:invalid_json") from exc
    if not isinstance(value, dict):
        raise RawIIError(f"{label}:object_required")
    return value, {"path": str(path.resolve()), "bytes": len(raw),
                   "sha256": hashlib.sha256(raw).hexdigest()}


def _unique_keys(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise RawIIError(f"duplicate_json_key:{key}")
        result[key] = value
    return result


def _hex(value: object, label: str, pattern: re.Pattern[str] = HEX64) -> str:
    if (not isinstance(value, str) or pattern.fullmatch(value) is None or
            int(value, 16) == 0):
        raise RawIIError(f"{label}:invalid_digest")
    return value.lower()


def _relative(value: object, label: str) -> str:
    if (not isinstance(value, str) or not value or os.path.isabs(value) or
            any(part in ("", ".", "..") for part in Path(value).parts)):
        raise RawIIError(f"{label}:relative_path_invalid")
    return value


def _occurrence_key(ordinal: object, source_relative: object,
                    digest: object, source_bytes: object, label: str
                    ) -> tuple[int, str, str, int]:
    if type(ordinal) is not int or ordinal < 0:
        raise RawIIError(f"{label}.ordinal:invalid")
    relative = _relative(source_relative, f"{label}.source_relative")
    normalized_digest = _hex(digest, f"{label}.source_sha256")
    if type(source_bytes) is not int or source_bytes < 0:
        raise RawIIError(f"{label}.source_bytes:invalid")
    return ordinal, relative, normalized_digest, source_bytes


def _nonnegative(value: object, label: str) -> int:
    if type(value) is not int or value < 0:
        raise RawIIError(f"{label}:nonnegative_integer_required")
    return value


def _positive(value: object, label: str) -> int:
    if type(value) is not int or value <= 0:
        raise RawIIError(f"{label}:positive_integer_required")
    return value


def _cell(value: object, label: str) -> dict[str, str]:
    if (not isinstance(value, dict) or set(value) != {"corpus", "profile", "regime"} or
            value.get("corpus") not in CORPORA or
            value.get("profile") not in CONTROL_PROFILES or
            value.get("regime") not in REGIMES):
        raise RawIIError(f"{label}:cell_invalid")
    return {key: str(value[key]) for key in ("corpus", "profile", "regime")}


def _wire_total(value: object, label: str) -> int:
    if (not isinstance(value, dict) or
            set(value) != {"compile_file_bytes", "file_chunk_bytes", "end_bytes", "total_bytes"}):
        raise RawIIError(f"{label}:fields_invalid")
    parts = [_positive(value[key], f"{label}.{key}")
             for key in ("compile_file_bytes", "file_chunk_bytes", "end_bytes")]
    total = _positive(value["total_bytes"], f"{label}.total_bytes")
    if sum(parts) != total:
        raise RawIIError(f"{label}:formula_mismatch")
    return total


def _load_witness(path: Path, cell: dict[str, str]) -> tuple[dict[tuple[int, str, str, int], dict[str, object]], dict[str, object]]:
    value, facts = _read(path, "raw_ii_witness")
    if (value.get("schema") != WITNESS_SCHEMA or value.get("semantics") != CURRENT_SEMANTICS or
            value.get("cell") != cell or value.get("split") != SPLITS[cell["corpus"]] or
            value.get("formula") != FORMULA):
        raise RawIIError("raw_ii_witness:scope_or_formula_invalid")
    rows = value.get("rows")
    if not isinstance(rows, list) or not rows:
        raise RawIIError("raw_ii_witness:rows_invalid")
    result: dict[tuple[int, str, str, int], dict[str, object]] = {}
    for index, row in enumerate(rows):
        if (not isinstance(row, dict) or set(row) != {
                "ordinal", "source_relative", "source_sha256", "source_bytes", "c_to_f"}):
            raise RawIIError(f"raw_ii_witness:row_invalid:{index}")
        key = _occurrence_key(row["ordinal"], row["source_relative"],
                              row["source_sha256"], row["source_bytes"],
                              f"raw_ii_witness.row[{index}]")
        if key in result:
            raise RawIIError("raw_ii_witness:duplicate_occurrence")
        result[key] = {"source_bytes": key[3],
                       "c_to_f_bytes": _wire_total(
                           row["c_to_f"], f"raw_ii_witness.row[{index}].c_to_f")}
    return result, facts


def _load_engine(path: Path, cell: dict[str, str]) -> tuple[str, dict[tuple[int, str, str, int], tuple[int, int, int, int]], dict[str, object]]:
    value, facts = _read(path, "raw_ii_engine_manifest")
    if (value.get("schema") != ENGINE_SCHEMA or value.get("semantics") != CURRENT_SEMANTICS or
            value.get("cell") != cell or value.get("split") != SPLITS[cell["corpus"]] or
            value.get("control_baseline") != normalizer.CONTROL_BASELINE or
            value.get("engine_scope") != ENGINE_SCOPE):
        raise RawIIError("raw_ii_engine_manifest:scope_invalid")
    model_id = value.get("model_id")
    if not isinstance(model_id, str) or not re.fullmatch(r"[A-Za-z0-9_.-]+", model_id):
        raise RawIIError("raw_ii_engine_manifest:model_id_invalid")
    rows = value.get("rows")
    if not isinstance(rows, list) or not rows:
        raise RawIIError("raw_ii_engine_manifest:rows_invalid")
    result: dict[tuple[int, str, str, int], tuple[int, int, int, int]] = {}
    for index, row in enumerate(rows):
        if (not isinstance(row, dict) or set(row) != {
                "ordinal", "source_relative", "source_sha256", "source_bytes",
                "f_to_c_bytes", "source_service_ns", "execution_service_ns",
                "elapsed_ns"}):
            raise RawIIError(f"raw_ii_engine_manifest:row_invalid:{index}")
        key = _occurrence_key(row["ordinal"], row["source_relative"],
                              row["source_sha256"], row["source_bytes"],
                              f"raw_ii_engine_manifest.row[{index}]")
        if key in result:
            raise RawIIError("raw_ii_engine_manifest:duplicate_occurrence")
        f_to_c = _positive(row["f_to_c_bytes"], f"raw_ii_engine_manifest.row[{index}].f_to_c_bytes")
        source_service = _nonnegative(
            row["source_service_ns"],
            f"raw_ii_engine_manifest.row[{index}].source_service_ns")
        execution_service = _nonnegative(
            row["execution_service_ns"],
            f"raw_ii_engine_manifest.row[{index}].execution_service_ns")
        elapsed = _positive(row["elapsed_ns"],
                            f"raw_ii_engine_manifest.row[{index}].elapsed_ns")
        if source_service + execution_service != elapsed:
            raise RawIIError(f"raw_ii_engine_manifest.row[{index}].service_formula_mismatch")
        result[key] = (f_to_c, source_service, execution_service, elapsed)
    return model_id, result, facts


def _load_plan(path: Path, cell: dict[str, str], depth: str) -> tuple[dict[str, Any], list[dict[str, Any]], dict[str, object]]:
    value, facts = _read(path, "plan")
    if value.get("schema") != "icecream-s8-depth-run-plan-v1" or value.get("semantics") != CURRENT_SEMANTICS:
        raise RawIIError("plan:schema_invalid")
    if value.get("cell") != cell or value.get("split") != SPLITS[cell["corpus"]]:
        raise RawIIError("plan:cell_mismatch")
    request = value.get("request")
    expected_depth: object = int(depth) if depth.isdigit() else depth
    if not isinstance(request, dict) or request.get("depth") != expected_depth:
        raise RawIIError("plan:depth_mismatch")
    contract = value.get("execution_contract")
    if (not isinstance(contract, dict) or contract.get("status") != "READY_RAW_II_CONTROL" or
            contract.get("producer") != "farmharness.s8_raw_ii_predictive_producer" or
            contract.get("control_baseline") != normalizer.CONTROL_BASELINE):
        raise RawIIError("plan:raw_ii_control_contract_invalid")
    inputs = value.get("inputs")
    scheduling = value.get("scheduling")
    if not isinstance(inputs, list) or not inputs or not isinstance(scheduling, dict):
        raise RawIIError("plan:inputs_or_scheduling_invalid")
    # Re-read the source authority and every selected occurrence immediately
    # before prediction.  A plan is a digest-bearing handoff, not permission
    # to use a source file that changed after planning.
    source_manifest = value.get("source_manifest")
    source_root = value.get("source_root")
    if (not isinstance(source_manifest, dict) or
            set(source_manifest) != {"path", "sha256", "bytes", "entries"} or
            not isinstance(source_root, str) or not source_root):
        raise RawIIError("plan:source_manifest_invalid")
    source_path = Path(str(source_manifest["path"]))
    try:
        actual_inputs, actual_manifest = depth_runner._manifest_inputs(
            source_path, Path(source_root), "source_manifest")
    except (DepthPlanError, OSError) as exc:
        raise RawIIError("plan:source_manifest_unavailable") from exc
    if (actual_manifest["sha256"] != source_manifest.get("sha256") or
            actual_manifest["bytes"] != source_manifest.get("bytes") or
            len(actual_inputs) != source_manifest.get("entries")):
        raise RawIIError("plan:source_manifest_changed")
    request = value.get("request")
    if not isinstance(request, dict) or request.get("depth") not in (100, 200, "full", "repeat-full"):
        raise RawIIError("plan:request_invalid")
    requested_depth = request["depth"]
    try:
        expected_inputs, expected_selection = depth_runner.select_inputs(
            actual_inputs, requested_depth)
    except DepthPlanError as exc:
        raise RawIIError("plan:input_selection_invalid") from exc
    if inputs != expected_inputs:
        raise RawIIError("plan:input_sequence_not_declared_selection")
    if request.get("source_selection") != expected_selection:
        raise RawIIError("plan:source_selection_invalid")
    for index, item in enumerate(inputs):
        if (not isinstance(item, dict) or set(item) != {
                "ordinal", "path", "source_relative", "sha256", "bytes"} or
                item["ordinal"] != index):
            raise RawIIError(f"plan:input_descriptor_invalid:{index}")
        key = _occurrence_key(item["ordinal"], item["source_relative"],
                              item["sha256"], item["bytes"],
                              f"plan.inputs[{index}]")
        expected = expected_inputs[index]
        if key != _occurrence_key(expected["ordinal"], expected["source_relative"],
                                   expected["sha256"], expected["bytes"],
                                   f"plan.expected[{index}]") or item["path"] != expected["path"]:
            raise RawIIError(f"plan:input_descriptor_mismatch:{index}")
        try:
            facts = depth_runner._digest(Path(item["path"]), f"plan.input[{index}]")
        except DepthPlanError as exc:
            raise RawIIError(f"plan:input_unavailable:{index}") from exc
        if facts["sha256"] != key[2] or facts["bytes"] != key[3]:
            raise RawIIError(f"plan:input_changed:{index}")
    try:
        expected = build_schedule(inputs, str(scheduling["topology"]), str(scheduling["depth_class"]))
    except (KeyError, DepthPlanError) as exc:
        raise RawIIError("plan:scheduling_invalid") from exc
    if scheduling != expected:
        raise RawIIError("plan:scheduling_authentication_failed")
    return value, inputs, {"path": facts["path"], "bytes": facts["bytes"], "sha256": facts["sha256"]}


def _product_identity(root: Path) -> dict[str, str]:
    """Read the explicit product checkout's real HEAD and tree identity."""
    if not root.is_absolute() or root.is_symlink() or not root.is_dir():
        raise RawIIError("product_root:unavailable")
    try:
        top = subprocess.run(["git", "-C", str(root), "rev-parse", "--show-toplevel"],
                             check=True, capture_output=True, text=True, timeout=15).stdout.strip()
        head = subprocess.run(["git", "-C", str(root), "rev-parse", "HEAD"],
                              check=True, capture_output=True, text=True, timeout=15).stdout.strip()
        tree = subprocess.run(["git", "-C", str(root), "rev-parse", "HEAD^{tree}"],
                              check=True, capture_output=True, text=True, timeout=15).stdout.strip()
        status = subprocess.run(["git", "-C", str(root), "status", "--porcelain",
                                 "--untracked-files=no"], check=True, capture_output=True,
                                text=True, timeout=15).stdout
    except (OSError, subprocess.SubprocessError) as exc:
        raise RawIIError("product_root:git_identity_unavailable") from exc
    if (Path(top).resolve() != root.resolve() or not HEX40.fullmatch(head) or
            not HEX40.fullmatch(tree) or status):
        raise RawIIError("product_root:git_identity_invalid")
    return {"root": str(root.resolve()), "head": head.lower(), "tree": tree.lower()}


def produce(plan_path: Path, witness_path: Path, engine_path: Path, output_dir: Path,
            depth: str, product_root: Path) -> dict[str, object]:
    cell_hint = None
    try:
        plan_raw, _facts = _read(plan_path, "plan")
        cell_hint = _cell(plan_raw.get("cell"), "plan")
    except RawIIError:
        raise
    plan, inputs, plan_facts = _load_plan(plan_path, cell_hint, depth)
    witness, witness_facts = _load_witness(witness_path, cell_hint)
    model_id, engine_rows, engine_facts = _load_engine(engine_path, cell_hint)
    result = plan.get("result")
    if (not isinstance(result, dict) or
            not isinstance(result.get("directory"), str) or
            not Path(result["directory"]).is_absolute() or
            Path(result["directory"]).resolve() != output_dir.resolve()):
        raise RawIIError("plan:result_directory_mismatch")
    product = _product_identity(product_root)
    seen: set[tuple[int, str, str, int]] = set()
    for index, item in enumerate(inputs):
        key = _occurrence_key(item.get("ordinal"), item.get("source_relative"),
                              item.get("sha256"), item.get("bytes"),
                              f"plan.inputs[{index}]")
        if key not in witness or key not in engine_rows:
            raise RawIIError(f"raw_ii:control_input_missing:{index}")
        if item.get("bytes") != witness[key]["source_bytes"]:  # type: ignore[index]
            raise RawIIError(f"raw_ii:witness_source_bytes_mismatch:{index}")
        seen.add(key)
    if set(witness) != seen or set(engine_rows) != seen:
        raise RawIIError("raw_ii:control_input_set_mismatch")
    if output_dir.exists() or output_dir.is_symlink() or not output_dir.is_absolute():
        raise RawIIError("output_dir:must_be_new_absolute_directory")
    scheduling = plan["scheduling"]
    cumulative_c = cumulative_f = cumulative_channel = 0
    cumulative_elapsed = 0
    serial_service = 0
    slots = scheduling.get("global_slots")
    slots_per_f = scheduling.get("slots_per_f")
    assignments = scheduling.get("assignments")
    if (type(slots) is not int or slots <= 0 or type(slots_per_f) is not int or
            slots_per_f <= 0 or not isinstance(assignments, list) or
            len(assignments) != len(inputs)):
        raise RawIIError("plan:scheduling_runtime_invalid")
    slot_available = [0] * slots
    relationship_ready: dict[int, int] = {}
    rows: list[dict[str, object]] = []
    for ordinal, item in enumerate(inputs):
        key = _occurrence_key(item["ordinal"], item["source_relative"],
                              item["sha256"], item["bytes"],
                              f"plan.inputs[{ordinal}]")
        c_to_f = int(witness[key]["c_to_f_bytes"])
        f_to_c, source_service, execution_service, elapsed = engine_rows[key]
        assignment = assignments[ordinal]
        if (not isinstance(assignment, dict) or assignment.get("ordinal") != ordinal or
                type(assignment.get("global_slot")) is not int or
                not 0 <= assignment["global_slot"] < slots or
                type(assignment.get("f_relationship")) is not int or
                assignment["f_relationship"] != assignment["global_slot"] // slots_per_f or
                assignment.get("per_f_slot") != assignment["global_slot"] % slots_per_f):
            raise RawIIError(f"plan:scheduling_assignment_invalid:{ordinal}")
        global_slot = assignment["global_slot"]
        relationship = assignment["f_relationship"]
        slot_admission = slot_available[global_slot]
        relationship_before = relationship_ready.get(relationship, 0)
        source_start = max(slot_admission, relationship_before)
        source_finish = source_start + source_service
        execution_start = source_finish
        finish = execution_start + execution_service
        slot_available[global_slot] = finish
        relationship_ready[relationship] = source_finish
        cumulative_c += c_to_f
        cumulative_f += f_to_c
        cumulative_channel += c_to_f + f_to_c
        cumulative_elapsed = max(cumulative_elapsed, finish)
        serial_service += elapsed
        rows.append({
            "step": ordinal,
            "tu_id": f"{cell_hint['corpus']}-{cell_hint['regime']}-tu-{ordinal:06d}-{key[2][:12]}",
            "cell": cell_hint,
            "model_id": model_id,
            "occurrence": {"ordinal": key[0], "source_relative": key[1],
                           "sha256": key[2], "bytes": key[3]},
            "cumulative": {"C_TO_F_bytes": cumulative_c, "F_TO_C_bytes": cumulative_f,
                            "channel_bytes": cumulative_channel, "elapsed_ns": cumulative_elapsed,
                            "throughput_bytes_per_s": cumulative_channel * 1_000_000_000 / cumulative_elapsed},
            "scheduling": {"global_slot": global_slot, "f_relationship": relationship,
                           "per_f_slot": assignment["per_f_slot"],
                           "source_start_ns": source_start, "source_finish_ns": source_finish,
                           "source_service_ns": source_service,
                           "execution_start_ns": execution_start,
                           "execution_finish_ns": finish,
                           "execution_service_ns": execution_service,
                           "service_ns": elapsed},
        })
    curve_raw = b"".join(_canonical(row) for row in rows)
    input_digest = hashlib.sha256(_canonical({"source_manifest_sha256": plan["source_manifest"]["sha256"],
                                              "inputs": inputs})).hexdigest()
    topology_digest = hashlib.sha256(_canonical(scheduling)).hexdigest()
    identity = {"corpus": cell_hint["corpus"], "profile": "RAW_II", "regime": cell_hint["regime"],
                "split": SPLITS[cell_hint["corpus"]], "run_id": f"{output_dir.name}-{plan_facts['sha256'][:12]}",
                "source_commit": product["head"], "source_tree": product["tree"],
                "input_digest": input_digest, "topology_digest": topology_digest, "model_id": model_id}
    comparison = normalizer.comparison_descriptor(plan_facts["sha256"], scheduling)
    manifest = {"schema": normalizer.MANIFEST_SCHEMA, "identity": identity, "comparison": comparison,
                "control_baseline": normalizer.CONTROL_BASELINE,
                "units": {"point": "step", "channel_bytes": "bytes", "elapsed_ns": "ns",
                          "C_TO_F_bytes": "bytes", "F_TO_C_bytes": "bytes",
                          "throughput_bytes_per_s": "bytes_per_s"},
                "curve": {"path": "predictive_sim.jsonl", "sha256": hashlib.sha256(curve_raw).hexdigest(),
                          "bytes": len(curve_raw)},
                "provenance": {"mode": "predictive_sim", "producer": SCHEMA, "trace_free": True}}
    producer = {"schema": SCHEMA, "semantics": CURRENT_SEMANTICS, "cell": cell_hint,
                "control_baseline": normalizer.CONTROL_BASELINE, "engine_scope": ENGINE_SCOPE,
                "plan": plan_facts, "witness": witness_facts, "engine": engine_facts,
                "formula": FORMULA, "identity": identity, "product_git": product,
                "scheduling": {"topology": scheduling["topology"],
                                "global_slots": scheduling["global_slots"],
                                "f_relationships": scheduling["f_relationships"],
                                "slots_per_f": scheduling["slots_per_f"],
                                "makespan_ns": cumulative_elapsed,
                                "serial_service_ns": serial_service}}
    output_dir.mkdir(parents=True)
    (output_dir / "predictive_sim.jsonl").write_bytes(curve_raw)
    (output_dir / "predictive_curve_manifest.json").write_bytes(_canonical(manifest))
    (output_dir / "producer_manifest.json").write_bytes(_canonical(producer))
    return producer


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--plan", type=Path, required=True)
    parser.add_argument("--raw-ii-witness", type=Path, required=True)
    parser.add_argument("--engine-manifest", type=Path, required=True)
    parser.add_argument("--product-root", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--repeat-plan", type=Path)
    parser.add_argument("--depth", choices=("100", "200", "full"), required=True)
    args = parser.parse_args(argv)
    try:
        produce(args.plan.absolute(), args.raw_ii_witness.absolute(), args.engine_manifest.absolute(),
                args.output_dir.absolute(), args.depth, args.product_root.absolute())
        if args.repeat_plan is not None:
            repeat_value, _repeat_facts = _read(args.repeat_plan.absolute(), "repeat_plan")
            repeat_result = repeat_value.get("result")
            if (not isinstance(repeat_result, dict) or
                    not isinstance(repeat_result.get("directory"), str)):
                raise RawIIError("repeat_plan:result_directory_invalid")
            produce(args.repeat_plan.absolute(), args.raw_ii_witness.absolute(),
                    args.engine_manifest.absolute(), Path(repeat_result["directory"]),
                    "repeat-full", args.product_root.absolute())
    except RawIIError as exc:
        print(f"s8_raw_ii_predictive_producer: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
