#!/usr/bin/env python3
"""Authenticated reuse of S8 direct-reference compiler witnesses.

The package is deliberately a small, immutable directory: ``manifest.jsonl``
contains one package record followed by one occurrence record per TU, and
``objects/<ordinal>.o`` contains the direct-reference bytes.  Reuse never
executes a compiler.  It re-snapshots the current inputs, compile database
commands, authority, and toolchain, then requires every remote object to
match its retained witness byte-for-byte.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import json
import os
import shlex
import stat
import shutil
import tempfile
from pathlib import Path
from typing import Any, Mapping, Sequence


SCHEMA = "icecream-s8-reference-witness-v1"
PACKAGE_SCHEMA = "icecream-s8-reference-witness-package-v1"
PLAN_SCHEMA = "icecream-s8-depth-run-plan-v1"
HEX64 = set("0123456789abcdef")


class ReferenceWitnessError(ValueError):
    """A witness or current input is absent, changed, or inconsistent."""


def _canonical(value: object) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"),
                      ensure_ascii=True, allow_nan=False).encode("ascii")


def _digest(value: object, label: str) -> str:
    if not isinstance(value, str) or len(value) != 64 or any(ch.lower() not in HEX64 for ch in value):
        raise ReferenceWitnessError(f"{label}:digest_invalid")
    result = value.lower()
    if int(result, 16) == 0:
        raise ReferenceWitnessError(f"{label}:digest_zero")
    return result


def snapshot(path: Path, label: str) -> tuple[bytes, str, int]:
    """Read a private regular file once and return bytes, digest, and size."""
    try:
        info = path.lstat()
    except OSError as exc:
        raise ReferenceWitnessError(f"{label}:unavailable:{path}") from exc
    if stat.S_ISLNK(info.st_mode) or not stat.S_ISREG(info.st_mode) or info.st_nlink != 1:
        raise ReferenceWitnessError(f"{label}:private_regular_file_required:{path}")
    try:
        raw = path.read_bytes()
    except OSError as exc:
        raise ReferenceWitnessError(f"{label}:read_failed:{path}") from exc
    digest = hashlib.sha256(raw).hexdigest()
    return raw, digest, len(raw)


def descriptor(path: Path, label: str) -> dict[str, Any]:
    _raw, digest, size = snapshot(path, label)
    return {"path": str(path.resolve()), "sha256": digest, "bytes": size}


def _check_descriptor(value: object, label: str, *, package: Path | None = None) -> tuple[Path, bytes]:
    if (not isinstance(value, Mapping) or set(value) != {"path", "sha256", "bytes"} or
            not isinstance(value.get("path"), str)):
        raise ReferenceWitnessError(f"{label}:descriptor_invalid")
    path = Path(value["path"])
    if package is not None:
        if path.is_absolute() or not path.parts or any(part in ("", ".", "..") for part in path.parts):
            raise ReferenceWitnessError(f"{label}:package_path_invalid")
        path = package / path
    elif not path.is_absolute():
        raise ReferenceWitnessError(f"{label}:absolute_path_required")
    raw, digest, size = snapshot(path, label)
    if digest != _digest(value.get("sha256"), f"{label}.sha256") or size != value.get("bytes"):
        raise ReferenceWitnessError(f"{label}:descriptor_mismatch")
    if type(value.get("bytes")) is not int or value["bytes"] <= 0:
        raise ReferenceWitnessError(f"{label}.bytes_invalid")
    return path, raw


def _json(path: Path, label: str) -> Any:
    raw, _digest_value, _size = snapshot(path, label)
    try:
        return json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ReferenceWitnessError(f"{label}:invalid_json") from exc


def _compile_entry_output(entry: Mapping[str, Any]) -> Path | None:
    directory, command = entry.get("directory"), entry.get("command")
    if not isinstance(directory, str) or not os.path.isabs(directory) or not isinstance(command, str):
        return None
    try:
        tokens = shlex.split(command)
    except ValueError:
        return None
    outputs: list[str] = []
    index = 0
    while index < len(tokens):
        token = tokens[index]
        if token == "-o":
            if index + 1 >= len(tokens):
                return None
            outputs.append(tokens[index + 1]); index += 2; continue
        if token.startswith("-o") and len(token) > 2:
            outputs.append(token[2:])
        index += 1
    if len(outputs) != 1 or not outputs[0]:
        return None
    output = Path(outputs[0])
    return (output if output.is_absolute() else Path(directory) / output).resolve()


def _compile_bindings(rows: Sequence[Mapping[str, Any]], label: str) -> list[dict[str, Any]]:
    caches: dict[str, tuple[str, list[Any]]] = {}
    result: list[dict[str, Any]] = []
    seen_tu_ids: set[str] = set()
    for ordinal, row in enumerate(rows):
        required = {"tu_id", "source", "source_relative", "sha256", "predictive_input",
                    "compile_db", "compile_db_sha256", "compile_source", "compile_output"}
        if not required.issubset(row):
            raise ReferenceWitnessError(f"{label}:{ordinal}:compile_binding_missing")
        tu_id = row.get("tu_id")
        source_relative = row.get("source_relative")
        payload = row["predictive_input"]
        if (not isinstance(tu_id, str) or not tu_id or tu_id in seen_tu_ids or
                not isinstance(source_relative, str) or not source_relative or
                ("ordinal" in row and row.get("ordinal") != ordinal) or
                not isinstance(payload, Mapping) or payload.get("ordinal") != ordinal):
            raise ReferenceWitnessError(f"{label}:{ordinal}:occurrence_identity_invalid")
        seen_tu_ids.add(tu_id)
        source = Path(str(row["source"]))
        source_desc = descriptor(source, f"{label}:{ordinal}.source")
        if source_desc["sha256"] != _digest(row["sha256"], f"{label}:{ordinal}.source_sha256"):
            raise ReferenceWitnessError(f"{label}:{ordinal}:source_digest_mismatch")
        if not isinstance(payload, Mapping):
            raise ReferenceWitnessError(f"{label}:{ordinal}:input_invalid")
        input_path = Path(str(payload.get("path", "")))
        input_desc = descriptor(input_path, f"{label}:{ordinal}.input")
        if (input_desc["sha256"] != _digest(payload.get("sha256"), f"{label}:{ordinal}.input_sha256") or
                input_desc["bytes"] != payload.get("bytes")):
            raise ReferenceWitnessError(f"{label}:{ordinal}:input_digest_mismatch")
        db_path = Path(str(row["compile_db"]))
        db_desc = descriptor(db_path, f"{label}:{ordinal}.compile_db")
        if db_desc["sha256"] != _digest(row["compile_db_sha256"], f"{label}:{ordinal}.compile_db_sha256"):
            raise ReferenceWitnessError(f"{label}:{ordinal}:compile_db_digest_mismatch")
        cached = caches.get(str(db_path.resolve()))
        if cached is None:
            value = _json(db_path, f"{label}:{ordinal}.compile_db")
            if not isinstance(value, list):
                raise ReferenceWitnessError(f"{label}:{ordinal}:compile_db_not_array")
            cached = (db_desc["sha256"], value); caches[str(db_path.resolve())] = cached
        matches: list[Mapping[str, Any]] = []
        source_path = source.resolve(); output_path = Path(str(row["compile_output"])).resolve()
        for entry in cached[1]:
            if (isinstance(entry, Mapping) and isinstance(entry.get("file"), str) and
                    isinstance(entry.get("command"), str) and
                    Path(entry["file"]).resolve() == source_path and
                    _compile_entry_output(entry) == output_path):
                matches.append(entry)
        if len(matches) != 1:
            raise ReferenceWitnessError(f"{label}:{ordinal}:compile_entry_not_unique")
        entry = matches[0]
        try:
            argv = shlex.split(str(entry["command"]))
        except ValueError as exc:
            raise ReferenceWitnessError(f"{label}:{ordinal}:compile_argv_invalid") from exc
        if not argv:
            raise ReferenceWitnessError(f"{label}:{ordinal}:compile_argv_empty")
        compile_identity = {"directory": str(entry["directory"]), "argv": argv,
                            "source": str(source.resolve()), "output": str(output_path)}
        result.append({"occurrence": {"ordinal": ordinal, "tu_id": tu_id,
                                       "source_relative": source_relative,
                                       "predictive_source_relative": payload["source_relative"],
                                       "sha256": source_desc["sha256"],
                                       "bytes": source_desc["bytes"]},
                       "input": {"path": str(input_path.resolve()),
                                 "sha256": input_desc["sha256"], "bytes": input_desc["bytes"]},
                       "compile": {"db": db_desc, "source": str(source.resolve()),
                                   "output": str(output_path), "argv": argv,
                                   "identity_sha256": hashlib.sha256(_canonical(compile_identity)).hexdigest()}})
    return result


def _authority_identity(value: Mapping[str, Any]) -> str:
    """Hash stable authority identity while excluding only volatile idle facts."""
    stable = json.loads(json.dumps(value))
    hosts = stable.get("hosts")
    if isinstance(hosts, Mapping):
        for item in hosts.values():
            if isinstance(item, dict):
                for field in ("idle", "cpu_sample", "cpu_sample_digest"):
                    item.pop(field, None)
    return hashlib.sha256(_canonical(stable)).hexdigest()


def _validate_product(value: object, label: str = "product") -> dict[str, Any]:
    if not isinstance(value, Mapping) or set(value) != {"image", "toolchain"}:
        raise ReferenceWitnessError(f"{label}:identity_invalid")
    image, toolchain = value.get("image"), value.get("toolchain")
    if (not isinstance(image, Mapping) or set(image) != {"reference", "image_id", "architecture", "os", "created"} or
            not all(isinstance(image.get(field), str) and image[field] for field in ("reference", "image_id", "architecture", "os", "created")) or
            image.get("architecture") != "amd64" or image.get("os") != "linux"):
        raise ReferenceWitnessError(f"{label}.image:identity_invalid")
    image_id = str(image["image_id"])
    if not image_id.startswith("sha256:"):
        raise ReferenceWitnessError(f"{label}.image.image_id:identity_invalid")
    _digest(image_id.removeprefix("sha256:"), f"{label}.image.image_id")
    if (not isinstance(toolchain, Mapping) or set(toolchain) not in ({"sha256", "bytes"},
                                                                       {"sha256", "bytes", "content"}) or
            type(toolchain.get("bytes")) is not int or toolchain["bytes"] <= 0):
        raise ReferenceWitnessError(f"{label}.toolchain:identity_invalid")
    _digest(toolchain.get("sha256"), f"{label}.toolchain.sha256")
    if "content" in toolchain and not isinstance(toolchain["content"], Mapping):
        raise ReferenceWitnessError(f"{label}.toolchain.content_invalid")
    return {"image": dict(image), "toolchain": dict(toolchain)}


def stable_toolchain_identity(image: Mapping[str, Any], binaries: Mapping[str, Any]) -> dict[str, Any]:
    """Return a stable compiler/image content identity, never an env tar hash."""
    product_image = _validate_product({"image": dict(image),
                                       "toolchain": {"sha256": "a" * 64, "bytes": 1}},
                                      "compiler_identity")["image"]
    if not isinstance(binaries, Mapping) or not binaries:
        raise ReferenceWitnessError("compiler_identity:binaries_missing")
    normalized = {}
    for name, digest in sorted(binaries.items()):
        if not isinstance(name, str) or not name or not isinstance(digest, str):
            raise ReferenceWitnessError("compiler_identity:binaries_invalid")
        normalized[name] = _digest(digest, f"compiler_identity.binaries.{name}")
    content = {"image": product_image, "binaries": normalized}
    raw = _canonical(content)
    return {"sha256": hashlib.sha256(raw).hexdigest(), "bytes": len(raw),
            "content": content}


def _validate_cell_attestation(cell_output: Path, experiment: Mapping[str, Any],
                               results_raw: bytes, batch: Path, plan: Path,
                               rows: Sequence[Mapping[str, Any]],
                               remote_objects: Sequence[Path]) -> dict[str, Any]:
    """Require the finalized PASS/evidence/timing identity before minting."""
    evidence_path = cell_output / "evidence.json"
    evidence = _json(evidence_path, "cell.evidence")
    if evidence.get("schema") != "icecream-s7-live-evidence-v2":
        raise ReferenceWitnessError("cell.evidence:schema_invalid")
    if evidence.get("cell") != experiment.get("cell"):
        raise ReferenceWitnessError("cell.evidence:cell_mismatch")
    declared_hash = evidence.get("evidence_sha256")
    clean = dict(evidence); clean.pop("evidence_sha256", None)
    if declared_hash != hashlib.sha256(_canonical(clean)).hexdigest():
        raise ReferenceWitnessError("cell.evidence:self_digest_mismatch")
    for field in ("source_commit", "source_tree"):
        value = evidence.get(field)
        if (not isinstance(value, str) or len(value) != 40 or
                any(ch not in HEX64 for ch in value.lower())):
            raise ReferenceWitnessError(f"cell.evidence:{field}_invalid")
        if experiment.get(field) != value:
            raise ReferenceWitnessError(f"cell.evidence:{field}_mismatch")
    runner = evidence.get("runner")
    if (not isinstance(runner, Mapping) or runner.get("name") != "p50compilee2e-run.sh" or
            not isinstance(runner.get("sha256"), str) or
            len(runner["sha256"]) != 64):
        raise ReferenceWitnessError("cell.evidence:runner_invalid")
    _digest(runner["sha256"], "cell.evidence.runner.sha256")
    if experiment.get("runner_sha256") != runner["sha256"]:
        raise ReferenceWitnessError("cell.evidence:runner_mismatch")
    if experiment.get("remote_compile_required") is not True:
        raise ReferenceWitnessError("cell.evidence:remote_compile_required")
    if experiment.get("execution_environment") != "external_farm_product_build":
        raise ReferenceWitnessError("cell.evidence:execution_environment_invalid")
    if (not isinstance(evidence.get("runtime_image"), Mapping) or
            not isinstance(experiment.get("runtime_image"), Mapping) or
            evidence.get("runtime_image") != experiment.get("runtime_image") or
            evidence.get("binary_sha256") != experiment.get("binary_sha256")):
        raise ReferenceWitnessError("cell.evidence:compiler_identity_mismatch")
    input_desc = evidence.get("input_manifest")
    expected_batch = descriptor(batch, "cell.batch_manifest")
    if (not isinstance(input_desc, Mapping) or input_desc.get("path") != "product-evidence/batch-manifest.jsonl" or
            input_desc.get("sha256") != expected_batch["sha256"] or input_desc.get("bytes") != expected_batch["bytes"]):
        raise ReferenceWitnessError("cell.evidence:input_manifest_mismatch")
    plan_desc = evidence.get("predictive_plan")
    expected_plan = descriptor(plan, "cell.predictive_plan")
    if (not isinstance(plan_desc, Mapping) or plan_desc.get("path") != "product-evidence/predictive-plan.json" or
            plan_desc.get("sha256") != expected_plan["sha256"] or plan_desc.get("bytes") != expected_plan["bytes"]):
        raise ReferenceWitnessError("cell.evidence:predictive_plan_mismatch")
    result_desc = evidence.get("evidence", {}).get("results") if isinstance(evidence.get("evidence"), Mapping) else None
    actual_result = hashlib.sha256(results_raw).hexdigest()
    if (not isinstance(result_desc, Mapping) or result_desc.get("path") != "results.jsonl" or
            result_desc.get("sha256") != actual_result or result_desc.get("bytes") != len(results_raw)):
        raise ReferenceWitnessError("cell.evidence:results_mismatch")
    timing_desc = evidence.get("evidence", {}).get("timing") if isinstance(evidence.get("evidence"), Mapping) else None
    timing_path = cell_output / "timing.jsonl"
    timing_raw, timing_sha, timing_bytes = snapshot(timing_path, "cell.timing")
    if (not isinstance(timing_desc, Mapping) or timing_desc.get("path") != "timing.jsonl" or
            timing_desc.get("sha256") != timing_sha or timing_desc.get("bytes") != timing_bytes):
        raise ReferenceWitnessError("cell.evidence:timing_mismatch")
    if experiment.get("artifact_retention", {}).get("mode") != "all":
        raise ReferenceWitnessError("cell:all_artifacts_required")
    try:
        timing_rows = [json.loads(line) for line in timing_raw.decode("utf-8").splitlines() if line.strip()]
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ReferenceWitnessError("cell.timing:invalid_jsonl") from exc
    if len(timing_rows) != len(rows):
        raise ReferenceWitnessError("cell.timing:occurrence_count_mismatch")
    for ordinal, (timing, remote) in enumerate(zip(timing_rows, remote_objects, strict=True)):
        if (not isinstance(timing, Mapping) or timing.get("ordinal") != ordinal or
                timing.get("tu_id") != rows[ordinal].get("tu_id") or
                timing.get("cell") != experiment.get("cell") or
                timing.get("remote_compile") is not True):
            raise ReferenceWitnessError(f"cell.timing:{ordinal}:identity_invalid")
        _raw, digest, size = snapshot(remote, f"cell.remote_object:{ordinal}")
        if timing.get("object_sha256") != digest or timing.get("returned_object_bytes", size) != size:
            raise ReferenceWitnessError(f"cell.timing:{ordinal}:object_mismatch")
    return evidence


def _read_rows(path: Path) -> list[dict[str, Any]]:
    raw, _digest_value, _size = snapshot(path, "batch_manifest")
    try:
        values = [json.loads(line) for line in raw.decode("utf-8").splitlines() if line.strip()]
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ReferenceWitnessError("batch_manifest:invalid_jsonl") from exc
    if not values or any(not isinstance(item, dict) for item in values):
        raise ReferenceWitnessError("batch_manifest:rows_invalid")
    return values


def _load_batch_rows(path: Path, expected_count: int) -> list[dict[str, Any]]:
    """Use the live runner's canonical batch validator before witness work."""
    try:
        try:
            from . import s8_real_c1f1_live_runner as live  # type: ignore
        except ImportError:  # pragma: no cover - direct script invocation
            import s8_real_c1f1_live_runner as live  # type: ignore
        return live.load_batch_manifest(path, expected_count)
    except Exception as exc:
        if isinstance(exc, ReferenceWitnessError):
            raise
        raise ReferenceWitnessError(f"batch_manifest:canonical_validation_failed:{exc}") from exc


