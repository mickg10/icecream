#!/usr/bin/env python3
"""Build the complete calibration loss-input grid from derived experiments.

This is a read-only scanner: it authenticates existing derived manifests and
records descriptors, then writes one new loss-input manifest.  Missing grid
contexts are deliberately retained as MISSING so downstream reports cannot
mistake partial calibration coverage for a complete matrix.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import stat
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

try:
    from .s8_schema import CALIBRATION_CORPORA, CURRENT_SEMANTICS, PROFILES, REGIMES
except ImportError:  # pragma: no cover
    from s8_schema import CALIBRATION_CORPORA, CURRENT_SEMANTICS, PROFILES, REGIMES


SCHEMA = "icecream-s8-loss-input-v1"
DERIVED_SCHEMA = "icecream-s8-derived-experiment-v1"
DEPTHS = ("100", "200", "full", "repeat-full")
TOPOLOGIES = ("C1F1", "C1F20")
PASS_FOR_DEPTH = {"100": "full-1", "200": "full-1", "full": "full-1", "repeat-full": "full-2"}
SUITES = {"C1F1": "C1F1/100000", "C1F20": "C1F20/40"}
HEX64 = re.compile(r"^[0-9a-fA-F]{64}$")
QUARANTINE_PARTS = frozenset({"excluded", "quarantined", "quarantine", "failed", "failed-attempts", "partial", "dry-run", "dry_run"})


class LossInputError(ValueError):
    """Raised when a derived experiment is unauthenticated or ambiguous."""


def _canonical(value: object) -> bytes:
    return (json.dumps(value, sort_keys=True, separators=(",", ":"),
                       ensure_ascii=True, allow_nan=False) + "\n").encode("ascii")


def _parse(raw: bytes, label: str) -> object:
    def pairs(items: list[tuple[str, object]]) -> dict[str, object]:
        result: dict[str, object] = {}
        for key, value in items:
            if key in result:
                raise LossInputError(f"{label}:duplicate_json_key:{key}")
            result[key] = value
        return result
    try:
        return json.loads(raw.decode("utf-8"), object_pairs_hook=pairs,
                          parse_constant=lambda value: (_ for _ in ()).throw(
                              LossInputError(f"{label}:non_finite")))
    except LossInputError:
        raise
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise LossInputError(f"{label}:invalid_json") from exc


def _unsafe(path: Path) -> bool:
    return any(part.lower() in QUARANTINE_PARTS for part in path.parts)


def _facts(path: Path, label: str) -> dict[str, Any]:
    try:
        info = path.lstat()
    except OSError as exc:
        raise LossInputError(f"{label}:unavailable:{path}") from exc
    if _unsafe(path) or stat.S_ISLNK(info.st_mode) or not stat.S_ISREG(info.st_mode) or info.st_nlink != 1:
        raise LossInputError(f"{label}:excluded_or_not_private:{path}")
    flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
    try:
        fd = os.open(path, flags)
    except OSError as exc:
        raise LossInputError(f"{label}:cannot_open:{path}") from exc
    try:
        before = os.fstat(fd)
        digest = hashlib.sha256()
        size = 0
        while block := os.read(fd, 1 << 20):
            size += len(block)
            digest.update(block)
        after = os.fstat(fd)
        if any(getattr(before, name) != getattr(after, name)
               for name in ("st_dev", "st_ino", "st_size", "st_mtime_ns", "st_ctime_ns")):
            raise LossInputError(f"{label}:changed_while_reading:{path}")
        return {"path": str(path), "sha256": digest.hexdigest(), "bytes": size}
    except OSError as exc:
        raise LossInputError(f"{label}:read_failed:{path}") from exc
    finally:
        os.close(fd)


def _descriptor(value: object, base: Path, label: str, *, relative: bool = False) -> tuple[Path, dict[str, Any]]:
    if not isinstance(value, dict) or set(value) != {"path", "sha256", "bytes"}:
        raise LossInputError(f"{label}:descriptor_invalid")
    raw_path = value["path"]
    if not isinstance(raw_path, str) or not raw_path:
        raise LossInputError(f"{label}:path_invalid")
    candidate = Path(raw_path)
    if relative and (candidate.is_absolute() or any(part in ("", ".", "..") for part in candidate.parts)):
        raise LossInputError(f"{label}:path_not_relative")
    if not relative and not candidate.is_absolute():
        raise LossInputError(f"{label}:path_not_absolute")
    path = (base / candidate if relative else candidate).resolve()
    if _unsafe(path):
        raise LossInputError(f"{label}:excluded_or_quarantined_path")
    digest = value["sha256"]
    if not isinstance(digest, str) or HEX64.fullmatch(digest) is None or int(digest, 16) == 0:
        raise LossInputError(f"{label}:sha256_invalid")
    if type(value["bytes"]) is not int or value["bytes"] <= 0:
        raise LossInputError(f"{label}:bytes_invalid")
    facts = _facts(path, label)
    if facts["sha256"] != digest.lower() or facts["bytes"] != value["bytes"]:
        raise LossInputError(f"{label}:digest_mismatch")
    return path, {"path": str(path), "sha256": facts["sha256"], "bytes": facts["bytes"]}


def _context(manifest: dict[str, Any], path: Path) -> tuple[tuple[str, str, str, str, str, str], dict[str, Any]]:
    if manifest.get("schema") != DERIVED_SCHEMA:
        raise LossInputError(f"derived:{path}:schema_invalid")
    if manifest.get("status") != "PASS":
        raise LossInputError(f"derived:{path}:non_pass_manifest")
    cell = manifest.get("cell")
    if not isinstance(cell, dict) or set(cell) != {"corpus", "profile", "regime"}:
        raise LossInputError(f"derived:{path}:cell_invalid")
    corpus, profile, regime = (cell.get("corpus"), cell.get("profile"), cell.get("regime"))
    if corpus not in CALIBRATION_CORPORA or profile not in PROFILES or regime not in REGIMES:
        raise LossInputError(f"derived:{path}:calibration_cell_invalid")
    split = manifest.get("split")
    if split != "calibration":
        raise LossInputError(f"derived:{path}:split_invalid")
    suite = manifest.get("suite")
    topology_value = manifest.get("topology")
    topology = next((key for key, expected in SUITES.items() if expected == topology_value), None)
    if topology is None or suite != topology_value:
        raise LossInputError(f"derived:{path}:topology_invalid")
    depth = manifest.get("depth_class")
    if depth not in DEPTHS:
        raise LossInputError(f"derived:{path}:depth_class_invalid")
    pass_id = manifest.get("pass_id")
    if pass_id != PASS_FOR_DEPTH[depth]:
        raise LossInputError(f"derived:{path}:pass_depth_mismatch")
    if depth == "repeat-full" and manifest.get("depth") != "full":
        raise LossInputError(f"derived:{path}:repeat_source_depth_invalid")
    runs = manifest.get("runs")
    if not isinstance(runs, list) or pass_id not in runs or (pass_id == "full-2" and "full-1" not in runs):
        raise LossInputError(f"derived:{path}:runs_invalid")
    if manifest.get("comparison_scored") is not True:
        raise LossInputError(f"derived:{path}:comparison_not_scored")
    records_path, records = _descriptor(manifest.get("records"), path.parent, "records", relative=True)
    if records_path.parent != path.parent or records_path.name != "records.jsonl":
        raise LossInputError(f"derived:{path}:records_not_local")
    for field in ("predictive_curve_manifest", "live_curve_manifest", "source_experiment_manifest"):
        _descriptor(manifest.get(field), path.parent, field)
    plan = manifest.get("predictive_plan_sha256")
    if not isinstance(plan, str) or HEX64.fullmatch(plan) is None or int(plan, 16) == 0:
        raise LossInputError(f"derived:{path}:plan_invalid")
    key = (corpus, profile, regime, topology, depth, pass_id)
    return key, {"records": {"path": str(records_path), "sha256": records["sha256"], "bytes": records["bytes"]}}


def _scan(root: Path) -> dict[tuple[str, str, str, str, str, str], dict[str, Any]]:
    root = root.resolve()
    if not root.is_dir() or root.is_symlink() or _unsafe(root):
        raise LossInputError("derived_root:invalid")
    found: dict[tuple[str, str, str, str, str, str], dict[str, Any]] = {}
    for path in sorted(root.rglob("experiment_manifest.json")):
        if _unsafe(path):
            raise LossInputError(f"derived_root:excluded_or_quarantined_path:{path}")
        raw_facts = _facts(path, "derived_manifest")
        with path.open("rb") as stream:
            manifest = _parse(stream.read(), f"derived_manifest:{path}")
        if not isinstance(manifest, dict) or manifest.get("schema") != DERIVED_SCHEMA:
            continue
        key, value = _context(manifest, path)
        # Authenticate the resolved file above, but publish the record path
        # relative to the canonical input root, as required by the loss
        # reporter's descriptor contract.
        records_path = Path(value["records"]["path"]).resolve()
        try:
            value["records"]["path"] = str(records_path.relative_to(root))
        except ValueError as exc:
            raise LossInputError(
                f"{path}:records.path_not_under_derived_root"
            ) from exc
        if key in found:
            raise LossInputError(f"duplicate_context:{key}")
        value.update({"experiment_manifest": {"path": str(path), "sha256": raw_facts["sha256"], "bytes": raw_facts["bytes"]}})
        found[key] = value
    return found


def build(derived_root: Path, output: Path, mode: str = "calibration") -> Path:
    if mode != "calibration":
        raise LossInputError("mode:unsupported")
    output = output.absolute()
    if output.exists() or output.is_symlink():
        raise LossInputError(f"output:already_exists:{output}")
    root = derived_root.resolve()
    found = _scan(root)
    entries: list[dict[str, Any]] = []
    for corpus in ("fmt", "RocksDB"):
        for profile in PROFILES:
            for regime in REGIMES:
                for topology in TOPOLOGIES:
                    for depth in DEPTHS:
                        pass_id = PASS_FOR_DEPTH[depth]
                        key = (corpus, profile, regime, topology, depth, pass_id)
                        if key in found:
                            entries.append({"cell": {"corpus": corpus, "profile": profile, "regime": regime},
                                            "split": "calibration", "topology": topology,
                                            "depth_class": depth, "pass_id": pass_id, "status": "PASS",
                                            "records": found[key]["records"]})
                        else:
                            entries.append({"cell": {"corpus": corpus, "profile": profile, "regime": regime},
                                            "split": "calibration", "topology": topology,
                                            "depth_class": depth, "pass_id": pass_id, "status": "MISSING",
                                            "records": None, "reason": "no authenticated PASS derived experiment for context"})
    output.parent.mkdir(parents=True, exist_ok=True)
    value = {"schema": SCHEMA, "semantics": CURRENT_SEMANTICS, "entries": entries}
    raw = _canonical(value)
    try:
        with output.open("xb") as stream:
            stream.write(raw)
            stream.flush()
            os.fsync(stream.fileno())
    except OSError as exc:
        raise LossInputError(f"output:write_failed:{exc}") from exc
    return output


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--derived-root", type=Path, required=True)
    parser.add_argument("--mode", choices=("calibration",), default="calibration")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        print(build(args.derived_root, args.output, args.mode))
    except LossInputError as exc:
        print(f"s8_loss_input_builder: {exc}", file=__import__("sys").stderr)
        return 77
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
