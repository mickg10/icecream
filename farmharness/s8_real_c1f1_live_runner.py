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
import shutil
import subprocess
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

try:
    from . import s6_live_route_acceptance as action_parser
    from . import s8_predictive_live_normalizer as normalizer
    from .s8_schema import PROFILES
except ImportError:  # pragma: no cover
    import s6_live_route_acceptance as action_parser
    import s8_predictive_live_normalizer as normalizer
    from s8_schema import PROFILES


SCHEMA = "icecream-s8-real-c1f1-live-runner-v1"
CELL = "DuckDB/{profile}/cold"
TOPOLOGY = "C1F1/100000"
SCRIPT = Path(__file__).resolve().parents[1] / "unittests/p50compilee2e-run.sh"
HEX64 = re.compile(r"^[0-9a-f]{64}$")
HEX40 = re.compile(r"^[0-9a-f]{40}$")
IMAGE = re.compile(r"^sha256:[0-9a-f]{64}$")
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
        required = {"tu_id", "source", "sha256"}
        if not required.issubset(value) or set(value) - required - {"compile_db", "compile_source"}:
            _fail(f"batch_manifest:{ordinal}:fields_invalid")
        tu_id, source = value["tu_id"], value["source"]
        if (not isinstance(tu_id, str) or SAFE.fullmatch(tu_id) is None or tu_id in seen
                or not isinstance(source, str) or not os.path.isabs(source)):
            _fail(f"batch_manifest:{ordinal}:identity_invalid")
        digest, size = _sha(Path(source))
        if value.get("sha256") != digest:
            _fail(f"batch_manifest:{ordinal}:source_digest_mismatch")
        row = {"ordinal": ordinal, "tu_id": tu_id, "source": source,
               "sha256": digest, "bytes": size}
        for key in ("compile_db", "compile_source"):
            if key in value:
                if not isinstance(value[key], str) or not os.path.isabs(value[key]):
                    _fail(f"batch_manifest:{ordinal}:{key}_invalid")
                _sha(Path(value[key]))
                row[key] = value[key]
        rows.append(row)
        seen.add(tu_id)
    return rows


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


def build_command(batch_manifest: Path, profile: str, image_id: str,
                  *, product_root: Path, script: Path = SCRIPT) -> list[str]:
    """Build the exact launch argv; this function never executes it."""
    if profile not in PROFILES:
        _fail("profile:undeclared")
    if IMAGE.fullmatch(image_id) is None:
        _fail("image_id:invalid")
    if not batch_manifest.is_absolute() or not batch_manifest.is_file() or batch_manifest.is_symlink():
        _fail("batch_manifest:unavailable")
    if not product_root.is_absolute() or not product_root.is_dir() or product_root.is_symlink():
        _fail("product_root:unavailable")
    if not script.is_file() or script.is_symlink():
        _fail("runner_script:unavailable")
    return ["env", f"ICECC_TEST_TOP_SRCDIR={product_root}",
            f"ICECC_TEST_TOP_BUILDDIR={product_root}",
            f"ICECC_P50_PROFILE={profile}", "ICECC_P50_C1F1_WARM=0",
            "ICECC_P50_C1F1_KEEP_WORK=1",
            f"ICECC_P50_C1F1_IMAGE_ID={image_id}",
            f"ICECC_P50_C1F1_BATCH_MANIFEST={batch_manifest}", str(script)]


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


def _product_identity(stdout: str, expected_image: str) -> tuple[dict[str, str], dict[str, str]]:
    image_lines = [line for line in stdout.splitlines() if line.startswith("S8_IMAGE_ID=")]
    if image_lines != [f"S8_IMAGE_ID={expected_image}"]:
        _fail("image_identity:missing_or_mismatch")
    binaries: dict[str, str] = {}
    for row in _fields(stdout, "S8_BINARY"):
        role, digest = row.get("role"), row.get("sha256")
        if not isinstance(role, str) or role in binaries or HEX64.fullmatch(digest or "") is None:
            _fail("binary_identity:invalid")
        binaries[role] = digest  # type: ignore[assignment]
    if set(binaries) != {"scheduler/icecc-scheduler", "daemon/iceccd", "client/icecc", "cache/icecc-cache-service"}:
        _fail("binary_identity:incomplete")
    return {"image_id": expected_image}, binaries


