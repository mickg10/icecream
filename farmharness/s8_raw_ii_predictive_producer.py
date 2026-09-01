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
import re
import stat
import sys
from pathlib import Path
from typing import Any

try:
    from . import s8_predictive_live_normalizer as normalizer
    from .s8_depth_runner import DepthPlanError, build_schedule
    from .s8_schema import CORPORA, CONTROL_PROFILES, CURRENT_SEMANTICS, REGIMES, SPLITS
except ImportError:  # pragma: no cover
    import s8_predictive_live_normalizer as normalizer
    from s8_depth_runner import DepthPlanError, build_schedule
    from s8_schema import CORPORA, CONTROL_PROFILES, CURRENT_SEMANTICS, REGIMES, SPLITS


SCHEMA = "icecream-s8-raw-ii-predictive-producer-v1"
ENGINE_SCHEMA = "icecream-s8-raw-ii-control-engine-v1"
WITNESS_SCHEMA = "icecream-s8-raw-ii-legacy-wire-witness-v1"
FORMULA = {
    "name": "legacy-filechunk-wire-v1",
    "c_to_f": "compile_file_bytes+file_chunk_bytes+end_bytes",
}
ENGINE_SCOPE = "raw_ii_control_engine"
MAX_BYTES = 64 * 1024 * 1024


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
        value = json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise RawIIError(f"{label}:invalid_json") from exc
    if not isinstance(value, dict):
        raise RawIIError(f"{label}:object_required")
    return value, {"path": str(path.resolve()), "bytes": len(raw),
                   "sha256": hashlib.sha256(raw).hexdigest()}


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


def _load_witness(path: Path, cell: dict[str, str]) -> tuple[dict[str, object], dict[str, object]]:
    value, facts = _read(path, "raw_ii_witness")
    if (value.get("schema") != WITNESS_SCHEMA or value.get("semantics") != CURRENT_SEMANTICS or
            value.get("cell") != cell or value.get("split") != SPLITS[cell["corpus"]] or
            value.get("formula") != FORMULA):
        raise RawIIError("raw_ii_witness:scope_or_formula_invalid")
    rows = value.get("rows")
    if not isinstance(rows, list) or not rows:
        raise RawIIError("raw_ii_witness:rows_invalid")
    result: dict[str, object] = {}
    for index, row in enumerate(rows):
        if (not isinstance(row, dict) or set(row) != {"source_sha256", "source_bytes", "c_to_f"} or
                not isinstance(row.get("source_sha256"), str) or
                len(row["source_sha256"]) != 64):
            raise RawIIError(f"raw_ii_witness:row_invalid:{index}")
        digest = row["source_sha256"].lower()
        _positive(row["source_bytes"], f"raw_ii_witness.row[{index}].source_bytes")
        if digest in result:
            raise RawIIError("raw_ii_witness:duplicate_source")
        result[digest] = {"source_bytes": row["source_bytes"],
                          "c_to_f_bytes": _wire_total(row["c_to_f"],
                                                       f"raw_ii_witness.row[{index}].c_to_f")}
    return result, facts


def _load_engine(path: Path, cell: dict[str, str]) -> tuple[str, dict[str, tuple[int, int]], dict[str, object]]:
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
    result: dict[str, tuple[int, int]] = {}
    for index, row in enumerate(rows):
        if (not isinstance(row, dict) or set(row) != {"source_sha256", "f_to_c_bytes", "elapsed_ns"} or
                not isinstance(row.get("source_sha256"), str) or len(row["source_sha256"]) != 64):
            raise RawIIError(f"raw_ii_engine_manifest:row_invalid:{index}")
        digest = row["source_sha256"].lower()
        if digest in result:
            raise RawIIError("raw_ii_engine_manifest:duplicate_source")
        f_to_c = _positive(row["f_to_c_bytes"], f"raw_ii_engine_manifest.row[{index}].f_to_c_bytes")
        elapsed = _positive(row["elapsed_ns"], f"raw_ii_engine_manifest.row[{index}].elapsed_ns")
        result[digest] = (f_to_c, elapsed)
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
    try:
        expected = build_schedule(inputs, str(scheduling["topology"]), str(scheduling["depth_class"]))
    except (KeyError, DepthPlanError) as exc:
        raise RawIIError("plan:scheduling_invalid") from exc
    if scheduling != expected:
        raise RawIIError("plan:scheduling_authentication_failed")
    return value, inputs, {"path": facts["path"], "bytes": facts["bytes"], "sha256": facts["sha256"]}


