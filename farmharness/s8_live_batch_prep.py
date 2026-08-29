#!/usr/bin/env python3
"""Prepare an authenticated real-compiler batch from one S8 depth plan.

The retained predictive corpus contains preprocessed ``.ii`` files while the
live product must compile the corresponding original source with its exact
compile-database entry.  This tool joins those two authorities by the CMake
output-relative path and emits a private, immutable batch package.  Duplicate
source files in different CMake targets remain distinct through their exact
compile-output identity.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any

try:
    from . import s8_real_c1f1_live_runner as live_runner
except ImportError:  # pragma: no cover
    import s8_real_c1f1_live_runner as live_runner


SCHEMA = "icecream-s8-live-batch-prep-v1"
TOPOLOGY_SCHEMA = "icecream-s8-topology-assignment-v1"


class BatchPrepError(ValueError):
    """A predictive input cannot be bound to one exact compile command."""


def _fail(reason: str) -> None:
    raise BatchPrepError(reason)


def _canonical(value: object) -> bytes:
    return live_runner._canonical(value)


def _descriptor(path: Path) -> dict[str, object]:
    digest, size = live_runner._sha(path)
    return {"path": str(path.resolve()), "sha256": digest, "bytes": size}


def _inside(path: Path, root: Path, label: str) -> Path:
    try:
        relative = path.resolve().relative_to(root.resolve())
    except ValueError as exc:
        raise BatchPrepError(f"{label}:outside_root") from exc
    if not relative.parts or any(part in ("", ".", "..") for part in relative.parts):
        _fail(f"{label}:relative_invalid")
    return relative


def _plan_identity(path: Path) -> tuple[dict[str, Any], list[dict[str, Any]], str]:
    try:
        value = json.loads(path.read_text())
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise BatchPrepError("predictive_plan:invalid_json") from exc
    if not isinstance(value, dict) or not isinstance(value.get("cell"), dict):
        _fail("predictive_plan:identity_invalid")
    cell = value["cell"]
    request = value.get("request")
    if not isinstance(request, dict):
        _fail("predictive_plan:request_invalid")
    depth_value = request.get("depth")
    if depth_value in (100, 200):
        depth = str(depth_value)
    elif depth_value in ("full", "repeat-full"):
        depth = "full"
    else:
        _fail("predictive_plan:depth_invalid")
    try:
        plan, inputs, plan_sha = live_runner.load_predictive_plan(
            path, corpus=str(cell.get("corpus")), profile=str(cell.get("profile")),
            regime=str(cell.get("regime")), depth=depth)
    except live_runner.LiveRunnerError as exc:
        raise BatchPrepError(str(exc)) from exc
    return plan, inputs, plan_sha


def _compile_map(path: Path, output_root: Path) -> tuple[list[dict[str, Any]], dict[str, dict[str, Any]]]:
    live_runner._sha(path)
    try:
        value = json.loads(path.read_text())
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise BatchPrepError("compile_db:invalid_json") from exc
    if not isinstance(value, list) or not value:
        _fail("compile_db:list_required")
    result: dict[str, dict[str, Any]] = {}
    for index, entry in enumerate(value):
        if not isinstance(entry, dict):
            continue
        directory = entry.get("directory")
        source = entry.get("file")
        output = entry.get("output")
        command = entry.get("command")
        if not all(isinstance(item, str) and item for item in (directory, source, output, command)):
            continue
        directory_path = Path(directory)
        source_path = Path(source)
        output_path = Path(output)
        if not directory_path.is_absolute() or not source_path.is_absolute():
            continue
        if not output_path.is_absolute():
            output_path = directory_path / output_path
        try:
            output_relative = _inside(output_path, output_root, f"compile_db:{index}.output")
        except BatchPrepError:
            continue
        if output_relative.suffix != ".o":
            continue
        predictive_relative = output_relative.with_suffix(".ii").as_posix()
        if predictive_relative in result:
            _fail(f"compile_db:duplicate_output:{predictive_relative}")
        normalized = dict(entry)
        normalized["directory"] = str(directory_path.resolve())
        normalized["file"] = str(source_path.resolve())
        normalized["output"] = str(output_path.resolve())
        result[predictive_relative] = normalized
    return value, result


def prepare(*, predictive_plan: Path, compile_db: Path, compile_output_root: Path,
            compile_source_root: Path, output: Path) -> Path:
    """Emit one new C1F1 batch package and return its directory."""
    for label, root in (("compile_output_root", compile_output_root),
                        ("compile_source_root", compile_source_root)):
        if not root.is_absolute() or root.is_symlink() or not root.is_dir():
            _fail(f"{label}:unavailable")
    if not output.is_absolute() or output.exists() or output.is_symlink():
        _fail("output:must_be_new_absolute_directory")
    plan, inputs, plan_sha = _plan_identity(predictive_plan)
    _all_entries, compile_entries = _compile_map(compile_db, compile_output_root)
    selected: list[dict[str, Any]] = []
    for ordinal, item in enumerate(inputs):
        entry = compile_entries.get(item["source_relative"])
        if entry is None:
            _fail(f"compile_db:input_unmatched:{ordinal}:{item['source_relative']}")
        source_path = Path(entry["file"])
        live_runner._sha(source_path)
        _inside(source_path, compile_source_root, f"compile_source:{ordinal}")
        selected.append(entry)

    output.mkdir(parents=True, exist_ok=False)
    selected_db = output / "selected-compile-commands.json"
    live_runner._write_new(selected_db, _canonical(selected) + b"\n")
    selected_db_descriptor = _descriptor(selected_db)

    rows: list[dict[str, Any]] = []
    assignments: list[dict[str, Any]] = []
    cell = plan["cell"]
    for ordinal, (item, entry) in enumerate(zip(inputs, selected, strict=True)):
        source_path = Path(entry["file"]).resolve()
        source_sha, _ = live_runner._sha(source_path)
        source_relative = _inside(source_path, compile_source_root,
                                  f"compile_source:{ordinal}").as_posix()
        tu_id = (f"{cell['corpus']}-{cell['regime']}-tu-{ordinal:06d}-"
                 f"{str(item['sha256'])[:12]}")
        row = {"tu_id": tu_id, "source": str(source_path),
               "source_relative": source_relative, "sha256": source_sha,
               "predictive_input": dict(item),
               "compile_db": str(selected_db.resolve()),
               "compile_db_sha256": selected_db_descriptor["sha256"],
               "compile_source": str(source_path),
               "compile_output": str(Path(entry["output"]).resolve())}
        rows.append(row)
        assignments.append({"ordinal": ordinal, "tu_id": tu_id,
                            "relationship": 0, "f_slot": 0})

    batch = output / "batch-manifest.jsonl"
    topology = output / "topology.json"
    batch_raw = b"".join(_canonical(row) + b"\n" for row in rows)
    topology_value = {"schema": TOPOLOGY_SCHEMA, "suite": live_runner.TOPOLOGY,
                      "assignments": assignments}
    live_runner._write_new(batch, batch_raw)
    live_runner._write_new(topology, _canonical(topology_value) + b"\n")

    # Re-open the emitted package through the consumer before declaring it.
    loaded_rows = live_runner.load_batch_manifest(batch, len(inputs))
    live_runner.bind_batch_to_plan(loaded_rows, inputs)
    live_runner.load_topology(topology, loaded_rows)

    manifest_value = {
        "schema": SCHEMA, "status": "READY", "cell": cell,
        "split": plan["split"], "count": len(inputs), "topology": live_runner.TOPOLOGY,
        "predictive_plan": {**_descriptor(predictive_plan), "validated_sha256": plan_sha},
        "source_compile_db": _descriptor(compile_db),
        "selected_compile_db": selected_db_descriptor,
        "batch_manifest": _descriptor(batch), "topology_manifest": _descriptor(topology),
        "compile_output_root": str(compile_output_root.resolve()),
        "compile_source_root": str(compile_source_root.resolve()),
    }
    live_runner._write_new(output / "prep-manifest.json", _canonical(manifest_value) + b"\n")
    return output


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--predictive-plan", type=Path, required=True)
    parser.add_argument("--compile-db", type=Path, required=True)
    parser.add_argument("--compile-output-root", type=Path, required=True)
    parser.add_argument("--compile-source-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        target = prepare(predictive_plan=args.predictive_plan.absolute(),
                         compile_db=args.compile_db.absolute(),
                         compile_output_root=args.compile_output_root.absolute(),
                         compile_source_root=args.compile_source_root.absolute(),
                         output=args.output.absolute())
    except (BatchPrepError, live_runner.LiveRunnerError) as exc:
        print(str(exc))
        return 77
    print(target)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
