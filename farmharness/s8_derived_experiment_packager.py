#!/usr/bin/env python3
"""Package one authenticated predictive/live join as a matrix experiment.

This adapter only reads retained manifests and invokes the curve normalizer;
it never opens or runs a product, simulator, container, or evidence trace.
"""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
import re
from pathlib import Path
from typing import Any

try:
    from . import s8_predictive_live_normalizer as normalizer
    from .s8_schema import CORPORA, PROFILES, REGIMES, SPLITS, TOPOLOGIES, DEPTH_CLASSES
except ImportError:  # pragma: no cover
    import s8_predictive_live_normalizer as normalizer
    from s8_schema import CORPORA, PROFILES, REGIMES, SPLITS, TOPOLOGIES, DEPTH_CLASSES


SCHEMA = "icecream-s8-derived-experiment-v1"
HEX64 = re.compile(r"^[0-9a-fA-F]{64}$")
HEX40 = re.compile(r"^[0-9a-fA-F]{40}$")
PASS_ID = re.compile(r"^[A-Za-z0-9_.:-]+$")


class PackagingError(ValueError):
    """Source authority or output is invalid."""


def _derived_depth_class(source_depth: str, pass_id: str) -> str:
    """Return the report depth label while preserving source authority."""
    return "repeat-full" if pass_id == "full-2" else source_depth


def _sha(value: object, label: str) -> str:
    if not isinstance(value, str) or HEX64.fullmatch(value) is None or int(value, 16) == 0:
        raise PackagingError(f"{label}:invalid_digest")
    return value.lower()


def _sha40(value: object, label: str) -> str:
    if not isinstance(value, str) or HEX40.fullmatch(value) is None or int(value, 16) == 0:
        raise PackagingError(f"{label}:invalid_digest")
    return value.lower()


def _authority_calibration_metadata(authority: dict[str, Any]) -> dict[str, str]:
    """Use only metadata explicitly authenticated by the retained authority."""
    value = authority.get("calibration_metadata")
    if not isinstance(value, dict) or "host_digest" not in value:
        raise PackagingError(
            "source_experiment_manifest.calibration_metadata:host_digest_missing"
        )
    try:
        return normalizer._validate_calibration_metadata(
            value, "source_experiment_manifest.calibration_metadata")
    except normalizer.NormalizationError as exc:
        raise PackagingError(str(exc)) from exc


def _authority_role_placement(authority: dict[str, Any]) -> dict[str, Any]:
    """Require the source experiment to identify a disjoint timing farm."""
    try:
        placement = normalizer._validate_role_placement(
            authority.get("role_placement"), "live")
    except normalizer.NormalizationError as exc:
        raise PackagingError(
            "source_experiment_manifest.role_placement:invalid") from exc
    if (placement is None or placement.get("mode") != "external_farm" or
            placement.get("timing_eligible") is not True):
        raise PackagingError(
            "source_experiment_manifest.role_placement:not_timing_eligible")
    return placement


def _read(path: Path, label: str, limit: int = 8 * 1024 * 1024) -> tuple[bytes, dict[str, Any]]:
    try:
        raw, facts = normalizer._snapshot(path, label, limit)
    except normalizer.NormalizationError as exc:
        raise PackagingError(str(exc)) from exc
    return raw, facts


def _json(path: Path, label: str) -> tuple[dict[str, Any], dict[str, Any]]:
    raw, facts = _read(path, label)
    value = normalizer.parse_json(raw, label)
    if not isinstance(value, dict):
        raise PackagingError(f"{label}:object_required")
    return value, facts


def _descriptor(path: Path, facts: dict[str, Any]) -> dict[str, Any]:
    return {"path": str(path), "sha256": facts["sha256"], "bytes": facts["bytes"]}


def _cell(value: object) -> tuple[str, str, str]:
    if not isinstance(value, dict) or set(value) != {"corpus", "profile", "regime"}:
        raise PackagingError("source.cell:invalid")
    result = tuple(value.get(key) for key in ("corpus", "profile", "regime"))
    if result[0] not in CORPORA or result[1] not in PROFILES or result[2] not in REGIMES:
        raise PackagingError("source.cell:undeclared")
    return result  # type: ignore[return-value]