def _plan_identity(path: Path) -> tuple[dict[str, Any], dict[str, Any]]:
    value = _json(path, "predictive_plan")
    if not isinstance(value, dict) or value.get("schema") != PLAN_SCHEMA:
        raise ReferenceWitnessError("predictive_plan:schema_invalid")
    source = value.get("source_manifest")
    if not isinstance(source, Mapping) or not isinstance(source.get("path"), str):
        raise ReferenceWitnessError("predictive_plan:source_manifest_missing")
    source_path = Path(source["path"])
    source_desc = descriptor(source_path, "source_manifest")
    if (source_desc["sha256"] != _digest(source.get("sha256"), "source_manifest.sha256") or
            source_desc["bytes"] != source.get("bytes")):
        raise ReferenceWitnessError("source_manifest:descriptor_mismatch")
    return value, source_desc


def _package_lines(path: Path) -> tuple[Path, dict[str, Any], list[dict[str, Any]]]:
    manifest = path.resolve()
    package = manifest.parent
    raw, _digest_value, _size = snapshot(manifest, "witness_manifest")
    try:
        values = [json.loads(line) for line in raw.decode("utf-8").splitlines() if line.strip()]
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ReferenceWitnessError("witness_manifest:invalid_jsonl") from exc
    if len(values) < 2 or not isinstance(values[0], dict) or values[0].get("kind") != "package":
        raise ReferenceWitnessError("witness_manifest:package_header_missing")
    header = values[0]
    if header.get("schema") != PACKAGE_SCHEMA or header.get("terminal_status") != "PASS":
        raise ReferenceWitnessError("witness_package:terminal_pass_required")
    records = values[1:]
    if (not all(isinstance(item, dict) and item.get("kind") == "occurrence" for item in records) or
            len(records) != header.get("record_count")):
        raise ReferenceWitnessError("witness_manifest:occurrence_count_invalid")
    return package, header, records


