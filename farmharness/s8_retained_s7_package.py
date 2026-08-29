#!/usr/bin/env python3
"""Build one authenticated S8 input package from a retained S7 live cell.

The builder does not run the product or predictor.  It copies the exact S7
summary/input/evidence, derives one measured-TU observation from the retained
product trace and client log, and emits the topology and predictive manifest
that both sides of the later comparison must bind.  Raw object size and the
aggregate result timer are deliberately not substituted for wire bytes or the
declared ``wait for cs`` measurement window.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import stat
import subprocess
import sys
from pathlib import Path
from typing import Any

try:
    from .s8_live_metric_producer import (
        EVIDENCE_SCHEMA, LIVE_SCHEMA, TIMING_SCHEMA, _canonical, _hex,
        _snapshot, _write_new,
    )
    from .s8_predictive_engine import MANIFEST_SCHEMA, SEMANTICS, TOPOLOGY_SCHEMA
    from .s8_schema import CORPORA, PROFILES, REGIMES, SPLITS
except ImportError:  # pragma: no cover - direct harness invocation.
    from s8_live_metric_producer import (
        EVIDENCE_SCHEMA, LIVE_SCHEMA, TIMING_SCHEMA, _canonical, _hex,
        _snapshot, _write_new,
    )
    from s8_predictive_engine import MANIFEST_SCHEMA, SEMANTICS, TOPOLOGY_SCHEMA
    from s8_schema import CORPORA, PROFILES, REGIMES, SPLITS


SCHEMA = "icecream-s8-retained-s7-package-v1"
HEX32 = re.compile(r"^[0-9a-f]{32}$")
WAIT_RE = re.compile(r"</wait for cs: ([0-9]+)ms>")
GOT_RE = re.compile(r"got ([0-9]+) bytes \([0-9]+%\)")
CHANNELS = {
    "ZSTD_TU": "direct",
    "ZSTD_ROUTE": "route",
    "P29": "route",
    "GRZ_RESIDUAL": "residual",
}


class PackageError(ValueError):
    """The retained evidence is absent, ambiguous, or inconsistent."""


def _json(raw: bytes, label: str) -> Any:
    try:
        return json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise PackageError(f"{label}:invalid_json") from exc


def _one_json_line(raw: bytes, label: str) -> dict[str, Any]:
    lines = raw.splitlines()
    if len(lines) != 1:
        raise PackageError(f"{label}:expected_one_record")
    value = _json(lines[0], label)
    if not isinstance(value, dict):
        raise PackageError(f"{label}:not_object")
    return value


def _descriptor(relative: str, raw: bytes) -> dict[str, object]:
    return {"path": relative, "sha256": hashlib.sha256(raw).hexdigest(),
            "bytes": len(raw)}


def _sha(value: object, label: str) -> str:
    try:
        return _hex(value, label)
    except ValueError as exc:
        raise PackageError(str(exc)) from exc


def _guid(value: object, label: str) -> str:
    if not isinstance(value, str) or HEX32.fullmatch(value.lower()) is None or int(value, 16) == 0:
        raise PackageError(f"{label}:invalid_guid")
    return value.lower()


def _source_tree(repository: Path, commit: str) -> str:
    try:
        info = repository.lstat()
    except OSError as exc:
        raise PackageError("source_repository:unavailable") from exc
    if stat.S_ISLNK(info.st_mode) or not stat.S_ISDIR(info.st_mode):
        raise PackageError("source_repository:not_private_directory")
    try:
        completed = subprocess.run(
            ["git", "-C", str(repository), "rev-parse", f"{commit}^{{tree}}"],
            check=True, capture_output=True, text=True, timeout=15,
        )
    except (OSError, subprocess.SubprocessError) as exc:
        raise PackageError("source_tree:unavailable") from exc
    value = completed.stdout.strip().lower()
    if not re.fullmatch(r"[0-9a-f]{40}", value) or int(value, 16) == 0:
        raise PackageError("source_tree:invalid")
    return value


def _trace_rows(raw: bytes, label: str) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for number, line in enumerate(raw.splitlines(), 1):
        value = _json(line, f"{label}:{number}")
        if not isinstance(value, dict):
            raise PackageError(f"{label}:{number}:not_object")
        rows.append(value)
    if not rows:
        raise PackageError(f"{label}:empty")
    return rows


def _one_begin(rows: list[dict[str, Any]], actor: str) -> dict[str, Any]:
    matches = [row for row in rows if row.get("action") == "TX_BEGIN" and row.get("actor") == actor]
    if len(matches) != 1:
        raise PackageError(f"{actor.lower()}_trace:tx_begin_count={len(matches)}")
    return matches[0]


def _copy(out: Path, name: str, raw: bytes) -> dict[str, object]:
    path = out / name
    _write_new(path, raw)
    return _descriptor(name, raw)


def build(s7_package: Path, replay: Path, source_repository: Path, out: Path) -> Path:
    """Validate retained evidence and create a new immutable package directory."""
    if out.exists() or out.is_symlink():
        raise PackageError("output:already_exists")
    try:
        package_info = s7_package.lstat()
        replay_info = replay.lstat()
    except OSError as exc:
        raise PackageError("input_package:unavailable") from exc
    if (stat.S_ISLNK(package_info.st_mode) or not stat.S_ISDIR(package_info.st_mode) or
            stat.S_ISLNK(replay_info.st_mode) or not stat.S_ISDIR(replay_info.st_mode)):
        raise PackageError("input_package:not_private_directory")

    results_raw, results_sha = _snapshot(s7_package / "results.jsonl", "results")
    results = _one_json_line(results_raw, "results")
    if results.get("schema") != LIVE_SCHEMA:
        raise PackageError("results:schema_invalid")
    for field in ("status", "live_status", "acceptance_status", "conformance_status"):
        if results.get(field) != "PASS":
            raise PackageError(f"results:{field}_not_pass")
    cell_label = results.get("cell")
    if not isinstance(cell_label, str) or len(cell_label.split("/")) != 3:
        raise PackageError("results:cell_invalid")
    corpus, profile, regime = cell_label.split("/")
    if corpus not in CORPORA or profile not in PROFILES or regime not in REGIMES:
        raise PackageError("results:cell_not_declared")
    measured = results.get("measured")
    if not isinstance(measured, dict):
        raise PackageError("results:measured_missing")
    input_sha = _sha(measured.get("input_sha256"), "results.measured.input_sha256")
    tu_seq = measured.get("tu_seq")
    if type(tu_seq) is not int or tu_seq < 0:
        raise PackageError("results.measured.tu_seq:invalid")
    binaries = results.get("binary_sha256")
    if not isinstance(binaries, dict) or not binaries:
        raise PackageError("results.binary_sha256:missing")
    binary_hashes = {name: _sha(digest, f"results.binary_sha256.{name}")
                     for name, digest in binaries.items() if isinstance(name, str)}
    if len(binary_hashes) != len(binaries):
        raise PackageError("results.binary_sha256:invalid")
    live_commit = results.get("live_source_commit")
    if not isinstance(live_commit, str) or re.fullmatch(r"[0-9a-f]{40}", live_commit) is None:
        raise PackageError("results.live_source_commit:invalid")
    live_tree = _source_tree(source_repository, live_commit)

    replay_files = {
        "replay-manifest.json": replay / "manifest.json",
        "identities.json": replay / "identities.json",
        "stage-ledger.jsonl": replay / "stage-ledger.jsonl",
        "measured-c-action-trace.jsonl": replay / "measured-c-action-trace.jsonl",
        "measured-f-action-trace.jsonl": replay / "measured-f-action-trace.jsonl",
        "input.ii": replay / "measured.ii",
    }
    retained: dict[str, tuple[bytes, str]] = {
        name: _snapshot(path, f"retained.{name}") for name, path in replay_files.items()
    }
    replay_manifest = _json(retained["replay-manifest.json"][0], "replay_manifest")
    identities = _json(retained["identities.json"][0], "identities")
    if not isinstance(replay_manifest, dict) or not isinstance(identities, dict):
        raise PackageError("replay:manifest_or_identity_invalid")
    replay_input_sha = replay_manifest.get(
        "measured_input_sha256", replay_manifest.get("input_sha256"))
    if (replay_manifest.get("status") != "PASS" or replay_manifest.get("cell") != cell_label or
            replay_input_sha != input_sha or
            replay_manifest.get("deletion_control", {}).get("status") != "PASS"):
        raise PackageError("replay:acceptance_mismatch")
    conformance = results.get("conformance")
    if not isinstance(conformance, dict) or conformance.get("status") != "PASS":
        raise PackageError("results.conformance:not_pass")
    expected_hashes = {
        "manifest_sha256": retained["replay-manifest.json"][1],
        "identity_sha256": retained["identities.json"][1],
        "stage_ledger_sha256": retained["stage-ledger.jsonl"][1],
    }
    for field, observed in expected_hashes.items():
        if conformance.get(field) != observed:
            raise PackageError(f"results.conformance.{field}:mismatch")
    if retained["input.ii"][1] != input_sha:
        raise PackageError("retained_input:digest_mismatch")

    c_rows = _trace_rows(retained["measured-c-action-trace.jsonl"][0], "c_trace")
    f_rows = _trace_rows(retained["measured-f-action-trace.jsonl"][0], "f_trace")
    c_begin, f_begin = _one_begin(c_rows, "C"), _one_begin(f_rows, "F")
    joined_fields = ("c_store_guid", "f_store_guid", "history_nonce", "tu_seq",
                     "transaction_digest", "raw_digest", "stage_bytes")
    if any(c_begin.get(field) != f_begin.get(field) for field in joined_fields):
        raise PackageError("live_trace:tx_begin_identity_mismatch")
    if c_begin.get("tu_seq") != tu_seq:
        raise PackageError("live_trace:tu_seq_mismatch")
    c_to_f = c_begin.get("stage_bytes")
    if type(c_to_f) is not int or c_to_f <= 0:
        raise PackageError("live_trace:stage_bytes_invalid")
    c_guid = _guid(c_begin.get("c_store_guid"), "live_trace.c_store_guid")
    f_guid = _guid(c_begin.get("f_store_guid"), "live_trace.f_store_guid")
    history_nonce = c_begin.get("history_nonce")
    if type(history_nonce) is not int or history_nonce <= 0:
        raise PackageError("live_trace.history_nonce:invalid")
    manifest_identity = replay_manifest.get("identity")
    if (not isinstance(manifest_identity, dict) or
            manifest_identity.get("c_store_guid") != c_guid or
            manifest_identity.get("f_store_guid") != f_guid or
            manifest_identity.get("history_nonce") != history_nonce or
            identities.get("c_store_guid") != c_guid or
            identities.get("f_store_guid") != f_guid):
        raise PackageError("replay:topology_identity_mismatch")

    runtime = results.get("runtime")
    if not isinstance(runtime, str) or not runtime or Path(runtime).is_absolute() or ".." in Path(runtime).parts:
        raise PackageError("results.runtime:invalid")
    client_log_raw, _client_log_sha = _snapshot(
        s7_package / runtime / "client-compile-measured.log", "client_log")
    try:
        client_text = client_log_raw.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise PackageError("client_log:not_utf8") from exc
    waits, returned = WAIT_RE.findall(client_text), GOT_RE.findall(client_text)
    if len(waits) != 1 or len(returned) != 1:
        raise PackageError("client_log:wire_or_wait_observation_ambiguous")
    elapsed_ns = int(waits[0]) * 1_000_000
    f_to_c = int(returned[0])
    if elapsed_ns <= 0 or f_to_c <= 0:
        raise PackageError("client_log:wire_or_wait_observation_invalid")

    topology = {
        "schema": TOPOLOGY_SCHEMA, "semantics": SEMANTICS,
        "cell": {"corpus": corpus, "profile": profile, "regime": regime},
        "topology": {
            "c_store_guid": c_guid, "f_store_guid": f_guid,
            "history_nonce": history_nonce, "c_workers": 1, "f_workers": 1,
            "cache_channel": CHANNELS[profile],
        },
        "state": {"c_cache": regime, "f_cache": regime, "generation": 0},
    }
    topology_raw = _canonical(topology) + b"\n"
    tu_id = f"{corpus}-{regime}-input-000000"
    timing = {
        "schema": TIMING_SCHEMA, "cell": cell_label, "phase": "measured",
        "tu_id": tu_id, "elapsed_ns": elapsed_ns,
        "channel_bytes": c_to_f + f_to_c,
        "c_to_f_bytes": c_to_f, "f_to_c_bytes": f_to_c,
        "measurement_window": "client_wait_for_cs",
    }
    timing_raw = _canonical(timing) + b"\n"
    predictive_manifest = {
        "schema": MANIFEST_SCHEMA, "semantics": SEMANTICS,
        "cell": {"corpus": corpus, "profile": profile, "regime": regime},
        "split": SPLITS[corpus], "predictive_mode": True,
        "input": _descriptor("input.ii", retained["input.ii"][0]),
        "topology_state": _descriptor("topology.json", topology_raw),
    }
    predictive_raw = _canonical(predictive_manifest) + b"\n"

    out.parent.mkdir(parents=True, exist_ok=True)
    out.mkdir()
    evidence_files = {
        "results": _copy(out, "results.jsonl", results_raw),
        "timing": _copy(out, "timing.jsonl", timing_raw),
    }
    derivation_files = {
        name.rsplit(".", 1)[0].replace("-", "_"): _copy(out, name, raw)
        for name, (raw, _digest) in retained.items()
    }
    derivation_files["client_log"] = _copy(out, "client-compile-measured.log", client_log_raw)
    _copy(out, "topology.json", topology_raw)
    _copy(out, "predictive-manifest.json", predictive_raw)
    evidence: dict[str, object] = {
        "schema": EVIDENCE_SCHEMA, "cell": cell_label, "run_id": results.get("run_id"),
        "source_commit": live_commit, "source_tree": live_tree,
        "topology_sha256": hashlib.sha256(topology_raw).hexdigest(),
        "binary_sha256": binary_hashes, "measurement_window": "client_wait_for_cs",
        "wire_observation": {"c_to_f_bytes": c_to_f, "f_to_c_bytes": f_to_c,
                             "channel_bytes": c_to_f + f_to_c},
        "evidence": evidence_files, "derivation_evidence": derivation_files,
    }
    evidence["evidence_sha256"] = hashlib.sha256(_canonical(evidence)).hexdigest()
    evidence_raw = _canonical(evidence) + b"\n"
    _copy(out, "evidence.json", evidence_raw)
    package_manifest = {
        "schema": SCHEMA, "status": "PASS", "cell": cell_label,
        "measurement_window": "client_wait_for_cs", "tu_id": tu_id,
        "source_commit": live_commit, "source_tree": live_tree,
        "input_sha256": input_sha, "topology_sha256": hashlib.sha256(topology_raw).hexdigest(),
        "binary_sha256": binary_hashes,
        "wire_bytes": {"C_TO_F": c_to_f, "F_TO_C": f_to_c,
                       "total": c_to_f + f_to_c},
        "elapsed_ns": elapsed_ns,
        "results_source_sha256": results_sha,
        "evidence_sha256": evidence["evidence_sha256"],
        "files": {
            "results": evidence_files["results"], "timing": evidence_files["timing"],
            "evidence": _descriptor("evidence.json", evidence_raw),
            "predictive_manifest": _descriptor("predictive-manifest.json", predictive_raw),
            "topology": _descriptor("topology.json", topology_raw),
            "input": _descriptor("input.ii", retained["input.ii"][0]),
        },
    }
    manifest_path = out / "package-manifest.json"
    _write_new(manifest_path, _canonical(package_manifest) + b"\n")
    return manifest_path


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--s7-package", type=Path, required=True)
    parser.add_argument("--replay", type=Path, required=True)
    parser.add_argument("--source-repository", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        print(build(args.s7_package.absolute(), args.replay.absolute(),
                    args.source_repository.absolute(), args.out.absolute()))
    except (PackageError, ValueError) as exc:
        print(str(exc), file=sys.stderr)
        return 77
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
