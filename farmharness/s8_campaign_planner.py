#!/usr/bin/env python3
"""Build a declarative, authenticated S8 expanded campaign index.

This module plans cells only.  It never runs a compiler, simulator, Docker,
farm driver, or producer.  Every executable cell is a descriptor whose
producer capability can be bound by a later native/live runner.
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


SCHEMA = "icecream-s8-expanded-campaign-v1"
DESCRIPTOR_SCHEMA = "icecream-s8-campaign-descriptor-v1"
RECOVERY_SCHEMA = "icecream-s8-image-authority-recovery-v1"
MATRIX_AUDIT_SCHEMA = "icecream-s8-matrix-audit-v1"
CAPABILITY_SCHEMA = "icecream-s8-native-live-runner-capability-v1"
CAPABILITY = "icecream.s8.native-live-runner-v1"
TIMESTAMP_RE = re.compile(r"^\d{8}T\d{6}Z$")
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")

CORPUS_AUTHORITY = (
    ("corpus", "LLVM", 1238),
    ("corpus2", "RocksDB", 622),
    ("corpus3", "DuckDB", 689),
    ("corpus4", "abseil+protobuf", 700),
    ("corpus5", "OpenCV", 1506),
    ("corpus6", "Godot", 2207),
    ("corpus7", "fmt", 50),
    ("corpus8", "spdlog", 34),
    ("corpus9", "Catch2", 857),
    ("corpus10", "nlohmann-json", 99),
    ("corpus11", "range-v3", 259),
)
METHODS = ("RAW_II", "ZSTD_TU", "ZSTD_ROUTE", "P29", "GRZ_RESIDUAL",
           "ZSTD_COHORT", "ZSTD_GLOBAL")
EXPECTED_METHODS = METHODS
IMPLEMENTED_METHODS = frozenset(("ZSTD_TU", "ZSTD_ROUTE", "P29", "GRZ_RESIDUAL"))
DEPTHS = ("100", "200", "full-1", "state-carrying full-2")
EXPECTED_DEPTHS = DEPTHS
REGIMES = ("cold", "warm")
EXPECTED_REGIMES = REGIMES
TOPOLOGIES = (
    {
        "id": "C1F1/100000", "f_relationships": 1,
        "execution_slots_per_f": 1, "global_execution_slots": 1,
        "stream_capacity_tus": 100000,
    },
    {
        "id": "C1F20/40", "f_relationships": 20,
        "execution_slots_per_f": 2, "global_execution_slots": 40,
        "stream_capacity_tus": None,
        "stream_capacity_status": "NOT_DECLARED",
    },
)
EXPECTED_TOPOLOGY_SIGNATURE = (
    ("C1F1/100000", 1, 1, 1, 100000, None),
    ("C1F20/40", 20, 2, 40, None, "NOT_DECLARED"),
)
HISTORICAL_IMAGES = (
    ("debian-gcc", "sha256:8609dfbcca764feeeb4ad9cd8d9bdba14302711eb3283b83b3d6cad60fe1cd4b"),
    ("conan-gcc", "sha256:c16fc7cf0c6c9a13cbdf6e2594330aa798916b7cae08dcc2475badfebe8848d2"),
    ("linuxbrew", "sha256:f67ac23a8a320b866ad12949779fd947272861125af538b1678e4ff2083d86e3"),
    ("fedora-clang-libcxx", "sha256:9ddfe0c3676ad4d0213dd965a622aad49a6c3a4b73589e874122e680641e3d90"),
)
HISTORICAL_GRID_COUNT = 4928
CURRENT_IMPLEMENTED_COUNT = 704


class PlannerError(ValueError):
    """An input authority or declarative contract is invalid."""


def _canonical(value: Any) -> bytes:
    return (json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n").encode()


def _private_file(path: Path, label: str) -> bytes:
    try:
        info = path.lstat()
    except OSError as exc:
        raise PlannerError(f"{label}:unavailable:{path}") from exc
    if stat.S_ISLNK(info.st_mode) or not stat.S_ISREG(info.st_mode) or info.st_nlink != 1:
        raise PlannerError(f"{label}:not_private_regular_file:{path}")
    try:
        return path.read_bytes()
    except OSError as exc:
        raise PlannerError(f"{label}:unreadable:{path}") from exc


def _file_descriptor(path: Path, raw: bytes | None = None) -> dict[str, object]:
    if raw is None:
        raw = _private_file(path, "file")
    return {"path": str(path.resolve()), "bytes": len(raw),
            "sha256": hashlib.sha256(raw).hexdigest()}


def _json(path: Path, label: str) -> tuple[dict[str, Any], bytes]:
    raw = _private_file(path, label)
    try:
        value = json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise PlannerError(f"{label}:invalid_json:{path}") from exc
    if not isinstance(value, dict):
        raise PlannerError(f"{label}:object_required")
    return value, raw


def _hex(value: object, label: str) -> str:
    if not isinstance(value, str) or not SHA256_RE.fullmatch(value):
        raise PlannerError(f"{label}:sha256_invalid")
    return value


def _snapshot_file(path: Path, root: Path, label: str) -> tuple[dict[str, object], Path]:
    """Hash one retained source snapshot through a no-follow descriptor."""
    try:
        resolved = path.resolve(strict=True)
        resolved.relative_to(root.resolve())
    except (OSError, ValueError) as exc:
        raise PlannerError(f"{label}:resolved_escape:{path}") from exc
    flags = os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0)
    try:
        fd = os.open(path, flags)
    except OSError as exc:
        raise PlannerError(f"{label}:unavailable:{path}") from exc
    try:
        before = os.fstat(fd)
        if (not stat.S_ISREG(before.st_mode) or before.st_nlink != 1 or
                resolved != path.resolve(strict=True)):
            raise PlannerError(f"{label}:not_private_regular_file:{path}")
        digest = hashlib.sha256()
        size = 0
        while True:
            chunk = os.read(fd, 1024 * 1024)
            if not chunk:
                break
            digest.update(chunk)
            size += len(chunk)
        after = os.fstat(fd)
        before_identity = (before.st_dev, before.st_ino, before.st_size,
                           before.st_mtime_ns, before.st_ctime_ns)
        after_identity = (after.st_dev, after.st_ino, after.st_size,
                          after.st_mtime_ns, after.st_ctime_ns)
        if before_identity != after_identity or size != before.st_size:
            raise PlannerError(f"{label}:mutated_during_read:{path}")
    finally:
        os.close(fd)
    return {"path": str(path), "resolved_path": str(resolved), "bytes": size,
            "sha256": digest.hexdigest()}, resolved


def _snapshot_descriptor(path: Path, label: str) -> tuple[dict[str, object], Path]:
    """Hash a declared capability artifact without following its final path."""
    try:
        path_info = path.lstat()
    except OSError as exc:
        raise PlannerError(f"{label}:unavailable:{path}") from exc
    if (stat.S_ISLNK(path_info.st_mode) or not stat.S_ISREG(path_info.st_mode) or
            path_info.st_nlink != 1):
        raise PlannerError(f"{label}:not_private_regular_file:{path}")
    try:
        resolved = path.resolve(strict=True)
    except OSError as exc:
        raise PlannerError(f"{label}:unavailable:{path}") from exc
    flags = os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0)
    try:
        fd = os.open(path, flags)
    except OSError as exc:
        raise PlannerError(f"{label}:unavailable:{path}") from exc
    try:
        before = os.fstat(fd)
        if (not stat.S_ISREG(before.st_mode) or before.st_nlink != 1 or
                (before.st_dev, before.st_ino) != (path_info.st_dev, path_info.st_ino)):
            raise PlannerError(f"{label}:not_private_regular_file:{path}")
        digest = hashlib.sha256()
        size = 0
        while True:
            chunk = os.read(fd, 1024 * 1024)
            if not chunk:
                break
            digest.update(chunk)
            size += len(chunk)
        after = os.fstat(fd)
        before_identity = (before.st_dev, before.st_ino, before.st_size,
                           before.st_mtime_ns, before.st_ctime_ns)
        after_identity = (after.st_dev, after.st_ino, after.st_size,
                           after.st_mtime_ns, after.st_ctime_ns)
        try:
            path_after = path.lstat()
        except OSError as exc:
            raise PlannerError(f"{label}:path_changed_during_read:{path}") from exc
        path_after_identity = (path_after.st_dev, path_after.st_ino,
                               path_after.st_size, path_after.st_mtime_ns,
                               path_after.st_ctime_ns)
        if (before_identity != after_identity or size != before.st_size or
                path_after_identity != after_identity or
                stat.S_ISLNK(path_after.st_mode) or not stat.S_ISREG(path_after.st_mode) or
                path_after.st_nlink != 1):
            raise PlannerError(f"{label}:mutated_during_read:{path}")
    finally:
        os.close(fd)
    return {"path": str(path), "resolved_path": str(resolved), "bytes": size,
            "sha256": digest.hexdigest()}, resolved


def _git_output(repo: Path, args: list[str], label: str) -> str:
    try:
        result = subprocess.run(["git", "-C", str(repo), *args], check=True,
                                capture_output=True, text=True, timeout=15)
    except (OSError, subprocess.SubprocessError) as exc:
        raise PlannerError(f"capability_manifest:{label}") from exc
    return result.stdout.strip()


def _authenticate_capability_source(source: dict[str, object]) -> dict[str, object]:
    source_path = Path(str(source["path"]))
    if not source_path.is_absolute():
        raise PlannerError("capability_manifest:source_path_not_absolute")
    snapshot, resolved = _snapshot_descriptor(source_path, "capability_manifest:source")
    if snapshot["bytes"] != source["bytes"]:
        raise PlannerError("capability_manifest:source_bytes_mismatch")
    if snapshot["sha256"] != str(source["sha256"]).lower():
        raise PlannerError("capability_manifest:source_sha256_mismatch")

    repo_text = _git_output(source_path.parent, ["rev-parse", "--show-toplevel"],
                            "source_git_unavailable")
    repo = Path(repo_text).resolve()
    try:
        relative = resolved.relative_to(repo)
    except ValueError as exc:
        raise PlannerError("capability_manifest:source_not_in_git_worktree") from exc
    relative_name = relative.as_posix()
    status = _git_output(repo, ["status", "--porcelain", "--untracked-files=no"],
                         "source_git_status_unavailable")
    if status:
        raise PlannerError("capability_manifest:source_git_dirty")
    head = _git_output(repo, ["rev-parse", "HEAD"], "source_git_head_unavailable")
    if head.lower() != str(source["commit"]).lower():
        raise PlannerError("capability_manifest:source_git_head_mismatch")
    tree = _git_output(repo, ["rev-parse", "HEAD^{tree}"], "source_git_tree_unavailable")
    declared_tree = source.get("tree")
    if declared_tree is not None and str(declared_tree).lower() != tree.lower():
        raise PlannerError("capability_manifest:source_git_tree_mismatch")
    tracked = _git_output(repo, ["ls-files", "--error-unmatch", "--", relative_name],
                          "source_git_path_untracked")
    if tracked != relative_name:
        raise PlannerError("capability_manifest:source_git_path_untracked")
    try:
        committed = subprocess.run(
            ["git", "-C", str(repo), "show", f"{head}:{relative_name}"],
            check=True, capture_output=True, timeout=15).stdout
    except (OSError, subprocess.SubprocessError) as exc:
        raise PlannerError("capability_manifest:source_git_blob_unavailable") from exc
    if len(committed) != snapshot["bytes"] or hashlib.sha256(committed).hexdigest() != snapshot["sha256"]:
        raise PlannerError("capability_manifest:source_git_blob_mismatch")
    return {**source, "path": str(source_path), "resolved_path": str(resolved),
            "bytes": snapshot["bytes"], "sha256": snapshot["sha256"],
            "git": {"root": str(repo), "head": head, "tree": tree,
                    "tracked_path": relative_name}}


def _load_corpus_inventory(path: Path) -> tuple[dict[str, object], list[dict[str, object]], list[dict[str, object]]]:
    value, raw = _json(path, "corpus_inventory")
    if value.get("schema") != "icecream-s8-image-authority-inventory-v1":
        raise PlannerError("corpus_inventory:schema_invalid")
    if value.get("read_only") is not True:
        raise PlannerError("corpus_inventory:not_read_only_authority")
    seal = value.get("seal")
    if (not isinstance(seal, dict) or seal.get("algorithm") != "sha256" or
            not isinstance(seal.get("canonical_without_seal_sha256"), str)):
        raise PlannerError("corpus_inventory:seal_missing")
    without_seal = dict(value)
    without_seal.pop("seal", None)
    if hashlib.sha256(_canonical(without_seal)).hexdigest() != seal["canonical_without_seal_sha256"]:
        raise PlannerError("corpus_inventory:seal_mismatch")
    entries = value.get("corpus_manifests")
    if not isinstance(entries, list) or len(entries) != len(CORPUS_AUTHORITY):
        raise PlannerError("corpus_inventory:expected_11_manifests")
    records: list[dict[str, object]] = []
    snapshots: list[dict[str, object]] = []
    seen_paths: set[str] = set()
    seen_resolved: set[str] = set()
    total = 0
    for entry, (manifest_id, project, count) in zip(entries, CORPUS_AUTHORITY):
        if not isinstance(entry, dict):
            raise PlannerError("corpus_inventory:manifest_record_invalid")
        if (entry.get("manifest_id"), entry.get("project"), entry.get("tu_count")) != (manifest_id, project, count):
            raise PlannerError(f"corpus_inventory:identity_or_count_mismatch:{manifest_id}")
        descriptor = entry.get("manifest")
        if not isinstance(descriptor, dict) or set(descriptor) != {"path", "sha256"}:
            raise PlannerError(f"corpus_inventory:manifest_descriptor_invalid:{manifest_id}")
        manifest_path = Path(descriptor["path"])
        if not manifest_path.is_absolute():
            raise PlannerError(f"corpus_inventory:manifest_path_not_absolute:{manifest_id}")
        manifest_raw = _private_file(manifest_path, f"manifest:{manifest_id}")
        observed_sha = hashlib.sha256(manifest_raw).hexdigest()
        if observed_sha != _hex(descriptor["sha256"], f"manifest:{manifest_id}"):
            raise PlannerError(f"corpus_inventory:manifest_digest_mismatch:{manifest_id}")
        try:
            text = manifest_raw.decode("utf-8")
        except UnicodeDecodeError as exc:
            raise PlannerError(f"manifest:{manifest_id}:not_utf8") from exc
        if not text.endswith("\n"):
            raise PlannerError(f"manifest:{manifest_id}:missing_final_newline")
        paths = text.splitlines()
        if len(paths) != count or any(not item for item in paths):
            raise PlannerError(f"manifest:{manifest_id}:tu_count_mismatch")
        if len(set(paths)) != len(paths):
            raise PlannerError(f"manifest:{manifest_id}:duplicate_source_path")
        corpus_root = manifest_path.parent
        corpus_snapshots = []
        for ordinal, source_path in enumerate(paths):
            candidate = Path(source_path)
            if not candidate.is_absolute():
                raise PlannerError(f"manifest:{manifest_id}:source_path_not_absolute:{ordinal}")
            if source_path in seen_paths:
                raise PlannerError(f"manifest:{manifest_id}:duplicate_source_path_global:{ordinal}")
            snapshot, resolved = _snapshot_file(candidate, corpus_root,
                                                f"snapshot:{manifest_id}:{ordinal}")
            if str(resolved) in seen_resolved:
                raise PlannerError(f"manifest:{manifest_id}:duplicate_resolved_source_path:{ordinal}")
            seen_paths.add(source_path)
            seen_resolved.add(str(resolved))
            item = {"corpus": manifest_id, "ordinal": ordinal, **snapshot}
            corpus_snapshots.append(item)
            snapshots.append(item)
        checkouts = entry.get("source_checkouts")
        source_commit = entry.get("source_commit")
        if (not isinstance(checkouts, list) or not checkouts or
                any(not isinstance(item, str) or not item for item in checkouts) or
                not isinstance(source_commit, str) or not source_commit):
            raise PlannerError(f"corpus_inventory:source_authority_invalid:{manifest_id}")
        records.append({
            "manifest_id": manifest_id, "project": project, "tu_count": count,
            "manifest": {"path": str(manifest_path), "bytes": len(manifest_raw),
                          "sha256": observed_sha},
            "ordered_source_paths": paths,
            "source_checkouts": list(checkouts), "source_commit": source_commit,
            "snapshot": {
                "count": len(corpus_snapshots),
                "total_raw_bytes": sum(int(item["bytes"]) for item in corpus_snapshots),
                "ordered_snapshot_digest": hashlib.sha256(
                    b"".join(_canonical(item) for item in corpus_snapshots)).hexdigest(),
            },
        })
        total += count
    if total != 8261:
        raise PlannerError(f"corpus_inventory:total_tu_count:{total}")
    return {"path": str(path.resolve()), "bytes": len(raw),
            "sha256": hashlib.sha256(raw).hexdigest()}, records, snapshots


def _load_image_recovery(path: Path, expected_sha256: str) -> tuple[dict[str, object], list[dict[str, object]]]:
    raw = _private_file(path, "image_recovery")
    observed = hashlib.sha256(raw).hexdigest()
    if (not isinstance(expected_sha256, str) or
            not SHA256_RE.fullmatch(expected_sha256.lower()) or
            observed != expected_sha256.lower()):
        raise PlannerError("image_recovery:sha256_mismatch")
    value = json.loads(raw.decode("utf-8"))
    if not isinstance(value, dict) or value.get("schema") != RECOVERY_SCHEMA:
        raise PlannerError("image_recovery:schema_invalid")
    conclusion = value.get("conclusion")
    if (not isinstance(conclusion, dict) or conclusion.get("rebuildable_now") != [] or
            conclusion.get("external_authority_required") != [item[1] for item in HISTORICAL_IMAGES]):
        raise PlannerError("image_recovery:conclusion_mutated")
    targets = value.get("targets")
    if not isinstance(targets, list) or len(targets) != len(HISTORICAL_IMAGES):
        raise PlannerError("image_recovery:target_count_invalid")
    result = []
    for target, (profile, image_id) in zip(targets, HISTORICAL_IMAGES):
        if (not isinstance(target, dict) or target.get("profile") != profile or
                target.get("image_id") != image_id or target.get("status") != "EXTERNAL_AUTHORITY_REQUIRED" or
                target.get("rebuildable_now") is not False):
            raise PlannerError(f"image_recovery:target_mutated:{profile}")
        result.append({"key": profile, "name": None, "image_id": image_id,
                       "registry_digest": None, "status": "MISSING_EXTERNAL_AUTHORITY",
                       "authority_report": str(path.resolve()),
                       "reason": "historical image identity is incomplete; external authority required"})
    return {"path": str(path.resolve()), "bytes": len(raw), "sha256": observed}, result


def _current_image(name: str | None, image_id: str | None) -> dict[str, object]:
    if (name is None) != (image_id is None):
        raise PlannerError("current_image:name_and_content_id_required_together")
    if name is None:
        return {"key": "current-pinned", "name": None, "image_id": None,
                "registry_digest": None, "status": "NOT_READY_MISSING_EXACT_CONTENT_ID",
                "reason": "supply an exact current image content ID before executable cells are ready"}
    if not name or not isinstance(image_id, str) or not re.fullmatch(r"sha256:[0-9a-f]{64}", image_id.lower()):
        raise PlannerError("current_image:content_id_invalid")
    return {"key": "current-pinned", "name": name, "image_id": image_id.lower(),
            "registry_digest": None, "status": "READY_EXECUTABLE_DIMENSION",
            "authority": "explicit_content_id_argument"}


def _load_matrix_audit(path: Path, expected_sha256: str) -> dict[str, object]:
    raw = _private_file(path, "matrix_audit")
    if (not isinstance(expected_sha256, str) or
            not SHA256_RE.fullmatch(expected_sha256.lower()) or
            hashlib.sha256(raw).hexdigest() != expected_sha256.lower()):
        raise PlannerError("matrix_audit:sha256_mismatch")
    try:
        value = json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise PlannerError("matrix_audit:invalid_json") from exc
    if not isinstance(value, dict) or value.get("schema") != MATRIX_AUDIT_SCHEMA or value.get("status") != "PASS":
        raise PlannerError("matrix_audit:schema_or_status_invalid")
    matrix = value.get("matrix")
    if (not isinstance(matrix, dict) or matrix.get("expected_cells") != 32 or
            matrix.get("completed_cells") != 32 or matrix.get("calibration_cells") != 16 or
            matrix.get("calibration_expected") != 16 or matrix.get("held_out_validation_cells") != 16 or
            matrix.get("held_out_validation_expected") != 16 or
            matrix.get("missing_cells") != [] or matrix.get("invalid_candidates") != []):
        raise PlannerError("matrix_audit:matrix_contract_invalid")
    cells = value.get("cells")
    if not isinstance(cells, list) or len(cells) != 32 or any(
            not isinstance(cell, dict) or cell.get("status") != "PASS" for cell in cells):
        raise PlannerError("matrix_audit:cells_invalid")
    return {"path": str(path.resolve()), "bytes": len(raw),
            "sha256": hashlib.sha256(raw).hexdigest(),
            "schema": MATRIX_AUDIT_SCHEMA, "status": "PASS",
            "matrix": {key: matrix[key] for key in (
                "expected_cells", "completed_cells", "calibration_cells",
                "calibration_expected", "held_out_validation_cells",
                "held_out_validation_expected", "missing_cells", "invalid_candidates")}}


def _load_capability(path: Path, expected_sha256: str) -> dict[str, object]:
    raw = _private_file(path, "capability_manifest")
    if (not isinstance(expected_sha256, str) or
            not SHA256_RE.fullmatch(expected_sha256.lower()) or
            hashlib.sha256(raw).hexdigest() != expected_sha256.lower()):
        raise PlannerError("capability_manifest:sha256_mismatch")
    try:
        value = json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise PlannerError("capability_manifest:invalid_json") from exc
    if not isinstance(value, dict) or value.get("schema") != CAPABILITY_SCHEMA or value.get("status") != "PASS":
        raise PlannerError("capability_manifest:schema_or_status_invalid")
    if value.get("capability") != CAPABILITY or not isinstance(value.get("producer_version"), str) or not value["producer_version"]:
        raise PlannerError("capability_manifest:producer_identity_invalid")
    source = value.get("source")
    if (not isinstance(source, dict) or not isinstance(source.get("commit"), str) or
            not re.fullmatch(r"[0-9a-f]{40}", source["commit"].lower()) or
            not isinstance(source.get("path"), str) or
            not Path(source["path"]).is_absolute() or
            not isinstance(source.get("bytes"), int) or source["bytes"] <= 0 or
            not isinstance(source.get("sha256"), str) or
            not SHA256_RE.fullmatch(source["sha256"].lower()) or
            (source.get("tree") is not None and
             (not isinstance(source.get("tree"), str) or
              not re.fullmatch(r"[0-9a-f]{40}", source["tree"].lower())))):
        raise PlannerError("capability_manifest:source_identity_invalid")
    binaries = value.get("binaries")
    if (not isinstance(binaries, dict) or set(binaries) != {"client", "daemon", "scheduler", "cache-service", "simulator"}):
        raise PlannerError("capability_manifest:binary_set_invalid")
    for name, descriptor in binaries.items():
        if (not isinstance(descriptor, dict) or not isinstance(descriptor.get("path"), str) or
                not Path(descriptor["path"]).is_absolute() or
                not isinstance(descriptor.get("bytes"), int) or descriptor["bytes"] <= 0 or
                not isinstance(descriptor.get("sha256"), str) or
                not SHA256_RE.fullmatch(descriptor["sha256"].lower())):
            raise PlannerError(f"capability_manifest:binary_invalid:{name}")
    authenticated_source = _authenticate_capability_source(source)
    authenticated_binaries: dict[str, object] = {}
    for name, descriptor in binaries.items():
        binary_path = Path(str(descriptor["path"]))
        snapshot, resolved = _snapshot_descriptor(binary_path,
                                                  f"capability_manifest:binary:{name}")
        if snapshot["bytes"] != descriptor["bytes"]:
            raise PlannerError(f"capability_manifest:binary_bytes_mismatch:{name}")
        if snapshot["sha256"] != str(descriptor["sha256"]).lower():
            raise PlannerError(f"capability_manifest:binary_sha256_mismatch:{name}")
        authenticated_binaries[name] = {
            **descriptor, "path": str(binary_path), "resolved_path": str(resolved),
            "bytes": snapshot["bytes"], "sha256": snapshot["sha256"],
        }
    return {"path": str(path.resolve()), "bytes": len(raw),
            "sha256": hashlib.sha256(raw).hexdigest(), "schema": CAPABILITY_SCHEMA,
            "capability": CAPABILITY, "producer_version": value["producer_version"],
            "source": authenticated_source, "binaries": authenticated_binaries,
            "status": "PASS"}


def _slug(value: str) -> str:
    result = re.sub(r"[^A-Za-z0-9]+", "-", value).strip("-").lower()
    if not result:
        raise PlannerError("result_path:empty_slug")
    return result


def _result_relative_directory(timestamp: str, image: dict[str, object], corpus: dict[str, object],
                               method: str, topology: dict[str, object], depth: str, regime: str) -> str:
    return "/".join(("experiments", "icecream", "s8-expanded", timestamp,
                      _slug(str(image["key"])), _slug(str(corpus["project"])), _slug(method),
                      _slug(str(topology["id"])), _slug(depth), _slug(regime)))


def _write_exact(path: Path, raw: bytes) -> None:
    if path.exists():
        if _private_file(path, "existing_output") != raw:
            raise PlannerError(f"output:mutation:{path.name}")
        return
    path.write_bytes(raw)


def plan_campaign(corpus_inventory: Path, image_recovery: Path, image_recovery_sha256: str,
                  matrix_audit: Path, matrix_audit_sha256: str, output_root: Path,
                  timestamp: str, current_image_name: str | None = None,
                  current_image_id: str | None = None, capability_manifest: Path | None = None,
                  capability_manifest_sha256: str | None = None) -> dict[str, object]:
    if not TIMESTAMP_RE.fullmatch(timestamp):
        raise PlannerError("timestamp:expected_YYYYMMDDTHHMMSSZ")
    if METHODS != EXPECTED_METHODS:
        raise PlannerError("method_contract:mutated")
    if DEPTHS != EXPECTED_DEPTHS or REGIMES != EXPECTED_REGIMES:
        raise PlannerError("depth_or_regime_contract:mutated")
    topology_signature = tuple(
        (item.get("id"), item.get("f_relationships"), item.get("execution_slots_per_f"),
         item.get("global_execution_slots"), item.get("stream_capacity_tus"),
         item.get("stream_capacity_status")) for item in TOPOLOGIES)
    if topology_signature != EXPECTED_TOPOLOGY_SIGNATURE:
        raise PlannerError("topology_contract:mutated")
    corpus_descriptor, corpora, snapshots = _load_corpus_inventory(corpus_inventory)
    recovery_descriptor, historical_images = _load_image_recovery(image_recovery, image_recovery_sha256)
    matrix_descriptor = _load_matrix_audit(matrix_audit, matrix_audit_sha256)
    current = _current_image(current_image_name, current_image_id)
    capability = None
    if (capability_manifest is None) != (capability_manifest_sha256 is None):
        raise PlannerError("capability_manifest:path_and_sha256_required_together")
    if capability_manifest is not None:
        capability = _load_capability(capability_manifest, str(capability_manifest_sha256))
    images = historical_images + [current]
    methods = [{
        "name": method,
        "root_status": "IMPLEMENTED" if method in IMPLEMENTED_METHODS else "NOT_IMPLEMENTED",
        "producer_capability": CAPABILITY if method in IMPLEMENTED_METHODS else None,
        "status_reason": "native/live runner interface available on current Root" if method in IMPLEMENTED_METHODS
        else "method is not implemented on current Root; no alias is permitted",
    } for method in METHODS]
    descriptors: list[dict[str, object]] = []
    result_paths: set[str] = set()
    for image in images:
        if image["key"] == "current-pinned" and image["image_id"] is None:
            continue
        for corpus in corpora:
            for method in METHODS:
                for topology in TOPOLOGIES:
                    for depth in DEPTHS:
                        for regime in REGIMES:
                            descriptor_id = "/".join((str(image["key"]), str(corpus["project"]), method,
                                                       str(topology["id"]), depth, regime))
                            if image["status"] == "MISSING_EXTERNAL_AUTHORITY":
                                status, reason, producer_capability = (
                                    "MISSING_EXTERNAL_AUTHORITY",
                                    "historical image cell is unavailable without external image authority",
                                    None)
                            elif method not in IMPLEMENTED_METHODS:
                                status, reason, producer_capability = (
                                    "NOT_READY", "method_not_implemented_on_current_root", None)
                            elif capability is None:
                                status, reason, producer_capability = (
                                    "NOT_READY", "required_producer_capability_not_integrated_on_planner_source", None)
                            else:
                                status, reason, producer_capability = "READY", "authenticated producer capability", CAPABILITY
                            result_relative_directory = _result_relative_directory(
                                timestamp, image, corpus, method, topology, depth, regime)
                            if result_relative_directory in result_paths:
                                raise PlannerError(f"result_path:collision:{result_relative_directory}")
                            result_paths.add(result_relative_directory)
                            descriptors.append({
                                "schema": DESCRIPTOR_SCHEMA, "descriptor_id": descriptor_id,
                                "image": dict(image), "corpus": {
                                    "manifest_id": corpus["manifest_id"], "project": corpus["project"],
                                    "tu_count": corpus["tu_count"],
                                    "manifest": corpus["manifest"], "source_commit": corpus["source_commit"],
                                    "snapshot": corpus["snapshot"],
                                }, "method": method, "topology": dict(topology), "depth": depth,
                                "regime": regime, "status": status, "reason": reason,
                                "producer_capability": producer_capability, "execution": "declarative_only",
                                "campaign_timestamp": timestamp,
                                "result_relative_directory": result_relative_directory,
                            })
    historical_count = sum(1 for item in descriptors if item["image"]["key"] != "current-pinned")
    current_count = len(descriptors) - historical_count
    if historical_count != HISTORICAL_GRID_COUNT:
        raise PlannerError(f"planner:historical_descriptor_count:{historical_count}")
    expected_current = CURRENT_IMPLEMENTED_COUNT if current["image_id"] is not None else 0
    if current_count != (CURRENT_IMPLEMENTED_COUNT + (len(corpora) * 3 * len(TOPOLOGIES) * len(DEPTHS) * len(REGIMES)) if current["image_id"] is not None else 0):
        # Current descriptors include explicit NOT_READY methods, so the
        # implementation subset is reported separately below.
        raise PlannerError("planner:current_descriptor_count")
    current_implemented = sum(1 for item in descriptors
                              if item["image"]["key"] == "current-pinned" and
                              item["method"] in IMPLEMENTED_METHODS)
    current_ready = sum(1 for item in descriptors
                        if item["image"]["key"] == "current-pinned" and item["status"] == "READY")
    if current_implemented != expected_current:
        raise PlannerError(f"planner:current_implemented_count:{current_implemented}")
    output_input = output_root
    try:
        output_info = output_input.lstat()
    except FileNotFoundError:
        output_info = None
    except OSError as exc:
        raise PlannerError(f"output:unavailable:{output_input}") from exc
    if output_info is not None and (stat.S_ISLNK(output_info.st_mode) or not stat.S_ISDIR(output_info.st_mode)):
        raise PlannerError("output:not_private_directory")
    output_root = output_input.resolve()
    output_root.mkdir(parents=True, exist_ok=True)
    snapshots_raw = b"".join(_canonical(item) for item in snapshots)
    descriptors_raw = b"".join(_canonical(item) for item in descriptors)
    _write_exact(output_root / "corpus-snapshots.jsonl", snapshots_raw)
    descriptor_path = output_root / "descriptors.jsonl"
    _write_exact(descriptor_path, descriptors_raw)
    index: dict[str, object] = {
        "schema": SCHEMA, "timestamp": timestamp, "campaign_root": str(output_root),
        "layout": "experiments/icecream/s8-expanded/<timestamp>/<image>/<corpus>/<method>/<topology>/<depth>/<regime>",
        "execution": {"mode": "declarative_only", "commands_emitted": False,
                       "required_producer_capability": CAPABILITY},
        "corpus_authority": {"inventory": corpus_descriptor, "total_manifests": len(corpora),
                              "total_tus": sum(int(item["tu_count"]) for item in corpora), "records": corpora,
                              "snapshots": {"path": "corpus-snapshots.jsonl", "bytes": len(snapshots_raw),
                                            "sha256": hashlib.sha256(snapshots_raw).hexdigest(), "count": len(snapshots)}},
        "methods": methods, "topologies": list(TOPOLOGIES), "depths": list(DEPTHS), "regimes": list(REGIMES),
        "images": images, "image_recovery": recovery_descriptor, "matrix_audit": matrix_descriptor,
        "capability_manifest": capability,
        "prerequisites": {
            "canonical_32_cell_one_tu_audit": {
                "status": "SATISFIED", "cell_count": 32,
                "scope": "original canonical one-TU audit only; not a completed depth campaign",
            },
            "firefox_retained_giant_four_block_extension": {"status": "NOT_RUN"},
        },
        "counts": {
            "historical_image_seven_method_grid": HISTORICAL_GRID_COUNT,
            "current_image_implemented_subset_theoretical": CURRENT_IMPLEMENTED_COUNT,
            "current_image_implemented_subset": current_implemented,
            "current_image_ready": current_ready,
            "descriptors_total": len(descriptors),
            "descriptor_status_counts": {
                status: sum(1 for item in descriptors if item["status"] == status)
                for status in ("READY", "NOT_READY", "MISSING_EXTERNAL_AUTHORITY")
            },
        },
        "descriptors": {"path": "descriptors.jsonl", "bytes": len(descriptors_raw),
                        "sha256": hashlib.sha256(descriptors_raw).hexdigest(), "count": len(descriptors)},
    }
    _write_exact(output_root / "campaign-index.json", _canonical(index))
    return index


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--corpus-inventory", type=Path, required=True)
    parser.add_argument("--image-recovery-report", type=Path, required=True)
    parser.add_argument("--image-recovery-sha256", required=True)
    parser.add_argument("--matrix-audit", type=Path, required=True)
    parser.add_argument("--matrix-audit-sha256", required=True)
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--timestamp", required=True)
    parser.add_argument("--current-image-name")
    parser.add_argument("--current-image-id")
    parser.add_argument("--capability-manifest", type=Path)
    parser.add_argument("--capability-manifest-sha256")
    args = parser.parse_args(argv)
    try:
        index = plan_campaign(args.corpus_inventory, args.image_recovery_report,
                              args.image_recovery_sha256, args.matrix_audit,
                              args.matrix_audit_sha256, args.output_root, args.timestamp,
                              args.current_image_name, args.current_image_id,
                              args.capability_manifest, args.capability_manifest_sha256)
    except (PlannerError, OSError, ValueError, json.JSONDecodeError) as exc:
        print(f"s8_campaign_planner: {exc}", file=sys.stderr)
        return 77
    print(json.dumps({"status": "PASS", "index": str(args.output_root.resolve() / "campaign-index.json"),
                      "descriptors": index["counts"]["descriptors_total"]}, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