def _package_digest(package: Path, header: Mapping[str, Any], records: Sequence[Mapping[str, Any]]) -> str:
    clean_header = dict(header); clean_header.pop("package_digest", None)
    chunks = [_canonical(clean_header) + b"\n"]
    for record in records:
        clean = dict(record); clean.pop("witness_package_digest", None)
        output = clean.get("direct_reference")
        if not isinstance(output, Mapping):
            raise ReferenceWitnessError("witness_record:direct_reference_missing")
        object_path, object_raw = _check_descriptor(output, "witness_record.direct_reference", package=package)
        chunks.extend((_canonical(clean) + b"\n", object_raw))
    return hashlib.sha256(b"".join(chunks)).hexdigest()


def _validate_records(package: Path, header: Mapping[str, Any], records: Sequence[Mapping[str, Any]]) -> None:
    package_digest = _digest(header.get("package_digest"), "witness_package.package_digest")
    if _package_digest(package, header, records) != package_digest:
        raise ReferenceWitnessError("witness_package:package_digest_mismatch")
    ordinals: list[int] = []
    tu_ids: set[str] = set()
    for index, record in enumerate(records):
        occurrence = record.get("occurrence")
        if (not isinstance(occurrence, Mapping) or occurrence.get("ordinal") != index or
                not isinstance(occurrence.get("tu_id"), str) or
                occurrence["tu_id"] in tu_ids or
                not isinstance(occurrence.get("source_relative"), str) or
                not isinstance(occurrence.get("predictive_source_relative"), str)):
            raise ReferenceWitnessError(f"witness_record:{index}:ordinal_invalid")
        ordinals.append(index)
        tu_ids.add(occurrence["tu_id"])
        direct = record.get("direct_reference")
        remote = record.get("remote_object")
        if (not isinstance(direct, Mapping) or not isinstance(remote, Mapping) or
                direct.get("sha256") != remote.get("sha256") or direct.get("bytes") != remote.get("bytes")):
            raise ReferenceWitnessError(f"witness_record:{index}:direct_remote_not_identical")
        _check_descriptor(direct, f"witness_record:{index}.direct_reference", package=package)
        _digest(record.get("witness_package_digest"), f"witness_record:{index}.witness_package_digest")
        if record["witness_package_digest"] != package_digest:
            raise ReferenceWitnessError(f"witness_record:{index}:package_digest_mismatch")


