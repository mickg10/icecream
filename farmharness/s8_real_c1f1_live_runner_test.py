from __future__ import annotations

import hashlib
import json
import subprocess
from pathlib import Path

import pytest

import s8_real_c1f1_live_runner as runner
from s8_first_triple_driver import _load_live_curve
from s8_predictive_live_normalizer import MANIFEST_SCHEMA, canonical_bytes


def _product(tmp_path: Path) -> tuple[Path, Path]:
    root = tmp_path / "product"
    for role in ("scheduler/icecc-scheduler", "daemon/iceccd", "client/icecc",
                 "cache/icecc-cache-service"):
        path = root / role
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(role.encode())
        path.chmod(0o755)
    script = root / "unittests/p50compilee2e-run.sh"
    script.parent.mkdir(parents=True)
    script.write_text("#!/bin/sh\nexit 0\n")
    script.chmod(0o755)
    subprocess.run(["git", "init", "-q", str(root)], check=True)
    subprocess.run(["git", "-C", str(root), "config", "user.email", "test@example.invalid"], check=True)
    subprocess.run(["git", "-C", str(root), "config", "user.name", "S8 test"], check=True)
    subprocess.run(["git", "-C", str(root), "add", "."], check=True)
    subprocess.run(["git", "-C", str(root), "commit", "-qm", "product"], check=True)
    return root, script


def _batch(tmp_path: Path, count: int = 100) -> Path:
    rows = []
    for index in range(count):
        source = tmp_path / f"tu-{index:03d}.cpp"
        raw = f"int duck_{index}() {{ return {index}; }}\n".encode()
        source.write_bytes(raw)
        rows.append({"tu_id": f"duck-tu-{index:03d}", "source": str(source),
                     "source_relative": f"translation-units/tu-{index:03d}.ii",
                     "sha256": hashlib.sha256(raw).hexdigest(),
                     "preprocessed_sha256": hashlib.sha256(raw).hexdigest(),
                     "preprocessed_bytes": len(raw)})
    path = tmp_path / "batch.jsonl"
    path.write_text("".join(json.dumps(row, sort_keys=True) + "\n" for row in rows))
    return path


def _topology(tmp_path: Path, batch: Path) -> Path:
    rows = [json.loads(line) for line in batch.read_text().splitlines()]
    path = tmp_path / "topology.json"
    path.write_text(json.dumps({"schema": "icecream-s8-topology-assignment-v1",
                                "suite": runner.TOPOLOGY,
                                "assignments": [{"ordinal": i, "tu_id": row["tu_id"],
                                                 "relationship": 0, "f_slot": 0}
                                                for i, row in enumerate(rows)]}, sort_keys=True))
    return path


def test_batch_manifest_and_c1f1_topology_are_authenticated(tmp_path: Path) -> None:
    batch = _batch(tmp_path)
    rows = runner.load_batch_manifest(batch)
    assert len(rows) == 100
    topology = _topology(tmp_path, batch)
    digest = runner.load_topology(topology, rows)
    assert len(digest) == 64


@pytest.mark.parametrize(("depth", "count"), [("100", 100), ("200", 200), ("full", 7)])
def test_depth_binds_manifest_count(tmp_path: Path, depth: str, count: int) -> None:
    batch = _batch(tmp_path, count)
    rows = runner.load_batch_manifest(batch, runner.selected_count(depth, count if depth == "full" else None))
    digest = runner.payload_descriptor_digest(rows, "a" * 64)
    rows[0]["preprocessed_bytes"] += 1
    assert runner.payload_descriptor_digest(rows, "a" * 64) != digest
    with pytest.raises(runner.LiveRunnerError):
        runner.load_batch_manifest(batch, count + 1)


def test_input_digest_matches_source_manifest_plus_ordered_payload_contract(tmp_path: Path) -> None:
    batch = _batch(tmp_path, 3)
    rows = runner.load_batch_manifest(batch, 3)
    source_manifest_sha, _ = runner._sha(batch)
    expected = {"source_manifest_sha256": source_manifest_sha,
                "inputs": runner.payload_descriptors(rows)}
    expected_digest = hashlib.sha256(runner._canonical(expected)).hexdigest()
    assert runner.payload_descriptor_digest(rows, source_manifest_sha) == expected_digest
    assert runner.payload_descriptors(rows)[0]["source_relative"] == "translation-units/tu-000.ii"


@pytest.mark.parametrize("profile", ["ZSTD_TU", "ZSTD_ROUTE", "P29", "GRZ_RESIDUAL"])
def test_dry_run_command_targets_real_single_lifecycle_for_all_profiles(tmp_path: Path,
                                                                          profile: str) -> None:
    batch = _batch(tmp_path)
    product, script = _product(tmp_path)
    command = runner.build_command(batch, profile, "sha256:" + "a" * 64,
                                   product_root=product, script=script, corpus="DuckDB",
                                   regime="warm", depth="100", passes=2)
    assert command[0] == "env"
    assert f"ICECC_P50_PROFILE={profile}" in command
    assert "ICECC_P50_C1F1_WARM=1" in command
    assert "ICECC_P50_C1F1_PASSES=2" in command
    assert "ICECC_P50_C1F1_EXPECTED_COUNT=100" in command
    assert "ICECC_P50_C1F1_TIMEOUT=3630" in command
    assert "ICECC_P50_C1F1_KEEP_WORK=1" in command
    assert any(item.startswith("ICECC_P50_C1F1_BATCH_MANIFEST=") for item in command)
    assert command[-1].endswith("unittests/p50compilee2e-run.sh")


