#!/usr/bin/env python3
"""Produce an authenticated S8 live metric curve from retained S7 evidence.

This is deliberately an evidence adapter, not a simulator.  It reads one S7
``results.jsonl`` summary, one producer-owned evidence manifest, and the
manifest-declared measured timing JSONL.  It never opens predictive output,
action traces, or a product executable to manufacture observations.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import stat
import sys
from pathlib import Path
from typing import Any

try:
    from . import s8_predictive_live_normalizer as normalizer
except ImportError:  # pragma: no cover - direct harness invocation.
    import s8_predictive_live_normalizer as normalizer


SCHEMA = "icecream-s8-live-metric-producer-v1"
EVIDENCE_SCHEMA = "icecream-s7-live-evidence-v1"
TIMING_SCHEMA = "icecream-s7-live-timing-v1"
LIVE_SCHEMA = "icecream-s7-live-cell-v1"
CURVE_MANIFEST_SCHEMA = normalizer.MANIFEST_SCHEMA
HEX64 = re.compile(r"^[0-9a-fA-F]{64}$")
HEX40 = re.compile(r"^[0-9a-fA-F]{40}$")
CELL_RE = re.compile(r"^(fmt|RocksDB)/(ZSTD_TU|ZSTD_ROUTE|P29|GRZ_RESIDUAL)/(cold|warm)$")
MAX_BYTES = 64 * 1024 * 1024


class ProducerError(ValueError):
    """Evidence is absent, unauthenticated, incomplete, or inconsistent."""


def _canonical(value: object) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"),
                      ensure_ascii=True, allow_nan=False).encode("ascii")


def _hex(value: object, label: str, pattern: re.Pattern[str] = HEX64) -> str:
    if not isinstance(value, str) or pattern.fullmatch(value) is None:
        raise ProducerError(f"{label}:invalid_digest")
    result = value.lower()
    if int(result, 16) == 0:
        raise ProducerError(f"{label}:zero_digest")
    return result


def _json(raw: bytes, label: str) -> object:
    try:
        return json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ProducerError(f"{label}:invalid_json") from exc


def _snapshot(path: Path, label: str, limit: int = MAX_BYTES) -> tuple[bytes, str]:
    try:
        info = path.lstat()
    except OSError as exc:
        raise ProducerError(f"{label}:unavailable:{path}") from exc
    if (stat.S_ISLNK(info.st_mode) or not stat.S_ISREG(info.st_mode) or
            info.st_nlink != 1):
        raise ProducerError(f"{label}:not_private_regular_file:{path}")
    flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
    try:
        fd = os.open(path, flags)
    except OSError as exc:
        raise ProducerError(f"{label}:cannot_open:{path}") from exc
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
                raise ProducerError(f"{label}:too_large")
            chunks.append(block)
            digest.update(block)
        after = os.fstat(fd)
        if any(getattr(before, field) != getattr(after, field)
               for field in ("st_dev", "st_ino", "st_size", "st_mtime_ns", "st_ctime_ns")):
            raise ProducerError(f"{label}:changed_while_reading:{path}")
        return b"".join(chunks), digest.hexdigest()
    finally:
        os.close(fd)


def _descriptor(path: Path, raw: bytes, digest: str | None = None) -> dict[str, object]:
    return {"path": path.name, "sha256": digest or hashlib.sha256(raw).hexdigest(),
            "bytes": len(raw)}


def _relative(package: Path, value: object, label: str) -> Path:
    if not isinstance(value, str) or not value:
        raise ProducerError(f"{label}:path_missing")
    candidate = Path(value)
    if candidate.is_absolute() or any(part in ("", ".", "..") for part in candidate.parts):
        raise ProducerError(f"{label}:path_not_private_relative")
    path = package / candidate
    try:
        path.resolve().relative_to(package.resolve())
    except ValueError as exc:
        raise ProducerError(f"{label}:path_escapes_package") from exc
    return path


def _descriptor_map(value: object, label: str) -> dict[str, dict[str, object]]:
    if not isinstance(value, dict) or not value:
        raise ProducerError(f"{label}:missing")
    result: dict[str, dict[str, object]] = {}
    for name, descriptor in value.items():
        if not isinstance(name, str) or not name or not isinstance(descriptor, dict):
            raise ProducerError(f"{label}:descriptor_invalid")
        if set(descriptor) != {"path", "sha256", "bytes"}:
            raise ProducerError(f"{label}.{name}:descriptor_fields_invalid")
        _hex(descriptor["sha256"], f"{label}.{name}.sha256")
        if type(descriptor["bytes"]) is not int or descriptor["bytes"] <= 0:
            raise ProducerError(f"{label}.{name}.bytes:invalid")
        result[name] = dict(descriptor)
    return result


def _read_summary(package: Path) -> tuple[dict[str, Any], bytes, str]:
    raw, digest = _snapshot(package / "results.jsonl", "results")
    lines = raw.splitlines()
    if len(lines) != 1:
        raise ProducerError("results:expected_one_summary_record")
    value = _json(lines[0], "results")
    if not isinstance(value, dict) or value.get("schema") != LIVE_SCHEMA:
        raise ProducerError("results:schema_invalid")
    cell = value.get("cell")
    if not isinstance(cell, str) or CELL_RE.fullmatch(cell) is None:
        raise ProducerError("results:cell_invalid")
    for field in ("status", "live_status", "acceptance_status", "conformance_status"):
        if value.get(field) != "PASS":
            raise ProducerError(f"results:{field}_not_pass")
    measured = value.get("measured")
    if not isinstance(measured, dict):
        raise ProducerError("results:measured_missing")
    _hex(measured.get("input_sha256"), "results.measured.input_sha256")
    return value, raw, digest


def _read_evidence(package: Path, summary: dict[str, Any], summary_raw: bytes,
                   summary_sha: str) -> tuple[dict[str, Any], dict[str, dict[str, object]], dict[str, str], str]:
    manifest_path = package / "evidence.json"
    raw, manifest_sha = _snapshot(manifest_path, "evidence_manifest", 4 * 1024 * 1024)
    value = _json(raw, "evidence_manifest")
    if not isinstance(value, dict) or value.get("schema") != EVIDENCE_SCHEMA:
        raise ProducerError("evidence_manifest:schema_invalid")
    if value.get("cell") != summary["cell"]:
        raise ProducerError("evidence_manifest:cell_mismatch")
    source_commit = _hex(value.get("source_commit"), "source_commit", HEX40)
    source_tree = _hex(value.get("source_tree"), "source_tree", HEX40)
    binaries = value.get("binary_sha256")
    if not isinstance(binaries, dict) or not binaries:
        raise ProducerError("binary_sha256:missing")
    binary_hashes = {name: _hex(digest, f"binary_sha256.{name}")
                     for name, digest in binaries.items()
                     if isinstance(name, str)}
    if len(binary_hashes) != len(binaries):
        raise ProducerError("binary_sha256:invalid")
    files = _descriptor_map(value.get("evidence"), "evidence")
    result_descriptor = files.get("results")
    if result_descriptor is None:
        raise ProducerError("evidence.results:missing")
    if result_descriptor["path"] != "results.jsonl" or result_descriptor["sha256"] != summary_sha:
        raise ProducerError("evidence.results:mismatch")
    if result_descriptor["bytes"] != len(summary_raw):
        raise ProducerError("evidence.results:byte_count_mismatch")
    declared = value.get("evidence_sha256")
    without_digest = dict(value)
    without_digest.pop("evidence_sha256", None)
    computed = hashlib.sha256(_canonical(without_digest)).hexdigest()
    if declared != computed:
        raise ProducerError("evidence_sha256:mismatch")
    summary_binaries = summary.get("binary_sha256")
    if not isinstance(summary_binaries, dict):
        raise ProducerError("binary_sha256:results_missing")
    normalized_summary_binaries = {
        name: _hex(digest, f"results.binary_sha256.{name}")
        for name, digest in summary_binaries.items()
        if isinstance(name, str)
    }
    if (len(normalized_summary_binaries) != len(summary_binaries) or
            normalized_summary_binaries != binary_hashes):
        raise ProducerError("binary_sha256:results_mismatch")
    timing = {name: descriptor for name, descriptor in files.items() if name != "results"}
    if not timing:
        raise ProducerError("evidence:timing_observation_missing")
    for name, descriptor in timing.items():
        path = _relative(package, descriptor["path"], f"evidence.{name}")
        observed, observed_sha = _snapshot(path, f"evidence.{name}")
        if observed_sha != descriptor["sha256"] or len(observed) != descriptor["bytes"]:
            raise ProducerError(f"evidence.{name}:digest_mismatch")
    return value, timing, {"source_commit": source_commit, "source_tree": source_tree,
                           **{f"binary_{name}_sha256": digest for name, digest in binary_hashes.items()}}, manifest_sha


def _timing_rows(package: Path, descriptors: dict[str, dict[str, object]], cell: str) -> list[dict[str, object]]:
    rows: list[dict[str, object]] = []
    for name, descriptor in descriptors.items():
        path = _relative(package, descriptor["path"], f"evidence.{name}")
        raw, _ = _snapshot(path, f"evidence.{name}")
        for line_number, line in enumerate(raw.splitlines(), 1):
            if not line.strip():
                raise ProducerError(f"timing:{name}:{line_number}:blank_line")
            value = _json(line, f"timing:{name}:{line_number}")
            if not isinstance(value, dict) or value.get("schema") != TIMING_SCHEMA:
                raise ProducerError(f"timing:{name}:{line_number}:schema_invalid")
            if value.get("cell") != cell:
                raise ProducerError(f"timing:{name}:{line_number}:cell_mismatch")
            # A warm run may retain prewarm rows; only explicit measured rows
            # are admissible.  Absence of the phase is accepted for cold.
            if value.get("phase", "measured") != "measured":
                continue
            tu_id = value.get("tu_id")
            if not isinstance(tu_id, str) or not tu_id:
                raise ProducerError(f"timing:{name}:{line_number}:tu_id_invalid")
            elapsed = value.get("elapsed_ns")
            channel = value.get("channel_bytes")
            if type(elapsed) is not int or elapsed <= 0:
                raise ProducerError(f"timing:{name}:{line_number}:elapsed_invalid")
            if type(channel) is not int or channel <= 0:
                raise ProducerError(f"timing:{name}:{line_number}:channel_bytes_invalid")
            rows.append({"tu_id": tu_id, "elapsed_ns": elapsed,
                         "channel_bytes": channel})
    # A cold S7 cell has exactly one measured TU.  Never manufacture extra
    # points by subdividing it; every emitted point below corresponds to one
    # retained measured TU.  Warm captures may include prewarm rows, which are
    # intentionally ignored unless their phase is explicitly measured.
    if not rows:
        raise ProducerError("timing:insufficient_measured_observations")
    if len({row["tu_id"] for row in rows}) != len(rows):
        raise ProducerError("timing:duplicate_tu_id")
    return rows


def _write_new(path: Path, raw: bytes) -> None:
    if path.exists() or path.is_symlink():
        raise ProducerError(f"output_already_exists:{path}")
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("xb") as stream:
        stream.write(raw)
        stream.flush()
        os.fsync(stream.fileno())


def _hold(out: Path, reason: str) -> Path:
    out.mkdir(parents=True, exist_ok=True)
    result = {"schema": SCHEMA, "status": "HOLD", "scored": False,
              "reason": reason}
    path = out / "producer-result.json"
    _write_new(path, _canonical(result) + b"\n")
    return path


def produce(package: Path, out: Path) -> Path:
    """Write a scored curve manifest, or a fail-closed HOLD result.

    ``package`` is an immutable retained S7 package.  The returned path is
    ``producer-result.json``; PASS additionally creates the referenced curve
    and ``live-curve-manifest.json`` in ``out``.
    """
    try:
        package = package.resolve()
        if not package.is_dir() or package.is_symlink():
            raise ProducerError("package:not_private_directory")
        summary, summary_raw, summary_sha = _read_summary(package)
        evidence, timing, provenance, evidence_manifest_sha = _read_evidence(
            package, summary, summary_raw, summary_sha)
        observations = _timing_rows(package, timing, summary["cell"])
    except ProducerError as exc:
        return _hold(out, str(exc))

    corpus, profile, regime = summary["cell"].split("/")
    input_digest = summary["measured"]["input_sha256"]
    # The source tree and input are the only S7 source identities available to
    # this adapter.  Topology is deliberately not invented: the evidence
    # manifest must carry it when a downstream comparison requires it.
    topology_digest = evidence.get("topology_sha256")
    try:
        topology_digest = _hex(topology_digest, "topology_sha256")
    except ProducerError:
        return _hold(out, "topology_sha256:missing_or_invalid")
    identity = {
        "corpus": corpus, "profile": profile, "regime": regime,
        "split": "calibration" if corpus in {"fmt", "RocksDB"} else "held_out_validation",
        "run_id": str(evidence.get("run_id", "s7-live")),
        "source_commit": provenance["source_commit"], "source_tree": provenance["source_tree"],
        "input_digest": input_digest, "topology_digest": topology_digest,
        "model_id": "s7-live-observed",
    }
    rows: list[dict[str, object]] = []
    elapsed_total = bytes_total = 0
    for step, observation in enumerate(observations):
        elapsed_total += int(observation["elapsed_ns"])
        bytes_total += int(observation["channel_bytes"])
        rows.append({
            "step": step, "tu_id": observation["tu_id"], "cell": {"corpus": corpus, "profile": profile, "regime": regime},
            "cumulative": {
                "channel_bytes": bytes_total, "elapsed_ns": elapsed_total,
                "throughput_bytes_per_s": (bytes_total * 1_000_000_000) / elapsed_total,
            },
        })
    curve_raw = b"".join(_canonical(row) + b"\n" for row in rows)
    curve_path = out / "live-curve.jsonl"
    manifest_path = out / "live-curve-manifest.json"
    result_path = out / "producer-result.json"
    binary_hashes = {key[7:-7]: value for key, value in provenance.items()
                     if key.startswith("binary_") and key.endswith("_sha256")}
    evidence_descriptor = {
        "results_sha256": summary_sha,
        "evidence_manifest_sha256": evidence_manifest_sha,
        "binary_sha256": binary_hashes,
        "evidence_sha256": evidence["evidence_sha256"],
    }
    manifest = {
        "schema": CURVE_MANIFEST_SCHEMA, "identity": identity,
        "units": {"point": "step", "channel_bytes": "bytes", "elapsed_ns": "ns",
                  "throughput_bytes_per_s": "bytes_per_s"},
        "curve": {"path": curve_path.name, "sha256": hashlib.sha256(curve_raw).hexdigest(), "bytes": len(curve_raw)},
        "provenance": {"mode": "live", "producer": "s8_live_metric_producer", "trace_free": False},
        "evidence": evidence_descriptor,
    }
    result = {
        "schema": SCHEMA, "status": "PASS", "scored": True,
        "cell": summary["cell"], "source_commit": identity["source_commit"],
        "source_tree": identity["source_tree"], "binary_sha256": binary_hashes,
        "input_sha256": identity["input_digest"],
        "topology_sha256": identity["topology_digest"],
        "evidence_sha256": evidence["evidence_sha256"],
        "evidence_files": {
            name: {"path": descriptor["path"], "sha256": descriptor["sha256"],
                   "bytes": descriptor["bytes"]}
            for name, descriptor in timing.items()
        },
        "curve": _descriptor(curve_path, curve_raw),
        "manifest": _descriptor(manifest_path, _canonical(manifest) + b"\n"),
        "observations": len(rows),
    }
    out.mkdir(parents=True, exist_ok=True)
    _write_new(curve_path, curve_raw)
    manifest_raw = _canonical(manifest) + b"\n"
    _write_new(manifest_path, manifest_raw)
    _write_new(result_path, _canonical(result) + b"\n")
    return result_path


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--live-package", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args(argv)
    result_path = produce(args.live_package.absolute(), args.out.absolute())
    value = json.loads(result_path.read_text())
    print(json.dumps(value, sort_keys=True, separators=(",", ":")))
    return 0 if value.get("status") == "PASS" else 77


if __name__ == "__main__":
    raise SystemExit(main())