def validate_reuse(package_manifest: Path, *, batch_manifest: Path, predictive_plan: Path,
                   authority: Path, image_identity: Mapping[str, Any] | None = None,
                   toolchain: Path | None = None,
                   toolchain_identity: Mapping[str, Any] | None = None) -> dict[str, Any]:
    """Validate a package against current paths and return a shell-safe map."""
    package, header, records = _package_lines(package_manifest)
    _validate_records(package, header, records)
    rows = _load_batch_rows(batch_manifest, len(records))
    plan, source_desc = _plan_identity(predictive_plan)
    authority_value = _json(authority, "authority")
    if not isinstance(authority_value, dict):
        raise ReferenceWitnessError("authority:object_required")
    current_depth = plan.get("request", {}).get("depth")
    expected_depth = header.get("cell", {}).get("depth")
    if isinstance(expected_depth, str) and expected_depth.isdigit():
        expected_depth = int(expected_depth)
    current_header_cell = {"corpus": plan.get("cell", {}).get("corpus"),
                           "profile": plan.get("cell", {}).get("profile"),
                           "regime": plan.get("cell", {}).get("regime"),
                           "depth": current_depth}
    expected_cell = header.get("cell")
    if not isinstance(expected_cell, Mapping):
        raise ReferenceWitnessError("reuse:cell_identity_missing")
    for field in ("corpus", "profile", "regime", "depth"):
        expected_value = expected_depth if field == "depth" else expected_cell.get(field)
        if expected_value != current_header_cell.get(field):
            raise ReferenceWitnessError(f"reuse:cell_{field}_mismatch")
    batch_desc = descriptor(batch_manifest, "batch_manifest")
    expected_batch = header.get("batch_manifest")
    if not isinstance(expected_batch, Mapping) or expected_batch.get("sha256") != batch_desc["sha256"] or expected_batch.get("bytes") != batch_desc["bytes"]:
        raise ReferenceWitnessError("reuse:batch_manifest_mismatch")
    expected_plan = header.get("predictive_plan")
    plan_desc = descriptor(predictive_plan, "predictive_plan")
    if not isinstance(expected_plan, Mapping) or expected_plan.get("sha256") != plan_desc["sha256"] or expected_plan.get("bytes") != plan_desc["bytes"]:
        raise ReferenceWitnessError("reuse:predictive_plan_mismatch")
    expected_source = header.get("source_manifest")
    if not isinstance(expected_source, Mapping) or expected_source.get("sha256") != source_desc["sha256"] or expected_source.get("bytes") != source_desc["bytes"]:
        raise ReferenceWitnessError("reuse:source_manifest_mismatch")
    authority_desc = descriptor(authority, "authority")
    expected_authority = header.get("authority")
    if not isinstance(expected_authority, Mapping) or expected_authority.get("identity_sha256") != _authority_identity(authority_value):
        raise ReferenceWitnessError("reuse:authority_identity_mismatch")
    bindings = _compile_bindings(rows, "reuse")
    if len(bindings) != len(records):
        raise ReferenceWitnessError("reuse:occurrence_count_mismatch")
    for index, (binding, record) in enumerate(zip(bindings, records, strict=True)):
        if record.get("occurrence") != binding["occurrence"] or record.get("input") != binding["input"] or record.get("compile") != binding["compile"]:
            raise ReferenceWitnessError(f"reuse:{index}:bound_input_or_command_mismatch")
    expected_product = _validate_product(header.get("product"), "reuse.product")
    if image_identity is not None and expected_product.get("image") != dict(image_identity):
        raise ReferenceWitnessError("reuse:compiler_image_mismatch")
    if toolchain is not None:
        raise ReferenceWitnessError("reuse:archive_toolchain_identity_unsupported")
    if toolchain_identity is not None:
        if (not isinstance(toolchain_identity.get("sha256"), str) or
                type(toolchain_identity.get("bytes")) is not int or
                expected_product.get("toolchain", {}).get("sha256") != toolchain_identity["sha256"] or
                expected_product.get("toolchain", {}).get("bytes") != toolchain_identity["bytes"] or
                ("content" in expected_product.get("toolchain", {}) and
                 "content" in toolchain_identity and
                 expected_product["toolchain"].get("content") != toolchain_identity.get("content"))):
            raise ReferenceWitnessError("reuse:toolchain_mismatch")
    records_out = []
    for record in records:
        direct = record["direct_reference"]
        object_path = (package / str(direct["path"])).resolve()
        object_path.relative_to(package.resolve())
        records_out.append({"ordinal": record["occurrence"]["ordinal"],
                            "object_path": str(object_path),
                            "sha256": direct["sha256"], "bytes": direct["bytes"]})
    return {"schema": SCHEMA, "mode": "reuse", "package_digest": header["package_digest"],
            "authority": authority_desc, "records": records_out}


