from __future__ import annotations

import copy
import hashlib
import importlib.util
import json
import os
import subprocess
import sys
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[3]
FORMAL = ROOT / "cache" / "formal"
RUNNER = FORMAL / "run_formal_aggregate.py"


def _digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _load_runner():
    spec = importlib.util.spec_from_file_location("p50_formal_aggregate", RUNNER)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _fake_formal_tree(tmp_path: Path, output: str = "== clean ==") -> tuple[Path, Path]:
    formal = tmp_path / "formal"
    formal.mkdir()
    jar = tmp_path / "tla2tools.jar"
    jar.write_bytes(b"pinned-test-tool\n")
    (formal / "Model.tla").write_text("---- MODULE Model ----\n====\n")
    (formal / "Model.cfg").write_text("SPECIFICATION Spec\n")
    (formal / "lane.sh").write_text(
        "#!/bin/sh\n"
        "# Model.cfg ExpectedInvariant\n"
        f"printf '%s\\n' '{output}'\n"
        "printf '%s\\n' '11 states generated, 7 distinct states found, 0 states left on queue.'\n"
    )
    manifest = {
        "schema": "icecream-p50-formal-aggregate-v1",
        "tool": {"name": "tla2tools.jar", "sha256": _digest(jar)},
        "default_workers": 8,
        "lanes": [
            {
                "id": "fake",
                "runner": "lane.sh",
                "timeout_seconds": 5,
                "rows": [
                    {
                        "id": "clean",
                        "module": "Model.tla",
                        "config": "Model.cfg",
                        "outcome": "clean",
                        "marker": "== clean ==",
                    }
                ],
            }
        ],
    }
    manifest_path = formal / "formal_aggregate_manifest.json"
    manifest_path.write_text(json.dumps(manifest))
    return manifest_path, jar


def _run_fake(
    manifest: Path,
    jar: Path,
    results: Path,
    *,
    workers: str | None = "1",
) -> subprocess.CompletedProcess[str]:
    environment = os.environ.copy()
    environment["TLA2TOOLS_JAR"] = str(jar)
    if workers is None:
        environment.pop("TLC_WORKERS", None)
    else:
        environment["TLC_WORKERS"] = workers
    return subprocess.run(
        [
            sys.executable,
            str(RUNNER),
            "--manifest",
            str(manifest),
            "--results-root",
            str(results),
        ],
        text=True,
        capture_output=True,
        env=environment,
        check=False,
        timeout=10,
    )


