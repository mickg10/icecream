from __future__ import annotations

import hashlib
import json
from pathlib import Path

import pytest

import s8_real_c1f1_live_runner as runner


def _batch(tmp_path: Path, count: int = 100) -> Path:
    rows = []
    for index in range(count):
        source = tmp_path / f"tu-{index:03d}.cpp"
        raw = f"int duck_{index}() {{ return {index}; }}\n".encode()
        source.write_bytes(raw)
        rows.append({"tu_id": f"duck-tu-{index:03d}", "source": str(source),
                     "sha256": hashlib.sha256(raw).hexdigest()})
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


@pytest.mark.parametrize("profile", ["ZSTD_TU", "ZSTD_ROUTE", "P29", "GRZ_RESIDUAL"])
def test_dry_run_command_targets_real_single_lifecycle_for_all_profiles(tmp_path: Path,
                                                                          profile: str) -> None:
    batch = _batch(tmp_path)
    command = runner.build_command(batch, profile, "sha256:" + "a" * 64,
                                   product_root=Path("/tanksmall/scratch/ictmp/wt-s8-real-multitu-live-luna-20260829"))
    assert command[0] == "env"
    assert f"ICECC_P50_PROFILE={profile}" in command
    assert "ICECC_P50_C1F1_WARM=0" in command
    assert "ICECC_P50_C1F1_KEEP_WORK=1" in command
    assert any(item.startswith("ICECC_P50_C1F1_BATCH_MANIFEST=") for item in command)
    assert command[-1].endswith("unittests/p50compilee2e-run.sh")


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