def validate_remote_objects(reuse_plan: Mapping[str, Any],
                            remote_objects: Sequence[Path]) -> list[dict[str, Any]]:
    """Compare every newly returned object to the validated witness map."""
    records = reuse_plan.get("records")
    if not isinstance(records, list) or len(records) != len(remote_objects):
        raise ReferenceWitnessError("reuse:remote_occurrence_count_mismatch")
    evidence: list[dict[str, Any]] = []
    for index, (record, path) in enumerate(zip(records, remote_objects, strict=True)):
        if not isinstance(record, Mapping) or record.get("ordinal") != index:
            raise ReferenceWitnessError(f"reuse:remote_ordinal_invalid:{index}")
        _raw, digest, size = snapshot(Path(path), f"reuse.remote_object:{index}")
        if digest != record.get("sha256") or size != record.get("bytes"):
            raise ReferenceWitnessError(f"reuse:remote_object_mismatch:{index}")
        evidence.append({"ordinal": index, "package_digest": reuse_plan.get("package_digest"),
                         "remote_sha256": digest, "remote_bytes": size,
                         "witness_sha256": record.get("sha256"),
                         "witness_bytes": record.get("bytes"), "status": "PASS"})
    return evidence


def package_files(package_manifest: Path) -> list[Path]:
    """Return the manifest and retained objects that an external transport stages."""
    package, _header, records = _package_lines(package_manifest)
    _validate_records(package, _header, records)
    return [package / "manifest.jsonl"] + [
        package / str(record["direct_reference"]["path"]) for record in records
    ]


