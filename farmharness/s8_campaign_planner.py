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
import re
import stat
import sys
from pathlib import Path
from typing import Any


SCHEMA = "icecream-s8-expanded-campaign-v1"
DESCRIPTOR_SCHEMA = "icecream-s8-campaign-descriptor-v1"
RECOVERY_SCHEMA = "icecream-s8-image-authority-recovery-v1"
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


def _load_corpus_inventory(path: Path) -> tuple[dict[str, object], list[dict[str, object]]]:
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
        })
        total += count
    if total != 8261:
        raise PlannerError(f"corpus_inventory:total_tu_count:{total}")
    return {"path": str(path.resolve()), "bytes": len(raw),
            "sha256": hashlib.sha256(raw).hexdigest()}, records


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


def _write_exact(path: Path, raw: bytes) -> None:
    if path.exists():
        if _private_file(path, "existing_output") != raw:
            raise PlannerError(f"output:mutation:{path.name}")
        return
    path.write_bytes(raw)


def plan_campaign(corpus_inventory: Path, image_recovery: Path, image_recovery_sha256: str,
                  output_root: Path, timestamp: str, current_image_name: str | None = None,
                  current_image_id: str | None = None) -> dict[str, object]:
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
    corpus_descriptor, corpora = _load_corpus_inventory(corpus_inventory)
    recovery_descriptor, historical_images = _load_image_recovery(image_recovery, image_recovery_sha256)
    current = _current_image(current_image_name, current_image_id)
    images = historical_images + [current]
    methods = [{
        "name": method,
        "root_status": "IMPLEMENTED" if method in IMPLEMENTED_METHODS else "NOT_IMPLEMENTED",
        "producer_capability": CAPABILITY if method in IMPLEMENTED_METHODS else None,
        "status_reason": "native/live runner interface available on current Root" if method in IMPLEMENTED_METHODS
        else "method is not implemented on current Root; no alias is permitted",
    } for method in METHODS]
    descriptors: list[dict[str, object]] = []
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
                                status, reason, capability = (
                                    "MISSING_EXTERNAL_AUTHORITY",
                                    "historical image cell is unavailable without external image authority",
                                    None)
                            elif method not in IMPLEMENTED_METHODS:
                                status, reason, capability = (
                                    "NOT_READY", "method_not_implemented_on_current_root", None)
                            else:
                                status, reason, capability = "READY", "declarative capability can be bound by native/live runner", CAPABILITY
                            descriptors.append({
                                "schema": DESCRIPTOR_SCHEMA, "descriptor_id": descriptor_id,
                                "image": dict(image), "corpus": {
                                    "manifest_id": corpus["manifest_id"], "project": corpus["project"],
                                    "tu_count": corpus["tu_count"],
                                    "manifest": corpus["manifest"], "source_commit": corpus["source_commit"],
                                }, "method": method, "topology": dict(topology), "depth": depth,
                                "regime": regime, "status": status, "reason": reason,
                                "producer_capability": capability, "execution": "declarative_only",
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
    descriptors_raw = b"".join(_canonical(item) for item in descriptors)
    descriptor_path = output_root / "descriptors.jsonl"
    _write_exact(descriptor_path, descriptors_raw)
    index: dict[str, object] = {
        "schema": SCHEMA, "timestamp": timestamp, "layout": "experiments/icecream/<campaign>/<timestamp>",
        "execution": {"mode": "declarative_only", "commands_emitted": False,
                       "required_producer_capability": CAPABILITY},
        "corpus_authority": {"inventory": corpus_descriptor, "total_manifests": len(corpora),
                              "total_tus": sum(int(item["tu_count"]) for item in corpora), "records": corpora},
        "methods": methods, "topologies": list(TOPOLOGIES), "depths": list(DEPTHS), "regimes": list(REGIMES),
        "images": images, "image_recovery": recovery_descriptor,
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
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--timestamp", required=True)
    parser.add_argument("--current-image-name")
    parser.add_argument("--current-image-id")
    args = parser.parse_args(argv)
    try:
        index = plan_campaign(args.corpus_inventory, args.image_recovery_report,
                              args.image_recovery_sha256, args.output_root, args.timestamp,
                              args.current_image_name, args.current_image_id)
    except (PlannerError, OSError, ValueError, json.JSONDecodeError) as exc:
        print(f"s8_campaign_planner: {exc}", file=sys.stderr)
        return 77
    print(json.dumps({"status": "PASS", "index": str(args.output_root.resolve() / "campaign-index.json"),
                      "descriptors": index["counts"]["descriptors_total"]}, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