def _timing_rows(stdout: str, rows: list[dict[str, Any]], work: Path) -> list[dict[str, Any]]:
    observations = _fields(stdout, "S8_BATCH_TU")
    if len(observations) != 2 * len(rows):
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
                parsed["relationship"] != 0 or parsed["f_slot"] != 0 or observed.get("remote_compile") != "1" or
                observed.get("byte_identical") != "1"):
            _fail(f"batch:{index}:product_metric_invalid")
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
        result.append({**observed, **parsed, "run": run, "ordinal": ordinal,
                       "elapsed_ns": parsed["compile_end_ns"] - parsed["compile_start_ns"],
                       "channel_bytes": 0,
                       "returned_object_bytes": parsed["remote_bytes"]})
    return result


def _action_stage(work: Path, expected_count: int) -> list[dict[str, Any]]:
    c_path, f_path = work / "s7-warm-c-action-trace.jsonl", work / "s7-warm-f-action-trace.jsonl"
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
        for field in ("tu_seq", "rel_seq", "transaction_digest"):
            if c_row[field] != f_row[field]:
                _fail(f"action_trace:{index}:{field}_mismatch")
    return [{"stage_bytes": int(row["stage_bytes"]),
             "c_store_guid": row["c_store_guid"], "f_store_guid": row["f_store_guid"],
             "tu_seq": int(row["tu_seq"]), "rel_seq": int(row["rel_seq"]),
             "state_digest": row["state_digest"], "transaction_digest": row["transaction_digest"],
             "f_state_digest": f_row["state_digest"]}
            for row, f_row in zip(c_begins, f_begins, strict=True)]


def _write_new(path: Path, raw: bytes) -> None:
    if path.exists() or path.is_symlink():
        _fail(f"output_exists:{path}")
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("xb") as stream:
        stream.write(raw)
        stream.flush()
        os.fsync(stream.fileno())