_VALIDATED_CELL_TOKEN = object()


@dataclass(frozen=True)
class _ValidatedCellPackage:
    """Private hand-off from the authenticated cell gate to the byte writer."""

    token: object
    package_dir: Path
    cell: Mapping[str, Any]
    batch_manifest: Path
    predictive_plan: Path
    authority: Path
    product: Mapping[str, Any]
    rows: Sequence[Mapping[str, Any]]
    direct_objects: Sequence[Path]
    remote_objects: Sequence[Path]
    batch_descriptor: Mapping[str, Any]
    plan_descriptor: Mapping[str, Any]
    source_descriptor: Mapping[str, Any]
    authority_descriptor: Mapping[str, Any]


def _write_validated_package(package: _ValidatedCellPackage) -> Path:
    """Atomically serialize a package after the retained-cell attestation."""
    if package.token is not _VALIDATED_CELL_TOKEN:
        raise ReferenceWitnessError("witness_package:authenticated_cell_required")
    package_dir = package.package_dir
    if package_dir.exists() or package_dir.is_symlink() or not package_dir.is_absolute():
        raise ReferenceWitnessError("witness_package:output_must_be_new_absolute_directory")
    if (len(package.rows) != len(package.direct_objects) or
            len(package.rows) != len(package.remote_objects)):
        raise ReferenceWitnessError("witness_package:occurrence_count_mismatch")
    product_identity = _validate_product(package.product, "witness_package.product")
    bindings = _compile_bindings(package.rows, "witness")
    package_parent = package_dir.parent
    package_parent.mkdir(parents=True, exist_ok=True)
    temporary = Path(tempfile.mkdtemp(prefix=f".{package_dir.name}.", dir=package_parent))
    published = False
    try:
        objects = temporary / "objects"; objects.mkdir()
        records: list[dict[str, Any]] = []
        for index, (binding, direct_path, remote_path) in enumerate(zip(
                bindings, package.direct_objects, package.remote_objects, strict=True)):
            direct_raw, direct_sha, direct_bytes = snapshot(Path(direct_path), f"direct_reference:{index}")
            remote_raw, remote_sha, remote_bytes = snapshot(Path(remote_path), f"remote_object:{index}")
            if direct_raw != remote_raw or direct_sha != remote_sha or direct_bytes != remote_bytes:
                raise ReferenceWitnessError(f"witness:{index}:direct_remote_not_identical")
            destination = objects / f"{index}.o"
            destination.write_bytes(direct_raw)
            destination_desc = {"path": f"objects/{index}.o", "sha256": direct_sha, "bytes": direct_bytes}
            records.append({"kind": "occurrence", **binding, "direct_reference": destination_desc,
                            "remote_object": dict(destination_desc)})
        # PASS is an invariant of _ValidatedCellPackage, which can only be
        # minted after create_from_cell authenticates the retained cell.
        header: dict[str, Any] = {"kind": "package", "schema": PACKAGE_SCHEMA,
                                  "terminal_status": "PASS", "cell": dict(package.cell),
                                  "batch_manifest": dict(package.batch_descriptor),
                                  "predictive_plan": dict(package.plan_descriptor),
                                  "source_manifest": dict(package.source_descriptor),
                                  "authority": dict(package.authority_descriptor),
                                  "product": json.loads(json.dumps(product_identity)),
                                  "record_count": len(records)}
        package_digest = _package_digest(temporary, header, records)
        header["package_digest"] = package_digest
        for record in records:
            record["witness_package_digest"] = package_digest
        manifest = temporary / "manifest.jsonl"
        manifest.write_bytes(b"".join(_canonical(item) + b"\n" for item in [header, *records]))
        os.replace(temporary, package_dir)
        published = True
    finally:
        if not published:
            shutil.rmtree(temporary, ignore_errors=True)
    return package_dir / "manifest.jsonl"