def test_packaged_manifest_selects_every_formal_lane_and_preflights() -> None:
    runner = _load_runner()
    manifest_path = FORMAL / "formal_aggregate_manifest.json"
    manifest = runner._load_manifest(manifest_path)
    prepared = [runner._prepare_lane(FORMAL, lane) for lane in manifest["lanes"]]
    assert [lane["id"] for lane in prepared] == [
        "core",
        "assignment-delivery",
        "assignment-identity",
        "global",
        "zstd-route-finput",
        "transfer-concurrency",
    ]
    assert [len(lane["rows"]) for lane in prepared] == [35, 7, 29, 17, 9, 21]
    assert manifest["tool"]["sha256"] == (
        "936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88"
    )
    assert manifest["default_workers"] == 8
    zstd = next(lane for lane in prepared if lane["id"] == "zstd-route-finput")
    assert zstd["environment"]["S6_PORTABLE_EXECUTION"] == "1"
    assert zstd["result_contract"] == "zstd-selected-jsonl"
    authority = FORMAL / "PORTABLE_EXECUTION_AUTHORITY.md"
    zstd_runner = (FORMAL / "run_zstd_route_finput_composition_tlc.sh").read_text()
    assert _digest(authority) in zstd_runner
    assert "/tmp/s6-zstd-route-finput-v5-correction-spec-20260828.md" in zstd_runner
    concurrency = next(lane for lane in prepared if lane["id"] == "transfer-concurrency")
    assert concurrency["runner"].name == "run_transfer_concurrency_tlc.sh"
    assert concurrency["timeout_seconds"] == 7200
    assert all(row["module"] == "Protocol50TransferConcurrency.tla" for row in concurrency["rows"])
    assert {row["id"] for row in concurrency["rows"]} >= {
        "safety-c2f1",
        "safety-c3f1",
        "safety-c4f1",
        "safety-c1f2",
        "safety-c1f3",
        "safety-c1f4",
        "overlap-c2f1",
        "overlap-c3f1",
        "overlap-c4f1",
        "overlap-c1f2",
        "overlap-c1f3",
        "overlap-c1f4",
        "blocked-link-progress",
        "byte-cap-pressure",
        "generation-replacement",
        "global-serialization-mutant",
        "profile-in-key-mutant",
        "stale-publication-mutant",
        "stale-credit-release-mutant",
        "duplicate-release-mutant",
        "lost-ack-early-release-mutant",
    }
    concurrency_runner = concurrency["runner"].read_text()
    assert "NoAllLinksRunning" in concurrency_runner
    assert "ReservationReleasedAtMostOnce" in concurrency_runner
    assert "Temporal properties were violated." in concurrency_runner
    assert "ROW_TIMEOUT_SECONDS=${ROW_TIMEOUT_SECONDS:-120}" in concurrency_runner
    assert "TLA2TOOLS_JAR does not match the pinned tool SHA-256" in concurrency_runner
    assert "ROW_TIMEOUT_SECONDS must be an integer in 1..300" in concurrency_runner
    gate_mutant = next(
        row for row in concurrency["rows"] if row["id"] == "global-serialization-mutant"
    )
    assert gate_mutant["outcome"] == "expected-failure"
    assert gate_mutant["property"] == "BlockedLinkDoesNotBlockHealthyProgress"
    for makefile in ("GNUmakefile", "Makefile.am"):
        make_text = (ROOT / makefile).read_text()
        target = make_text.split("protocol50-formal:", 1)[1]
        assert "run_formal_aggregate.py" in target


def test_expected_temporal_failure_is_bound_as_one_named_property() -> None:
    runner = _load_runner()
    manifest = runner._load_manifest(FORMAL / "formal_aggregate_manifest.json")
    lane = copy.deepcopy(
        next(item for item in manifest["lanes"] if item["id"] == "transfer-concurrency")
    )
    row = next(item for item in lane["rows"] if item["id"] == "global-serialization-mutant")
    row["invariant"] = "ReservationCountsMatchOwnedOperations"
    with pytest.raises(runner.Refusal, match="exactly one expected invariant or property"):
        runner._prepare_lane(FORMAL, lane)


def test_zstd_declaration_must_match_its_same_id_authority_row() -> None:
    runner = _load_runner()
    manifest = runner._load_manifest(FORMAL / "formal_aggregate_manifest.json")
    lane = copy.deepcopy(next(item for item in manifest["lanes"] if item["id"] == "zstd-route-finput"))
    lane["rows"][0]["config"] = "Protocol50ZstdRouteCollapsedTUMutant.cfg"
    lane["rows"][0]["invariant"] = "DistinctOperationTUs"
    with pytest.raises(
        runner.Refusal, match="core-05 config does not match its selection authority"
    ):
        runner._prepare_lane(FORMAL, lane)


def test_zstd_returned_row_must_match_the_bound_authority(tmp_path: Path) -> None:
    runner = _load_runner()
    state = tmp_path / "state"
    state.mkdir()
    expected = {
        "row-1": {
            "id": "row-1",
            "module": "Model.tla",
            "config": "Expected.cfg",
            "module_sha256": "1" * 64,
            "config_sha256": "2" * 64,
            "expected_exit": 12,
            "expected_wait": "nonzero",
            "expected_phase": "invariant",
            "expected": "ExpectedInvariant",
            "row_sha256": "3" * 64,
        }
    }
    returned = {**expected["row-1"], "status": "pass", "config": "Other.cfg"}
    (state / "results.jsonl").write_text(json.dumps(returned) + "\n")
    (state / "suite-summary.json").write_text(
        json.dumps({"sany_status": "pass", "selected_rows": 1}) + "\n"
    )
    problems = runner._check_zstd_contract(state, expected)
    assert "selected row authority mismatch: row-1/config" in problems


