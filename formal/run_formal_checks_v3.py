#!/usr/bin/env python3
"""Corrections layered on the pinned v2 formal acceptance runner.

Version 2 already owns tool hashes, TLC option preflight, TLAPS backend pinning,
clean-checkout checks, one-worker liveness, and stable/differential comparison.
This overlay changes only execution paths proven red by the first real run:

* expected counterexamples may stop with unexplored states on TLC's queue;
* modern ``-dumpTrace json`` output is normalized from TLC's real
  ``counterexample.state`` graph representation before manifest validation;
* the stable textual trace and modern JSON trace pass through one canonical
  state-array normalizer;
* every Python trace/adapter self-test is run with bytecode disabled; and
* peak RSS must be positive, not merely present.
"""

from __future__ import annotations

import os
import sys
from pathlib import Path
from typing import Any, Mapping

sys.dont_write_bytecode = True

import run_formal_checks_v2 as v2


def run_python_self_tests(
    formal_dir: Path, artifacts: Path
) -> list[dict[str, Any]]:
    env = dict(os.environ)
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    results: list[dict[str, Any]] = []
    scripts = (
        "trace_to_harness_test.py",
        "tlc_text_trace_test.py",
        "normalize_tlc_trace_test.py",
    )
    for script in scripts:
        path = formal_dir / script
        if not path.is_file():
            raise v2.FormalRunError(f"missing Python self-test: {path}")
        result = v2.run_capture(
            [sys.executable, str(path)],
            formal_dir,
            timeout=120,
            env=env,
        )
        log_path = artifacts / "self-tests" / f"{script}.log"
        log_path.parent.mkdir(parents=True, exist_ok=True)
        log_path.write_text(result["output"], encoding="utf-8")
        if result["returncode"] != 0:
            raise v2.FormalRunError(
                f"Python self-test failed: {script}; see {log_path}"
            )
        results.append(
            {
                "script": script,
                "sha256": v2.sha256_file(path),
                "command": result["command"],
                "returncode": result["returncode"],
                "elapsed_seconds": result["elapsed_seconds"],
                "log_sha256": v2.sha256_file(log_path),
            }
        )
    return results


def _normalize_trace(
    *,
    formal_dir: Path,
    source_path: Path,
    run_dir: Path,
) -> tuple[Path, dict[str, Any]]:
    normalizer_path = formal_dir / "normalize_tlc_trace.py"
    normalizer = v2.import_module(normalizer_path, "normalize_tlc_trace")
    normalized_path = run_dir / "trace.normalized.json"
    metadata_path = run_dir / "trace-normalization.json"
    try:
        trace = normalizer.normalize_trace(source_path)
        normalizer.write_normalized(trace, normalized_path)
    except Exception as exc:
        raise v2.FormalRunError(
            f"counterexample trace normalization failed: {exc}"
        ) from exc
    if len(trace.states) < 2:
        raise v2.FormalRunError(
            "normalized counterexample must contain at least two states"
        )
    metadata = {
        "source_path": str(source_path),
        "source_format": trace.source_format,
        "source_sha256": trace.source_sha256,
        "normalizer_sha256": v2.sha256_file(normalizer_path),
        "normalized_path": str(normalized_path),
        "normalized_sha256": v2.sha256_file(normalized_path),
        "state_count": len(trace.states),
    }
    v2.write_json(metadata_path, metadata)
    return normalized_path, metadata