def create_package(*_args: Any, **_kwargs: Any) -> Path:
    """Reject the removed raw packaging API.

    New packages must come from ``create_from_cell`` so terminal PASS and all
    retained evidence are authenticated before any bytes are published.
    """
    raise ReferenceWitnessError("witness_package:authenticated_cell_required")


def create_from_cell(cell_output: Path, *, authority: Path, package_dir: Path) -> Path:
    """Collect a reusable package from a retained PASS cell.

    The cell must have been finalized with ``retain_all_artifacts`` so every
    full-1 remote/local pair is available.  This function intentionally does
    not infer missing objects or rerun a compiler.
    """
    cell_output = cell_output.resolve()
    experiment_path = cell_output / "experiment_manifest.json"
    experiment = _json(experiment_path, "cell.experiment_manifest")
    results_path = cell_output / "results.jsonl"
    results_raw, _results_sha, _results_bytes = snapshot(results_path, "cell.results")
    results = _read_rows(results_path)
    if len(results) != 1 or results[0].get("status") != "PASS":
        raise ReferenceWitnessError("cell:terminal_pass_required")
    cell = experiment.get("cell") if isinstance(experiment, Mapping) else None
    if not isinstance(cell, Mapping):
        raise ReferenceWitnessError("cell:identity_missing")
    product_image = experiment.get("runtime_image")
    if not isinstance(product_image, Mapping):
        raise ReferenceWitnessError("cell:compiler_image_missing")
    batch = cell_output / "product-evidence" / "batch-manifest.jsonl"
    plan = cell_output / "product-evidence" / "predictive-plan.json"
    rows = _load_batch_rows(batch, len(_read_rows(batch)))
    count = len(rows)
    direct: list[Path] = []
    remote: list[Path] = []
    for ordinal in range(count):
        direct_path = cell_output / "product-evidence" / f"local-full-1-{ordinal}.o"
        remote_path = cell_output / "product-evidence" / f"remote-full-1-{ordinal}.o"
        direct.append(direct_path); remote.append(remote_path)
        snapshot(direct_path, f"cell.local_object:{ordinal}")
        snapshot(remote_path, f"cell.remote_object:{ordinal}")
    _validate_cell_attestation(cell_output, experiment, results_raw, batch, plan, rows, remote)
    binaries = experiment.get("binary_sha256")
    if not isinstance(product_image, Mapping) or not isinstance(binaries, Mapping):
        raise ReferenceWitnessError("cell:compiler_identity_missing")
    toolchain_identity = stable_toolchain_identity(product_image, binaries)
    depth = experiment.get("depth")
    if not isinstance(depth, (str, int)):
        raise ReferenceWitnessError("cell:depth_missing")
    authority_value = _json(authority, "authority")
    if not isinstance(authority_value, dict):
        raise ReferenceWitnessError("authority:object_required")
    validated = _ValidatedCellPackage(
        token=_VALIDATED_CELL_TOKEN,
        package_dir=package_dir,
        cell={**dict(cell), "depth": depth, "topology": experiment.get("topology")},
        batch_manifest=batch,
        predictive_plan=plan,
        authority=authority,
        product={"image": dict(product_image), "toolchain": dict(toolchain_identity)},
        rows=rows,
        direct_objects=direct,
        remote_objects=remote,
        batch_descriptor=descriptor(batch, "batch_manifest"),
        plan_descriptor=descriptor(plan, "predictive_plan"),
        source_descriptor=_plan_identity(plan)[1],
        authority_descriptor={**descriptor(authority, "authority"),
                             "identity_sha256": _authority_identity(authority_value)},
    )
    return _write_validated_package(validated)


