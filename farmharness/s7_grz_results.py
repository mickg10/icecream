#!/usr/bin/env python3
"""Normalize one retained GRZ S7 run into its canonical live result row.

The standard runner's output directory is an input, never an output target.
All runtime, source, object, and product paths are explicit CLI arguments so
this command cannot silently select another run or manufacture missing facts.
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


SCHEMA = "icecream-s7-live-cell-v1"
PROFILE = "GRZ_RESIDUAL"
CELL_RE = re.compile(r"^(fmt|RocksDB)/GRZ_RESIDUAL/(cold|warm)$")
HEX = re.compile(r"^[0-9a-f]{64}$")
HEX40 = re.compile(r"^[0-9a-f]{40}$")
HEX32 = re.compile(r"^[0-9a-f]{32}$")
WAIT_RE = re.compile(r"</wait for cs: ([0-9]+)ms>")
GOT_RE = re.compile(r"got ([0-9]+) bytes \([0-9]+%\)")


class ResultsError(ValueError):
    pass


def canonical(value: object) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"),
                      ensure_ascii=True, allow_nan=False).encode("ascii")


def snapshot(path: Path, label: str, limit: int = 128 * 1024 * 1024) -> tuple[bytes, str]:
    try:
        info = path.lstat()
    except OSError as exc:
        raise ResultsError(f"{label}:unavailable:{path}") from exc
    if stat.S_ISLNK(info.st_mode) or not stat.S_ISREG(info.st_mode) or info.st_nlink != 1:
        raise ResultsError(f"{label}:not_private_regular_file:{path}")
    flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
    try:
        fd = os.open(path, flags)
    except OSError as exc:
        raise ResultsError(f"{label}:cannot_open:{path}") from exc
    try:
        before = os.fstat(fd)
        digest = hashlib.sha256()
        chunks: list[bytes] = []
        size = 0
        while True:
            block = os.read(fd, 1 << 20)
            if not block:
                break
            size += len(block)
            if size > limit:
                raise ResultsError(f"{label}:too_large")
            chunks.append(block)
            digest.update(block)
        after = os.fstat(fd)
        if any(getattr(before, field) != getattr(after, field)
               for field in ("st_dev", "st_ino", "st_size", "st_mtime_ns", "st_ctime_ns")):
            raise ResultsError(f"{label}:changed_while_reading:{path}")
        return b"".join(chunks), digest.hexdigest()
    finally:
        os.close(fd)


def digest(value: object, label: str, pattern: re.Pattern[str] = HEX) -> str:
    if not isinstance(value, str) or pattern.fullmatch(value) is None or int(value, 16) == 0:
        raise ResultsError(f"{label}:invalid_digest")
    return value.lower()


def descriptor(path: Path, raw: bytes, relative: str | None = None) -> dict[str, object]:
    return {"path": relative or path.name, "sha256": hashlib.sha256(raw).hexdigest(),
            "bytes": len(raw)}


def json_one(raw: bytes, label: str) -> dict[str, Any]:
    lines = raw.splitlines()
    if len(lines) != 1:
        raise ResultsError(f"{label}:expected_one_record")
    try:
        value = json.loads(lines[0].decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ResultsError(f"{label}:invalid_json") from exc
    if not isinstance(value, dict):
        raise ResultsError(f"{label}:not_object")
    return value


def jsonl(raw: bytes, label: str) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for n, line in enumerate(raw.splitlines(), 1):
        try:
            value = json.loads(line.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise ResultsError(f"{label}:{n}:invalid_json") from exc
        if not isinstance(value, dict):
            raise ResultsError(f"{label}:{n}:not_object")
        rows.append(value)
    if not rows:
        raise ResultsError(f"{label}:empty")
    return rows


def parse_binary(value: str) -> tuple[str, Path]:
    if "=" not in value:
        raise ResultsError("binary:expected_name=path")
    name, path = value.split("=", 1)
    if not name or not path:
        raise ResultsError("binary:expected_name=path")
    return name, Path(path)


def _tx_begin(path: Path, actor: str) -> tuple[dict[str, Any], bytes, str]:
    raw, sha = snapshot(path, f"{actor.lower()}_trace")
    rows = jsonl(raw, f"{actor.lower()}_trace")
    matches = [row for row in rows if row.get("action") == "TX_BEGIN" and row.get("actor") == actor]
    if len(matches) != 1:
        raise ResultsError(f"{actor.lower()}_trace:tx_begin_count={len(matches)}")
    return matches[0], raw, sha


def _inside(path: Path, root: Path, label: str) -> str:
    try:
        return path.resolve().relative_to(root.resolve()).as_posix()
    except ValueError as exc:
        raise ResultsError(f"{label}:outside_experiment") from exc


def _evidence_path(path: Path, root: Path) -> str:
    """Use a relative descriptor when possible, otherwise retain explicit path."""
    try:
        return path.resolve().relative_to(root.resolve()).as_posix()
    except ValueError:
        return str(path)


def build(*, experiment: Path, runtime: Path, replay: Path, corpus: str,
          regime: str, source_repository: Path, source_commit: str,
          source_file: Path, build_root: Path, local_object: Path,
          remote_object: Path, binaries: list[str], out: Path,
          prewarm_input: Path | None = None,
          prewarm_c_trace: Path | None = None,
          prewarm_f_trace: Path | None = None) -> Path:
    """Authenticate and write exactly one canonical result row."""
    if out.exists() or out.is_symlink():
        raise ResultsError("output:already_exists")
    if corpus not in {"fmt", "RocksDB"} or regime not in {"cold", "warm"}:
        raise ResultsError("cell:unsupported")
    experiment = experiment.resolve(); runtime = runtime.resolve(); replay = replay.resolve()
    source_repository = source_repository.resolve(); source_file = source_file.resolve()
    build_root = build_root.resolve(); local_object = local_object.resolve(); remote_object = remote_object.resolve()
    if not experiment.is_dir() or experiment.is_symlink() or not replay.is_dir() or replay.is_symlink():
        raise ResultsError("experiment_or_replay:not_private_directory")
    if (not runtime.is_dir() or runtime.is_symlink() or
            not source_repository.is_dir() or source_repository.is_symlink() or
            not build_root.is_dir() or build_root.is_symlink()):
        raise ResultsError("runtime_or_source:not_private_directory")
    source_commit = digest(source_commit, "source_commit", HEX40)
    try:
        tree = subprocess.run(["git", "-C", str(source_repository), "rev-parse", f"{source_commit}^{{tree}}"],
                              check=True, capture_output=True, text=True, timeout=15).stdout.strip()
    except (OSError, subprocess.SubprocessError) as exc:
        raise ResultsError("source_tree:unavailable") from exc
    source_tree = digest(tree, "source_tree", HEX40)
    source_raw, source_sha = snapshot(source_file, "source")
    local_raw, local_sha = snapshot(local_object, "local_object")
    remote_raw, remote_sha = snapshot(remote_object, "remote_object")
    if local_raw != remote_raw or len(local_raw) != len(remote_raw):
        raise ResultsError("objects:not_byte_identical")
    binary_hashes: dict[str, str] = {}
    binary_descriptors: dict[str, dict[str, object]] = {}
    for spec in binaries:
        name, path = parse_binary(spec)
        _inside(path, build_root, f"binary.{name}")
        raw, sha = snapshot(path, f"binary.{name}")
        binary_hashes[name] = sha
        binary_descriptors[name] = descriptor(path, raw, _evidence_path(path, experiment))
    replay_manifest_path = replay / "manifest.json"
    identities_path = replay / "identities.json"
    stage_path = replay / "stage-ledger.jsonl"
    c_trace_path = replay / "measured-c-action-trace.jsonl"
    f_trace_path = replay / "measured-f-action-trace.jsonl"
    input_path = replay / "measured.ii"
    replay_manifest_raw, replay_manifest_sha = snapshot(replay_manifest_path, "replay_manifest")
    replay_manifest = json_one(replay_manifest_raw, "replay_manifest")
    identities_raw, identities_sha = snapshot(identities_path, "identities")
    identities = json_one(identities_raw, "identities")
    stage_raw, stage_sha = snapshot(stage_path, "stage_ledger")
    input_raw, input_sha = snapshot(input_path, "measured_input")
    c_begin, c_raw, c_sha = _tx_begin(c_trace_path, "C")
    f_begin, f_raw, f_sha = _tx_begin(f_trace_path, "F")
    joined = ("c_store_guid", "f_store_guid", "history_nonce", "tu_seq",
              "transaction_digest", "raw_digest", "stage_bytes")
    if any(c_begin.get(field) != f_begin.get(field) for field in joined):
        raise ResultsError("traces:tx_begin_identity_mismatch")
    if c_begin.get("stage_bytes", 0) <= 0 or not isinstance(c_begin.get("stage_bytes"), int):
        raise ResultsError("traces:stage_bytes_invalid")
    input_sha = digest(input_sha, "input_sha256")
    if replay_manifest.get("status") != "PASS" or replay_manifest.get("cell") != f"{corpus}/{PROFILE}/{regime}":
        raise ResultsError("replay_manifest:acceptance_mismatch")
    if replay_manifest.get("input_sha256", replay_manifest.get("measured_input_sha256")) != input_sha:
        raise ResultsError("replay_manifest:input_mismatch")
    if replay_manifest.get("deletion_control", {}).get("status") != "PASS" or replay_manifest.get("mutation_control", {}).get("status") != "PASS":
        raise ResultsError("replay_manifest:controls_not_pass")
    c_guid, f_guid = c_begin.get("c_store_guid"), c_begin.get("f_store_guid")
    if not isinstance(c_guid, str) or HEX32.fullmatch(c_guid) is None or not isinstance(f_guid, str) or HEX32.fullmatch(f_guid) is None:
        raise ResultsError("traces:guid_invalid")
    if (identities.get("c_store_guid") != c_guid or identities.get("f_store_guid") != f_guid or
            identities.get("history_nonce") != c_begin.get("history_nonce")):
        raise ResultsError("identities:topology_mismatch")
    if source_file.read_bytes() != source_raw:
        raise ResultsError("source:unstable")
    client_log = runtime / "client-compile-measured.log"
    client_raw, client_sha = snapshot(client_log, "client_log")
    try:
        client_text = client_raw.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise ResultsError("client_log:not_utf8") from exc
    waits, returned = WAIT_RE.findall(client_text), GOT_RE.findall(client_text)
    if len(waits) != 1 or len(returned) != 1:
        raise ResultsError("client_log:wire_or_wait_observation_ambiguous")
    elapsed_ns, f_to_c = int(waits[0]) * 1_000_000, int(returned[0])
    if elapsed_ns <= 0 or f_to_c <= 0:
        raise ResultsError("client_log:wire_or_wait_observation_invalid")
    prewarm_descriptors: dict[str, object] = {}
    prewarm_count = 0
    if regime == "warm":
        if not all((prewarm_input, prewarm_c_trace, prewarm_f_trace)):
            raise ResultsError("warm:prewarm_evidence_required")
        for label, path in (("prewarm_input", prewarm_input), ("prewarm_c_trace", prewarm_c_trace), ("prewarm_f_trace", prewarm_f_trace)):
            raw, _ = snapshot(Path(path), label)
            prewarm_descriptors[label] = descriptor(Path(path), raw, _inside(Path(path), replay.parent, label))
        prewarm_c_rows = jsonl(Path(prewarm_c_trace).read_bytes(), "prewarm_c_trace")
        prewarm_f_rows = jsonl(Path(prewarm_f_trace).read_bytes(), "prewarm_f_trace")
        prewarm_count = sum(row.get("action") == "TX_BEGIN" for row in prewarm_c_rows + prewarm_f_rows)
        if prewarm_count != 2 or not any(row.get("tu_seq") == 0 for row in prewarm_c_rows + prewarm_f_rows):
            raise ResultsError("warm:prewarm_identity_invalid")
    cell = f"{corpus}/{PROFILE}/{regime}"
    descriptors = {
        "replay_manifest": descriptor(replay_manifest_path, replay_manifest_raw, _inside(replay_manifest_path, experiment, "replay_manifest")),
        "identities": descriptor(identities_path, identities_raw, _inside(identities_path, experiment, "identities")),
        "stage_ledger": descriptor(stage_path, stage_raw, _inside(stage_path, experiment, "stage_ledger")),
        "measured_c_trace": descriptor(c_trace_path, c_raw, _inside(c_trace_path, experiment, "measured_c_trace")),
        "measured_f_trace": descriptor(f_trace_path, f_raw, _inside(f_trace_path, experiment, "measured_f_trace")),
        "measured_input": descriptor(input_path, input_raw, _inside(input_path, experiment, "measured_input")),
        "client_log": descriptor(client_log, client_raw, _inside(client_log, experiment, "client_log")),
        "source": descriptor(source_file, source_raw, _evidence_path(source_file, experiment)),
        "local_object": descriptor(local_object, local_raw, _evidence_path(local_object, experiment)),
        "remote_object": descriptor(remote_object, remote_raw, _evidence_path(remote_object, experiment)),
        "binaries": binary_descriptors,
        **prewarm_descriptors,
    }
    evidence_sha = hashlib.sha256(canonical({"cell": cell, "files": descriptors})).hexdigest()
    row = {
        "schema": SCHEMA, "cell": cell, "corpus": corpus, "profile": PROFILE, "regime": regime,
        "status": "PASS", "live_status": "PASS", "acceptance_status": "PASS", "conformance_status": "PASS",
        "run_id": experiment.name, "runtime": _inside(runtime, experiment, "runtime"),
        "live_source_commit": source_commit, "replay_source_commit": source_commit,
        "replay_source_tree": source_tree,
        "source": {"relative": source_file.name, "sha256": source_sha, "bytes": len(source_raw)},
        "binary_sha256": binary_hashes,
        "measured": {"input_sha256": input_sha, "tu_seq": c_begin["tu_seq"],
                     "source_transfer_bytes": int(c_begin["stage_bytes"]),
                     "remote_bytes": len(remote_raw), "remote_sha256": remote_sha,
                     "local_bytes": len(local_raw), "local_sha256": local_sha,
                     "byte_identical": True, "remote_compile_ms": None,
                     "f_to_c_wire_bytes": f_to_c, "elapsed_ns": elapsed_ns,
                     "measurement_window": "client_wait_for_cs"},
        "conformance": {"status": "PASS", "action_count": replay_manifest.get("action_count"),
                        "measured_action_count": replay_manifest.get("measured_action_count"),
                        "prewarm_action_count": replay_manifest.get("prewarm_action_count", 0),
                        "deletion_control": "PASS", "mutation_control": "PASS",
                        "manifest_sha256": replay_manifest_sha, "identity_sha256": identities_sha,
                        "stage_ledger_sha256": stage_sha},
        "topology": {"c_store_guid": c_guid, "f_store_guid": f_guid,
                     "history_nonce": c_begin["history_nonce"], "tu_seq": c_begin["tu_seq"]},
        "evidence": {"sha256": evidence_sha, "files": descriptors,
                     "measurement_window": "client_wait_for_cs",
                     "prewarm_bound": regime == "warm", "prewarm_count": prewarm_count},
    }
    out.parent.mkdir(parents=True, exist_ok=True)
    out.mkdir()
    result_path = out / "results.jsonl"
    result_raw = canonical(row) + b"\n"
    with result_path.open("xb") as stream:
        stream.write(result_raw); stream.flush(); os.fsync(stream.fileno())
    return result_path


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--experiment", type=Path, required=True)
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--replay", type=Path, required=True)
    parser.add_argument("--corpus", choices=("fmt", "RocksDB"), required=True)
    parser.add_argument("--regime", choices=("cold", "warm"), required=True)
    parser.add_argument("--source-repository", type=Path, required=True)
    parser.add_argument("--source-commit", required=True)
    parser.add_argument("--source-file", type=Path, required=True)
    parser.add_argument("--build-root", type=Path, required=True)
    parser.add_argument("--local-object", type=Path, required=True)
    parser.add_argument("--remote-object", type=Path, required=True)
    parser.add_argument("--binary", action="append", default=[], metavar="NAME=PATH")
    parser.add_argument("--prewarm-input", type=Path)
    parser.add_argument("--prewarm-c-trace", type=Path)
    parser.add_argument("--prewarm-f-trace", type=Path)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        print(build(experiment=args.experiment.absolute(), runtime=args.runtime.absolute(),
                    replay=args.replay.absolute(), corpus=args.corpus, regime=args.regime,
                    source_repository=args.source_repository.absolute(), source_commit=args.source_commit,
                    source_file=args.source_file.absolute(), build_root=args.build_root.absolute(),
                    local_object=args.local_object.absolute(), remote_object=args.remote_object.absolute(),
                    binaries=args.binary, out=args.out.absolute(), prewarm_input=args.prewarm_input,
                    prewarm_c_trace=args.prewarm_c_trace, prewarm_f_trace=args.prewarm_f_trace))
    except (ResultsError, OSError, ValueError) as exc:
        print(str(exc), file=sys.stderr)
        return 77
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
