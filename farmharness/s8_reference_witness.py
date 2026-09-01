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
    for ordinal, row in enumerate(rows):
        required = {"source", "source_relative", "sha256", "predictive_input",
                    "compile_db", "compile_db_sha256", "compile_source", "compile_output"}
        if not required.issubset(row):
            raise ReferenceWitnessError(f"{label}:{ordinal}:compile_binding_missing")
        source = Path(str(row["source"]))
        source_desc = descriptor(source, f"{label}:{ordinal}.source")
        if source_desc["sha256"] != _digest(row["sha256"], f"{label}:{ordinal}.source_sha256"):
            raise ReferenceWitnessError(f"{label}:{ordinal}:source_digest_mismatch")
        payload = row["predictive_input"]
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
        result.append({"occurrence": {"ordinal": ordinal,
                                       "source_relative": row["source_relative"],
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
    _digest(image_id.removeprefix("sha256:"), f"{label}.image.image_id")
    if (not isinstance(toolchain, Mapping) or set(toolchain) != {"sha256", "bytes"} or
            type(toolchain.get("bytes")) is not int or toolchain["bytes"] <= 0):
        raise ReferenceWitnessError(f"{label}.toolchain:identity_invalid")
    _digest(toolchain.get("sha256"), f"{label}.toolchain.sha256")
    return {"image": dict(image), "toolchain": dict(toolchain)}


def _read_rows(path: Path) -> list[dict[str, Any]]:
    raw, _digest_value, _size = snapshot(path, "batch_manifest")
    try:
        values = [json.loads(line) for line in raw.decode("utf-8").splitlines() if line.strip()]
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ReferenceWitnessError("batch_manifest:invalid_jsonl") from exc
    if not values or any(not isinstance(item, dict) for item in values):
        raise ReferenceWitnessError("batch_manifest:rows_invalid")
    return values


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
    for index, record in enumerate(records):
        occurrence = record.get("occurrence")
        if not isinstance(occurrence, Mapping) or occurrence.get("ordinal") != index:
            raise ReferenceWitnessError(f"witness_record:{index}:ordinal_invalid")
        ordinals.append(index)
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
                   toolchain: Path | None = None) -> dict[str, Any]:
    """Validate a package against current paths and return a shell-safe map."""
    package, header, records = _package_lines(package_manifest)
    _validate_records(package, header, records)
    rows = _read_rows(batch_manifest)
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
        toolchain_desc = descriptor(toolchain, "toolchain")
        if expected_product.get("toolchain") != {"sha256": toolchain_desc["sha256"], "bytes": toolchain_desc["bytes"]}:
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


def create_package(package_dir: Path, *, cell: Mapping[str, Any], batch_manifest: Path,
                   predictive_plan: Path, authority: Path, product: Mapping[str, Any],
                   rows: Sequence[Mapping[str, Any]], direct_objects: Sequence[Path],
                   remote_objects: Sequence[Path]) -> Path:
    """Create a new package only from a terminal PASS direct/remote pair."""
    if package_dir.exists() or package_dir.is_symlink() or not package_dir.is_absolute():
        raise ReferenceWitnessError("witness_package:output_must_be_new_absolute_directory")
    if len(rows) != len(direct_objects) or len(rows) != len(remote_objects):
        raise ReferenceWitnessError("witness_package:occurrence_count_mismatch")
    plan, source_desc = _plan_identity(predictive_plan)
    batch_desc = descriptor(batch_manifest, "batch_manifest")
    plan_desc = descriptor(predictive_plan, "predictive_plan")
    authority_value = _json(authority, "authority")
    if not isinstance(authority_value, dict):
        raise ReferenceWitnessError("authority:object_required")
    product_identity = _validate_product(product, "witness_package.product")
    bindings = _compile_bindings(rows, "witness")
    package_parent = package_dir.parent
    package_parent.mkdir(parents=True, exist_ok=True)
    temporary = Path(tempfile.mkdtemp(prefix=f".{package_dir.name}.", dir=package_parent))
    objects = temporary / "objects"; objects.mkdir()
    records: list[dict[str, Any]] = []
    for index, (binding, direct_path, remote_path) in enumerate(zip(bindings, direct_objects, remote_objects, strict=True)):
        direct_raw, direct_sha, direct_bytes = snapshot(Path(direct_path), f"direct_reference:{index}")
        remote_raw, remote_sha, remote_bytes = snapshot(Path(remote_path), f"remote_object:{index}")
        if direct_raw != remote_raw or direct_sha != remote_sha or direct_bytes != remote_bytes:
            raise ReferenceWitnessError(f"witness:{index}:direct_remote_not_identical")
        destination = objects / f"{index}.o"
        destination.write_bytes(direct_raw)
        destination_desc = {"path": f"objects/{index}.o", "sha256": direct_sha, "bytes": direct_bytes}
        records.append({"kind": "occurrence", **binding, "direct_reference": destination_desc,
                        "remote_object": dict(destination_desc)})
    header: dict[str, Any] = {"kind": "package", "schema": PACKAGE_SCHEMA,
                              "terminal_status": "PASS", "cell": dict(cell),
                              "batch_manifest": batch_desc, "predictive_plan": plan_desc,
                              "source_manifest": source_desc,
                              "authority": {**descriptor(authority, "authority"),
                                            "identity_sha256": _authority_identity(authority_value)},
                              "product": json.loads(json.dumps(product_identity)),
                              "record_count": len(records)}
    package_digest = _package_digest(temporary, header, records)
    header["package_digest"] = package_digest
    for record in records:
        record["witness_package_digest"] = package_digest
    manifest = temporary / "manifest.jsonl"
    manifest.write_bytes(b"".join(_canonical(item) + b"\n" for item in [header, *records]))
    try:
        os.replace(temporary, package_dir)
    except OSError:
        shutil.rmtree(temporary, ignore_errors=True)
        raise
    return package_dir / "manifest.jsonl"


def create_from_cell(cell_output: Path, *, authority: Path, package_dir: Path) -> Path:
    """Collect a reusable package from a retained PASS cell.

    The cell must have been finalized with ``retain_all_artifacts`` so every
    full-1 remote/local pair is available.  This function intentionally does
    not infer missing objects or rerun a compiler.
    """
    cell_output = cell_output.resolve()
    experiment_path = cell_output / "experiment_manifest.json"
    experiment = _json(experiment_path, "cell.experiment_manifest")
    results = _read_rows(cell_output / "results.jsonl")
    if len(results) != 1 or results[0].get("status") != "PASS":
        raise ReferenceWitnessError("cell:terminal_pass_required")
    cell = experiment.get("cell") if isinstance(experiment, Mapping) else None
    if not isinstance(cell, Mapping):
        raise ReferenceWitnessError("cell:identity_missing")
    product_image = experiment.get("runtime_image")
    if not isinstance(product_image, Mapping):
        authority_value = _json(authority, "authority")
        try:
            product_image = authority_value["hosts"]["q3"]["image"]
        except (KeyError, TypeError) as exc:
            raise ReferenceWitnessError("cell:compiler_image_missing") from exc
    preparation = experiment.get("environment_preparation")
    if (not isinstance(preparation, Mapping) or not isinstance(preparation.get("archive_sha256"), str) or
            type(preparation.get("archive_bytes")) is not int or preparation["archive_bytes"] <= 0):
        raise ReferenceWitnessError("cell:toolchain_identity_missing")
    batch = cell_output / "product-evidence" / "batch-manifest.jsonl"
    plan = cell_output / "product-evidence" / "predictive-plan.json"
    rows = _read_rows(batch)
    count = len(rows)
    direct: list[Path] = []
    remote: list[Path] = []
    for ordinal in range(count):
        direct_path = cell_output / "product-evidence" / f"local-full-1-{ordinal}.o"
        remote_path = cell_output / "product-evidence" / f"remote-full-1-{ordinal}.o"
        direct.append(direct_path); remote.append(remote_path)
        snapshot(direct_path, f"cell.local_object:{ordinal}")
        snapshot(remote_path, f"cell.remote_object:{ordinal}")
    depth = experiment.get("depth")
    if not isinstance(depth, (str, int)):
        raise ReferenceWitnessError("cell:depth_missing")
    return create_package(
        package_dir, cell={**dict(cell), "depth": depth,
                           "topology": experiment.get("topology")},
        batch_manifest=batch, predictive_plan=plan, authority=authority,
        product={"image": dict(product_image),
                 "toolchain": {"sha256": preparation["archive_sha256"],
                               "bytes": preparation["archive_bytes"]}},
        rows=rows, direct_objects=direct, remote_objects=remote)


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
    verify.add_argument("--toolchain", type=Path, required=True)
    verify.add_argument("--output", type=Path, required=True)
    create = sub.add_parser("create")
    create.add_argument("--cell-output", type=Path, required=True)
    create.add_argument("--authority", type=Path, required=True)
    create.add_argument("--package-dir", type=Path, required=True)
    args = parser.parse_args()
    try:
        if args.command == "verify":
            result = validate_reuse(args.package, batch_manifest=args.batch_manifest,
                                    predictive_plan=args.predictive_plan, authority=args.authority,
                                    image_identity={"reference": args.image_reference,
                                                    "image_id": args.image_id,
                                                    "architecture": args.image_architecture,
                                                    "os": args.image_os,
                                                    "created": args.image_created},
                                    toolchain=args.toolchain)
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