def finalize(stdout: str, returncode: int, *, batch_manifest: Path, topology: Path,
             output: Path, profile: str, image_id: str, product_root: Path,
             source_repo: Path, timestamp: str | None = None) -> Path:
    """Turn one completed product invocation into two authenticated live curves."""
    if returncode != 0 or "PASS: all-P50 C1F1" not in stdout:
        _fail("product_run:did_not_pass")
    rows = load_batch_manifest(batch_manifest)
    topology_sha = load_topology(topology, rows)
    work_lines = [line.split("=", 1)[1] for line in stdout.splitlines() if line.startswith("S7_WORKDIR=")]
    if len(work_lines) != 1:
        _fail("product_run:workdir_missing")
    work = Path(work_lines[0])
    if not work.is_dir():
        _fail("product_run:workdir_unavailable")
    _, binaries = _product_identity(stdout, image_id)
    for role, expected in binaries.items():
        actual, _ = _sha(product_root / role)
        if actual != expected:
            _fail(f"binary_identity:{role}:digest_mismatch")
    try:
        commit = subprocess.check_output(["git", "-C", str(source_repo), "rev-parse", "HEAD"], text=True).strip()
        tree = subprocess.check_output(["git", "-C", str(source_repo), "rev-parse", "HEAD^{tree}"], text=True).strip()
    except (OSError, subprocess.SubprocessError) as exc:
        raise LiveRunnerError("source_identity:unavailable") from exc
    if HEX40.fullmatch(commit) is None or HEX40.fullmatch(tree) is None:
        _fail("source_identity:invalid")
    observations = _timing_rows(stdout, rows, work)
    stages = _action_stage(work, len(observations))
    if len(stages) != len(observations):
        _fail("action_trace:stage_count_mismatch")
    for observation, action in zip(observations, stages, strict=True):
        observation.update(action)
        observation["channel_bytes"] = action["stage_bytes"]
        if action["stage_bytes"] <= 0:
            _fail("action_trace:zero_stage_bytes")
    input_sha, _ = _sha(batch_manifest)
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
    retained_input_sha, retained_input_bytes = _sha(retained / "batch-manifest.jsonl")
    retained_topology_sha, retained_topology_bytes = _sha(retained / "topology.json")
    if retained_input_sha != input_sha or retained_topology_sha != topology_sha:
        _fail("manifest_snapshot:changed_during_copy")
    for path in (work / "s7-warm-c-action-trace.jsonl", work / "s7-warm-f-action-trace.jsonl"):
        shutil.copy2(path, retained / path.name)
    for item in observations:
        for field in ("preprocessed_path", "remote_path", "local_path"):
            source = Path(item[field])
            shutil.copy2(source, retained / source.name)
        shutil.copy2(work / f"client-compile-{item['run']}-{item['ordinal']}.log",
                     retained / f"client-compile-{item['run']}-{item['ordinal']}.log")
    summary_value = {"schema": "icecream-s7-live-cell-v1",
                     "cell": f"DuckDB/{profile}/cold", "status": "PASS",
                     "live_status": "PASS", "acceptance_status": "PASS",
                     "conformance_status": "PASS", "binary_sha256": binaries,
                     "measured": {"input_sha256": input_sha}}
    summary_raw = _canonical(summary_value) + b"\n"
    _write_new(target / "results.jsonl", summary_raw)
    timing_by_run: dict[str, bytes] = {}
    for run in ("full-1", "full-2"):
        timing_rows = [{"schema": "icecream-s7-live-timing-v1", "cell": f"DuckDB/{profile}/cold",
                        "phase": "measured", "ordinal": item["ordinal"], "tu_id": item["tu_id"],
                        "elapsed_ns": item["elapsed_ns"], "channel_bytes": item["channel_bytes"],
                        "object_sha256": item["remote_sha256"], "remote_compile": True,
                        "wait_for_cs_ns": item["wait_for_cs_ns"],
                        "returned_object_bytes": item["returned_object_bytes"], "run": run,
                        "assignment": item["assignment"], "relationship": item["relationship"],
                        "f_slot": item["f_slot"], "tu_seq": item["tu_seq"],
                        "rel_seq": item["rel_seq"], "c_store_guid": item["c_store_guid"],
                        "f_store_guid": item["f_store_guid"], "state_digest": item["state_digest"],
                        "f_state_digest": item["f_state_digest"],
                        "transaction_digest": item["transaction_digest"]}
                       for item in observations if item["run"] == run]
        timing_raw = b"".join(_canonical(row) + b"\n" for row in timing_rows)
        timing_by_run[run] = timing_raw
        _write_new(target / f"timing_{run}.jsonl", timing_raw)
    # The current S8 live normalizer consumes one measured curve and rejects
    # duplicate TU identities.  Keep full-1 as the canonical live package and
    # retain the state-carrying repeat as a separately named witness.
    timing_raw = timing_by_run["full-1"]
    _write_new(target / "timing.jsonl", timing_raw)
    c_action_raw = (retained / "s7-warm-c-action-trace.jsonl").read_bytes()
    f_action_raw = (retained / "s7-warm-f-action-trace.jsonl").read_bytes()
    evidence_sha = hashlib.sha256(_canonical({
        "results": hashlib.sha256(summary_raw).hexdigest(),
        "c_action": hashlib.sha256(c_action_raw).hexdigest(),
        "f_action": hashlib.sha256(f_action_raw).hexdigest(),
        "image_id": image_id,
    })).hexdigest()
    evidence_value = {"schema": "icecream-s7-live-evidence-v1",
                      "cell": f"DuckDB/{profile}/cold", "run_id": "s8-real-c1f1",
                      "source_commit": commit, "source_tree": tree,
                      "input_manifest_sha256": input_sha, "topology_sha256": topology_sha,
                      "input_manifest": {"path": "product-evidence/batch-manifest.jsonl",
                                          "sha256": retained_input_sha,
                                          "bytes": retained_input_bytes},
                      "topology": {"path": "product-evidence/topology.json",
                                   "sha256": retained_topology_sha,
                                   "bytes": retained_topology_bytes},
                      "runner": {"name": "p50compilee2e-run.sh", "sha256": hashlib.sha256(SCRIPT.read_bytes()).hexdigest()},
                      "binary_sha256": binaries, "state_carrying_repeat": True,
                      "evidence": {"results": {"path": "results.jsonl", "sha256": hashlib.sha256(summary_raw).hexdigest(), "bytes": len(summary_raw)},
                                   "timing": {"path": "timing.jsonl", "sha256": hashlib.sha256(timing_raw).hexdigest(), "bytes": len(timing_raw)}},
                      "witness": {"c_action": {"path": "product-evidence/s7-warm-c-action-trace.jsonl", "sha256": hashlib.sha256(c_action_raw).hexdigest(), "bytes": len(c_action_raw)},
                                   "f_action": {"path": "product-evidence/s7-warm-f-action-trace.jsonl", "sha256": hashlib.sha256(f_action_raw).hexdigest(), "bytes": len(f_action_raw)},
                                   "timing_full_2": {"path": "timing_full-2.jsonl", "sha256": hashlib.sha256(timing_by_run["full-2"]).hexdigest(), "bytes": len(timing_by_run["full-2"])}}}
    evidence_value["evidence_sha256"] = hashlib.sha256(_canonical(evidence_value)).hexdigest()
    evidence_raw = _canonical(evidence_value) + b"\n"
    _write_new(target / "evidence.json", evidence_raw)
    evidence_manifest_sha = hashlib.sha256(evidence_raw).hexdigest()
    records: list[dict[str, Any]] = []
    manifests: dict[str, str] = {}
    for run in ("full-1", "full-2"):
        selected = [row for row in observations if row["run"] == run]
        elapsed = channel = 0
        curve: list[dict[str, Any]] = []
        for step, (item, source) in enumerate(zip(selected, rows, strict=True)):
            elapsed += item["elapsed_ns"]
            channel += item["channel_bytes"]
            curve.append({"step": step, "tu_id": source["tu_id"], "cell": {"corpus": "DuckDB", "profile": profile, "regime": "cold"},
                          "cumulative": {"channel_bytes": channel, "elapsed_ns": elapsed,
                                         "throughput_bytes_per_s": channel * 1_000_000_000 / elapsed}})
        curve_raw = b"".join(_canonical(row) + b"\n" for row in curve)
        curve_name = f"live_curve_{run}.jsonl"
        manifest_name = f"live_curve_manifest_{run}.json"
        manifest_value = {"schema": normalizer.MANIFEST_SCHEMA,
                          "identity": {"corpus": "DuckDB", "profile": profile, "regime": "cold",
                                        "split": "held_out_validation", "run_id": run,
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
    experiment = {"schema": SCHEMA, "cell": {"corpus": "DuckDB", "profile": profile, "regime": "cold"},
                  "topology": TOPOLOGY, "depth": 100, "runs": ["full-1", "full-2"],
                  "same_service_state": True, "input_manifest_sha256": input_sha,
                  "topology_sha256": topology_sha, "image_id": image_id,
                  "binary_sha256": binaries, "source_commit": commit, "source_tree": tree,
                  "curve_manifests": manifests, "remote_compile_required": True}
    _write_new(target / "experiment_manifest.json", _canonical(experiment) + b"\n")
    return target


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--batch-manifest", type=Path, required=True)
    parser.add_argument("--topology", type=Path, required=True)
    parser.add_argument("--profile", choices=PROFILES, required=True)
    parser.add_argument("--image-id", required=True)
    parser.add_argument("--product-root", type=Path, required=True)
    parser.add_argument("--source-repo", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--timestamp", help="UTC experiment directory, YYYYMMDDTHHMMSSZ")
    parser.add_argument("--execute", action="store_true", help="execute one real run; intentionally separate from dry-run tests")
    args = parser.parse_args(argv)
    batch_manifest = args.batch_manifest.absolute()
    topology = args.topology.absolute()
    rows = load_batch_manifest(batch_manifest)
    load_topology(topology, rows)
    command = build_command(batch_manifest, args.profile, args.image_id,
                            product_root=args.product_root.absolute())
    if not args.execute:
        print(json.dumps({"schema": SCHEMA, "status": "DRY_RUN", "command": command}, sort_keys=True))
        return 0
    completed = subprocess.run(command, text=True, capture_output=True, check=False)
    try:
        path = finalize(completed.stdout, completed.returncode, batch_manifest=batch_manifest,
                        topology=topology, output=args.output.absolute(), profile=args.profile,
                        image_id=args.image_id, product_root=args.product_root.absolute(),
                        source_repo=args.source_repo.absolute(), timestamp=args.timestamp)
    except LiveRunnerError as exc:
        print(str(exc))
        return 77
    print(path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