def validate_tlc_result(
    *,
    check: Mapping[str, Any],
    toolchain: v2.Toolchain,
    command_result: v2.CommandResult,
    trace_path: Path,
    formal_dir: Path,
    run_dir: Path,
) -> dict[str, Any]:
    log = v2.read_text(command_result.log_path)
    if command_result.timed_out:
        raise v2.FormalRunError(
            f"{check['id']}: TLC exceeded its whole-run timeout"
        )
    if (
        command_result.peak_rss_kib is None
        or command_result.peak_rss_kib <= 0
    ):
        raise v2.FormalRunError(
            f"{check['id']}: positive peak RSS evidence is missing"
        )
    v2.reject_runtime_or_parser_failure(log)
    stats = v2.parse_tlc_stats(log)
    if not stats["generated_states"] or not stats["distinct_states"]:
        raise v2.FormalRunError(
            f"{check['id']}: zero or missing generated/distinct state count"
        )
    if stats["depth"] is None:
        raise v2.FormalRunError(
            f"{check['id']}: complete-state-graph depth is missing"
        )

    expected = check["expected"]
    property_name = check.get("property")
    adapter_result = None
    trace_conversion = None
    normalized_path: Path | None = None

    if expected == "pass":
        if stats["states_left_on_queue"] not in (None, 0):
            raise v2.FormalRunError(
                f"{check['id']}: passing run ended with states on the queue"
            )
        if command_result.returncode != 0:
            raise v2.FormalRunError(
                f"{check['id']}: fixed check exited "
                f"{command_result.returncode}, expected 0"
            )
        if not v2.re.search(
            r"No error has been found", log, v2.re.IGNORECASE
        ):
            raise v2.FormalRunError(
                f"{check['id']}: TLC did not report a completed no-error run"
            )
        if v2.re.search(
            r"\b(?:violated|counterexample)\b", log, v2.re.IGNORECASE
        ):
            raise v2.FormalRunError(
                f"{check['id']}: fixed run contains a violation"
            )

    elif expected == "counterexample":
        if command_result.returncode == 0:
            raise v2.FormalRunError(
                f"{check['id']}: mutant unexpectedly exited 0"
            )
        if not property_name or not v2.expected_counterexample_seen(
            log, property_name
        ):
            raise v2.FormalRunError(
                f"{check['id']}: direct violation of "
                f"{property_name!r} was not reported"
            )

        if toolchain.supports_dump_trace:
            if not trace_path.is_file():
                raise v2.FormalRunError(
                    f"{check['id']}: native JSON trace is absent: {trace_path}"
                )
            source_path = trace_path
            source_mode = "native-json-dumpTrace"
            stable_conversion = None
        else:
            stable_conversion = v2.convert_text_trace(
                formal_dir, command_result.log_path, trace_path
            )
            source_path = trace_path
            source_mode = "stable-text-conversion"

        normalized_path, normalization = _normalize_trace(
            formal_dir=formal_dir,
            source_path=source_path,
            run_dir=run_dir,
        )
        trace_conversion = {
            "mode": source_mode,
            "stable_conversion": stable_conversion,
            "normalization": normalization,
        }

        manifest_rel = check.get("trace_manifest")
        if not manifest_rel:
            raise v2.FormalRunError(
                f"{check['id']}: missing trace manifest"
            )
        manifest_path = formal_dir / manifest_rel
        adapter = v2.import_module(
            formal_dir / "trace_to_harness.py", "trace_to_harness"
        )
        try:
            adapter_result = adapter.validate_trace(
                normalized_path,
                manifest_path,
                tlc_log_path=command_result.log_path,
            )
        except Exception as exc:
            raise v2.FormalRunError(
                f"{check['id']}: trace adapter rejected trace: {exc}"
            ) from exc
        if (
            adapter_result["trace"]["state_count"]
            != normalization["state_count"]
        ):
            raise v2.FormalRunError(
                f"{check['id']}: adapter state count disagrees"
            )
        v2.write_json(run_dir / "harness.json", adapter_result)

    else:
        raise v2.FormalRunError(
            f"{check['id']}: unknown expected result {expected!r}"
        )

    return {
        "returncode": command_result.returncode,
        "timed_out": command_result.timed_out,
        "elapsed_seconds": command_result.elapsed_seconds,
        "peak_rss_kib": command_result.peak_rss_kib,
        "stats": stats,
        "log_sha256": v2.sha256_file(command_result.log_path),
        "metrics_sha256": v2.sha256_file(command_result.metrics_path),
        "trace_sha256": (
            v2.sha256_file(trace_path) if trace_path.exists() else None
        ),
        "normalized_trace_sha256": (
            v2.sha256_file(normalized_path)
            if normalized_path is not None
            else None
        ),
        "trace_conversion": trace_conversion,
        "trace_adapter": adapter_result,
    }


# v2.main resolves these names dynamically from its module globals.
v2.run_python_self_tests = run_python_self_tests
v2.validate_tlc_result = validate_tlc_result


def main(argv: list[str] | None = None) -> int:
    return v2.main(argv)


if __name__ == "__main__":
    raise SystemExit(main())