def test_aggregate_retains_authenticated_unique_success_bundles(tmp_path: Path) -> None:
    manifest, jar = _fake_formal_tree(tmp_path)
    results = tmp_path / "results"
    first = _run_fake(manifest, jar, results)
    second = _run_fake(manifest, jar, results)
    assert first.returncode == second.returncode == 0
    bundles = sorted(results.glob("aggregate-*"))
    assert len(bundles) == 2
    assert bundles[0] != bundles[1]
    for bundle in bundles:
        summary = json.loads((bundle / "summary.json").read_text())
        authority = json.loads((bundle / "authority.json").read_text())
        lane = summary["lanes"][0]
        assert summary["status"] == "PASS"
        assert lane["status"] == "PASS"
        assert lane["state_counts"] == [
            {"generated": 11, "distinct": 7, "queued": 0}
        ]
        assert authority["tool_sha256"] == _digest(jar)
        assert authority["inputs"]["Model.cfg"]["sha256"] == _digest(
            manifest.parent / "Model.cfg"
        )


def test_aggregate_uses_authenticated_default_workers(tmp_path: Path) -> None:
    manifest, jar = _fake_formal_tree(tmp_path)
    results = tmp_path / "results"
    completed = _run_fake(manifest, jar, results, workers=None)
    assert completed.returncode == 0
    bundle = next(results.glob("aggregate-*"))
    authority = json.loads((bundle / "authority.json").read_text())
    assert authority["workers"] == 8


def test_aggregate_refuses_invalid_default_workers_before_starting_a_lane(
    tmp_path: Path,
) -> None:
    manifest, jar = _fake_formal_tree(tmp_path)
    value = json.loads(manifest.read_text())
    value["default_workers"] = 0
    manifest.write_text(json.dumps(value))
    results = tmp_path / "results"
    completed = _run_fake(manifest, jar, results, workers=None)
    assert completed.returncode == 2
    assert "default_workers is outside 1..64" in completed.stderr
    assert not results.exists()


def test_aggregate_never_promotes_skip_to_pass(tmp_path: Path) -> None:
    manifest, jar = _fake_formal_tree(tmp_path, "== clean == SKIP")
    results = tmp_path / "results"
    completed = _run_fake(manifest, jar, results)
    assert completed.returncode == 1
    bundle = next(results.glob("aggregate-*"))
    summary = json.loads((bundle / "summary.json").read_text())
    assert summary["status"] == "FAIL"
    assert "runner reported SKIP" in summary["lanes"][0]["problems"]


def test_aggregate_refuses_wrong_tool_before_starting_a_lane(tmp_path: Path) -> None:
    manifest, jar = _fake_formal_tree(tmp_path)
    jar.write_bytes(b"wrong-tool\n")
    results = tmp_path / "results"
    completed = _run_fake(manifest, jar, results)
    assert completed.returncode == 2
    assert "does not match the aggregate manifest digest" in completed.stderr
    assert not results.exists()


def test_aggregate_refuses_a_missing_selected_input_before_lane_start(
    tmp_path: Path,
) -> None:
    manifest, jar = _fake_formal_tree(tmp_path)
    (manifest.parent / "Model.cfg").unlink()
    results = tmp_path / "results"
    completed = _run_fake(manifest, jar, results)
    assert completed.returncode == 2
    assert "missing, not regular, or a symlink" in completed.stderr
    assert not results.exists()