def _cli() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    verify = sub.add_parser("verify")
    verify.add_argument("--package", type=Path, required=True)
    verify.add_argument("--batch-manifest", type=Path, required=True)
    verify.add_argument("--predictive-plan", type=Path, required=True)
    verify.add_argument("--authority", type=Path, required=True)
    verify.add_argument("--image-id", required=True)
    verify.add_argument("--image-reference", required=True)
    verify.add_argument("--image-architecture", required=True)
    verify.add_argument("--image-os", required=True)
    verify.add_argument("--image-created", required=True)
    verify.add_argument("--toolchain", type=Path,
                        help="deprecated archive identity; rejected for reuse")
    verify.add_argument("--toolchain-sha256")
    verify.add_argument("--toolchain-bytes", type=int)
    verify.add_argument("--output", type=Path, required=True)
    create = sub.add_parser("create")
    create.add_argument("--cell-output", type=Path, required=True)
    create.add_argument("--authority", type=Path, required=True)
    create.add_argument("--package-dir", type=Path, required=True)
    args = parser.parse_args()
    try:
        if args.command == "verify":
            if (args.toolchain is not None or args.toolchain_sha256 is None or
                    args.toolchain_bytes is None):
                raise ReferenceWitnessError("reuse:stable_toolchain_identity_required")
            result = validate_reuse(args.package, batch_manifest=args.batch_manifest,
                                    predictive_plan=args.predictive_plan, authority=args.authority,
                                    image_identity={"reference": args.image_reference,
                                                    "image_id": args.image_id,
                                                    "architecture": args.image_architecture,
                                                    "os": args.image_os,
                                                    "created": args.image_created},
                                    toolchain=args.toolchain,
                                    toolchain_identity=({"sha256": args.toolchain_sha256,
                                                         "bytes": args.toolchain_bytes}
                                                        if args.toolchain_sha256 is not None else None))
            args.output.write_bytes(_canonical(result) + b"\n")
            return 0
        if args.command == "create":
            print(create_from_cell(args.cell_output, authority=args.authority,
                                   package_dir=args.package_dir))
            return 0
    except (ReferenceWitnessError, OSError) as exc:
        print(f"REFERENCE_WITNESS_FAIL {exc}", file=os.sys.stderr)
        return 1
    return 2


if __name__ == "__main__":
    raise SystemExit(_cli())
