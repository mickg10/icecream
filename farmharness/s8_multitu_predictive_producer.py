#!/usr/bin/env python3
"""Produce one authenticated, trace-free predictive curve for an S8 depth plan.

The depth plan is the authority for the ordered translation-unit sequence.
Each listed file is re-hashed immediately before it is passed to the current
S8 simulator, so the output cannot silently become a different corpus.  The
same authenticated topology object is used for every point and startup state
is carried across the sequence.  This module emits no live observations and
does not invoke Docker or the live product.
"""

from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import os
import re
import stat
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any

try:
    from . import s8_depth_runner as depth_runner
    from . import s8_predictive_engine as engine
    from .s8_schema import CURRENT_SEMANTICS, CORPORA, PROFILES, REGIMES, SPLITS
    from .s8_predictive_live_normalizer import MANIFEST_SCHEMA as CURVE_MANIFEST_SCHEMA
except ImportError:  # pragma: no cover
    import s8_depth_runner as depth_runner
    import s8_predictive_engine as engine
    from s8_schema import CURRENT_SEMANTICS, CORPORA, PROFILES, REGIMES, SPLITS
    from s8_predictive_live_normalizer import MANIFEST_SCHEMA as CURVE_MANIFEST_SCHEMA


SCHEMA = "icecream-s8-multitu-predictive-producer-v1"
SEMANTICS = CURRENT_SEMANTICS
PRODUCER = "s8-multitu-predictive-producer-v1"
PLAN_SCHEMA = depth_runner.SCHEMA
HEX40 = re.compile(r"^[0-9a-fA-F]{40}$")
HEX64 = re.compile(r"^[0-9a-fA-F]{64}$")
SAFE_ID = re.compile(r"^[A-Za-z0-9_.-]+$")
MAX_PLAN_BYTES = 8 * 1024 * 1024
MAX_INPUT_BYTES = 512 * 1024 * 1024
MAX_PRODUCT_OUTPUT_BYTES = 64 * 1024 * 1024


class MultiTUPredictiveError(ValueError):
    """Raised when a depth plan or simulator input is not authenticated."""


class _XXH128(ctypes.Structure):
    _fields_ = [("low64", ctypes.c_uint64), ("high64", ctypes.c_uint64)]


try:
    _XXHASH = ctypes.CDLL("libxxhash.so.0")
    _XXH3_128 = _XXHASH.XXH3_128bits
    _XXH3_128.argtypes = (ctypes.c_void_p, ctypes.c_size_t)
    _XXH3_128.restype = _XXH128
except (AttributeError, OSError):  # pragma: no cover - fail closed if product dep is absent.
    _XXH3_128 = None


def _product_digest128(raw: bytes) -> str:
    """Compute the product's canonical XXH3-128 digest without a Python package."""
    if _XXH3_128 is None:
        raise MultiTUPredictiveError("product_simulator:libxxhash_unavailable")
    buffer = ctypes.create_string_buffer(raw)
    digest = _XXH3_128(buffer, len(raw))
    return f"{digest.high64:016x}{digest.low64:016x}"


def canonical_bytes(value: object) -> bytes:
    try:
        return json.dumps(value, sort_keys=True, separators=(",", ":"),
                          ensure_ascii=True, allow_nan=False).encode("ascii")
    except (TypeError, ValueError, OverflowError, UnicodeError) as exc:
        raise MultiTUPredictiveError("canonical_json:invalid_value") from exc