def test_timeout_scales_and_is_capped() -> None:
    assert runner.derive_timeout(100, 1, False) < runner.derive_timeout(100, 2, True)
    assert runner.derive_timeout(100000, 2, True) == runner.MAX_TIMEOUT_SECONDS


def test_topology_requires_exact_tu_identity(tmp_path: Path) -> None:
    batch = _batch(tmp_path)
    rows = runner.load_batch_manifest(batch)
    topology = _topology(tmp_path, batch)
    value = json.loads(topology.read_text())
    value["assignments"][0].pop("tu_id")
    topology.write_text(json.dumps(value))
    with pytest.raises(runner.LiveRunnerError, match="identity_mismatch"):
        runner.load_topology(topology, rows)


def test_product_identity_rejects_tracked_edit(tmp_path: Path) -> None:
    product, script = _product(tmp_path)
    (product / "unittests/p50compilee2e-run.sh").write_text("#!/bin/sh\nexit 1\n")
    with pytest.raises(runner.LiveRunnerError, match="tracked_or_index_dirty"):
        runner.product_identity(product, script)


def test_batch_shell_excludes_warm_prewarm_and_carries_optional_repeat() -> None:
    shell = (Path(__file__).resolve().parents[1] / "unittests/p50compilee2e-run.sh").read_text()
    assert "run_batch prewarm 0" in shell
    assert "run_batch full-1 1" in shell
    assert 'if test "$passes" = 2; then' in shell
    assert "s7-measured-c-action-trace.jsonl" in Path(__file__).resolve().parent.joinpath(
        "s8_real_c1f1_live_runner.py").read_text()
    assert "shutil.rmtree(work)" in Path(__file__).resolve().parent.joinpath(
        "s8_real_c1f1_live_runner.py").read_text()


def test_unsupported_topology_fails_closed(tmp_path: Path) -> None:
    batch = _batch(tmp_path)
    rows = runner.load_batch_manifest(batch)
    topology = tmp_path / "bad-topology.json"
    topology.write_text(json.dumps({"suite": "C1F20/40", "assignments": []}))
    with pytest.raises(runner.LiveRunnerError, match="unsupported_suite"):
        runner.load_topology(topology, rows)


def test_source_mutation_is_rejected_before_launch(tmp_path: Path) -> None:
    batch = _batch(tmp_path)
    rows = runner.load_batch_manifest(batch)
    Path(rows[0]["source"]).write_bytes(b"changed\n")
    with pytest.raises(runner.LiveRunnerError, match="source_digest_mismatch"):
        runner.load_batch_manifest(batch)


def test_predictive_payload_descriptor_is_required(tmp_path: Path) -> None:
    batch = _batch(tmp_path)
    values = [json.loads(line) for line in batch.read_text().splitlines()]
    values[0].pop("preprocessed_sha256")
    batch.write_text("".join(json.dumps(value) + "\n" for value in values))
    with pytest.raises(runner.LiveRunnerError, match="fields_invalid"):
        runner.load_batch_manifest(batch)


def test_runner_curve_is_accepted_by_current_live_intake(tmp_path: Path) -> None:
    identity = {"corpus": "DuckDB", "profile": "ZSTD_TU", "regime": "cold",
                "split": "held_out_validation", "run_id": "full-1",
                "source_commit": "a" * 40, "source_tree": "b" * 40,
                "input_digest": "c" * 64, "topology_digest": "d" * 64,
                "model_id": "s8-real-live"}
    curve = [{"step": 0, "tu_id": "tu-0",
              "cell": {"corpus": "DuckDB", "profile": "ZSTD_TU", "regime": "cold"},
              "cumulative": {"channel_bytes": 10, "elapsed_ns": 20,
                              "throughput_bytes_per_s": 500000000.0}}]
    curve_path = tmp_path / "live_curve.jsonl"
    curve_raw = b"".join(canonical_bytes(row) + b"\n" for row in curve)
    curve_path.write_bytes(curve_raw)
    manifest = {"schema": MANIFEST_SCHEMA, "identity": identity,
                "units": {"point": "step", "channel_bytes": "bytes", "elapsed_ns": "ns",
                          "throughput_bytes_per_s": "bytes_per_s"},
                "curve": {"path": curve_path.name, "sha256": hashlib.sha256(curve_raw).hexdigest(),
                          "bytes": len(curve_raw)},
                "provenance": {"mode": "live", "producer": "s8_real_c1f1_live_runner",
                               "trace_free": False}}
    manifest_path = tmp_path / "live_curve_manifest.json"
    manifest_path.write_bytes(canonical_bytes(manifest) + b"\n")
    artifact = _load_live_curve(manifest_path,
                                cell={key: identity[key] for key in ("corpus", "profile", "regime")})
    assert artifact["provenance"]["producer"] == "s8_real_c1f1_live_runner"
