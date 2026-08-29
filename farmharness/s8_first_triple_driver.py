#!/usr/bin/env python3
"""Produce one scored S8 predictive/live/comparison record set.

The predictor consumes only its authenticated input/topology manifest.  The
live side is an independently supplied, authenticated cumulative-curve
manifest; the S7 package is used only for its authenticated PASS summary and
input identity.  The shared normalizer owns alignment, point errors, and the
loss curve.  An absent, fabricated, or otherwise invalid live metric curve is
never converted into an UNSCORED comparison.  The driver accepts every cell
from the shared 32-cell contract; the default remains the original first
triple for source compatibility.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import stat
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

try:  # Works with ``python -m farmharness...`` and PYTHONPATH=farmharness.
    from . import s8_predictive_live_normalizer as normalizer
    from .s8_schema import CORPORA, PROFILES, REGIMES, SPLITS
    from .s8_predictive_engine import PredictionError, load_inputs, predict
except ImportError:  # pragma: no cover - direct harness invocation.
    import s8_predictive_live_normalizer as normalizer
    from s8_schema import CORPORA, PROFILES, REGIMES, SPLITS
    from s8_predictive_engine import PredictionError, load_inputs, predict


CELL = {"corpus": "fmt", "profile": "ZSTD_TU", "regime": "cold"}
LIVE_SCHEMA = "icecream-s7-live-cell-v1"
DRIVER_SCHEMA = "icecream-s8-first-triple-driver-v2"
LIVE_PRODUCERS = frozenset({
    "s7_live_observation", "s7_warm_replay", "s7_zstd_tu_cells",
    "s8_live_metric_producer",
})
CELL_FIELDS = frozenset(("corpus", "profile", "regime"))
# S7's retained ``wait for cs`` measurement covers the remote compile and
# result-return window.  Source compression/uplink, startup, and commit are
# retained in the raw predictor artifact but are not comparable live metrics.
LIVE_ELAPSED_COMPONENTS = ("compile", "result_return")


class DriverError(ValueError):
    """An authenticated input, S7 package, or live curve is invalid."""


def _cell_label(cell: dict[str, str]) -> str:
    return f"{cell['corpus']}/{cell['profile']}/{cell['regime']}"


def _validate_cell(value: object, label: str = "cell") -> dict[str, str]:
    """Validate and copy exactly one cell from the immutable S8 contract."""
    if not isinstance(value, dict) or set(value) != CELL_FIELDS:
        raise DriverError(f"{label}:fields_invalid")
    if (not all(isinstance(value[field], str) for field in CELL_FIELDS) or
            value["corpus"] not in CORPORA or value["profile"] not in PROFILES or
            value["regime"] not in REGIMES):
        raise DriverError(f"{label}:not_declared")
    return {field: value[field] for field in ("corpus", "profile", "regime")}


def _require_cell(actual: object, expected: dict[str, str], label: str) -> None:
    """Reject a corpus/profile/regime identity mismatch before any output."""
    if actual != expected:
        raise DriverError(f"{label}:cell_mismatch")


def _regular_snapshot(path: Path, label: str, limit: int = 64 * 1024 * 1024) -> tuple[bytes, str]:
    try:
        info = path.lstat()
    except OSError as exc:
        raise DriverError(f"{label}:unavailable:{path}") from exc
    if stat.S_ISLNK(info.st_mode) or not stat.S_ISREG(info.st_mode) or info.st_nlink != 1:
        raise DriverError(f"{label}:not_private_regular_file:{path}")
    flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
    try:
        fd = os.open(path, flags)
    except OSError as exc:
        raise DriverError(f"{label}:cannot_open:{path}") from exc
    try:
        before = os.fstat(fd)
        chunks: list[bytes] = []
        digest = hashlib.sha256()
        total = 0
        while True:
            try:
                block = os.read(fd, 1 << 20)
            except OSError as exc:
                raise DriverError(f"{label}:read_failed:{path}") from exc
            if not block:
                break
            total += len(block)
            if total > limit:
                raise DriverError(f"{label}:too_large")
            chunks.append(block)
            digest.update(block)
        after = os.fstat(fd)
        if any(getattr(before, field) != getattr(after, field)
               for field in ("st_dev", "st_ino", "st_size", "st_mtime_ns", "st_ctime_ns")):
            raise DriverError(f"{label}:changed_while_reading:{path}")
        return b"".join(chunks), digest.hexdigest()
    finally:
        os.close(fd)


def _write_new(path: Path, raw: bytes, label: str) -> None:
    if path.exists() or path.is_symlink():
        raise DriverError(f"{label}:output_already_exists:{path}")
    try:
        with path.open("xb") as stream:
            stream.write(raw)
            stream.flush()
            os.fsync(stream.fileno())
    except OSError as exc:
        raise DriverError(f"{label}:output_write_failed:{path}") from exc


def _live_summary(package: Path, cell: dict[str, str] = CELL) -> tuple[dict[str, Any], bytes, str, Path]:
    try:
        info = package.lstat()
    except OSError as exc:
        raise DriverError(f"live_package:unavailable:{package}") from exc
    if not stat.S_ISDIR(info.st_mode) or stat.S_ISLNK(info.st_mode):
        raise DriverError("live_package:not_private_directory")
    results_path = package / "results.jsonl"
    raw, digest = _regular_snapshot(results_path, "live_results", 16 * 1024 * 1024)
    lines = raw.splitlines()
    if len(lines) != 1:
        raise DriverError("live_results:expected_one_summary_record")
    try:
        value = normalizer.parse_json(lines[0], "live_results")
    except normalizer.NormalizationError as exc:
        raise DriverError(str(exc)) from exc
    if not isinstance(value, dict) or value.get("schema") != LIVE_SCHEMA:
        raise DriverError("live_results:schema_invalid")
    if value.get("cell") != _cell_label(cell):
        raise DriverError("live_results:cell_mismatch")
    for field in CELL_FIELDS:
        if field in value and value[field] != cell[field]:
            raise DriverError(f"live_results:{field}_mismatch")
    if "split" in value and value["split"] != SPLITS[cell["corpus"]]:
        raise DriverError("live_results:split_mismatch")
    for field in ("status", "live_status", "acceptance_status", "conformance_status"):
        if value.get(field) != "PASS":
            raise DriverError(f"live_results:{field}_not_pass")
    measured = value.get("measured")
    if not isinstance(measured, dict):
        raise DriverError("live_results:measured_record_missing")
    input_sha = measured.get("input_sha256")
    if (not isinstance(input_sha, str) or len(input_sha) != 64 or
            any(char not in "0123456789abcdef" for char in input_sha)):
        raise DriverError("live_results:measured_input_sha256_invalid")
    return value, raw, digest, results_path


def _load_live_curve(path: Path, cell: dict[str, str] = CELL) -> dict[str, object]:
    if path is None:
        raise DriverError("missing_live_metric_curve")
    try:
        artifact = normalizer._load_manifest(path.absolute(), "live")
    except normalizer.NormalizationError as exc:
        raise DriverError(f"live_metric_curve_invalid:{exc}") from exc
    provenance = artifact["provenance"]
    assert isinstance(provenance, dict)
    if provenance["producer"] not in LIVE_PRODUCERS:
        raise DriverError("live_metric_curve_producer_not_authenticated")
    rows = artifact["rows"]
    if isinstance(rows, list) and any(
            isinstance(row, dict) and row.get("status") in {"UNSCORED", "HOLD"}
            for row in rows):
        raise DriverError("live_metric_curve_unscored")
    identity = artifact["identity"]
    assert isinstance(identity, dict)
    _require_cell({field: identity[field] for field in CELL_FIELDS}, cell,
                  "live_metric_curve")
    return artifact


def _normalizer_curve(predictive_raw: bytes, path: Path) -> tuple[bytes, str]:
    """Project raw predictor points to the independently observed metric set.

    The predictor may retain directional channel counters and elapsed
    components for analysis.  The retained S7 live curve exposes only total
    channel bytes, elapsed nanoseconds, and the derived throughput.  This
    authenticated view deliberately compares exactly that common set and
    never invents points or uses a digest as a metric.
    """
    def nonnegative_int(value: object, label: str) -> int:
        if type(value) is not int or value < 0:
            raise DriverError(f"predictive_curve:{label}:nonnegative_integer_required")
        return value

    rows: list[dict[str, object]] = []
    for number, line in enumerate(predictive_raw.splitlines(), 1):
        try:
            value = normalizer.parse_json(line, f"predictive_curve:{number}")
        except normalizer.NormalizationError as exc:
            raise DriverError(str(exc)) from exc
        if not isinstance(value, dict):
            raise DriverError("predictive_curve:not_object")
        if not isinstance(value.get("step"), int) or not isinstance(value.get("tu_id"), str):
            raise DriverError("predictive_curve:point_identity_invalid")
        cumulative = value.get("cumulative")
        if not isinstance(cumulative, dict):
            raise DriverError("predictive_curve:missing_cumulative_metrics")
        channel_bytes = nonnegative_int(cumulative.get("channel_bytes"),
                                        "channel_bytes")
        elapsed_components = value.get("elapsed_ns")
        if not isinstance(elapsed_components, dict):
            raise DriverError("predictive_curve:elapsed_components:missing")
        elapsed_ns = sum(nonnegative_int(elapsed_components.get(component),
                                         f"elapsed_ns.{component}")
                         for component in LIVE_ELAPSED_COMPONENTS)
        if elapsed_ns == 0:
            raise DriverError("predictive_curve:elapsed_ns:must_be_positive")
        throughput = (channel_bytes * 1_000_000_000) / elapsed_ns
        if not math.isfinite(throughput) or throughput < 0:
            raise DriverError("predictive_curve:throughput_bytes_per_s:invalid")
        rows.append({"step": value["step"], "tu_id": value["tu_id"],
                     "cumulative": {
                         "channel_bytes": channel_bytes,
                         "elapsed_ns": elapsed_ns,
                         "throughput_bytes_per_s": throughput,
                     }})
    if not rows:
        raise DriverError("predictive_curve:empty")
    raw = b"".join(normalizer.canonical_bytes(row) + b"\n" for row in rows)
    _write_new(path, raw, "predictive_normalizer_curve")
    return raw, hashlib.sha256(raw).hexdigest()


def _curve_manifest(path: Path, curve: Path, curve_raw: bytes, identity: dict[str, str]) -> bytes:
    value = {
        "schema": normalizer.MANIFEST_SCHEMA,
        "identity": identity,
        "units": {"point": "step", "channel_bytes": "bytes", "elapsed_ns": "ns",
                  "throughput_bytes_per_s": "bytes_per_s"},
        "curve": {"path": curve.name, "sha256": hashlib.sha256(curve_raw).hexdigest(),
                  "bytes": len(curve_raw)},
        "provenance": {"mode": "predictive_sim", "producer": "s8_predictive_engine",
                       "trace_free": True},
    }
    return normalizer.canonical_bytes(value) + b"\n"


def _timestamped_directory(root: Path, cell: dict[str, str] = CELL) -> Path:
    try:
        info = root.lstat()
        if stat.S_ISLNK(info.st_mode) or not stat.S_ISDIR(info.st_mode):
            raise DriverError(f"experiments:not_private_directory:{root}")
    except FileNotFoundError:
        root.mkdir(parents=True)
    except OSError as exc:
        raise DriverError(f"experiments:unavailable:{root}") from exc
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    path = root / f"s8-{cell['corpus']}-{cell['profile']}-{cell['regime']}-{stamp}-{os.getpid()}"
    try:
        path.mkdir()
    except FileExistsError as exc:
        raise DriverError(f"experiments:timestamp_collision:{path}") from exc
    return path


def run(manifest: Path, live_package: Path, live_manifest: Path | None,
        experiments: Path, cell: dict[str, str] | None = None) -> Path:
    """Run predictor and shared normalizer into one immutable experiment dir."""
    target_cell = _validate_cell(CELL if cell is None else cell, "requested_cell")
    live, live_raw, live_sha, live_results_path = _live_summary(live_package, target_cell)
    live_artifact = _load_live_curve(live_manifest, target_cell)
    live_identity = live_artifact["identity"]
    assert isinstance(live_identity, dict)
    if live_identity["input_digest"] != live["measured"]["input_sha256"]:
        raise DriverError("live_metric_curve_and_s7_input_digest_mismatch")
    try:
        loaded = load_inputs(manifest)
    except PredictionError as exc:
        raise DriverError(f"predictive_manifest:{exc}") from exc
    input_facts, topology_facts = loaded[2], loaded[3]
    manifest_cell = loaded[6]
    _require_cell(manifest_cell, target_cell, "predictive_manifest")
    if input_facts["sha256"] != live_identity["input_digest"]:
        raise DriverError("predictive_input_and_live_input_digest_mismatch")

    experiment = _timestamped_directory(experiments, target_cell)
    try:
        predictive_path = experiment / "predictive_sim.jsonl"
        predictive_sidecar = predict(manifest, predictive_path)
        predictive_raw = predictive_path.read_bytes()
        normalizer_curve_path = experiment / "predictive_curve.jsonl"
        normalizer_curve_raw, _normalizer_curve_sha = _normalizer_curve(
            predictive_raw, normalizer_curve_path)
        identity = {key: str(value) for key, value in live_identity.items()}
        identity["topology_digest"] = topology_facts["sha256"]
        predictive_manifest_path = experiment / "predictive_curve_manifest.json"
        _write_new(predictive_manifest_path,
                   _curve_manifest(predictive_manifest_path, normalizer_curve_path,
                                   normalizer_curve_raw, identity),
                   "predictive_curve_manifest")
        normalized_path = experiment / "records.jsonl"
        try:
            records = normalizer.normalize(predictive_manifest_path, live_manifest.absolute(), normalized_path)
        except normalizer.NormalizationError as exc:
            raise DriverError(f"comparison:{exc}") from exc
        if len(records) != 3 or records[2].get("record_type") != "comparison":
            raise DriverError("comparison:record_shape_invalid")
        if "loss_curve" not in records[2] or "point_errors" not in records[2]:
            raise DriverError("comparison:scored_metrics_missing")
        _write_new(experiment / "live_summary.jsonl", live_raw, "live_summary")
        driver_manifest = {
            "schema": DRIVER_SCHEMA, "cell": target_cell,
            "split": SPLITS[target_cell["corpus"]], "status": "PASS",
            "records": {"path": normalized_path.name,
                        "sha256": hashlib.sha256(normalized_path.read_bytes()).hexdigest(),
                        "bytes": normalized_path.stat().st_size},
            "predictive_sim": {"path": predictive_path.name,
                                "sha256": hashlib.sha256(predictive_raw).hexdigest(),
                                "bytes": len(predictive_raw),
                                "producer_manifest": predictive_sidecar},
            "live_summary": {"path": "live_summary.jsonl", "sha256": live_sha,
                             "bytes": len(live_raw), "source": str(live_results_path.absolute())},
            "live_curve_manifest": {"path": str(live_manifest.absolute()),
                                     "manifest_sha256": live_artifact["manifest_sha256"],
                                     "curve_sha256": live_artifact["curve_sha256"]},
            "comparison_scored": True,
            "trace_free_predictive": True,
        }
        _write_new(experiment / "experiment_manifest.json",
                   normalizer.canonical_bytes(driver_manifest) + b"\n",
                   "experiment_manifest")
    except (OSError, KeyError, TypeError, ValueError) as exc:
        raise DriverError(f"experiment:write_failed:{exc}") from exc
    return experiment


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--live-package", type=Path, required=True)
    parser.add_argument("--live-manifest", type=Path,
                        help="separately authenticated live cumulative-curve manifest")
    parser.add_argument("--experiments", type=Path, default=Path("experiments"))
    parser.add_argument("--corpus", choices=CORPORA, default=CELL["corpus"])
    parser.add_argument("--profile", choices=PROFILES, default=CELL["profile"])
    parser.add_argument("--regime", choices=REGIMES, default=CELL["regime"])
    args = parser.parse_args(argv)
    try:
        print(run(args.manifest.absolute(), args.live_package.absolute(),
                  args.live_manifest.absolute() if args.live_manifest else None,
                  args.experiments.absolute(),
                  {"corpus": args.corpus, "profile": args.profile, "regime": args.regime}))
    except (DriverError, PredictionError) as exc:
        print(str(exc), file=sys.stderr)
        return 77
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
