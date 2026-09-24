#!/usr/bin/env python3
"""Run the selected Protocol-50 formal lanes as one portable, retained gate."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path, PurePosixPath
from typing import Any


class Refusal(RuntimeError):
    """A fail-closed input or dependency refusal."""


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _write_json(path: Path, value: Any) -> None:
    temporary = path.with_name(f".{path.name}.tmp")
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")
    os.replace(temporary, path)


def _relative_file(root: Path, value: object, subject: str) -> Path:
    if not isinstance(value, str) or not value:
        raise Refusal(f"{subject} must be a nonempty relative path")
    pure = PurePosixPath(value)
    if pure.is_absolute() or any(part in {"", ".", ".."} for part in pure.parts):
        raise Refusal(f"{subject} is not a safe relative path: {value!r}")
    path = root.joinpath(*pure.parts)
    if path.is_symlink() or not path.is_file():
        raise Refusal(f"{subject} is missing, not regular, or a symlink: {value}")
    return path


def _load_manifest(path: Path) -> dict[str, Any]:
    try:
        manifest = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError) as exc:
        raise Refusal(f"cannot read aggregate manifest: {exc}") from exc
    if not isinstance(manifest, dict) or manifest.get("schema") != "icecream-p50-formal-aggregate-v1":
        raise Refusal("aggregate manifest has the wrong schema")
    default_workers = manifest.get("default_workers")
    if (
        not isinstance(default_workers, int)
        or isinstance(default_workers, bool)
        or default_workers < 1
        or default_workers > 64
    ):
        raise Refusal("aggregate manifest default_workers is outside 1..64")
    tool = manifest.get("tool")
    if not isinstance(tool, dict) or not re.fullmatch(r"[0-9a-f]{64}", str(tool.get("sha256", ""))):
        raise Refusal("aggregate manifest lacks a valid pinned tool SHA-256")
    lanes = manifest.get("lanes")
    if not isinstance(lanes, list) or not lanes:
        raise Refusal("aggregate manifest selects no lanes")
    ids = [lane.get("id") for lane in lanes if isinstance(lane, dict)]
    if len(ids) != len(lanes) or len(set(ids)) != len(ids):
        raise Refusal("aggregate lane identities are missing or duplicated")
    return manifest


def _load_selection_authority(path: Path, lane_id: str) -> dict[str, dict[str, Any]]:
    rows: dict[str, dict[str, Any]] = {}
    try:
        lines = path.read_text().splitlines()
    except OSError as exc:
        raise Refusal(f"cannot read lane {lane_id} selection authority: {exc}") from exc
    for line_number, line in enumerate(lines, 1):
        if not line.strip():
            continue
        try:
            row = json.loads(line)
        except json.JSONDecodeError as exc:
            raise Refusal(
                f"lane {lane_id} selection authority line {line_number} is invalid JSON"
            ) from exc
        row_id = row.get("id") if isinstance(row, dict) else None
        if not isinstance(row_id, str) or not row_id:
            raise Refusal(
                f"lane {lane_id} selection authority line {line_number} lacks an ID"
            )
        if row_id in rows:
            raise Refusal(f"lane {lane_id} selection authority duplicates ID {row_id}")
        rows[row_id] = row
    if not rows:
        raise Refusal(f"lane {lane_id} selection authority has no rows")
    return rows


def _authority_outcome(
    lane_id: str, row_id: str, row: dict[str, Any]
) -> tuple[str, str | None]:
    expected_exit = row.get("expected_exit")
    expected_wait = row.get("expected_wait")
    expected_phase = row.get("expected_phase")
    expected = row.get("expected")
    if (
        expected_exit == 0
        and expected_wait == "zero"
        and expected_phase == "clean"
        and isinstance(expected, str)
        and expected
    ):
        return "clean", None
    if (
        isinstance(expected_exit, int)
        and not isinstance(expected_exit, bool)
        and expected_exit != 0
        and expected_wait == "nonzero"
        and expected_phase == "invariant"
        and isinstance(expected, str)
        and expected
    ):
        return "expected-failure", expected
    if (
        isinstance(expected_exit, int)
        and not isinstance(expected_exit, bool)
        and expected_exit != 0
        and expected_wait == "nonzero"
        and expected_phase == "initial-invariant"
        and isinstance(expected, str)
        and expected.startswith("initial:")
        and expected != "initial:"
    ):
        return "expected-failure", expected.removeprefix("initial:")
    raise Refusal(
        f"lane {lane_id}/{row_id} has an unsupported selection-authority outcome"
    )


def _bind_authority_row(
    formal_root: Path,
    lane_id: str,
    row_id: str,
    declaration: dict[str, Any],
    authority: dict[str, Any],
) -> dict[str, Any]:
    module_name = declaration["module"]
    config_name = declaration["config"]
    outcome = declaration["outcome"]
    invariant = declaration.get("invariant")
    authority_outcome, authority_invariant = _authority_outcome(
        lane_id, row_id, authority
    )
    claimed = {
        "module": module_name,
        "config": config_name,
        "outcome": outcome,
        "invariant": invariant,
    }
    authoritative = {
        "module": authority.get("module"),
        "config": authority.get("config"),
        "outcome": authority_outcome,
        "invariant": authority_invariant,
    }
    for field in claimed:
        if claimed[field] != authoritative[field]:
            raise Refusal(
                f"lane {lane_id}/{row_id} {field} does not match its selection authority"
            )
    for field, name in (("module_sha256", module_name), ("config_sha256", config_name)):
        expected_digest = authority.get(field)
        if not isinstance(expected_digest, str) or not re.fullmatch(
            r"[0-9a-f]{64}", expected_digest
        ):
            raise Refusal(f"lane {lane_id}/{row_id} has an invalid authority {field}")
        if _sha256(formal_root / name) != expected_digest:
            raise Refusal(
                f"lane {lane_id}/{row_id} {field} does not match the selected file"
            )
    row_digest = authority.get("row_sha256")
    if not isinstance(row_digest, str) or not re.fullmatch(r"[0-9a-f]{64}", row_digest):
        raise Refusal(f"lane {lane_id}/{row_id} has an invalid authority row_sha256")
    calculated = hashlib.sha256(
        json.dumps(
            {key: value for key, value in authority.items() if key != "row_sha256"},
            sort_keys=True,
            separators=(",", ":"),
        ).encode()
    ).hexdigest()
    if calculated != row_digest:
        raise Refusal(f"lane {lane_id}/{row_id} authority row digest does not match")
    return authority


def _prepare_lane(formal_root: Path, lane: dict[str, Any]) -> dict[str, Any]:
    lane_id = lane.get("id")
    if not isinstance(lane_id, str) or not re.fullmatch(r"[a-z0-9][a-z0-9-]*", lane_id):
        raise Refusal("invalid aggregate lane identity")
    runner = _relative_file(formal_root, lane.get("runner"), f"lane {lane_id} runner")
    try:
        timeout_seconds = int(lane.get("timeout_seconds"))
    except (TypeError, ValueError) as exc:
        raise Refusal(f"lane {lane_id} has an invalid timeout") from exc
    if timeout_seconds < 1 or timeout_seconds > 86400:
        raise Refusal(f"lane {lane_id} timeout is outside 1..86400 seconds")
    rows = lane.get("rows")
    if not isinstance(rows, list) or not rows:
        raise Refusal(f"lane {lane_id} selects no rows")
    contract = lane.get("result_contract", "log-markers")
    if contract not in {"log-markers", "zstd-selected-jsonl"}:
        raise Refusal(f"lane {lane_id} has an unknown result contract")
    runner_text = runner.read_text(errors="replace")
    binding_text = runner_text
    prepared_rows: list[dict[str, str]] = []
    authority_bindings: dict[str, dict[str, Any]] = {}
    input_paths: dict[str, Path] = {str(runner.relative_to(formal_root)): runner}
    selection_authority_value = lane.get("selection_authority")
    selection_rows: dict[str, dict[str, Any]] | None = None
    if selection_authority_value is not None:
        selection_authority = _relative_file(
            formal_root,
            selection_authority_value,
            f"lane {lane_id} selection authority",
        )
        input_paths[str(selection_authority.relative_to(formal_root))] = selection_authority
        binding_text += "\n" + selection_authority.read_text(errors="replace")
        selection_rows = _load_selection_authority(selection_authority, lane_id)
    if contract == "zstd-selected-jsonl" and selection_rows is None:
        raise Refusal(f"lane {lane_id} requires a row selection authority")
    row_ids: set[str] = set()
    for index, raw in enumerate(rows):
        if not isinstance(raw, dict):
            raise Refusal(f"lane {lane_id} row {index} is not an object")
        row_id = raw.get("id")
        module_name = raw.get("module")
        config_name = raw.get("config")
        outcome = raw.get("outcome")
        invariant = raw.get("invariant")
        property_name = raw.get("property")
        marker = raw.get("marker")
        if not isinstance(row_id, str) or not row_id or row_id in row_ids:
            raise Refusal(f"lane {lane_id} has a missing or duplicate row identity")
        row_ids.add(row_id)
        if outcome not in {"clean", "expected-failure"}:
            raise Refusal(f"lane {lane_id}/{row_id} has an invalid outcome")
        if outcome == "expected-failure":
            has_invariant = isinstance(invariant, str) and bool(invariant)
            has_property = isinstance(property_name, str) and bool(property_name)
            if has_invariant == has_property:
                raise Refusal(
                    f"lane {lane_id}/{row_id} must select exactly one expected invariant or property"
                )
        elif invariant is not None or property_name is not None:
            raise Refusal(
                f"lane {lane_id}/{row_id} clean outcome cannot select an expected failure"
            )
        if marker is not None and (not isinstance(marker, str) or not marker):
            raise Refusal(f"lane {lane_id}/{row_id} has an invalid log marker")
        module = _relative_file(formal_root, module_name, f"lane {lane_id}/{row_id} module")
        config = _relative_file(formal_root, config_name, f"lane {lane_id}/{row_id} config")
        for selected in (module, config):
            input_paths[str(selected.relative_to(formal_root))] = selected
        if str(config_name) not in binding_text:
            raise Refusal(f"lane {lane_id}/{row_id} is not selected by its runner")
        if invariant is not None and str(invariant) not in binding_text:
            raise Refusal(f"lane {lane_id}/{row_id} invariant is not bound by its runner")
        if property_name is not None and str(property_name) not in binding_text:
            raise Refusal(f"lane {lane_id}/{row_id} property is not bound by its runner")
        prepared = {
            "id": row_id,
            "module": str(module_name),
            "config": str(config_name),
            "outcome": outcome,
            **({"invariant": invariant} if invariant is not None else {}),
            **({"property": property_name} if property_name is not None else {}),
            **({"marker": marker} if marker is not None else {}),
        }
        prepared_rows.append(prepared)
        if selection_rows is not None:
            authority = selection_rows.get(row_id)
            if authority is None:
                raise Refusal(
                    f"lane {lane_id}/{row_id} is absent from its selection authority"
                )
            authority_bindings[row_id] = _bind_authority_row(
                formal_root, lane_id, row_id, prepared, authority
            )
    extras = lane.get("extra_inputs", [])
    if not isinstance(extras, list):
        raise Refusal(f"lane {lane_id} extra_inputs is not a list")
    for index, value in enumerate(extras):
        selected = _relative_file(formal_root, value, f"lane {lane_id} extra input {index}")
        input_paths[str(selected.relative_to(formal_root))] = selected
    environment = lane.get("environment", {})
    if not isinstance(environment, dict) or any(
        not isinstance(key, str) or not isinstance(value, str)
        for key, value in environment.items()
    ):
        raise Refusal(f"lane {lane_id} environment must contain string pairs")
    return {
        "id": lane_id,
        "runner": runner,
        "timeout_seconds": timeout_seconds,
        "rows": prepared_rows,
        "inputs": input_paths,
        "environment": environment,
        "result_contract": contract,
        "authority_bindings": authority_bindings,
    }


def _state_counts(text: str) -> list[dict[str, int | None]]:
    values = []
    pattern = re.compile(
        r"(?m)^\s*(\d+) states generated, (\d+) distinct states found"
        r"(?:, (\d+) states left on queue\.?)?"
    )
    for generated, distinct, queued in pattern.findall(text):
        values.append(
            {
                "generated": int(generated),
                "distinct": int(distinct),
                "queued": int(queued) if queued else None,
            }
        )
    return values


def _terminate(process: subprocess.Popen[bytes]) -> None:
    try:
        os.killpg(process.pid, signal.SIGTERM)
        process.wait(timeout=2)
    except (ProcessLookupError, subprocess.TimeoutExpired):
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        process.wait()


def _check_zstd_contract(
    state_root: Path, expected_rows: dict[str, dict[str, Any]]
) -> list[str]:
    problems: list[str] = []
    rows_path = state_root / "results.jsonl"
    summary_path = state_root / "suite-summary.json"
    if rows_path.is_symlink() or not rows_path.is_file():
        return ["selected-row result ledger is missing"]
    if summary_path.is_symlink() or not summary_path.is_file():
        return ["selected-row suite summary is missing"]
    try:
        rows = [json.loads(line) for line in rows_path.read_text().splitlines() if line.strip()]
        summary = json.loads(summary_path.read_text())
    except (OSError, json.JSONDecodeError) as exc:
        return [f"cannot parse selected-row result contract: {exc}"]
    selected = [row for row in rows if row.get("status") != "not-started"]
    selected_ids = [row.get("id") for row in selected]
    if len(selected_ids) != len(set(selected_ids)):
        problems.append("selected-row result ledger contains duplicate identities")
    observed = {row.get("id"): row for row in selected}
    expected_ids = set(expected_rows)
    if set(observed) != expected_ids:
        problems.append("selected-row identities differ from the aggregate manifest")
    for row_id in sorted(expected_ids):
        result = observed.get(row_id, {})
        if result.get("status") != "pass":
            problems.append(f"selected row did not pass: {row_id}")
        for field in (
            "module",
            "config",
            "module_sha256",
            "config_sha256",
            "expected_exit",
            "expected_wait",
            "expected_phase",
            "expected",
            "row_sha256",
        ):
            if result.get(field) != expected_rows[row_id].get(field):
                problems.append(f"selected row authority mismatch: {row_id}/{field}")
    if summary.get("sany_status") != "pass":
        problems.append("SANY did not pass")
    if summary.get("selected_rows") != len(expected_ids):
        problems.append("selected-row count differs from the aggregate manifest")
    return problems


def _run_lane(
    formal_root: Path,
    run_root: Path,
    lane: dict[str, Any],
    jar: Path,
    workers: str,
) -> dict[str, Any]:
    lane_root = run_root / "lanes" / lane["id"]
    state_root = run_root / "state" / lane["id"]
    temp_root = run_root / "tmp" / lane["id"]
    lane_root.mkdir(parents=True)
    temp_root.mkdir(parents=True)
    stdout_path = lane_root / "stdout.log"
    stderr_path = lane_root / "stderr.log"
    command = ["sh", str(lane["runner"])]
    environment = os.environ.copy()
    environment.update(lane["environment"])
    environment.update(
        {
            "TLA2TOOLS_JAR": str(jar),
            "TLC_WORKERS": workers,
            "TLC_STATE_ROOT": str(state_root),
            "ZSTD_ROUTE_FINPUT_COMPOSITION_STATE_ROOT": str(state_root),
            "TMPDIR": str(temp_root),
            "TMP": str(temp_root),
            "TEMP": str(temp_root),
            "TEMPDIR": str(temp_root),
        }
    )
    if lane["result_contract"] == "zstd-selected-jsonl":
        environment["ROW_IDS"] = ",".join(row["id"] for row in lane["rows"])
        environment["FULL_MATRIX"] = "0"
        environment["S6_TRANSPLANT_REVIEW"] = "0"
        environment["S6_V5_SPEC_PATH"] = str(
            formal_root / "PORTABLE_EXECUTION_AUTHORITY.md"
        )
    started = time.time()
    monotonic = time.monotonic()
    timed_out = False
    with stdout_path.open("wb") as stdout, stderr_path.open("wb") as stderr:
        process = subprocess.Popen(
            command,
            cwd=formal_root,
            env=environment,
            stdout=stdout,
            stderr=stderr,
            start_new_session=True,
        )
        try:
            return_code = process.wait(timeout=lane["timeout_seconds"])
        except subprocess.TimeoutExpired:
            timed_out = True
            _terminate(process)
            return_code = 124
    elapsed = time.monotonic() - monotonic
    stdout_text = stdout_path.read_text(errors="replace")
    stderr_text = stderr_path.read_text(errors="replace")
    combined = stdout_text + "\n" + stderr_text
    problems = []
    if timed_out:
        problems.append("lane timeout")
    if return_code != 0:
        problems.append(f"runner exit {return_code}")
    if re.search(r"(?m)(?:^|\b)SKIP(?:\b|$)", combined):
        problems.append("runner reported SKIP")
    if lane["result_contract"] == "log-markers":
        for row in lane["rows"]:
            marker = row.get("marker")
            if marker and marker not in combined:
                problems.append(f"missing row marker: {row['id']}")
    else:
        problems.extend(
            _check_zstd_contract(state_root, lane["authority_bindings"])
        )
    result = {
        "id": lane["id"],
        "status": "PASS" if not problems else "FAIL",
        "command": command,
        "cwd": str(formal_root),
        "timeout_seconds": lane["timeout_seconds"],
        "timed_out": timed_out,
        "return_code": return_code,
        "utc_start": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(started)),
        "utc_end": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "elapsed_seconds": elapsed,
        "stdout": str(stdout_path.relative_to(run_root)),
        "stdout_sha256": _sha256(stdout_path),
        "stderr": str(stderr_path.relative_to(run_root)),
        "stderr_sha256": _sha256(stderr_path),
        "state_root": str(state_root.relative_to(run_root)),
        "state_counts": _state_counts(combined),
        "selected_rows": lane["rows"],
        "problems": problems,
    }
    _write_json(lane_root / "result.json", result)
    return result


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--manifest",
        type=Path,
        default=Path(__file__).with_name("formal_aggregate_manifest.json"),
    )
    parser.add_argument("--results-root", type=Path)
    args = parser.parse_args(argv)
    manifest_path = args.manifest.absolute()
    formal_root = manifest_path.parent
    run_root: Path | None = None
    try:
        manifest = _load_manifest(manifest_path)
        jar_value = os.environ.get("TLA2TOOLS_JAR", "")
        jar = Path(jar_value)
        if not jar_value or not jar.is_absolute() or jar.is_symlink() or not jar.is_file():
            raise Refusal("TLA2TOOLS_JAR must name an absolute, regular, nonsymlink file")
        jar_sha256 = _sha256(jar)
        if jar_sha256 != manifest["tool"]["sha256"]:
            raise Refusal("TLA2TOOLS_JAR does not match the aggregate manifest digest")
        if shutil.which("java") is None:
            raise Refusal("java is required for every selected formal lane")
        workers = os.environ.get("TLC_WORKERS", str(manifest["default_workers"]))
        if not re.fullmatch(r"[1-9][0-9]*", workers):
            raise Refusal("TLC_WORKERS must be a positive integer")
        prepared = [_prepare_lane(formal_root, lane) for lane in manifest["lanes"]]
        selected_inputs: dict[str, Path] = {
            "formal_aggregate_manifest.json": manifest_path,
            "run_formal_aggregate.py": Path(__file__).resolve(),
        }
        for lane in prepared:
            for relative, path in lane["inputs"].items():
                selected_inputs[relative] = path
        root_value = args.results_root or Path(
            os.environ.get(
                "P50_FORMAL_RESULTS_ROOT",
                str(Path(os.environ.get("ICEFARM_TMPDIR", "/tmp")) / "icecream-formal"),
            )
        )
        results_root = root_value.resolve()
        results_root.mkdir(parents=True, exist_ok=True)
        if results_root.is_symlink() or not results_root.is_dir():
            raise Refusal("formal results root must be a nonsymlink directory")
        run_root = Path(tempfile.mkdtemp(prefix="aggregate-", dir=results_root))
        (run_root / "lanes").mkdir()
        (run_root / "state").mkdir()
        (run_root / "tmp").mkdir()
        input_receipt = {
            name: {"sha256": _sha256(path), "bytes": path.stat().st_size}
            for name, path in sorted(selected_inputs.items())
        }
        _write_json(
            run_root / "authority.json",
            {
                "schema": manifest["schema"],
                "manifest_path": str(manifest_path),
                "manifest_sha256": _sha256(manifest_path),
                "tool_path": str(jar),
                "tool_sha256": jar_sha256,
                "workers": int(workers),
                "inputs": input_receipt,
            },
        )
        lane_results = []
        for lane in prepared:
            result = _run_lane(formal_root, run_root, lane, jar, workers)
            lane_results.append(result)
            if result["status"] != "PASS":
                break
        status = "PASS" if len(lane_results) == len(prepared) and all(
            result["status"] == "PASS" for result in lane_results
        ) else "FAIL"
        summary = {
            "schema": "icecream-p50-formal-aggregate-result-v1",
            "status": status,
            "selected_lane_count": len(prepared),
            "completed_lane_count": len(lane_results),
            "lanes": lane_results,
            "utc_end": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        }
        _write_json(run_root / "summary.json", summary)
        print(f"FORMAL_AGGREGATE_{status}={run_root}")
        return 0 if status == "PASS" else 1
    except Refusal as exc:
        if run_root is not None:
            _write_json(
                run_root / "summary.json",
                {
                    "schema": "icecream-p50-formal-aggregate-result-v1",
                    "status": "REFUSED",
                    "reason": str(exc),
                    "utc_end": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                },
            )
        print(f"formal aggregate refused: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
