#!/usr/bin/env python3
"""Run and authenticate an ordered, real C1F1 S8 live batch.

The shell lifecycle is the existing ``p50compilee2e-run.sh`` product gate:
one scheduler, C/F daemons, cache services, and one client/F relationship
remain alive while the batch is compiled.  This module only authenticates the
manifest before launch and turns the product's captured rows into the current
S8 live-curve schema.  It never accepts caller-supplied measurements.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import signal
import shutil
import subprocess
import tempfile
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

try:
    from . import s6_live_route_acceptance as action_parser
    from . import s8_predictive_live_normalizer as normalizer
    from .s8_schema import CORPORA, PROFILES, REGIMES, SPLITS
except ImportError:  # pragma: no cover
    import s6_live_route_acceptance as action_parser
    import s8_predictive_live_normalizer as normalizer
    from s8_schema import CORPORA, PROFILES, REGIMES, SPLITS


SCHEMA = "icecream-s8-real-c1f1-live-runner-v1"
TOPOLOGY = "C1F1/100000"
SCRIPT = Path(__file__).resolve().parents[1] / "unittests/p50compilee2e-run.sh"
HEX64 = re.compile(r"^[0-9a-f]{64}$")
HEX40 = re.compile(r"^[0-9a-f]{40}$")
SAFE = re.compile(r"^[A-Za-z0-9_.-]+$")
TIMESTAMP = re.compile(r"^\d{8}T\d{6}Z$")


class LiveRunnerError(ValueError):
    """A live run is missing product evidence or has mismatched identity."""


def _fail(reason: str) -> None:
    raise LiveRunnerError(reason)


def _canonical(value: object) -> bytes:
    return normalizer.canonical_bytes(value)


def _sha(path: Path) -> tuple[str, int]:
    try:
        info = path.lstat()
    except OSError as exc:
        raise LiveRunnerError(f"file_unavailable:{path}") from exc
    if path.is_symlink() or not path.is_file() or info.st_nlink != 1:
        _fail(f"file_not_private:{path}")
    digest = hashlib.sha256()
    size = 0
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
            size += len(block)
    return digest.hexdigest(), size


def _hex(value: object, label: str) -> str:
    if not isinstance(value, str) or HEX64.fullmatch(value) is None or int(value, 16) == 0:
        _fail(f"{label}:invalid_digest")
    return value.lower()


DEPTH_COUNTS = {"100": 100, "200": 200}
MAX_TIMEOUT_SECONDS = 4 * 60 * 60


def selected_count(depth: str, full_count: int | None = None) -> int:
    if depth in DEPTH_COUNTS:
        return DEPTH_COUNTS[depth]
    if depth == "full" and isinstance(full_count, int) and full_count > 0:
        return full_count
    _fail("depth:full_count_required" if depth == "full" else "depth:undeclared")
    raise AssertionError("unreachable")


def derive_timeout(tu_count: int, passes: int = 2, warm: bool = False) -> int:
    """Bound a real run by selected work, including warm prewarm work."""
    if type(tu_count) is not int or tu_count <= 0 or passes not in (1, 2):
        _fail("timeout:arguments_invalid")
    work_passes = passes + (1 if warm else 0)
    # 12 seconds/TU/pass is deliberately conservative for a remote compile;
    # cap runaway manifests while allowing a multi-hour full-corpus run.
    return min(MAX_TIMEOUT_SECONDS, max(180, 30 + tu_count * 12 * work_passes))


def load_batch_manifest(path: Path, expected_count: int = 100) -> list[dict[str, Any]]:
    """Authenticate the exact source snapshot list used by the shell gate."""
    _sha(path)
    try:
        values = [json.loads(line) for line in path.read_text().splitlines() if line.strip()]
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise LiveRunnerError("batch_manifest:invalid_jsonl") from exc
    if len(values) != expected_count:
        _fail(f"batch_manifest:expected_{expected_count}_rows")
    rows: list[dict[str, Any]] = []
    seen: set[str] = set()
    for ordinal, value in enumerate(values):
        if not isinstance(value, dict):
            _fail(f"batch_manifest:{ordinal}:object_required")
        required = {"tu_id", "source", "source_relative", "sha256", "predictive_input"}
        if not required.issubset(value) or set(value) - required - {"compile_db", "compile_source"}:
            _fail(f"batch_manifest:{ordinal}:fields_invalid")
        tu_id, source = value["tu_id"], value["source"]
        source_relative = value["source_relative"]
        if (not isinstance(tu_id, str) or SAFE.fullmatch(tu_id) is None or tu_id in seen
                or not isinstance(source, str) or not os.path.isabs(source)
                or not isinstance(source_relative, str) or not source_relative or
                os.path.isabs(source_relative) or any(part in ("", ".", "..")
                                                       for part in Path(source_relative).parts)):
            _fail(f"batch_manifest:{ordinal}:identity_invalid")
        digest, size = _sha(Path(source))
        if value.get("sha256") != digest:
            _fail(f"batch_manifest:{ordinal}:source_digest_mismatch")
        predictive_input = value.get("predictive_input")
        if (not isinstance(predictive_input, dict) or
                set(predictive_input) != {"ordinal", "path", "source_relative", "sha256", "bytes"} or
                predictive_input["ordinal"] != ordinal or
                not isinstance(predictive_input["path"], str) or not os.path.isabs(predictive_input["path"]) or
                not isinstance(predictive_input["source_relative"], str) or
                not predictive_input["source_relative"] or os.path.isabs(predictive_input["source_relative"]) or
                any(part in ("", ".", "..") for part in Path(predictive_input["source_relative"]).parts)):
            _fail(f"batch_manifest:{ordinal}:predictive_input_invalid")
        payload_sha = _hex(predictive_input["sha256"], f"batch_manifest:{ordinal}.predictive_input.sha256")
        payload_bytes = predictive_input["bytes"]
        if type(payload_bytes) is not int or payload_bytes <= 0:
            _fail(f"batch_manifest:{ordinal}:predictive_input.bytes_invalid")
        observed_payload_sha, observed_payload_bytes = _sha(Path(predictive_input["path"]))
        if observed_payload_sha != payload_sha or observed_payload_bytes != payload_bytes:
            _fail(f"batch_manifest:{ordinal}:predictive_input_digest_mismatch")
        predictive_input = {"ordinal": ordinal, "path": predictive_input["path"],
                            "source_relative": predictive_input["source_relative"],
                            "sha256": payload_sha, "bytes": payload_bytes}
        row = {"ordinal": ordinal, "tu_id": tu_id, "source": source,
               "source_relative": source_relative,
               "sha256": digest, "bytes": size, "predictive_input": predictive_input}
        for key in ("compile_db", "compile_source"):
            if key in value:
                if not isinstance(value[key], str) or not os.path.isabs(value[key]):
                    _fail(f"batch_manifest:{ordinal}:{key}_invalid")
                _sha(Path(value[key]))
                row[key] = value[key]
        rows.append(row)
        seen.add(tu_id)
    return rows


def payload_descriptors(rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
    return [dict(row["predictive_input"]) for row in rows]


def payload_descriptor_digest(rows: list[dict[str, Any]], source_manifest_sha256: str) -> str:
    """Use the same source-manifest-plus-ordered-input contract as S8."""
    aggregate = {"source_manifest_sha256": source_manifest_sha256,
                 "inputs": payload_descriptors(rows)}
    return hashlib.sha256(_canonical(aggregate)).hexdigest()


def load_predictive_plan(path: Path, *, corpus: str, profile: str, regime: str,
                         depth: str) -> tuple[dict[str, Any], list[dict[str, Any]], str]:
    """Authenticate the depth plan that owns the predictive `.ii` sequence."""
    plan_sha, _ = _sha(path)
    try:
        value = json.loads(path.read_text())
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise LiveRunnerError("predictive_plan:invalid_json") from exc
    if not isinstance(value, dict) or value.get("schema") != "icecream-s8-depth-run-plan-v1":
        _fail("predictive_plan:schema_invalid")
    if (value.get("cell") != {"corpus": corpus, "profile": profile, "regime": regime} or
            value.get("split") != SPLITS[corpus]):
        _fail("predictive_plan:cell_mismatch")
    request = value.get("request")
    if not isinstance(request, dict):
        _fail("predictive_plan:request_missing")
    requested_depth = request.get("depth")
    if (depth == "full" and requested_depth != "full") or (depth != "full" and requested_depth != int(depth)):
        _fail("predictive_plan:depth_mismatch")
    source = value.get("source_manifest")
    inputs = value.get("inputs")
    if (not isinstance(source, dict) or set(source) != {"path", "sha256", "bytes", "entries"} or
            not isinstance(inputs, list) or not inputs):
        _fail("predictive_plan:source_or_inputs_invalid")
    if (not isinstance(source["path"], str) or not os.path.isabs(source["path"]) or
            _hex(source.get("sha256"), "predictive_plan.source_manifest.sha256") != source["sha256"] or
            type(source.get("bytes")) is not int or source["bytes"] <= 0 or
            type(source.get("entries")) is not int or source["entries"] <= 0):
        _fail("predictive_plan:source_descriptor_invalid")
    source_path = Path(source["path"])
    source_sha, source_bytes = _sha(source_path)
    if source_sha != source["sha256"] or source_bytes != source["bytes"]:
        _fail("predictive_plan:source_manifest_mismatch")
    source_root = value.get("source_root")
    if (not isinstance(source_root, str) or not os.path.isabs(source_root) or
            not Path(source_root).is_dir() or Path(source_root).is_symlink()):
        _fail("predictive_plan:source_root_invalid")
    try:
        source_lines = [line.strip() for line in source_path.read_text().splitlines()]
    except (OSError, UnicodeError) as exc:
        raise LiveRunnerError("predictive_plan:source_manifest_unreadable") from exc
    if source["entries"] != len(source_lines) or len(inputs) > len(source_lines):
        _fail("predictive_plan:source_manifest_entries_mismatch")
    if depth != "full" and len(inputs) != int(depth):
        _fail("predictive_plan:input_count_mismatch")
    for ordinal, item in enumerate(inputs):
        if (not isinstance(item, dict) or set(item) != {"ordinal", "path", "source_relative", "sha256", "bytes"} or
                item["ordinal"] != ordinal or not isinstance(item["path"], str) or
                not os.path.isabs(item["path"]) or not isinstance(item["source_relative"], str) or
                not item["source_relative"] or os.path.isabs(item["source_relative"]) or
                any(part in ("", ".", "..") for part in Path(item["source_relative"]).parts) or
                _hex(item.get("sha256"), f"predictive_plan.input[{ordinal}].sha256") != item["sha256"] or
                type(item.get("bytes")) is not int or item["bytes"] <= 0):
            _fail(f"predictive_plan:input_descriptor_invalid:{ordinal}")
        item_path = Path(item["path"])
        root = Path(source_root).resolve()
        if not _inside(item_path, root) or str(item_path.resolve().relative_to(root)) != item["source_relative"]:
            _fail(f"predictive_plan:input_root_mismatch:{ordinal}")
        digest, size = _sha(item_path)
        if digest != item["sha256"] or size != item["bytes"]:
            _fail(f"predictive_plan:input_mismatch:{ordinal}")
        if ordinal >= len(source_lines):
            _fail("predictive_plan:source_manifest_short")
        listed = Path(source_lines[ordinal])
        if not listed.is_absolute():
            listed = Path(source_root) / listed
        if listed.resolve() != item_path.resolve():
            _fail(f"predictive_plan:input_sequence_mismatch:{ordinal}")
    return value, [dict(item) for item in inputs], plan_sha


def bind_batch_to_plan(rows: list[dict[str, Any]], plan_inputs: list[dict[str, Any]]) -> None:
    if len(rows) != len(plan_inputs):
        _fail("predictive_plan:batch_input_count_mismatch")
    for ordinal, row in enumerate(rows):
        if row["predictive_input"] != plan_inputs[ordinal]:
            _fail(f"predictive_plan:batch_input_mismatch:{ordinal}")


def load_topology(path: Path, rows: list[dict[str, Any]]) -> str:
    """Require an explicit C1F1 assignment map bound to the ordered inputs."""
    digest, _ = _sha(path)
    try:
        value = json.loads(path.read_text())
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise LiveRunnerError("topology:invalid_json") from exc
    if (not isinstance(value, dict) or
            value.get("schema") != "icecream-s8-topology-assignment-v1" or
            value.get("suite") != TOPOLOGY):
        _fail("topology:unsupported_suite")
    assignments = value.get("assignments", value.get("inputs"))
    if not isinstance(assignments, list) or len(assignments) != len(rows):
        _fail("topology:assignment_count_mismatch")
    for ordinal, (item, row) in enumerate(zip(assignments, rows, strict=True)):
        if (not isinstance(item, dict) or item.get("ordinal") != ordinal or
                item.get("tu_id") != row["tu_id"]):
            _fail(f"topology:{ordinal}:identity_mismatch")
        if item.get("relationship") != 0 or item.get("f_slot") != 0:
            _fail(f"topology:{ordinal}:C1F1_assignment_invalid")
    return digest


def _inside(path: Path, root: Path) -> bool:
    try:
        path.resolve().relative_to(root.resolve())
    except ValueError:
        return False
    return True


def product_identity(product_root: Path, script: Path = SCRIPT) -> tuple[str, str, dict[str, str], str]:
    """Bind source and all runtime executables to the selected product tree."""
    if (not product_root.is_absolute() or product_root.is_symlink() or
            not product_root.is_dir()):
        _fail("product_root:unavailable")
    root = product_root.resolve()
    script_resolved = script.resolve()
    if not _inside(script_resolved, root) or script.is_symlink() or not script.is_file():
        _fail("runner_script:outside_product_root")
    roles = ("scheduler/icecc-scheduler", "daemon/iceccd", "client/icecc",
             "cache/icecc-cache-service")
    binaries: dict[str, str] = {}
    for role in roles:
        path = root / role
        if not _inside(path, root):
            _fail(f"binary_identity:{role}:outside_product_root")
        digest, _ = _sha(path)
        binaries[role] = digest
    try:
        status = subprocess.check_output(
            ["git", "-C", str(root), "status", "--porcelain", "--untracked-files=no"],
            text=True, stderr=subprocess.STDOUT)
        commit = subprocess.check_output(["git", "-C", str(root), "rev-parse", "HEAD"], text=True).strip()
        tree = subprocess.check_output(["git", "-C", str(root), "rev-parse", "HEAD^{tree}"], text=True).strip()
    except (OSError, subprocess.SubprocessError) as exc:
        raise LiveRunnerError("source_identity:unavailable") from exc
    if status:
        _fail("source_identity:tracked_or_index_dirty")
    if HEX40.fullmatch(commit) is None or HEX40.fullmatch(tree) is None:
        _fail("source_identity:invalid")
    script_sha, _ = _sha(script)
    return commit, tree, binaries, script_sha


def build_command(batch_manifest: Path, profile: str,
                  *, product_root: Path, corpus: str = "DuckDB",
                  regime: str = "cold", depth: str = "100", full_count: int | None = None,
                  passes: int = 2, workdir: Path | None = None,
                  predictive_plan: Path | None = None, script: Path = SCRIPT) -> list[str]:
    """Build the exact launch argv; this function never executes it."""
    if profile not in PROFILES:
        _fail("profile:undeclared")
    if corpus not in CORPORA:
        _fail("corpus:undeclared")
    if regime not in REGIMES:
        _fail("regime:undeclared")
    count = selected_count(depth, full_count)
    if passes not in (1, 2):
        _fail("passes:undeclared")
    derive_timeout(count, passes, regime == "warm")
    if predictive_plan is None or not predictive_plan.is_absolute() or not predictive_plan.is_file() or predictive_plan.is_symlink():
        _fail("predictive_plan:unavailable")
    load_predictive_plan(predictive_plan, corpus=corpus, profile=profile,
                         regime=regime, depth=depth)
    if not batch_manifest.is_absolute() or not batch_manifest.is_file() or batch_manifest.is_symlink():
        _fail("batch_manifest:unavailable")
    load_batch_manifest(batch_manifest, count)
    product_identity(product_root, script)
    command = ["env", f"ICECC_TEST_TOP_SRCDIR={product_root}",
            f"ICECC_TEST_TOP_BUILDDIR={product_root}",
            f"ICECC_P50_PROFILE={profile}", f"ICECC_P50_CORPUS={corpus}",
            f"ICECC_P50_C1F1_WARM={int(regime == 'warm')}",
            f"ICECC_P50_C1F1_TIMEOUT={derive_timeout(count, passes, regime == 'warm')}",
            f"ICECC_P50_C1F1_PASSES={passes}",
            f"ICECC_P50_C1F1_EXPECTED_COUNT={count}",
            "ICECC_P50_C1F1_KEEP_WORK=1",
            f"ICECC_P50_C1F1_BATCH_MANIFEST={batch_manifest}",
            f"ICECC_P50_PREDICTIVE_PLAN={predictive_plan}"]
    if workdir is not None:
        if (not workdir.is_absolute() or workdir.name.startswith("p50compilee2e.") is False):
            _fail("workdir:invalid")
        command.append(f"ICECC_P50_C1F1_WORKDIR={workdir}")
    command.append(str(script))
    return command


def _fields(stdout: str, prefix: str) -> list[dict[str, str]]:
    result = []
    for line in stdout.splitlines():
        if not line.startswith(prefix + " "):
            continue
        row: dict[str, str] = {}
        for token in line.split()[1:]:
            if "=" in token:
                key, val = token.split("=", 1)
                row[key] = val
        result.append(row)
    return result


def _binary_identity(stdout: str) -> dict[str, str]:
    binaries: dict[str, str] = {}
    for row in _fields(stdout, "S8_BINARY"):
        role, digest = row.get("role"), row.get("sha256")
        if not isinstance(role, str) or role in binaries or HEX64.fullmatch(digest or "") is None:
            _fail("binary_identity:invalid")
        binaries[role] = digest  # type: ignore[assignment]
    if set(binaries) != {"scheduler/icecc-scheduler", "daemon/iceccd", "client/icecc", "cache/icecc-cache-service"}:
        _fail("binary_identity:incomplete")
    return binaries


def _validate_product_log_evidence(work: Path, observations: list[dict[str, Any]],
                                   profile: str) -> None:
    """Require per-TU profile/session evidence from the product log.

    The shell gate may only enforce that a command returned successfully; it
    must not turn that assertion into a trusted metric.  The selected profile
    and CACHE_SESSION handoff are therefore checked here for every measured
    client invocation, alongside the byte/object and action evidence below.
    """
    profile_markers = {
        "P29": ("P29", "CACHE_SESSION"),
        "ZSTD_TU": ("ZSTD_TU", "CACHE_SESSION"),
        "ZSTD_ROUTE": ("ZSTD_ROUTE", "CACHE_SESSION"),
        "GRZ_RESIDUAL": ("GRZ_RESIDUAL", "CACHE_SESSION"),
    }
    try:
        markers = profile_markers[profile]
    except KeyError as exc:  # pragma: no cover - argparse/schema guards this
        raise LiveRunnerError("profile:undeclared") from exc
    for index, observed in enumerate(observations):
        log = work / f"client-compile-{observed['run']}-{observed['ordinal']}.log"
        try:
            raw = log.read_text(encoding="utf-8", errors="replace")
        except OSError as exc:
            raise LiveRunnerError(f"batch:{index}:client_log_missing") from exc
        if not all(marker in raw for marker in markers):
            _fail(f"batch:{index}:product_profile_evidence_missing")
        if re.search(r"building myself|building_local|local build forced|fallback_local|client_exception", raw):
            _fail(f"batch:{index}:product_local_fallback")


def _timing_rows(stdout: str, rows: list[dict[str, Any]], work: Path,
                 passes: int) -> list[dict[str, Any]]:
    observations = _fields(stdout, "S8_BATCH_TU")
    if len(observations) != passes * len(rows):
        _fail("batch:incomplete_product_rows")
    result: list[dict[str, Any]] = []
    for index, observed in enumerate(observations):
        run = observed.get("run")
        ordinal = int(observed.get("ordinal", "-1")) if observed.get("ordinal", "").isdigit() else -1
        expected = rows[ordinal] if 0 <= ordinal < len(rows) else None
        expected_run = "full-1" if index < len(rows) else "full-2"
        if (run != expected_run or expected is None or
                observed.get("tu_id") != expected["tu_id"]):
            _fail(f"batch:{index}:identity_invalid")
        if ordinal != index % len(rows):
            _fail(f"batch:{index}:order_invalid")
        for key in ("source_sha256", "preprocessed_sha256", "remote_sha256", "local_sha256"):
            _hex(observed.get(key), f"batch:{index}.{key}")
        if observed["source_sha256"] != expected["sha256"]:
            _fail(f"batch:{index}:source_digest_mismatch")
        integer_fields = ("preprocessed_bytes", "remote_bytes", "local_bytes", "compile_start_ns",
                          "compile_end_ns", "wait_for_cs_ns", "assignment", "relationship", "f_slot")
        parsed = {}
        for key in integer_fields:
            try:
                parsed[key] = int(observed[key])
            except (KeyError, TypeError, ValueError) as exc:
                raise LiveRunnerError(f"batch:{index}:{key}_invalid") from exc
        if (parsed["preprocessed_bytes"] <= 0 or parsed["remote_bytes"] <= 0 or
                parsed["local_bytes"] <= 0 or parsed["compile_end_ns"] <= parsed["compile_start_ns"] or
                parsed["wait_for_cs_ns"] <= 0 or parsed["assignment"] != 0 or
                parsed["relationship"] != 0 or parsed["f_slot"] != 0):
            _fail(f"batch:{index}:product_metric_invalid")
        if (observed["preprocessed_sha256"] != expected["predictive_input"]["sha256"] or
                parsed["preprocessed_bytes"] != expected["predictive_input"]["bytes"]):
            _fail(f"batch:{index}:predictive_payload_mismatch")
        remote_path, local_path, pre_path = Path(observed["remote_path"]), Path(observed["local_path"]), Path(observed["preprocessed_path"])
        for path, expected_sha, expected_bytes, label in (
                (remote_path, observed["remote_sha256"], parsed["remote_bytes"], "remote_object"),
                (local_path, observed["local_sha256"], parsed["local_bytes"], "local_object"),
                (pre_path, observed["preprocessed_sha256"], parsed["preprocessed_bytes"], "preprocessed")):
            expected_parent = work if label == "preprocessed" else work / "out"
            if path.parent != expected_parent:
                _fail(f"batch:{index}:{label}_path_invalid")
            digest, size = _sha(path)
            if digest != expected_sha or size != expected_bytes:
                _fail(f"batch:{index}:{label}_digest_mismatch")
        if remote_path.read_bytes() != local_path.read_bytes():
            _fail(f"batch:{index}:object_not_byte_identical")
        result.append({**observed, **parsed, "run": run, "ordinal": ordinal,
                       "elapsed_ns": parsed["compile_end_ns"] - parsed["compile_start_ns"],
                       "client_elapsed_ns": parsed["compile_end_ns"] - parsed["compile_start_ns"],
                       "measured_elapsed_ns": parsed["wait_for_cs_ns"],
                       "channel_bytes": 0,
                       "returned_object_bytes": parsed["remote_bytes"]})
    return result


def _action_stage_paths(c_path: Path, f_path: Path, expected_count: int) -> list[dict[str, Any]]:
    if not c_path.is_file() or not f_path.is_file():
        _fail("action_trace:missing")
    c_rows = action_parser.parse_action(c_path.read_text())
    f_rows = action_parser.parse_action(f_path.read_text())
    c_begins = [row for row in c_rows if row["action"] == "TX_BEGIN" and row["actor"] == "C"]
    f_begins = [row for row in f_rows if row["action"] == "TX_BEGIN" and row["actor"] == "F"]
    if len(c_begins) != expected_count or len(f_begins) != expected_count:
        _fail("action_trace:TX_BEGIN_count_mismatch")
    identities = {(row["c_store_guid"], row["f_store_guid"]) for row in c_begins + f_begins}
    if len(identities) != 1:
        _fail("action_trace:C_F_identity_changed")
    for index, (c_row, f_row) in enumerate(zip(c_begins, f_begins, strict=True)):
        # The two role traces must describe the same transaction, not merely
        # the same long-lived service GUIDs.  Keep the F-side state digest as
        # evidence below; C's stage bytes are the measured C->F channel.
        for field in ("tu_seq", "rel_seq", "history_nonce", "raw_digest", "transaction_digest"):
            if c_row[field] != f_row[field]:
                _fail(f"action_trace:{index}:{field}_mismatch")
        if index and (c_row["tu_seq"] != c_begins[index - 1]["tu_seq"] + 1 or
                      c_row["rel_seq"] != c_begins[index - 1]["rel_seq"] + 1):
            _fail(f"action_trace:{index}:sequence_not_contiguous")
    return [{"c_to_f_bytes": int(row["stage_bytes"]),
             "c_store_guid": row["c_store_guid"], "f_store_guid": row["f_store_guid"],
             "tu_seq": int(row["tu_seq"]), "rel_seq": int(row["rel_seq"]),
             "history_nonce": int(row["history_nonce"]),
             "raw_digest": row["raw_digest"], "state_digest": row["state_digest"],
             "transaction_digest": row["transaction_digest"],
             "f_state_digest": f_row["state_digest"], "f_raw_digest": f_row["raw_digest"]}
            for row, f_row in zip(c_begins, f_begins, strict=True)]


def _action_stage(work: Path, expected_count: int) -> list[dict[str, Any]]:
    # These are the post-prewarm slices emitted by the shell gate.  Reading
    # the full warm trace would admit prewarm transactions as measurements.
    return _action_stage_paths(work / "s7-measured-c-action-trace.jsonl",
                               work / "s7-measured-f-action-trace.jsonl",
                               expected_count)


def _validate_warm_continuation(prewarm: list[dict[str, Any]],
                                measured: list[dict[str, Any]], expected_count: int) -> None:
    """Bind measured TU zero to the terminal prewarm transaction state."""
    if len(prewarm) != expected_count or len(measured) < expected_count:
        _fail("action_trace:prewarm_count_mismatch")
    terminal, first = prewarm[-1], measured[0]
    if (terminal["c_store_guid"] != first["c_store_guid"] or
            terminal["f_store_guid"] != first["f_store_guid"] or
            first["tu_seq"] != terminal["tu_seq"] + 1 or
            first["rel_seq"] != terminal["rel_seq"] + 1 or
            first["history_nonce"] != terminal["history_nonce"]):
        _fail("action_trace:measured_state_does_not_continue_prewarm")


def _write_new(path: Path, raw: bytes) -> None:
    if path.exists() or path.is_symlink():
        _fail(f"output_exists:{path}")
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("xb") as stream:
        stream.write(raw)
        stream.flush()
        os.fsync(stream.fileno())


def finalize(stdout: str, returncode: int, *, batch_manifest: Path, topology: Path,
             predictive_plan: Path, output: Path, profile: str, product_root: Path,
             corpus: str = "DuckDB", regime: str = "cold", depth: str = "100",
             full_count: int | None = None, passes: int = 2,
             artifact_sample: int = 2, retain_all_artifacts: bool = False,
             launch_identity: dict[str, Any] | None = None,
             timestamp: str | None = None) -> Path:
    """Turn one completed product invocation into two authenticated live curves."""
    if returncode != 0 or "PASS: all-P50 C1F1" not in stdout:
        _fail("product_run:did_not_pass")
    if corpus not in CORPORA or regime not in REGIMES:
        _fail("cell:undeclared")
    plan, plan_inputs, plan_sha = load_predictive_plan(
        predictive_plan, corpus=corpus, profile=profile, regime=regime, depth=depth)
    if full_count is None and depth == "full":
        full_count = len(plan_inputs)
    expected_count = selected_count(depth, full_count)
    if passes not in (1, 2) or type(artifact_sample) is not int or artifact_sample < 0:
        _fail("run_options:invalid")
    rows = load_batch_manifest(batch_manifest, expected_count)
    bind_batch_to_plan(rows, plan_inputs)
    topology_sha = load_topology(topology, rows)
    work_lines = [line.split("=", 1)[1] for line in stdout.splitlines() if line.startswith("S7_WORKDIR=")]
    if len(work_lines) != 1:
        _fail("product_run:workdir_missing")
    work = Path(work_lines[0])
    if work.is_symlink() or not work.is_dir():
        _fail("product_run:workdir_unavailable")
    def output_value(prefix: str) -> str:
        values = [line.split("=", 1)[1] for line in stdout.splitlines()
                  if line.startswith(prefix + "=")]
        if len(values) != 1:
            _fail(f"product_run:{prefix}:missing_or_duplicate")
        return values[0]
    if output_value("S8_BATCH_COUNT") != str(expected_count):
        _fail("product_run:batch_count_mismatch")
    if output_value("S8_BATCH_PASSES") != str(passes):
        _fail("product_run:batch_passes_mismatch")
    if output_value("S8_BATCH_WARM") != str(int(regime == "warm")):
        _fail("product_run:batch_regime_mismatch")
    if "S8_SCHEDULING mode=serial execution_slots=1 max_concurrency=1" not in stdout.splitlines():
        _fail("product_run:scheduling_identity_missing")
    expected_binaries = _binary_identity(stdout)
    commit, tree, binaries, runner_sha = product_identity(product_root)
    if binaries != expected_binaries:
        _fail("binary_identity:stdout_mismatch")
    if launch_identity is not None:
        if (launch_identity.get("source_commit") != commit or
                launch_identity.get("source_tree") != tree or
                launch_identity.get("binary_sha256") != binaries or
                launch_identity.get("runner_sha256") != runner_sha):
            _fail("product_identity:changed_during_run")
    observations = _timing_rows(stdout, rows, work, passes)
    _validate_product_log_evidence(work, observations, profile)
    stages = _action_stage(work, len(observations))
    if len(stages) != len(observations):
        _fail("action_trace:stage_count_mismatch")
    for observation, action in zip(observations, stages, strict=True):
        observation.update(action)
        observation["c_to_f_bytes"] = action["c_to_f_bytes"]
        observation["f_to_c_bytes"] = observation["returned_object_bytes"]
        observation["channel_bytes"] = (observation["c_to_f_bytes"] +
                                         observation["f_to_c_bytes"])
        observation["elapsed_ns"] = observation["measured_elapsed_ns"]
        if action["c_to_f_bytes"] <= 0:
            _fail("action_trace:zero_stage_bytes")
    prewarm_stages: list[dict[str, Any]] = []
    if regime == "warm":
        prewarm_stages = _action_stage_paths(
            work / "s7-prewarm-c-action-trace.jsonl",
            work / "s7-prewarm-f-action-trace.jsonl", expected_count)
        _validate_warm_continuation(prewarm_stages, stages, expected_count)
    batch_manifest_sha, batch_manifest_bytes = _sha(batch_manifest)
    input_sha = hashlib.sha256(_canonical({"source_manifest_sha256": plan["source_manifest"]["sha256"],
                                           "inputs": plan_inputs})).hexdigest()
    if timestamp is None:
        timestamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    if TIMESTAMP.fullmatch(timestamp) is None:
        _fail("timestamp:invalid")
    target = output / "icecream" / TOPOLOGY.replace("/", "-") / timestamp / profile
    target.parent.mkdir(parents=True, exist_ok=True)
    target.mkdir(parents=True, exist_ok=False)
    # Preserve the product-generated files, not just their digests.  The
    # source workdir remains untouched; these are private copies in the new
    # immutable experiment directory.
    retained = target / "product-evidence"
    retained.mkdir()
    shutil.copy2(batch_manifest, retained / "batch-manifest.jsonl")
    shutil.copy2(topology, retained / "topology.json")
    shutil.copy2(predictive_plan, retained / "predictive-plan.json")
    payload_raw = _canonical({"source_manifest_sha256": plan["source_manifest"]["sha256"],
                              "inputs": plan_inputs})
    _write_new(retained / "input-descriptors.json", payload_raw)
    retained_input_sha, retained_input_bytes = _sha(retained / "batch-manifest.jsonl")
    retained_payload_sha, retained_payload_bytes = _sha(retained / "input-descriptors.json")
    retained_topology_sha, retained_topology_bytes = _sha(retained / "topology.json")
    retained_plan_sha, retained_plan_bytes = _sha(retained / "predictive-plan.json")
    if (retained_input_sha != batch_manifest_sha or retained_topology_sha != topology_sha or
            retained_plan_sha != plan_sha or retained_payload_sha != input_sha):
        _fail("manifest_snapshot:changed_during_copy")
    trace_paths = [work / "s7-measured-c-action-trace.jsonl", work / "s7-measured-f-action-trace.jsonl"]
    if regime == "warm":
        trace_paths.extend((work / "s7-prewarm-c-action-trace.jsonl", work / "s7-prewarm-f-action-trace.jsonl"))
    for path in trace_paths:
        if not path.is_file():
            _fail(f"action_trace:retained_file_missing:{path.name}")
        shutil.copy2(path, retained / path.name)
    for item in observations:
        if retain_all_artifacts or item["ordinal"] < artifact_sample:
            for field in ("preprocessed_path", "remote_path", "local_path"):
                source = Path(item[field])
                shutil.copy2(source, retained / source.name)
            log = work / f"client-compile-{item['run']}-{item['ordinal']}.log"
            shutil.copy2(log, retained / log.name)
    cell = f"{corpus}/{profile}/{regime}"
    split = SPLITS[corpus]
    summary_value = {"schema": "icecream-s7-live-cell-v1",
                     "cell": cell, "split": split, "status": "PASS",
                     "live_status": "PASS", "acceptance_status": "PASS",
                     "conformance_status": "PASS", "binary_sha256": binaries,
                     "measured": {"input_sha256": input_sha}}
    summary_raw = _canonical(summary_value) + b"\n"
    _write_new(target / "results.jsonl", summary_raw)
    timing_by_run: dict[str, bytes] = {}
    run_names = ["full-1"] + (["full-2"] if passes == 2 else [])
    for run in run_names:
        timing_rows = [{"schema": "icecream-s7-live-timing-v1", "cell": cell,
                        "phase": "measured", "ordinal": item["ordinal"], "tu_id": item["tu_id"],
                        "elapsed_ns": item["elapsed_ns"], "channel_bytes": item["channel_bytes"],
                        "C_TO_F_bytes": item["c_to_f_bytes"], "F_TO_C_bytes": item["f_to_c_bytes"],
                        "object_sha256": item["remote_sha256"], "remote_compile": True,
                        "wait_for_cs_ns": item["wait_for_cs_ns"],
                        "client_elapsed_ns": item["client_elapsed_ns"],
                        "returned_object_bytes": item["returned_object_bytes"], "run": run,
                        "measurement_window": "compile+result_return",
                        "assignment": item["assignment"], "relationship": item["relationship"],
                        "f_slot": item["f_slot"], "tu_seq": item["tu_seq"],
                        "rel_seq": item["rel_seq"], "c_store_guid": item["c_store_guid"],
                        "f_store_guid": item["f_store_guid"], "state_digest": item["state_digest"],
                        "f_state_digest": item["f_state_digest"],
                        "transaction_digest": item["transaction_digest"],
                        "raw_digest": item["raw_digest"], "f_raw_digest": item["f_raw_digest"],
                        "history_nonce": item["history_nonce"]}
                       for item in observations if item["run"] == run]
        timing_raw = b"".join(_canonical(row) + b"\n" for row in timing_rows)
        timing_by_run[run] = timing_raw
        _write_new(target / f"timing_{run}.jsonl", timing_raw)
    # The current S8 live normalizer consumes one measured curve and rejects
    # duplicate TU identities.  Keep full-1 as the canonical live package and
    # retain the state-carrying repeat as a separately named witness.
    timing_raw = timing_by_run["full-1"]
    _write_new(target / "timing.jsonl", timing_raw)
    c_action_raw = (retained / "s7-measured-c-action-trace.jsonl").read_bytes()
    f_action_raw = (retained / "s7-measured-f-action-trace.jsonl").read_bytes()
    prewarm_descriptor = None
    if regime == "warm":
        pre_c_raw = (retained / "s7-prewarm-c-action-trace.jsonl").read_bytes()
        pre_f_raw = (retained / "s7-prewarm-f-action-trace.jsonl").read_bytes()
        prewarm_descriptor = {"count": len(prewarm_stages), "input_digest": input_sha,
                              "c_action_sha256": hashlib.sha256(pre_c_raw).hexdigest(),
                              "f_action_sha256": hashlib.sha256(pre_f_raw).hexdigest(),
                              "terminal_tu_seq": prewarm_stages[-1]["tu_seq"],
                              "terminal_rel_seq": prewarm_stages[-1]["rel_seq"]}
    evidence_sha = hashlib.sha256(_canonical({
        "results": hashlib.sha256(summary_raw).hexdigest(),
        "c_action": hashlib.sha256(c_action_raw).hexdigest(),
        "f_action": hashlib.sha256(f_action_raw).hexdigest(),
    })).hexdigest()
    witness = {"c_action": {"path": "product-evidence/s7-measured-c-action-trace.jsonl", "sha256": hashlib.sha256(c_action_raw).hexdigest(), "bytes": len(c_action_raw)},
               "f_action": {"path": "product-evidence/s7-measured-f-action-trace.jsonl", "sha256": hashlib.sha256(f_action_raw).hexdigest(), "bytes": len(f_action_raw)}}
    if passes == 2:
        witness["timing_full_2"] = {"path": "timing_full-2.jsonl", "sha256": hashlib.sha256(timing_by_run["full-2"]).hexdigest(), "bytes": len(timing_by_run["full-2"])}
    evidence_value = {"schema": "icecream-s7-live-evidence-v1",
                      "cell": cell, "split": split, "run_id": "s8-real-c1f1",
                      "source_commit": commit, "source_tree": tree,
                      "input_manifest_sha256": batch_manifest_sha, "input_digest": input_sha,
                      "topology_sha256": topology_sha,
                      "input_manifest": {"path": "product-evidence/batch-manifest.jsonl",
                                          "sha256": retained_input_sha,
                                          "bytes": retained_input_bytes},
                      "input_descriptors": {"path": "product-evidence/input-descriptors.json",
                                            "sha256": retained_payload_sha,
                                            "bytes": retained_payload_bytes},
                      "predictive_plan": {"path": "product-evidence/predictive-plan.json",
                                          "sha256": retained_plan_sha, "bytes": retained_plan_bytes},
                      "topology": {"path": "product-evidence/topology.json",
                                   "sha256": retained_topology_sha,
                                   "bytes": retained_topology_bytes},
                      "runner": {"name": "p50compilee2e-run.sh", "sha256": runner_sha},
                      "binary_sha256": binaries, "execution_environment": "host_product_build",
                      "state_carrying_repeat": passes == 2,
                      "measurement_window": "compile+result_return",
                      "prewarm": prewarm_descriptor,
                      "evidence": {"results": {"path": "results.jsonl", "sha256": hashlib.sha256(summary_raw).hexdigest(), "bytes": len(summary_raw)},
                                   "timing": {"path": "timing.jsonl", "sha256": hashlib.sha256(timing_raw).hexdigest(), "bytes": len(timing_raw)}},
                      "witness": witness}
    evidence_value["evidence_sha256"] = hashlib.sha256(_canonical(evidence_value)).hexdigest()
    evidence_raw = _canonical(evidence_value) + b"\n"
    _write_new(target / "evidence.json", evidence_raw)
    evidence_manifest_sha = hashlib.sha256(evidence_raw).hexdigest()
    records: list[dict[str, Any]] = []
    manifests: dict[str, str] = {}
    for run in run_names:
        selected = [row for row in observations if row["run"] == run]
        elapsed = channel = 0
        curve: list[dict[str, Any]] = []
        for step, (item, source) in enumerate(zip(selected, rows, strict=True)):
            elapsed += item["elapsed_ns"]
            channel += item["channel_bytes"]
            curve.append({"step": step, "tu_id": source["tu_id"], "cell": {"corpus": corpus, "profile": profile, "regime": regime},
                          "cumulative": {"channel_bytes": channel, "elapsed_ns": elapsed,
                                         "throughput_bytes_per_s": channel * 1_000_000_000 / elapsed}})
        curve_raw = b"".join(_canonical(row) + b"\n" for row in curve)
        curve_name = f"live_curve_{run}.jsonl"
        manifest_name = f"live_curve_manifest_{run}.json"
        manifest_value = {"schema": normalizer.MANIFEST_SCHEMA,
                          "identity": {"corpus": corpus, "profile": profile, "regime": regime,
                                        "split": split, "run_id": run,
                                        "source_commit": commit, "source_tree": tree,
                                        "input_digest": input_sha, "topology_digest": topology_sha,
                                        "model_id": "s8-real-live"},
                          "units": {"point": "step", "channel_bytes": "bytes", "elapsed_ns": "ns",
                                    "throughput_bytes_per_s": "bytes_per_s"},
                          "curve": {"path": curve_name, "sha256": hashlib.sha256(curve_raw).hexdigest(), "bytes": len(curve_raw)},
                          "provenance": {"mode": "live", "producer": "s8_real_c1f1_live_runner", "trace_free": False},
                          "evidence": {"results_sha256": hashlib.sha256(summary_raw).hexdigest(),
                                       "evidence_manifest_sha256": evidence_manifest_sha,
                                       "binary_sha256": binaries, "evidence_sha256": evidence_value["evidence_sha256"]}}
        manifest_raw = _canonical(manifest_value) + b"\n"
        _write_new(target / curve_name, curve_raw)
        _write_new(target / manifest_name, manifest_raw)
        artifact = {"identity": manifest_value["identity"], "units": manifest_value["units"],
                    "provenance": manifest_value["provenance"], "manifest_sha256": hashlib.sha256(manifest_raw).hexdigest(),
                    "curve_sha256": hashlib.sha256(curve_raw).hexdigest(), "rows": curve,
                    "evidence": manifest_value["evidence"]}
        records.append(normalizer._normalized_record("live", artifact))
        manifests[run] = manifest_name
    _write_new(target / "live_curve.jsonl", (target / "live_curve_full-1.jsonl").read_bytes())
    _write_new(target / "live_curve_manifest.json", (target / "live_curve_manifest_full-1.json").read_bytes())
    _write_new(target / "records.jsonl", b"".join(_canonical(record) + b"\n" for record in records))
    experiment = {"schema": SCHEMA, "cell": {"corpus": corpus, "profile": profile, "regime": regime},
                  "split": split, "topology": TOPOLOGY, "depth": depth,
                  "declared_count": expected_count, "runs": ["full-1"] + (["full-2"] if passes == 2 else []),
                  "same_service_state": True, "input_manifest_sha256": batch_manifest_sha,
                  "input_digest": input_sha,
                  "topology_sha256": topology_sha, "predictive_plan_sha256": plan_sha,
                  "execution_environment": "host_product_build",
                  "binary_sha256": binaries, "source_commit": commit, "source_tree": tree,
                  "runner_sha256": runner_sha,
                  "launch_identity": launch_identity,
                  "curve_manifests": manifests, "remote_compile_required": True,
                  "scheduling": {"mode": "serial", "execution_slots": 1, "max_concurrency": 1},
                  "artifact_retention": {"mode": "all" if retain_all_artifacts else "sample",
                                         "sample_tus_per_run": artifact_sample},
                  "prewarm": regime == "warm", "prewarm_evidence": prewarm_descriptor}
    _write_new(target / "experiment_manifest.json", _canonical(experiment) + b"\n")
    # The lifecycle temporary tree is diagnostic state on failure, but must
    # not survive a fully authenticated successful retention.
    if not work.name.startswith("p50compilee2e."):
        _fail("workdir:unsafe_cleanup_path")
    shutil.rmtree(work)
    return target


def _run_product(command: list[str], timeout: int) -> tuple[str, int]:
    """Run the shell lifecycle as one process group with signal cleanup."""
    process = subprocess.Popen(command, text=True, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, start_new_session=True)
    previous: dict[int, Any] = {}

    def interrupt(signum: int, _frame: object) -> None:
        if process.poll() is None:
            os.killpg(process.pid, signal.SIGTERM)
        raise KeyboardInterrupt

    try:
        for signum in (signal.SIGINT, signal.SIGTERM):
            previous[signum] = signal.signal(signum, interrupt)
        try:
            stdout, _ = process.communicate(timeout=timeout)
            return stdout, process.returncode
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGTERM)
            try:
                stdout, _ = process.communicate(timeout=10)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                stdout, _ = process.communicate()
            raise LiveRunnerError(f"product_run:timeout:{timeout}")
        except KeyboardInterrupt as exc:
            try:
                stdout, _ = process.communicate(timeout=10)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                stdout, _ = process.communicate()
            raise LiveRunnerError("product_run:interrupted") from exc
    finally:
        for signum, handler in previous.items():
            signal.signal(signum, handler)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--batch-manifest", type=Path, required=True)
    parser.add_argument("--predictive-plan", type=Path, required=True)
    parser.add_argument("--topology", type=Path, required=True)
    parser.add_argument("--profile", choices=PROFILES, required=True)
    parser.add_argument("--product-root", type=Path, required=True)
    parser.add_argument("--corpus", choices=CORPORA, default="DuckDB")
    parser.add_argument("--regime", choices=REGIMES, default="cold")
    parser.add_argument("--depth", choices=("100", "200", "full"), default="100")
    parser.add_argument("--full-count", type=int)
    parser.add_argument("--passes", type=int, choices=(1, 2), default=2)
    parser.add_argument("--artifact-sample", type=int, default=2)
    parser.add_argument("--retain-all-artifacts", action="store_true")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--timestamp", help="UTC experiment directory, YYYYMMDDTHHMMSSZ")
    parser.add_argument("--execute", action="store_true", help="execute one real run; intentionally separate from dry-run tests")
    args = parser.parse_args(argv)
    batch_manifest = args.batch_manifest.absolute()
    predictive_plan = args.predictive_plan.absolute()
    topology = args.topology.absolute()
    _plan, plan_inputs, _plan_sha = load_predictive_plan(
        predictive_plan, corpus=args.corpus, profile=args.profile,
        regime=args.regime, depth=args.depth)
    if args.depth == "full":
        args.full_count = len(plan_inputs)
    count = selected_count(args.depth, args.full_count)
    rows = load_batch_manifest(batch_manifest, count)
    load_topology(topology, rows)
    run_workdir: Path | None = None
    if args.execute:
        run_workdir = Path(tempfile.mkdtemp(prefix="p50compilee2e.", dir=tempfile.gettempdir()))
        run_workdir.rmdir()
    launch_identity = None
    if args.execute:
        launch_commit, launch_tree, launch_binaries, launch_runner_sha = product_identity(args.product_root.absolute())
        launch_identity = {"source_commit": launch_commit, "source_tree": launch_tree,
                           "binary_sha256": launch_binaries, "runner_sha256": launch_runner_sha}
    command = build_command(batch_manifest, args.profile,
                            product_root=args.product_root.absolute(), corpus=args.corpus,
                            regime=args.regime, depth=args.depth, full_count=args.full_count,
                            passes=args.passes, workdir=run_workdir,
                            predictive_plan=predictive_plan)
    if not args.execute:
        print(json.dumps({"schema": SCHEMA, "status": "DRY_RUN", "command": command}, sort_keys=True))
        return 0
    try:
        timeout = derive_timeout(count, args.passes, args.regime == "warm")
        try:
            stdout, returncode = _run_product(command, timeout)
        except LiveRunnerError as exc:
            print(f"timeout_seconds={timeout}")
            print(f"workdir={run_workdir}" if run_workdir is not None else "workdir=unknown")
            print(str(exc))
            return 77
        path = finalize(stdout, returncode, batch_manifest=batch_manifest,
                        topology=topology, output=args.output.absolute(), profile=args.profile,
                        predictive_plan=predictive_plan, product_root=args.product_root.absolute(),
                        corpus=args.corpus, regime=args.regime, depth=args.depth,
                        full_count=args.full_count, passes=args.passes,
                        artifact_sample=args.artifact_sample,
                        retain_all_artifacts=args.retain_all_artifacts,
                        launch_identity=launch_identity,
                        timestamp=args.timestamp)
    except LiveRunnerError as exc:
        print(str(exc))
        if run_workdir is not None:
            print(f"workdir={run_workdir}")
        return 77
    print(path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