def _authority(source_dir: Path, pass_id: str) -> tuple[dict[str, Any], Path, dict[str, Any]]:
    if not PASS_ID.fullmatch(pass_id):
        raise PackagingError("pass_id:invalid")
    manifest_path = source_dir / "experiment_manifest.json"
    authority, authority_facts = _json(manifest_path, "source_experiment_manifest")
    # Real live-runner authorities predate the derived-experiment wrapper and
    # have no status field; an explicitly non-PASS authority is still rejected.
    if "status" in authority and authority.get("status") != "PASS":
        raise PackagingError("source_experiment_manifest:not_pass")
    cell = _cell(authority.get("cell"))
    split = authority.get("split")
    if split != SPLITS[cell[0]]:
        raise PackagingError("source.split:policy_mismatch")
    topology = authority.get("topology")
    suite = authority.get("suite")
    if not isinstance(topology, str) or not topology.startswith(tuple(f"{x}/" for x in TOPOLOGIES)):
        raise PackagingError("source.topology:invalid")
    if not isinstance(suite, str) or suite != topology:
        raise PackagingError("source.suite:mismatch")
    depth = authority.get("depth")
    if not isinstance(depth, str) or depth not in set(DEPTH_CLASSES) - {"legacy", "repeat-full"}:
        raise PackagingError("source.depth:invalid")
    declared_count = authority.get("declared_count")
    if type(declared_count) is not int or declared_count <= 0:
        raise PackagingError("source.declared_count:invalid")
    runs = authority.get("runs")
    if not isinstance(runs, list) or any(not isinstance(run, str) for run in runs) or pass_id not in runs:
        raise PackagingError("source.runs:pass_mismatch")
    curves = authority.get("curve_manifests")
    if not isinstance(curves, dict) or pass_id not in curves or not isinstance(curves[pass_id], str):
        raise PackagingError("source.curve_manifests:pass_missing")
    selected = (source_dir / curves[pass_id]).resolve()
    if selected.parent != source_dir.resolve():
        raise PackagingError("source.curve_manifests:path_not_private")
    plans = authority.get("predictive_plan_sha256_by_run")
    if not isinstance(plans, dict) or pass_id not in plans:
        raise PackagingError("source.predictive_plan_sha256_by_run:pass_missing")
    _sha(plans[pass_id], "source.predictive_plan_sha256_by_run")
    metadata = _authority_calibration_metadata(authority)
    role_placement = _authority_role_placement(authority)
    if authority.get("execution_scope") != "external_farm_timing":
        raise PackagingError(
            "source_experiment_manifest.execution_scope:not_timing_eligible")
    return authority, selected, {"path": manifest_path, "facts": authority_facts, "cell": cell,
                                 "split": split, "topology": topology, "suite": suite,
                                 "depth": depth, "declared_count": declared_count, "runs": runs,
                                 "plan_sha256": plans[pass_id].lower(),
                                 "calibration_metadata": metadata,
                                 "execution_scope": authority.get("execution_scope"),
                                 "role_placement": role_placement}


def _bind_live(authority: dict[str, Any], live: dict[str, Any], cell: tuple[str, str, str],
               pass_id: str) -> None:
    identity = live.get("identity")
    if not isinstance(identity, dict):
        raise PackagingError("live.identity:missing")
    if any(identity.get(key) != expected for key, expected in zip(
            ("corpus", "profile", "regime", "split"), (*cell, SPLITS[cell[0]]))):
        raise PackagingError("live.identity:source_cell_mismatch")
    for source_key, identity_key in (("source_commit", "source_commit"), ("source_tree", "source_tree"),
                                     ("input_digest", "input_digest"), ("topology_sha256", "topology_digest")):
        validator = _sha40 if source_key in {"source_commit", "source_tree"} else _sha
        if source_key in authority and validator(authority[source_key], f"source.{source_key}") != validator(identity.get(identity_key), f"live.{identity_key}"):
            raise PackagingError(f"live.identity:{identity_key}_mismatch")
    comparison = live.get("comparison")
    if not isinstance(comparison, dict):
        raise PackagingError("live.comparison:missing")
    if (live.get("execution_scope") != authority.get("execution_scope") or
            live.get("role_placement") != authority.get("role_placement")):
        raise PackagingError("live.role_placement:authority_mismatch")