def _unique_keys(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise MultiTUPredictiveError(f"duplicate_json_key:{key}")
        result[key] = value
    return result


def _parse(raw: bytes, label: str) -> object:
    try:
        return json.loads(raw.decode("utf-8"), object_pairs_hook=_unique_keys,
                          parse_constant=lambda token: (_ for _ in ()).throw(
                              MultiTUPredictiveError(f"{label}:non_finite")))
    except MultiTUPredictiveError:
        raise
    except (UnicodeError, json.JSONDecodeError) as exc:
        raise MultiTUPredictiveError(f"{label}:invalid_json") from exc


def _snapshot(path: Path, label: str, limit: int) -> tuple[bytes, dict[str, object]]:
    try:
        info = path.lstat()
    except OSError as exc:
        raise MultiTUPredictiveError(f"{label}:unavailable:{path}") from exc
    if (stat.S_ISLNK(info.st_mode) or not stat.S_ISREG(info.st_mode) or
            info.st_nlink != 1):
        raise MultiTUPredictiveError(f"{label}:not_private_regular_file:{path}")
    flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
    try:
        fd = os.open(path, flags)
    except OSError as exc:
        raise MultiTUPredictiveError(f"{label}:cannot_open:{path}") from exc
    try:
        before = os.fstat(fd)
        chunks: list[bytes] = []
        digest = hashlib.sha256()
        total = 0
        while True:
            try:
                block = os.read(fd, 1 << 20)
            except OSError as exc:
                raise MultiTUPredictiveError(f"{label}:read_failed:{path}") from exc
            if not block:
                break
            total += len(block)
            if total > limit:
                raise MultiTUPredictiveError(f"{label}:too_large:{path}")
            chunks.append(block)
            digest.update(block)
        after = os.fstat(fd)
        if any(getattr(before, field) != getattr(after, field)
               for field in ("st_dev", "st_ino", "st_size", "st_mtime_ns", "st_ctime_ns")):
            raise MultiTUPredictiveError(f"{label}:changed_while_reading:{path}")
        return b"".join(chunks), {"path": str(path.resolve()), "sha256": digest.hexdigest(),
                                  "bytes": total}
    finally:
        os.close(fd)


def _digest(path: Path, label: str, limit: int) -> dict[str, object]:
    _raw, facts = _snapshot(path, label, limit)
    return facts


def _sha(value: object, label: str, pattern: re.Pattern[str]) -> str:
    if not isinstance(value, str) or pattern.fullmatch(value) is None or int(value, 16) == 0:
        raise MultiTUPredictiveError(f"{label}:invalid_digest")
    return value.lower()


def _descriptor(path: Path, label: str) -> dict[str, object]:
    facts = _digest(path, label, MAX_PLAN_BYTES)
    return {"path": facts["path"], "sha256": facts["sha256"], "bytes": facts["bytes"]}


def _check_cell(plan: dict[str, object]) -> dict[str, str]:
    cell = plan.get("cell")
    if (not isinstance(cell, dict) or set(cell) != {"corpus", "profile", "regime"} or
            cell.get("corpus") not in CORPORA or cell.get("profile") not in PROFILES or
            cell.get("regime") not in REGIMES):
        raise MultiTUPredictiveError("plan:cell_invalid")
    expected = SPLITS[cell["corpus"]]
    if plan.get("split") != expected:
        raise MultiTUPredictiveError("plan:split_policy_mismatch")
    return {key: str(cell[key]) for key in ("corpus", "profile", "regime")}


def _validate_matrix(plan: dict[str, object], cell: dict[str, str]) -> None:
    value = plan.get("matrix_precondition")
    if not isinstance(value, dict) or value.get("schema") != depth_runner.MATRIX_AUDIT_SCHEMA or \
            value.get("status") != "PASS":
        raise MultiTUPredictiveError("plan:matrix_precondition_invalid")
    path_value = value.get("path")
    if not isinstance(path_value, str):
        raise MultiTUPredictiveError("plan:matrix_precondition_path_invalid")
    facts = _digest(Path(path_value), "matrix_precondition", MAX_PLAN_BYTES)
    if facts["sha256"] != value.get("sha256") or facts["bytes"] != value.get("bytes"):
        raise MultiTUPredictiveError("plan:matrix_precondition_changed")
    try:
        depth_runner._matrix_precondition(Path(path_value), cell)
    except depth_runner.DepthPlanError as exc:
        raise MultiTUPredictiveError(str(exc)) from exc


def _validate_plan(plan_path: Path) -> tuple[dict[str, object], dict[str, str], list[dict[str, object]], dict[str, object]]:
    plan_raw, plan_facts = _snapshot(plan_path, "plan", MAX_PLAN_BYTES)
    value = _parse(plan_raw, "plan")
    if not isinstance(value, dict) or value.get("schema") != PLAN_SCHEMA or \
            value.get("semantics") != SEMANTICS:
        raise MultiTUPredictiveError("plan:schema_or_semantics_invalid")
    contract = value.get("execution_contract")
    if (not isinstance(contract, dict) or
            contract.get("status") != "READY_MULTI_TU_PREDICTOR" or
            contract.get("producer") != "farmharness.s8_multitu_predictive_producer"):
        raise MultiTUPredictiveError("plan:multi_tu_producer_contract_invalid")
    cell = _check_cell(value)
    _validate_matrix(value, cell)
    source = value.get("source_manifest")
    root_value = value.get("source_root")
    if not isinstance(source, dict) or set(source) != {"path", "sha256", "bytes", "entries"} or \
            not isinstance(root_value, str) or not root_value:
        raise MultiTUPredictiveError("plan:source_manifest_invalid")
    source_path = Path(str(source["path"]))
    source_facts = _digest(source_path, "source_manifest", MAX_PLAN_BYTES)
    if (source_facts["sha256"] != source["sha256"] or source_facts["bytes"] != source["bytes"] or
            type(source["entries"]) is not int or source["entries"] <= 0):
        raise MultiTUPredictiveError("plan:source_manifest_changed")
    try:
        all_inputs, manifest_read_facts = depth_runner._manifest_inputs(
            source_path, Path(root_value), "source_manifest")
    except depth_runner.DepthPlanError as exc:
        raise MultiTUPredictiveError(str(exc)) from exc
    if (manifest_read_facts["sha256"] != source_facts["sha256"] or
            manifest_read_facts["bytes"] != source_facts["bytes"]):
        raise MultiTUPredictiveError("plan:source_manifest_changed_while_reading")
    inputs = value.get("inputs")
    if not isinstance(inputs, list) or not inputs or len(inputs) > len(all_inputs):
        raise MultiTUPredictiveError("plan:inputs_invalid")
    request = value.get("request")
    if not isinstance(request, dict):
        raise MultiTUPredictiveError("plan:request_invalid")
    depth = request.get("depth")
    if depth not in (100, 200, "full", "repeat-full"):
        raise MultiTUPredictiveError("plan:depth_invalid")
    expected_len = depth if isinstance(depth, int) else len(all_inputs)
    if len(inputs) != expected_len or request.get("requested_curve_points") not in (depth, "full", "repeat-full"):
        raise MultiTUPredictiveError("plan:depth_input_count_mismatch")
    if inputs != all_inputs[:len(inputs)]:
        raise MultiTUPredictiveError("plan:input_sequence_not_manifest_prefix")
    for index, item in enumerate(inputs):
        if not isinstance(item, dict) or set(item) != {"ordinal", "path", "source_relative", "sha256", "bytes"}:
            raise MultiTUPredictiveError(f"plan:input_descriptor_invalid:{index}")
        if item["ordinal"] != index:
            raise MultiTUPredictiveError("plan:input_ordinal_invalid")
        _sha(item["sha256"], f"plan.inputs[{index}].sha256", HEX64)
        if type(item["bytes"]) is not int or item["bytes"] < 0:
            raise MultiTUPredictiveError("plan:input_bytes_invalid")
        raw, facts = _snapshot(Path(item["path"]), f"input[{index}]", MAX_INPUT_BYTES)
        del raw
        if facts["sha256"] != item["sha256"] or facts["bytes"] != item["bytes"]:
            raise MultiTUPredictiveError(f"input[{index}]:manifest_binding_mismatch")
    if depth == "repeat-full":
        repeat = value.get("repeat_of")
        if not isinstance(repeat, dict) or set(repeat) != {"path", "sha256", "bytes"}:
            raise MultiTUPredictiveError("plan:repeat_of_invalid")
        prior_path = Path(str(repeat["path"]))
        prior_raw, prior_facts = _snapshot(prior_path, "repeat_of", MAX_PLAN_BYTES)
        if prior_facts["sha256"] != repeat["sha256"] or prior_facts["bytes"] != repeat["bytes"]:
            raise MultiTUPredictiveError("plan:repeat_of_changed")
        prior = _parse(prior_raw, "repeat_of")
        if (not isinstance(prior, dict) or prior.get("request", {}).get("depth") != "full" or
                prior.get("cell") != value.get("cell") or prior.get("inputs") != inputs):
            raise MultiTUPredictiveError("plan:repeat_of_sequence_mismatch")
    result = value.get("result")
    if not isinstance(result, dict) or not isinstance(result.get("directory"), str):
        raise MultiTUPredictiveError("plan:result_invalid")
    result_dir = Path(result["directory"])
    if (not result_dir.is_absolute() or
            depth_runner.TIMESTAMPED_DIR.fullmatch(result_dir.name) is None or
            result_dir.exists() or result_dir.is_symlink()):
        raise MultiTUPredictiveError("plan:result_directory_must_be_new_absolute_path")
    return value, cell, inputs, {"plan": plan_facts, "source": source_facts, "source_path": source_path}


def _validate_scheduling(plan: dict[str, object], inputs: list[dict[str, object]]) -> tuple[dict[str, object], list[int], int]:
    """Require the plan's complete deterministic topology/assignment declaration."""
    scheduling = plan.get("scheduling")
    if not isinstance(scheduling, dict):
        raise MultiTUPredictiveError("plan:scheduling_missing")
    topology = scheduling.get("topology")
    if not isinstance(topology, str):
        raise MultiTUPredictiveError("plan:scheduling_topology_invalid")
    try:
        expected = depth_runner.build_schedule(inputs, topology)
    except depth_runner.DepthPlanError as exc:
        raise MultiTUPredictiveError(str(exc)) from exc
    if scheduling != expected:
        raise MultiTUPredictiveError("plan:scheduling_authenticated_mismatch")
    assignments_value = scheduling["assignments"]
    assert isinstance(assignments_value, list)
    # The native batch map addresses F relationships; the execution slot is
    # retained separately in each plan/curve assignment for scheduling.
    assignments = [int(item["f_relationship"]) for item in assignments_value]
    relationship_count = int(scheduling["f_relationships"])
    if len(assignments) != len(inputs) or relationship_count not in (1, 20):
        raise MultiTUPredictiveError("plan:scheduling_assignment_invalid")
    return scheduling, assignments, relationship_count


def _check_optional_assignment(path: Path | None, assignments: list[int],
                               relationship_count: int) -> dict[str, object] | None:
    """Accept a legacy map only as an equality check, never as authority."""
    if path is None:
        return None
    raw, facts = _snapshot(path, "relationship_assignment", MAX_PLAN_BYTES)
    value = _parse(raw, "relationship_assignment")
    if (not isinstance(value, dict) or set(value) != {"cardinality", "assignments"} or
            value.get("cardinality") != relationship_count or
            value.get("assignments") != assignments):
        raise MultiTUPredictiveError("relationship_assignment:does_not_match_authenticated_plan")
    return {"path": facts["path"], "sha256": facts["sha256"], "bytes": facts["bytes"],
            "authority": "plan_schedule_equality_check"}


def _product_binary(path: Path | None) -> tuple[Path, dict[str, object]]:
    candidate = path or (Path(__file__).resolve().parents[1] / "cache" / "sim" / ".p50sim.bin")
    if not candidate.is_absolute():
        candidate = candidate.absolute()
    try:
        mode = candidate.lstat()
    except OSError as exc:
        raise MultiTUPredictiveError(f"product_simulator:unavailable:{candidate}") from exc
    if (stat.S_ISLNK(mode.st_mode) or not stat.S_ISREG(mode.st_mode) or
            mode.st_nlink != 1 or not os.access(candidate, os.X_OK)):
        raise MultiTUPredictiveError(f"product_simulator:not_executable_private_file:{candidate}")
    return candidate, _digest(candidate, "product_simulator", MAX_PRODUCT_OUTPUT_BYTES)


def _product_rows(simulator: Path, inputs: list[dict[str, object]], assignments: list[int],
                  relationship_count: int, cell: dict[str, str], model_id: str,
                  topology_digest: str, assignment_map: Path | None,
                  second_inputs: list[dict[str, object]] | None = None,
                  second_assignments: list[int] | None = None) -> list[list[dict[str, object]]]:
    """Run the exact product endpoint batch and project only authenticated rows."""
    with tempfile.TemporaryDirectory(prefix="s8-p50batch-") as scratch:
        if any("\n" in str(item["path"]) or "\r" in str(item["path"]) for item in inputs) or \
                (second_inputs is not None and any("\n" in str(item["path"]) or
                                                    "\r" in str(item["path"])
                                                    for item in second_inputs)):
            raise MultiTUPredictiveError("product_simulator:input_path_contains_newline")
        manifest = Path(scratch) / "segment-1.manifest"
        manifest.write_text("".join(str(item["path"]) + "\n" for item in inputs), encoding="utf-8")
        mapping = Path(scratch) / "segment-1.assignment"
        mapping.write_text("cardinality=" + str(relationship_count) + "\n" +
                           "".join(str(value) + "\n" for value in assignments), encoding="ascii")
        output = Path(scratch) / "batch.jsonl"
        command = [str(simulator), "--batch-manifest", str(manifest),
                   "--batch-assignment-map", str(mapping), "--batch-output", str(output)]
        if second_inputs is not None:
            manifest2 = Path(scratch) / "segment-2.manifest"
            manifest2.write_text("".join(str(item["path"]) + "\n" for item in second_inputs),
                                 encoding="utf-8")
            mapping2 = Path(scratch) / "segment-2.assignment"
            values2 = second_assignments if second_assignments is not None else assignments
            mapping2.write_text("cardinality=" + str(relationship_count) + "\n" +
                                "".join(str(value) + "\n" for value in values2), encoding="ascii")
            command += ["--batch-manifest-2", str(manifest2),
                        "--batch-assignment-map-2", str(mapping2)]
        env = os.environ.copy()
        env["ICECC_P50_PROFILE"] = cell["profile"]
        try:
            completed = subprocess.run(command, env=env, stdin=subprocess.DEVNULL,
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                       check=False, timeout=180)
        except (OSError, subprocess.TimeoutExpired) as exc:
            raise MultiTUPredictiveError("product_simulator:execution_failed") from exc
        if completed.returncode != 0:
            detail = completed.stderr.decode("utf-8", "replace").strip()[:512]
            raise MultiTUPredictiveError(f"product_simulator:rejected:{detail}")
        raw, facts = _snapshot(output, "product_batch_output", MAX_PRODUCT_OUTPUT_BYTES)
        if facts["bytes"] == 0:
            raise MultiTUPredictiveError("product_simulator:empty_output")
        segments: list[list[dict[str, object]]] = [[], [] if second_inputs is not None else []]
        for line_number, line in enumerate(raw.splitlines(), 1):
            value = _parse(line, f"product_batch_output:{line_number}")
            if not isinstance(value, dict) or value.get("schema") != "icecream-p50sim-batch-v1":
                raise MultiTUPredictiveError("product_simulator:row_schema_invalid")
            if value.get("profile") != cell["profile"]:
                raise MultiTUPredictiveError("product_simulator:profile_mismatch")
            segment = value.get("segment")
            if segment not in ("full-1", "full-2"):
                raise MultiTUPredictiveError("product_simulator:segment_invalid")
            bucket = 0 if segment == "full-1" else 1
            segments[bucket].append(value)
        expected = [len(inputs), len(second_inputs) if second_inputs is not None else 0]
        previous_by_relationship: dict[str, int] = {}
        state_by_relationship: dict[str, str] = {}
        c_guids: set[str] = set()
        relationship_f_guids: dict[str, str] = {}
        for bucket, count in enumerate(expected):
            if len(segments[bucket]) != count:
                raise MultiTUPredictiveError("product_simulator:point_count_mismatch")
            for ordinal, row in enumerate(segments[bucket]):
                relationship_id = row.get("relationship_id")
                previous_seq = previous_by_relationship.get(str(relationship_id), -1)
                if row.get("tu_index") != ordinal or type(row.get("tu_seq")) is not int or \
                        row["tu_seq"] <= previous_seq or row.get("committed") is not True:
                    raise MultiTUPredictiveError("product_simulator:order_or_commit_invalid")
                previous_by_relationship[str(relationship_id)] = row["tu_seq"]
                item = (inputs if bucket == 0 else second_inputs)[ordinal]
                if row.get("raw_bytes") != item["bytes"]:
                    raise MultiTUPredictiveError("product_simulator:raw_size_mismatch")
                _raw_after, facts_after = _snapshot(Path(item["path"]),
                                                     f"product_input[{bucket}:{ordinal}]",
                                                     MAX_INPUT_BYTES)
                if facts_after["sha256"] != item["sha256"] or facts_after["bytes"] != item["bytes"]:
                    raise MultiTUPredictiveError("product_simulator:input_changed_during_run")
                if (not isinstance(row.get("raw_digest"), str) or
                        len(row["raw_digest"]) != 32 or int(row["raw_digest"], 16) == 0):
                    raise MultiTUPredictiveError("product_simulator:raw_digest_invalid")
                if _product_digest128(_raw_after) != row["raw_digest"]:
                    raise MultiTUPredictiveError("product_simulator:raw_digest_binding_invalid")
                if row.get("relationship_id") != (
                        f"c1f{relationship_count}-r{(assignments if bucket == 0 else second_assignments)[ordinal]:02d}"):
                    raise MultiTUPredictiveError(
                        f"product_simulator:relationship_identity_invalid:{row.get('relationship_id')}"
                        f"!=c1f{relationship_count}-r{(assignments if bucket == 0 else second_assignments)[ordinal]:02d}")
                if (not isinstance(row.get("c_store_guid"), str) or
                        len(row["c_store_guid"]) != 32 or
                        not isinstance(row.get("f_store_guid"), str) or
                        len(row["f_store_guid"]) != 32):
                    raise MultiTUPredictiveError("product_simulator:store_identity_invalid")
                c_guids.add(row["c_store_guid"])
                prior_f = relationship_f_guids.setdefault(row["relationship_id"], row["f_store_guid"])
                if prior_f != row["f_store_guid"]:
                    raise MultiTUPredictiveError("product_simulator:relationship_f_identity_changed")
                for key in ("encoded_source_bytes", "c_to_f_bytes", "f_to_c_bytes",
                            "peer_c_read_bytes", "peer_f_read_bytes",
                            "simulator_execution_ns", "action_records"):
                    if type(row.get(key)) is not int or row[key] < 0:
                        raise MultiTUPredictiveError(f"product_simulator:{key}_invalid")
                if row["action_records"] > 256:
                    raise MultiTUPredictiveError("product_simulator:action_bound_exceeded")
                for key in ("transaction_digest", "state_digest", "state_before_digest"):
                    if (not isinstance(row.get(key), str) or len(row[key]) != 32 or
                            int(row[key], 16) == 0):
                        raise MultiTUPredictiveError(f"product_simulator:{key}_invalid")
                if row["encoded_source_bytes"] == 0 and row["raw_bytes"] != 0:
                    raise MultiTUPredictiveError("product_simulator:encoded_source_empty")
                if (row["c_to_f_bytes"] != row["peer_f_read_bytes"] or
                        row["f_to_c_bytes"] != row["peer_c_read_bytes"]):
                    raise MultiTUPredictiveError("product_simulator:peer_read_conservation_invalid")
                prior_state = state_by_relationship.get(str(relationship_id))
                if prior_state is not None and row["state_before_digest"] != prior_state:
                    raise MultiTUPredictiveError("product_simulator:state_chain_invalid")
                state_by_relationship[str(relationship_id)] = row["state_digest"]
        if len(c_guids) != 1 or len(set(relationship_f_guids.values())) != len(relationship_f_guids):
            raise MultiTUPredictiveError("product_simulator:relationship_store_identity_invalid")
        return segments


def _product_curve_rows(product_rows: list[dict[str, object]], inputs: list[dict[str, object]],
                        cell: dict[str, str], model_id: str, topology_digest: str,
                        topology: dict[str, object], calibration: dict[str, object] | None,
                        cumulative: dict[str, int],
                        scheduling: dict[str, object],
                        relationship_states: dict[str, dict[str, object]] | None = None
                        ) -> list[dict[str, object]]:
    rows: list[dict[str, object]] = []
    if relationship_states is None:
        relationship_states = {}
    slots = scheduling.get("global_slots")
    slots_per_f = scheduling.get("slots_per_f")
    assignments = scheduling.get("assignments")
    if (type(slots) is not int or slots <= 0 or type(slots_per_f) is not int or
            slots_per_f <= 0 or not isinstance(assignments, list) or
            len(assignments) != len(inputs)):
        raise MultiTUPredictiveError("plan:scheduling_runtime_invalid")
    available = [0] * slots
    serial_elapsed = 0
    for ordinal, (product, item) in enumerate(zip(product_rows, inputs, strict=True)):
        raw, facts = _snapshot(Path(item["path"]), f"predictive_input[{ordinal}]", MAX_INPUT_BYTES)
        if facts["sha256"] != item["sha256"] or facts["bytes"] != item["bytes"]:
            raise MultiTUPredictiveError("product_simulator:input_binding_changed")
        relationship_id = product["relationship_id"]
        if not isinstance(relationship_id, str):
            raise MultiTUPredictiveError("product_simulator:relationship_id_invalid")
        state = relationship_states.get(relationship_id)
        was_started = state is not None and int(state.get("next_step", 0)) > 0
        if state is None:
            state = engine.new_relationship_state(topology, cell, topology_digest)
        simulated, state = engine.predict_sequential(raw, topology, cell, state, calibration)
        encoded_source = product["encoded_source_bytes"]
        if type(encoded_source) is not int or encoded_source < 0:
            raise MultiTUPredictiveError("product_simulator:encoded_source_invalid")
        engine.apply_encoded_source_bytes(simulated, encoded_source, topology, cell,
                                           0 if was_started else None)
        relationship_states[relationship_id] = state
        modeled_channel = simulated["channel_bytes"]
        modeled_elapsed = simulated["elapsed_ns"]
        if not isinstance(modeled_channel, dict) or not isinstance(modeled_elapsed, dict):
            raise MultiTUPredictiveError("predictive_engine:model_shape_invalid")
        c_to_f = encoded_source
        f_to_c = modeled_channel["F_TO_C"]
        elapsed = modeled_elapsed["total"]
        if type(f_to_c) is not int or type(elapsed) is not int or f_to_c < 0 or elapsed < 0:
            raise MultiTUPredictiveError("predictive_engine:model_value_invalid")
        assignment = assignments[ordinal]
        if (not isinstance(assignment, dict) or type(assignment.get("global_slot")) is not int or
                not 0 <= assignment["global_slot"] < slots or
                assignment.get("ordinal") != ordinal or
                assignment.get("f_relationship") != assignment["global_slot"] // slots_per_f or
                assignment.get("per_f_slot") != assignment["global_slot"] % slots_per_f):
            raise MultiTUPredictiveError("plan:scheduling_assignment_invalid")
        global_slot = assignment["global_slot"]
        service = elapsed
        start = available[global_slot]
        finish = start + service
        available[global_slot] = finish
        serial_elapsed += service
        cumulative["C_TO_F_bytes"] += c_to_f
        cumulative["F_TO_C_bytes"] += f_to_c
        cumulative["channel_bytes"] += c_to_f + f_to_c
        cumulative["elapsed_ns"] = max(cumulative["elapsed_ns"], finish)
        before = product["state_before_digest"]
        after = product["state_digest"]
        if not isinstance(relationship_id, str) or not isinstance(before, str) or \
                not isinstance(after, str):
            raise MultiTUPredictiveError("product_simulator:state_identity_invalid")
        rows.append({
            "schema": engine.OBSERVATIONS_SCHEMA,
            "step": ordinal,
            "tu_id": f"{cell['corpus']}-{cell['regime']}-tu-{ordinal:06d}-{str(item['sha256'])[:12]}",
            "cell": cell,
            "model_id": model_id,
            "channel_bytes": {"C_TO_F": c_to_f, "F_TO_C": f_to_c,
                               "total": c_to_f + f_to_c},
            "elapsed_ns": {("commit" if key == "transaction_commit" else key): value
                            for key, value in modeled_elapsed.items()},
            "elapsed_components_ns": {("commit" if key == "transaction_commit" else key): value
                                       for key, value in modeled_elapsed.items()},
            "startup_ns": modeled_elapsed["startup"],
            "relationship_mode": engine.RELATIONSHIP_MODES[cell["profile"]],
            "relationship_state": {"relationship_id": relationship_id,
                                    "topology_digest": topology_digest,
                                    "before_digest": before, "after_digest": after,
                                    "transition": ("tu_reset" if engine.RELATIONSHIP_MODES[cell["profile"]] == "tu"
                                                    else ("relationship_continue" if was_started
                                                          else "relationship_start")),
                                    "reset_point": engine.RELATIONSHIP_MODES[cell["profile"]] == "tu"},
            "relationship_id": relationship_id,
            "scheduling": {"global_slot": global_slot,
                            "f_relationship": assignment["f_relationship"],
                            "per_f_slot": assignment["per_f_slot"],
                            "start_ns": start, "finish_ns": finish,
                            "service_ns": service},
            "product_completion": {"raw_bytes": product["raw_bytes"],
                                    "raw_digest": product["raw_digest"],
                                    "tx_digest": product["transaction_digest"],
                                    "encoded_source_bytes": encoded_source,
                                    "wire_c_to_f_bytes": product["c_to_f_bytes"],
                                    "wire_f_to_c_bytes": product["f_to_c_bytes"],
                                    "peer_c_read_bytes": product["peer_c_read_bytes"],
                                    "peer_f_read_bytes": product["peer_f_read_bytes"],
                                    "simulator_execution_ns": product["simulator_execution_ns"],
                                    "completion_record_count": product["action_records"],
                                    "committed": product["committed"]},
            "input_sha256": item["sha256"],
            "source_relative": item["source_relative"],
            "topology_digest": topology_digest,
            "provenance": "product_p50sim_batch",
            "cumulative": dict(cumulative),
        })
    if rows:
        prior_finish: dict[int, int] = {}
        for row in rows:
            event = row["scheduling"]
            slot = int(event["global_slot"])
            if event["start_ns"] < prior_finish.get(slot, 0):
                raise MultiTUPredictiveError("plan:scheduling_concurrency_overlap")
            if event["finish_ns"] != event["start_ns"] + event["service_ns"]:
                raise MultiTUPredictiveError("plan:scheduling_event_invalid")
            prior_finish[slot] = event["finish_ns"]
        if cumulative["elapsed_ns"] != max(row["scheduling"]["finish_ns"] for row in rows):
            raise MultiTUPredictiveError("plan:scheduling_makespan_invalid")
        rows[-1]["schedule_summary"] = {"makespan_ns": cumulative["elapsed_ns"],
                                         "serial_service_ns": serial_elapsed,
                                         "global_slots": slots}
    return rows


def _emit_product_segment(plan: dict[str, object], plan_facts: dict[str, object],
                          directory: Path, rows: list[dict[str, object]], cell: dict[str, str],
                          source_commit: str, source_tree: str, input_digest: str,
                          topology_digest: str, model_id: str, assignment_facts: dict[str, object] | None,
                          relationship_count: int, simulator_facts: dict[str, object],
                          assignment_digest: str, input_manifest_sha256: str,
                          scheduling: dict[str, object],
                          continuation: bool, paired: bool = False) -> dict[str, object]:
    curve_raw = b"".join(canonical_bytes(row) + b"\n" for row in rows)
    curve_sha = hashlib.sha256(curve_raw).hexdigest()
    identity = {"corpus": cell["corpus"], "profile": cell["profile"],
                "regime": cell["regime"], "split": SPLITS[cell["corpus"]],
                "run_id": f"{directory.name}-{plan_facts['plan']['sha256'][:12]}",
                "source_commit": source_commit, "source_tree": source_tree,
                "input_digest": input_digest, "topology_digest": topology_digest,
                "model_id": model_id}
    curve_manifest = {"schema": CURVE_MANIFEST_SCHEMA, "identity": identity,
                      "units": {"point": "step", "channel_bytes": "bytes", "elapsed_ns": "ns"},
                      "curve": {"path": "predictive_sim.jsonl", "sha256": curve_sha,
                                "bytes": len(curve_raw)},
                      "provenance": {"mode": "predictive_sim", "producer": PRODUCER,
                                     "trace_free": True}}
    curve_manifest_raw = canonical_bytes(curve_manifest) + b"\n"
    producer = {"schema": SCHEMA, "semantics": SEMANTICS, "cell": cell,
                "split": SPLITS[cell["corpus"]], "identity": identity,
                "request": plan["request"], "paired_continuation": continuation,
                "plan": {"path": plan_facts["plan"]["path"],
                         "sha256": plan_facts["plan"]["sha256"],
                         "bytes": plan_facts["plan"]["bytes"]},
                "topology_digest": topology_digest,
                "scheduling": {"schema": scheduling["schema"],
                                "topology": scheduling["topology"],
                                "f_relationships": scheduling["f_relationships"],
                                "slots_per_f": scheduling["slots_per_f"],
                                "global_slots": scheduling["global_slots"],
                                "stream_capacity_tus": scheduling["stream_capacity_tus"],
                                "service_duration_model": scheduling["service_duration_model"],
                                "assignment_policy": scheduling["assignment_policy"],
                                "assignment_epoch_reset": scheduling["assignment_epoch_reset"],
                                "makespan_ns": rows[-1]["cumulative"]["elapsed_ns"] if rows else 0,
                                "serial_service_ns": sum(row["scheduling"]["service_ns"]
                                                          for row in rows)},
                "relationship": {"mode": engine.RELATIONSHIP_MODES[cell["profile"]],
                                  "relationship_count": relationship_count,
                                  "state_after_digests": [row["relationship_state"]["after_digest"]
                                                           for row in rows],
                                  "reset_points": [],
                                  "chain_digest": hashlib.sha256(
                                      canonical_bytes([row["relationship_state"]["after_digest"]
                                                       for row in rows])).hexdigest()},
                "codec": {"schema": engine.RELATIONSHIP_CODEC_SCHEMA,
                          "mode": "paired_continuation" if paired else "product_endpoint_batch",
                          "native_product": True,
                          "roundtrip_verified": True,
                          "relationship_count": relationship_count,
                          "paired_stream_scope": "full-1+full-2" if continuation else "single-segment",
                          "assignment": assignment_facts,
                          "simulator": simulator_facts},
                "batch_binding": {"simulator_sha256": simulator_facts["sha256"],
                                  "source_commit": source_commit, "source_tree": source_tree,
                                  "input_manifest_sha256": input_manifest_sha256,
                                  "assignment_sha256": assignment_digest},
                "outputs": {"predictive_sim": {"path": "predictive_sim.jsonl",
                                                  "sha256": curve_sha, "bytes": len(curve_raw),
                                                  "points": len(rows)},
                            "predictive_manifest": {"path": "predictive_curve_manifest.json",
                                                     "sha256": hashlib.sha256(curve_manifest_raw).hexdigest(),
                                                     "bytes": len(curve_manifest_raw)}},
                "live_observation": "not_emitted_separate_authenticated_input_required",
                "provenance": {"trace_free": True, "ordered_inputs": True,
                               "same_topology_state": True,
                               "simulator": "cache/sim/p50sim product endpoint batch"}}
    directory.parent.mkdir(parents=True, exist_ok=True)
    if directory.exists() or directory.is_symlink():
        raise MultiTUPredictiveError("output_dir:already_exists")
    directory.mkdir()
    _write_new(directory / "predictive_sim.jsonl", curve_raw, "predictive_sim")
    _write_new(directory / "predictive_curve_manifest.json", curve_manifest_raw,
               "predictive_curve_manifest")
    _write_new(directory / "producer_manifest.json", canonical_bytes(producer) + b"\n",
               "producer_manifest")
    return producer


def produce(plan_path: Path, engine_manifest: Path, source_commit: str,
            source_tree: str, output_dir: Path | None = None,
            calibration_bundle: Path | None = None,
            assignment_map: Path | None = None,
            sim_binary: Path | None = None) -> dict[str, object]:
    """Authenticate a depth plan and write predictive outputs to one new dir."""
    plan, cell, inputs, plan_facts = _validate_plan(plan_path)
    if plan["request"]["depth"] == "repeat-full":
        raise MultiTUPredictiveError(
            "repeat_full:paired_producer_required_to_restore_terminal_codec_state")
    scheduling, assignments, relationship_count = _validate_scheduling(plan, inputs)
    assignment_facts = _check_optional_assignment(assignment_map, assignments, relationship_count)
    source_commit = _sha(source_commit, "source_commit", HEX40)
    source_tree = _sha(source_tree, "source_tree", HEX40)
    manifest, _template_raw, _template_input, topology_facts, template_sha, topology, template_cell = engine.load_inputs(engine_manifest)
    if template_cell != cell or manifest["split"] != SPLITS[cell["corpus"]]:
        raise MultiTUPredictiveError("engine_manifest:cell_or_split_mismatch")
    calibration = (engine.load_calibration_bundle(calibration_bundle)
                   if calibration_bundle is not None else None)
    model_id = (str(calibration["model_id"]) if calibration is not None
                else str(engine.BASE_MODEL["id"]))
    if not SAFE_ID.fullmatch(model_id):
        raise MultiTUPredictiveError("model_id:invalid")
    result_value = plan["result"]
    assert isinstance(result_value, dict)
    result_dir = Path(str(result_value["directory"]))
    if output_dir is not None and output_dir.resolve() != result_dir.resolve():
        raise MultiTUPredictiveError("output_dir:must_match_plan_result_directory")
    output_dir = result_dir
    if output_dir.exists() or output_dir.is_symlink():
        raise MultiTUPredictiveError("output_dir:already_exists")

    topology_digest = str(topology_facts["sha256"])
    aggregate_input = {"source_manifest_sha256": plan["source_manifest"]["sha256"],
                       "inputs": inputs}
    input_digest = hashlib.sha256(canonical_bytes(aggregate_input)).hexdigest()
    run_id = f"{output_dir.name}-{plan_facts['plan']['sha256'][:12]}"
    identity = {"corpus": cell["corpus"], "profile": cell["profile"],
                "regime": cell["regime"], "split": SPLITS[cell["corpus"]],
                "run_id": run_id, "source_commit": source_commit,
                "source_tree": source_tree, "input_digest": input_digest,
                "topology_digest": topology_digest, "model_id": model_id}

    simulator, simulator_facts = _product_binary(sim_binary)
    product_assignments = assignments
    assignment_digest = hashlib.sha256(canonical_bytes(
        {"topology": scheduling["topology"], "cardinality": relationship_count,
         "assignments": product_assignments, "schedule": scheduling})).hexdigest()
    if assignment_facts is None:
        assignment_facts = {"authority": "authenticated_plan_scheduling",
                            "sha256": assignment_digest}
    product_segments = _product_rows(simulator, inputs, product_assignments, relationship_count,
                                     cell, model_id, topology_digest, assignment_map)
    product_rows = _product_curve_rows(product_segments[0], inputs, cell, model_id,
                                        topology_digest, topology, calibration,
                                        {"C_TO_F_bytes": 0, "F_TO_C_bytes": 0,
                                         "channel_bytes": 0, "elapsed_ns": 0},
                                        scheduling)
    return _emit_product_segment(plan, plan_facts, output_dir, product_rows, cell,
                                 source_commit, source_tree, input_digest, topology_digest,
                                 model_id, assignment_facts, relationship_count,
                                 simulator_facts, assignment_digest,
                                 str(plan["source_manifest"]["sha256"]), scheduling, False)

def produce_pair(first_plan_path: Path, repeat_plan_path: Path, engine_manifest: Path,
                 source_commit: str, source_tree: str,
                 calibration_bundle: Path | None = None,
                 assignment_map: Path | None = None,
                 sim_binary: Path | None = None) -> tuple[dict[str, object], dict[str, object]]:
    """Run full-1 and repeat-full in one process with shared codec contexts."""
    first, cell, first_inputs, first_facts = _validate_plan(first_plan_path)
    repeat, repeat_cell, repeat_inputs, repeat_facts = _validate_plan(repeat_plan_path)
    if first["request"]["depth"] != "full" or repeat["request"]["depth"] != "repeat-full":
        raise MultiTUPredictiveError("repeat_full:paired_plans_must_be_full_then_repeat_full")
    if cell != repeat_cell or first_inputs != repeat_inputs:
        raise MultiTUPredictiveError("repeat_full:paired_input_identity_mismatch")
    if first["source_manifest"] != repeat["source_manifest"]:
        raise MultiTUPredictiveError("repeat_full:paired_source_manifest_mismatch")
    scheduling, assignments, relationship_count = _validate_scheduling(first, first_inputs)
    repeat_scheduling, repeat_assignments, repeat_relationship_count = _validate_scheduling(
        repeat, repeat_inputs)
    if (repeat_scheduling != scheduling or repeat_assignments != assignments or
            repeat_relationship_count != relationship_count):
        raise MultiTUPredictiveError("repeat_full:paired_scheduling_mismatch")
    assignment_facts = _check_optional_assignment(assignment_map, assignments, relationship_count)
    source_commit = _sha(source_commit, "source_commit", HEX40)
    source_tree = _sha(source_tree, "source_tree", HEX40)
    manifest, _template_raw, _template_input, topology_facts, template_sha, topology, template_cell = engine.load_inputs(engine_manifest)
    if template_cell != cell or manifest["split"] != SPLITS[cell["corpus"]]:
        raise MultiTUPredictiveError("engine_manifest:cell_or_split_mismatch")
    calibration = (engine.load_calibration_bundle(calibration_bundle)
                   if calibration_bundle is not None else None)
    model_id = str(calibration["model_id"]) if calibration is not None else str(engine.BASE_MODEL["id"])
    topology_digest = str(topology_facts["sha256"])
    first_dir = Path(str(first["result"]["directory"]))
    repeat_dir = Path(str(repeat["result"]["directory"]))
    for directory in (first_dir, repeat_dir):
        if directory.exists() or directory.is_symlink():
            raise MultiTUPredictiveError("repeat_full:result_directory_already_exists")
    aggregate_input = {"source_manifest_sha256": first["source_manifest"]["sha256"],
                       "inputs": first_inputs}
    input_digest = hashlib.sha256(canonical_bytes(aggregate_input)).hexdigest()
    simulator, simulator_facts = _product_binary(sim_binary)
    product_assignments = assignments
    assignment_digest = hashlib.sha256(canonical_bytes(
        {"topology": scheduling["topology"], "cardinality": relationship_count,
         "assignments": product_assignments, "schedule": scheduling})).hexdigest()
    if assignment_facts is None:
        assignment_facts = {"authority": "authenticated_plan_scheduling",
                            "sha256": assignment_digest}
    product_segments = _product_rows(simulator, first_inputs, product_assignments,
                                     relationship_count, cell, model_id, topology_digest,
                                     assignment_map, repeat_inputs, product_assignments)
    relationship_states: dict[str, dict[str, object]] = {}
    first_rows = _product_curve_rows(
        product_segments[0], first_inputs, cell, model_id, topology_digest, topology, calibration,
        {"C_TO_F_bytes": 0, "F_TO_C_bytes": 0, "channel_bytes": 0, "elapsed_ns": 0},
        scheduling,
        relationship_states)
    repeat_rows = _product_curve_rows(
        product_segments[1], repeat_inputs, cell, model_id, topology_digest, topology, calibration,
        {"C_TO_F_bytes": 0, "F_TO_C_bytes": 0, "channel_bytes": 0, "elapsed_ns": 0},
        scheduling,
        relationship_states)
    first_producer = _emit_product_segment(
        first, first_facts, first_dir, first_rows, cell, source_commit, source_tree,
        input_digest, topology_digest, model_id, assignment_facts, relationship_count,
        simulator_facts, assignment_digest, str(first["source_manifest"]["sha256"]),
        scheduling, False, True)
    repeat_producer = _emit_product_segment(
        repeat, repeat_facts, repeat_dir, repeat_rows, cell, source_commit, source_tree,
        input_digest, topology_digest, model_id, assignment_facts, relationship_count,
        simulator_facts, assignment_digest, str(first["source_manifest"]["sha256"]),
        scheduling, True, True)
    return first_producer, repeat_producer

def _write_new(path: Path, raw: bytes, label: str) -> None:
    if path.exists() or path.is_symlink():
        raise MultiTUPredictiveError(f"{label}:output_already_exists")
    try:
        with path.open("xb") as stream:
            stream.write(raw)
            stream.flush()
            os.fsync(stream.fileno())
    except OSError as exc:
        raise MultiTUPredictiveError(f"{label}:output_write_failed") from exc


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--plan", type=Path, required=True)
    parser.add_argument("--repeat-plan", type=Path,
                        help="paired repeat-full plan; runs full-1 and full-2 in one product process")
    parser.add_argument("--engine-manifest", type=Path, required=True)
    parser.add_argument("--source-commit", required=True)
    parser.add_argument("--source-tree", required=True)
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--calibration-manifest", type=Path)
    parser.add_argument("--assignment-map", type=Path,
                        help="authenticated JSON {cardinality,assignments} mapping")
    parser.add_argument("--sim-binary", type=Path,
                        help="exact product-linked cache/sim/p50sim batch executable")
    args = parser.parse_args(argv)
    try:
        if args.repeat_plan is not None:
            if args.output_dir is not None:
                raise MultiTUPredictiveError("repeat_full:output_dir_is_plan_bound")
            produce_pair(args.plan.absolute(), args.repeat_plan.absolute(),
                         args.engine_manifest.absolute(), args.source_commit, args.source_tree,
                         args.calibration_manifest.absolute() if args.calibration_manifest else None,
                         args.assignment_map.absolute() if args.assignment_map else None,
                         args.sim_binary.absolute() if args.sim_binary else None)
        else:
            produce(args.plan.absolute(), args.engine_manifest.absolute(), args.source_commit,
                    args.source_tree,
                    args.output_dir.absolute() if args.output_dir else None,
                    args.calibration_manifest.absolute() if args.calibration_manifest else None,
                    args.assignment_map.absolute() if args.assignment_map else None,
                    args.sim_binary.absolute() if args.sim_binary else None)
    except (MultiTUPredictiveError, engine.PredictionError) as exc:
        print(f"s8_multitu_predictive_producer: {exc}", file=sys.stderr)
        return 77
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