def produce(plan_path: Path, witness_path: Path, engine_path: Path, output_dir: Path,
            depth: str) -> dict[str, object]:
    cell_hint = None
    try:
        plan_raw, _facts = _read(plan_path, "plan")
        cell_hint = _cell(plan_raw.get("cell"), "plan")
    except RawIIError:
        raise
    plan, inputs, plan_facts = _load_plan(plan_path, cell_hint, depth)
    witness, witness_facts = _load_witness(witness_path, cell_hint)
    model_id, engine_rows, engine_facts = _load_engine(engine_path, cell_hint)
    seen = set()
    for index, item in enumerate(inputs):
        digest = str(item.get("sha256", "")).lower()
        if digest not in witness or digest not in engine_rows:
            raise RawIIError(f"raw_ii:control_input_missing:{index}")
        if item.get("bytes") != witness[digest]["source_bytes"]:  # type: ignore[index]
            raise RawIIError(f"raw_ii:witness_source_bytes_mismatch:{index}")
        seen.add(digest)
    if set(witness) != seen or set(engine_rows) != seen:
        raise RawIIError("raw_ii:control_input_set_mismatch")
    if output_dir.exists() or output_dir.is_symlink() or not output_dir.is_absolute():
        raise RawIIError("output_dir:must_be_new_absolute_directory")
    scheduling = plan["scheduling"]
    cumulative_c = cumulative_f = cumulative_channel = cumulative_elapsed = 0
    rows: list[dict[str, object]] = []
    for ordinal, item in enumerate(inputs):
        digest = str(item["sha256"]).lower()
        c_to_f = int(witness[digest]["c_to_f_bytes"])  # type: ignore[index]
        f_to_c, elapsed = engine_rows[digest]
        cumulative_c += c_to_f
        cumulative_f += f_to_c
        cumulative_channel += c_to_f + f_to_c
        cumulative_elapsed += elapsed
        rows.append({
            "step": ordinal,
            "tu_id": f"{cell_hint['corpus']}-{cell_hint['regime']}-tu-{ordinal:06d}-{digest[:12]}",
            "cell": cell_hint,
            "model_id": model_id,
            "cumulative": {"C_TO_F_bytes": cumulative_c, "F_TO_C_bytes": cumulative_f,
                            "channel_bytes": cumulative_channel, "elapsed_ns": cumulative_elapsed,
                            "throughput_bytes_per_s": cumulative_channel * 1_000_000_000 / cumulative_elapsed},
        })
    curve_raw = b"".join(_canonical(row) for row in rows)
    input_digest = hashlib.sha256(_canonical({"source_manifest_sha256": plan["source_manifest"]["sha256"],
                                              "inputs": inputs})).hexdigest()
    source_commit = hashlib.sha1(str(plan["source_manifest"]["sha256"]).encode()).hexdigest()
    source_tree = hashlib.sha1(str(plan["source_root"]).encode()).hexdigest()
    topology_digest = hashlib.sha256(_canonical(scheduling)).hexdigest()
    identity = {"corpus": cell_hint["corpus"], "profile": "RAW_II", "regime": cell_hint["regime"],
                "split": SPLITS[cell_hint["corpus"]], "run_id": f"{output_dir.name}-{plan_facts['sha256'][:12]}",
                "source_commit": source_commit, "source_tree": source_tree,
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
                "formula": FORMULA, "identity": identity}
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
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--repeat-plan", type=Path)
    parser.add_argument("--depth", choices=("100", "200", "full"), required=True)
    args = parser.parse_args(argv)
    try:
        produce(args.plan.absolute(), args.raw_ii_witness.absolute(), args.engine_manifest.absolute(),
                args.output_dir.absolute(), args.depth)
        if args.repeat_plan is not None:
            repeat_value, _repeat_facts = _read(args.repeat_plan.absolute(), "repeat_plan")
            repeat_result = repeat_value.get("result")
            if (not isinstance(repeat_result, dict) or
                    not isinstance(repeat_result.get("directory"), str)):
                raise RawIIError("repeat_plan:result_directory_invalid")
            produce(args.repeat_plan.absolute(), args.raw_ii_witness.absolute(),
                    args.engine_manifest.absolute(), Path(repeat_result["directory"]),
                    "repeat-full")
    except RawIIError as exc:
        print(f"s8_raw_ii_predictive_producer: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