def _derived_manifest(source: dict[str, Any], pass_id: str,
                      predictive_manifest: Path, predictive_facts: dict[str, Any],
                      live_manifest: Path, live_facts: dict[str, Any],
                      records_facts: dict[str, Any]) -> dict[str, Any]:
    """Build the derived manifest from authenticated source descriptors."""
    return {
        "schema": SCHEMA, "status": "PASS",
        "cell": dict(zip(("corpus", "profile", "regime"), source["cell"])),
        "split": source["split"], "topology": source["topology"],
        "suite": source["suite"], "depth": source["depth"],
        "depth_class": _derived_depth_class(source["depth"], pass_id),
        "pass_id": pass_id, "runs": source["runs"],
        "requested_curve_points": source["declared_count"],
        "records": {"path": "records.jsonl", "sha256": records_facts["sha256"],
                    "bytes": records_facts["bytes"]},
        "predictive_curve_manifest": _descriptor(predictive_manifest, predictive_facts),
        "live_curve_manifest": _descriptor(live_manifest, live_facts),
        "source_experiment_manifest": _descriptor(source["path"], source["facts"]),
        "predictive_plan_sha256": source["plan_sha256"], "comparison_scored": True,
        "calibration_metadata": source["calibration_metadata"],
        "execution_scope": source["execution_scope"],
        "role_placement": source["role_placement"],
    }


def package(predictive_manifest: Path, source_dir: Path, pass_id: str,
            output_root: Path) -> Path:
    predictive_manifest = predictive_manifest.resolve()
    source_dir = source_dir.resolve()
    authority, live_manifest, source = _authority(source_dir, pass_id)
    if not predictive_manifest.is_file() or predictive_manifest.is_symlink():
        raise PackagingError("predictive_manifest:unavailable")
    predictive, p_facts = _json(predictive_manifest, "predictive_manifest")
    try:
        p_artifact = normalizer._load_manifest(predictive_manifest, "predictive_sim")
        l_artifact = normalizer._load_manifest(live_manifest, "live")
    except normalizer.NormalizationError as exc:
        raise PackagingError(f"curve_manifest_invalid:{exc}") from exc
    if p_artifact["comparison"] is None or p_artifact["comparison"]["plan_sha256"] != source["plan_sha256"]:
        raise PackagingError("predictive_plan_sha256_mismatch")
    if l_artifact["comparison"] != p_artifact["comparison"]:
        raise PackagingError("live.comparison:plan_binding_mismatch")
    live_value, live_facts = _json(live_manifest, "live_manifest")
    _bind_live(authority, live_value, source["cell"], pass_id)
    output_root = output_root.resolve()
    output_root.mkdir(parents=True, exist_ok=True)
    for suffix in range(1000):
        name = f"derived-{pass_id}-{source['depth']}-{datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ')}" + (f"-{suffix:03d}" if suffix else "")
        out = output_root / name
        try:
            out.mkdir()
            break
        except FileExistsError:
            continue
    else:
        raise PackagingError("output:timestamp_collision")
    records_path = out / "records.jsonl"
    try:
        records = normalizer.normalize(
            predictive_manifest, live_manifest, records_path,
            authenticated_metadata=source["calibration_metadata"])
    except normalizer.NormalizationError as exc:
        raise PackagingError(f"normalization:{exc}") from exc
    records_raw, records_facts = _read(records_path, "records")
    del records_raw
    manifest = _derived_manifest(source, pass_id, predictive_manifest, p_facts,
                                 live_manifest, live_facts, records_facts)
    raw = normalizer.canonical_bytes(manifest) + b"\n"
    manifest_path = out / "experiment_manifest.json"
    try:
        with manifest_path.open("xb") as stream:
            stream.write(raw); stream.flush(); os.fsync(stream.fileno())
    except OSError as exc:
        raise PackagingError(f"output:manifest_write_failed:{exc}") from exc
    return out


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--predictive-manifest", type=Path, required=True)
    parser.add_argument("--source-live-experiment", type=Path, required=True)
    parser.add_argument("--pass-id", required=True)
    parser.add_argument("--output-root", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        package(args.predictive_manifest, args.source_live_experiment, args.pass_id, args.output_root)
    except PackagingError as exc:
        print(f"s8_derived_experiment_packager: {exc}", file=__import__('sys').stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
