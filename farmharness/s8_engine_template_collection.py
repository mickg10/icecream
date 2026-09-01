#!/usr/bin/env python3
"""Collect retained S8 engine templates into one immutable tree.

The retained S7 packages already contain authenticated predictive manifests,
inputs, and topology declarations for the declared calibration or explicitly
selected held-out corpus/profile/regime buckets.  This tool validates those
packages against the current predictive engine schema and copies only the
three engine inputs into a uniform path that the S8 campaign driver can address
with one format string.  It never runs the simulator, product, Docker, or a
benchmark.  Calibration remains the default scope; held-out collection must
be explicitly selected.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import stat
import sys
from pathlib import Path
from typing import Any, Iterable, Mapping, Sequence

try:  # package invocation
    from . import s8_predictive_engine as engine
    from .s8_schema import (CALIBRATION_CORPORA, CORPORA, HELD_OUT_CORPORA,
                            PROFILES, REGIMES)
except ImportError:  # direct invocation
    import s8_predictive_engine as engine
    from s8_schema import (CALIBRATION_CORPORA, CORPORA, HELD_OUT_CORPORA,
                           PROFILES, REGIMES)


SCHEMA = "icecream-s8-engine-template-collection-v1"
PACKAGE_SCHEMA = "icecream-s8-retained-s7-package-v1"
TEMPLATE = "engines/{corpus}/{profile}-{regime}/engine-manifest.json"
HEX64 = re.compile(r"^[0-9a-f]{64}$")
CALIBRATION_ORDER = tuple(corpus for corpus in CORPORA if corpus in CALIBRATION_CORPORA)
HELD_OUT_ORDER = tuple(corpus for corpus in CORPORA if corpus in HELD_OUT_CORPORA)
EXPECTED_CELLS = tuple(
    (corpus, profile, regime)
    for corpus in CALIBRATION_ORDER
    for profile in PROFILES
    for regime in REGIMES
)
HELD_OUT_CELLS = tuple(
    (corpus, profile, regime)
    for corpus in HELD_OUT_ORDER
    for profile in PROFILES
    for regime in REGIMES
)


class CollectionError(ValueError):
    """A retained package or immutable collection is invalid."""


def canonical(value: object) -> bytes:
    return (json.dumps(value, sort_keys=True, separators=(",", ":"),
                       ensure_ascii=True, allow_nan=False) + "\n").encode("ascii")


def _reject_symlink_ancestors(path: Path, label: str) -> None:
    """Reject aliases in the lexical absolute path, not only its final node."""
    path = Path(path)
    if not path.is_absolute():
        raise CollectionError(f"{label}:path_not_absolute")
    current = Path(path.anchor)
    for component in path.parts[1:]:
        current /= component
        if current.is_symlink():
            raise CollectionError(f"{label}:path_alias:{current}")


def _snapshot(path: Path, label: str) -> tuple[bytes, dict[str, object]]:
    _reject_symlink_ancestors(path, label)
    try:
        info = path.lstat()
    except OSError as exc:
        raise CollectionError(f"{label}:unavailable:{path}") from exc
    if stat.S_ISLNK(info.st_mode) or not stat.S_ISREG(info.st_mode) or info.st_nlink != 1:
        raise CollectionError(f"{label}:not_private_regular_file:{path}")
    try:
        raw = path.read_bytes()
    except OSError as exc:
        raise CollectionError(f"{label}:unreadable:{path}") from exc
    return raw, {"path": str(path), "bytes": len(raw),
                 "sha256": hashlib.sha256(raw).hexdigest()}


def _parse(raw: bytes, label: str) -> dict[str, Any]:
    try:
        value = json.loads(raw.decode("utf-8"), object_pairs_hook=_unique_pairs,
                           parse_constant=lambda token: (_ for _ in ()).throw(
                               CollectionError(f"{label}:non_finite:{token}")))
    except CollectionError:
        raise
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise CollectionError(f"{label}:invalid_json") from exc
    if not isinstance(value, dict):
        raise CollectionError(f"{label}:object_required")
    return value


def _unique_pairs(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise CollectionError(f"json:duplicate_key:{key}")
        result[key] = value
    return result


def _relative_artifact(root: Path, descriptor: object, label: str
                       ) -> tuple[Path, bytes, dict[str, object]]:
    if not isinstance(descriptor, Mapping) or set(descriptor) != {"path", "bytes", "sha256"}:
        raise CollectionError(f"{label}:descriptor_invalid")
    relative = descriptor.get("path")
    size = descriptor.get("bytes")
    digest = descriptor.get("sha256")
    if (not isinstance(relative, str) or not relative or Path(relative).is_absolute() or
            any(part in ("", ".", "..") for part in Path(relative).parts)):
        raise CollectionError(f"{label}:path_invalid")
    if type(size) is not int or size < 0 or not isinstance(digest, str) or not HEX64.fullmatch(digest):
        raise CollectionError(f"{label}:facts_invalid")
    _reject_symlink_ancestors(root, label)
    current = root
    for component in Path(relative).parts:
        current /= component
        if current.is_symlink():
            raise CollectionError(f"{label}:path_alias")
    try:
        current.resolve().relative_to(root.resolve())
    except (OSError, ValueError) as exc:
        raise CollectionError(f"{label}:path_outside_package") from exc
    raw, facts = _snapshot(current, label)
    if facts["bytes"] != size or facts["sha256"] != digest:
        raise CollectionError(f"{label}:descriptor_mismatch")
    return current, raw, facts


def _cell(value: object, label: str,
          expected_cells: tuple[tuple[str, str, str], ...] = EXPECTED_CELLS
          ) -> tuple[str, str, str]:
    if not isinstance(value, str):
        raise CollectionError(f"{label}:cell_invalid")
    parts = tuple(value.split("/"))
    if parts not in expected_cells:
        raise CollectionError(f"{label}:cell_not_in_scope:{value}")
    return parts  # type: ignore[return-value]


def _package(path: Path, expected_cells: tuple[tuple[str, str, str], ...] = EXPECTED_CELLS,
             split: str = "calibration") -> dict[str, object]:
    raw, package_facts = _snapshot(path, "package_manifest")
    value = _parse(raw, "package_manifest")
    if value.get("schema") != PACKAGE_SCHEMA or value.get("status") != "PASS":
        raise CollectionError("package_manifest:not_pass")
    cell = _cell(value.get("cell"), "package_manifest", expected_cells)
    files = value.get("files")
    if not isinstance(files, Mapping):
        raise CollectionError("package_manifest:files_invalid")
    manifest_path, manifest_raw, manifest_facts = _relative_artifact(
        path.parent, files.get("predictive_manifest"), "predictive_manifest")
    _input_path, input_raw, input_facts = _relative_artifact(
        path.parent, files.get("input"), "input")
    _topology_path, topology_raw, topology_facts = _relative_artifact(
        path.parent, files.get("topology"), "topology")
    try:
        loaded, *_rest, loaded_cell = engine.load_inputs(manifest_path)
    except engine.PredictionError as exc:
        raise CollectionError(f"predictive_manifest:{exc}") from exc
    observed = (loaded_cell["corpus"], loaded_cell["profile"], loaded_cell["regime"])
    if observed != cell or loaded.get("split") != split:
        raise CollectionError("predictive_manifest:cell_or_split_mismatch")
    return {
        "cell": cell,
        "package_manifest": package_facts,
        "manifest_raw": manifest_raw,
        "manifest_facts": manifest_facts,
        "input_raw": input_raw,
        "input_facts": input_facts,
        "topology_raw": topology_raw,
        "topology_facts": topology_facts,
    }


def _write_new(path: Path, raw: bytes) -> dict[str, object]:
    path.parent.mkdir(parents=True, exist_ok=True)
    try:
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o444)
        with os.fdopen(fd, "wb") as stream:
            stream.write(raw)
            stream.flush()
            os.fsync(stream.fileno())
        os.chmod(path, 0o444)
    except OSError as exc:
        raise CollectionError(f"output:write_failed:{path}") from exc
    return {"path": str(path), "bytes": len(raw),
            "sha256": hashlib.sha256(raw).hexdigest()}


def collect(package_manifests: Iterable[Path], output: Path, *,
            corpora: Iterable[str] = CALIBRATION_CORPORA) -> Path:
    """Validate exactly 16 packages and atomically publish a uniform collection."""
    output = Path(output).absolute()
    _reject_symlink_ancestors(output.parent, "output_parent")
    if output.exists() or output.is_symlink():
        raise CollectionError("output:already_exists")
    if not output.parent.is_dir() or output.parent.is_symlink():
        raise CollectionError("output:parent_invalid")
    selected_corpora = tuple(corpora)
    selected_set = frozenset(selected_corpora)
    if (len(selected_corpora) != len(selected_set) or
            selected_set not in (CALIBRATION_CORPORA, HELD_OUT_CORPORA)):
        raise CollectionError("corpora:scope_must_be_calibration_or_held_out")
    expected_cells = (EXPECTED_CELLS if selected_set == CALIBRATION_CORPORA
                      else HELD_OUT_CELLS)
    split = "calibration" if selected_set == CALIBRATION_CORPORA else "held_out_validation"
    rows: dict[tuple[str, str, str], dict[str, object]] = {}
    for path in package_manifests:
        row = _package(Path(path).absolute(), expected_cells, split)
        cell = row["cell"]
        assert isinstance(cell, tuple)
        if cell in rows:
            raise CollectionError(f"package_manifest:duplicate_cell:{'/'.join(cell)}")
        rows[cell] = row
    missing = sorted(set(expected_cells) - set(rows))
    extra = sorted(set(rows) - set(expected_cells))
    if missing or extra:
        raise CollectionError(
            "package_manifest:cell_set_mismatch:missing=" + ",".join("/".join(v) for v in missing) +
            ":extra=" + ",".join("/".join(v) for v in extra))

    temporary = output.with_name(f".{output.name}.{os.getpid()}.tmp")
    if temporary.exists() or temporary.is_symlink():
        raise CollectionError("output:temporary_exists")
    temporary.mkdir(mode=0o700)
    entries: list[dict[str, object]] = []
    try:
        for cell in expected_cells:
            corpus, profile, regime = cell
            row = rows[cell]
            directory = temporary / "engines" / corpus / f"{profile}-{regime}"
            directory.mkdir(parents=True)
            input_facts = _write_new(directory / "input.ii", row["input_raw"])  # type: ignore[arg-type]
            topology_facts = _write_new(directory / "topology.json", row["topology_raw"])  # type: ignore[arg-type]
            manifest_facts = _write_new(directory / "engine-manifest.json", row["manifest_raw"])  # type: ignore[arg-type]
            for facts in (input_facts, topology_facts, manifest_facts):
                facts["path"] = str(Path(str(facts["path"])).relative_to(temporary))
            entries.append({
                "cell": {"corpus": corpus, "profile": profile, "regime": regime},
                "source_package_manifest": row["package_manifest"],
                "engine_manifest": manifest_facts,
                "input": input_facts,
                "topology": topology_facts,
            })
        payload: dict[str, object] = {
            "schema": SCHEMA,
            "status": "PASS",
            "cell_count": len(entries),
            "scope": split,
            "engine_manifest_template": TEMPLATE,
            "cells": entries,
        }
        payload["collection_sha256"] = hashlib.sha256(canonical(payload)).hexdigest()
        _write_new(temporary / "collection-manifest.json", canonical(payload))
        for directory in sorted((path for path in temporary.rglob("*") if path.is_dir()),
                                key=lambda value: len(value.parts), reverse=True):
            directory.chmod(0o555)
        temporary.chmod(0o555)
        os.rename(temporary, output)
        parent_fd = os.open(output.parent, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
        try:
            os.fsync(parent_fd)
        finally:
            os.close(parent_fd)
    except Exception:
        shutil.rmtree(temporary, ignore_errors=True)
        raise
    return output / "collection-manifest.json"


def audit(manifest_path: Path) -> dict[str, object]:
    """Rehash every retained template and re-run current engine input validation."""
    manifest_path = Path(manifest_path).absolute()
    raw, facts = _snapshot(manifest_path, "collection_manifest")
    value = _parse(raw, "collection_manifest")
    errors: list[str] = []
    unsigned = dict(value)
    digest = unsigned.pop("collection_sha256", None)
    if (not isinstance(digest, str) or not HEX64.fullmatch(digest) or
            hashlib.sha256(canonical(unsigned)).hexdigest() != digest):
        errors.append("collection_sha256:mismatch")
    split = value.get("scope", "calibration")
    if split not in ("calibration", "held_out_validation"):
        errors.append("collection:scope_invalid")
        split = "calibration"
    expected_cells = (EXPECTED_CELLS if split == "calibration" else HELD_OUT_CELLS)
    if (value.get("schema") != SCHEMA or value.get("status") != "PASS" or
            value.get("cell_count") != len(expected_cells) or
            value.get("engine_manifest_template") != TEMPLATE):
        errors.append("collection:metadata_invalid")
    entries = value.get("cells")
    seen: set[tuple[str, str, str]] = set()
    if not isinstance(entries, list) or len(entries) != len(expected_cells):
        errors.append("collection:cells_invalid")
        entries = []
    for index, entry in enumerate(entries):
        try:
            if not isinstance(entry, Mapping):
                raise CollectionError("entry:not_object")
            c = entry.get("cell")
            if not isinstance(c, Mapping) or set(c) != {"corpus", "profile", "regime"}:
                raise CollectionError("entry:cell_invalid")
            cell = (c["corpus"], c["profile"], c["regime"])
            if cell not in expected_cells or cell in seen:
                raise CollectionError("entry:cell_duplicate_or_unknown")
            seen.add(cell)
            expected = TEMPLATE.format(corpus=cell[0], profile=cell[1], regime=cell[2])
            descriptor = entry.get("engine_manifest")
            if not isinstance(descriptor, Mapping) or descriptor.get("path") != expected:
                raise CollectionError("entry:manifest_path_invalid")
            engine_path, _engine_raw, _engine_facts = _relative_artifact(
                manifest_path.parent, descriptor, "engine_manifest")
            expected_parent = Path(expected).parent
            input_descriptor = entry.get("input")
            topology_descriptor = entry.get("topology")
            if (not isinstance(input_descriptor, Mapping) or
                    input_descriptor.get("path") != str(expected_parent / "input.ii")):
                raise CollectionError("entry:input_path_invalid")
            if (not isinstance(topology_descriptor, Mapping) or
                    topology_descriptor.get("path") != str(expected_parent / "topology.json")):
                raise CollectionError("entry:topology_path_invalid")
            _relative_artifact(manifest_path.parent, input_descriptor, "input")
            _relative_artifact(manifest_path.parent, topology_descriptor, "topology")
            source_descriptor = entry.get("source_package_manifest")
            if not isinstance(source_descriptor, Mapping):
                raise CollectionError("entry:source_package_invalid")
            source_path = source_descriptor.get("path")
            if not isinstance(source_path, str) or not Path(source_path).is_absolute():
                raise CollectionError("entry:source_package_path_invalid")
            source_raw, source_facts = _snapshot(Path(source_path), "source_package_manifest")
            del source_raw
            if (source_facts["bytes"] != source_descriptor.get("bytes") or
                    source_facts["sha256"] != source_descriptor.get("sha256")):
                raise CollectionError("entry:source_package_changed")
            loaded, *_rest, loaded_cell = engine.load_inputs(engine_path)
            if ((loaded_cell["corpus"], loaded_cell["profile"], loaded_cell["regime"]) != cell or
                    loaded.get("split") != split):
                raise CollectionError("entry:engine_cell_mismatch")
        except (CollectionError, engine.PredictionError, OSError, KeyError) as exc:
            errors.append(f"cell[{index}]:{exc}")
    if seen != set(expected_cells):
        errors.append("collection:cell_set_incomplete")
    return {"schema": SCHEMA, "status": "PASS" if not errors else "FAIL",
            "manifest": facts, "errors": errors,
            "engine_manifest_template": str(manifest_path.parent / TEMPLATE)}


def discover(package_root: Path) -> list[Path]:
    """Discover only retained calibration packages at the declared layout."""
    package_root = Path(package_root).absolute()
    _reject_symlink_ancestors(package_root, "package_root")
    if package_root.is_symlink() or not package_root.is_dir():
        raise CollectionError("package_root:invalid")
    return sorted(package_root.glob("s8-retained-*/*/package-manifest.json"))


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--package-root", type=Path)
    group.add_argument("--package-manifest", type=Path, nargs="+")
    group.add_argument("--audit", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--scope", choices=("calibration", "held_out_validation"),
                        default="calibration")
    args = parser.parse_args(argv)
    try:
        if args.audit is not None:
            result = audit(args.audit)
        else:
            if args.output is None:
                raise CollectionError("output:required")
            packages = (args.package_manifest if args.package_manifest is not None
                        else discover(args.package_root))
            corpora = (CALIBRATION_CORPORA if args.scope == "calibration"
                       else HELD_OUT_CORPORA)
            manifest = collect(packages, args.output, corpora=corpora)
            result = audit(manifest)
    except (CollectionError, OSError, UnicodeError, json.JSONDecodeError) as exc:
        result = {"schema": SCHEMA, "status": "FAIL", "errors": [str(exc)]}
    sys.stdout.buffer.write(canonical(result))
    return 0 if result.get("status") == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
