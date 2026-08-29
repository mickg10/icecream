from __future__ import annotations

import hashlib
import json
from pathlib import Path

import pytest

import s8_campaign_planner as planner


def _authority(tmp_path: Path) -> tuple[Path, Path, str, Path, str]:
    tmp_path.mkdir(parents=True, exist_ok=True)
    manifests = []
    for index, (manifest_id, project, count) in enumerate(planner.CORPUS_AUTHORITY):
        snapshot_root = tmp_path / "snapshots" / manifest_id
        snapshot_root.mkdir(parents=True, exist_ok=True)
        paths = []
        for i in range(count):
            snapshot = snapshot_root / f"tu-{i:04d}.ii"
            snapshot.write_bytes(f"{manifest_id}:{i}\n".encode())
            paths.append(str(snapshot))
        path = tmp_path / f"{manifest_id}.manifest.txt"
        path.write_text("\n".join(paths) + "\n")
        raw = path.read_bytes()
        manifests.append({
            "availability": "PRESENT", "manifest_id": manifest_id, "project": project,
            "tu_count": count,
            "manifest": {"path": str(path), "sha256": hashlib.sha256(raw).hexdigest()},
            "metadata": {"path": str(tmp_path / f"metadata-{index}.json"), "sha256": "0" * 64},
            "source_checkouts": [f"/retained/source/{manifest_id}"],
            "source_commit": f"commit-{index}",
        })
    inventory = {
        "schema": "icecream-s8-image-authority-inventory-v1", "read_only": True,
        "generated_utc": "20260829T000000Z", "authority": {"scope": "fixture"},
        "corpus_manifests": manifests,
    }
    inventory["seal"] = {"algorithm": "sha256",
                          "canonical_without_seal_sha256": hashlib.sha256(
                              planner._canonical(inventory)).hexdigest()}
    inventory_path = tmp_path / "inventory.json"
    inventory_path.write_bytes(planner._canonical(inventory))

    targets = []
    for profile, image_id in planner.HISTORICAL_IMAGES:
        targets.append({"profile": profile, "image_id": image_id,
                        "image_name": None, "registry_digest": None,
                        "compiler_executable_version": None, "stdlib": None,
                        "dockerfile": None, "build_context": None, "source_commit": None,
                        "status": "EXTERNAL_AUTHORITY_REQUIRED", "rebuildable_now": False})
    recovery = {
        "schema": planner.RECOVERY_SCHEMA,
        "conclusion": {"rebuildable_now": [],
                        "external_authority_required": [item[1] for item in planner.HISTORICAL_IMAGES]},
        "targets": targets,
    }
    recovery_path = tmp_path / "recovery.json"
    recovery_path.write_bytes(planner._canonical(recovery))
    matrix = {"schema": planner.MATRIX_AUDIT_SCHEMA, "status": "PASS",
              "matrix": {"expected_cells": 32, "completed_cells": 32,
                          "calibration_cells": 16, "calibration_expected": 16,
                          "held_out_validation_cells": 16,
                          "held_out_validation_expected": 16,
                          "missing_cells": [], "invalid_candidates": []},
              "cells": [{"status": "PASS", "cell": str(i)} for i in range(32)]}
    matrix_path = tmp_path / "matrix-audit.json"
    matrix_path.write_bytes(planner._canonical(matrix))
    return (inventory_path, recovery_path, hashlib.sha256(recovery_path.read_bytes()).hexdigest(),
            matrix_path, hashlib.sha256(matrix_path.read_bytes()).hexdigest())


def _plan(tmp_path: Path, current: bool = False):
    inventory, recovery, recovery_sha, matrix, matrix_sha = _authority(tmp_path / "authority")
    output = tmp_path / "experiments" / "icecream" / "s8-expanded" / "20260829T000000Z"
    kwargs = {}
    if current:
        kwargs = {"current_image_name": "icecream/farm-node:test",
                  "current_image_id": "sha256:" + "a" * 64}
    result = planner.plan_campaign(inventory, recovery, recovery_sha, matrix, matrix_sha,
                                   output, "20260829T000000Z", **kwargs)
    return result, output, inventory, recovery, recovery_sha


def test_authenticates_all_manifests_and_preserves_order(tmp_path: Path) -> None:
    result, output, _, _, _ = _plan(tmp_path)
    authority = result["corpus_authority"]
    assert authority["total_manifests"] == 11
    assert authority["total_tus"] == 8261
    assert [item["manifest_id"] for item in authority["records"]] == [item[0] for item in planner.CORPUS_AUTHORITY]
    assert [len(item["ordered_source_paths"]) for item in authority["records"]] == [item[2] for item in planner.CORPUS_AUTHORITY]
    assert authority["records"][0]["ordered_source_paths"][:2] == [
        str(tmp_path / "authority" / "snapshots" / "corpus" / "tu-0000.ii"),
        str(tmp_path / "authority" / "snapshots" / "corpus" / "tu-0001.ii")]
    assert json.loads((output / "campaign-index.json").read_text())["counts"]["historical_image_seven_method_grid"] == 4928
    assert len((output / "descriptors.jsonl").read_text().splitlines()) == 4928


def test_declares_distinct_methods_topologies_depths_and_statuses(tmp_path: Path) -> None:
    result, output, _, _, _ = _plan(tmp_path, current=True)
    assert [item["name"] for item in result["methods"]] == list(planner.METHODS)
    assert [item["root_status"] for item in result["methods"]] == [
        "NOT_IMPLEMENTED", "IMPLEMENTED", "IMPLEMENTED", "IMPLEMENTED", "IMPLEMENTED",
        "NOT_IMPLEMENTED", "NOT_IMPLEMENTED"]
    assert result["topologies"][0]["stream_capacity_tus"] == 100000
    assert result["topologies"][1]["global_execution_slots"] == 40
    assert result["topologies"][1]["stream_capacity_tus"] is None
    assert result["depths"] == ["100", "200", "full-1", "state-carrying full-2"]
    assert result["counts"]["historical_image_seven_method_grid"] == 4928
    assert result["counts"]["current_image_implemented_subset"] == 704
    assert result["counts"]["descriptor_status_counts"] == {
        "READY": 0, "NOT_READY": 1232, "MISSING_EXTERNAL_AUTHORITY": 4928}
    assert result["counts"]["current_image_ready"] == 0
    lines = (output / "descriptors.jsonl").read_text().splitlines()
    assert all("docker run" not in line and "simulator" not in line for line in lines)
    current_implemented = [json.loads(line) for line in lines
                           if json.loads(line)["image"]["key"] == "current-pinned"
                           and json.loads(line)["method"] in planner.IMPLEMENTED_METHODS]
    assert len(current_implemented) == 704
    assert {item["status"] for item in current_implemented} == {"NOT_READY"}
    assert {item["reason"] for item in current_implemented} == {
        "required_producer_capability_not_integrated_on_planner_source"}
    assert result["corpus_authority"]["snapshots"]["count"] == 8261
    first = json.loads(lines[0])
    assert first["result_relative_directory"] == (
        "experiments/icecream/s8-expanded/20260829T000000Z/"
        "debian-gcc/llvm/raw-ii/c1f1-100000/100/cold")


def test_historical_images_are_held_and_current_is_separate(tmp_path: Path) -> None:
    result, _, _, _, _ = _plan(tmp_path, current=True)
    historical = result["images"][:4]
    assert [item["image_id"] for item in historical] == [item[1] for item in planner.HISTORICAL_IMAGES]
    assert all(item["status"] == "MISSING_EXTERNAL_AUTHORITY" for item in historical)
    assert result["images"][-1]["status"] == "READY_EXECUTABLE_DIMENSION"


def test_manifest_digest_mutation_fails_closed(tmp_path: Path) -> None:
    inventory, recovery, recovery_sha, matrix, matrix_sha = _authority(tmp_path / "authority")
    source = tmp_path / "authority" / "corpus.manifest.txt"
    source.write_text(source.read_text() + "/retained/corpus/build/tu-mutated.ii\n")
    with pytest.raises(planner.PlannerError, match="manifest_digest_mismatch"):
        planner.plan_campaign(inventory, recovery, recovery_sha, matrix, matrix_sha, tmp_path / "out", "20260829T000000Z")


def test_inventory_seal_and_recovery_digest_mutations_fail_closed(tmp_path: Path) -> None:
    inventory, recovery, recovery_sha, matrix, matrix_sha = _authority(tmp_path / "authority")
    value = json.loads(inventory.read_text())
    value["corpus_manifests"][0]["tu_count"] = 1
    inventory.write_text(json.dumps(value) + "\n")
    with pytest.raises(planner.PlannerError, match="seal_mismatch"):
        planner.plan_campaign(inventory, recovery, recovery_sha, matrix, matrix_sha, tmp_path / "out", "20260829T000000Z")
    inventory, recovery, recovery_sha, matrix, matrix_sha = _authority(tmp_path / "authority2")
    with pytest.raises(planner.PlannerError, match="sha256_mismatch"):
        planner.plan_campaign(inventory, recovery, "0" * 64, matrix, matrix_sha, tmp_path / "out2", "20260829T000000Z")
    inventory, recovery, recovery_sha, matrix, matrix_sha = _authority(tmp_path / "authority3")
    matrix_value = json.loads(matrix.read_text())
    matrix_value["matrix"]["completed_cells"] = 31
    matrix.write_bytes(planner._canonical(matrix_value))
    with pytest.raises(planner.PlannerError, match="matrix_contract_invalid"):
        planner.plan_campaign(inventory, recovery, recovery_sha, matrix, hashlib.sha256(matrix.read_bytes()).hexdigest(),
                              tmp_path / "out3", "20260829T000000Z")


def test_method_and_topology_mutations_fail_closed(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    inventory, recovery, recovery_sha, matrix, matrix_sha = _authority(tmp_path / "authority")
    monkeypatch.setattr(planner, "METHODS", planner.METHODS + ("FAKE_ALIAS",))
    with pytest.raises(planner.PlannerError, match="method_contract"):
        planner.plan_campaign(inventory, recovery, recovery_sha, matrix, matrix_sha, tmp_path / "out", "20260829T000000Z")
    monkeypatch.setattr(planner, "METHODS", ("RAW_II", "ZSTD_TU", "ZSTD_ROUTE", "P29", "GRZ_RESIDUAL", "ZSTD_COHORT", "ZSTD_GLOBAL"))
    monkeypatch.setattr(planner, "TOPOLOGIES", (dict(planner.TOPOLOGIES[0], stream_capacity_tus=999), planner.TOPOLOGIES[1]))
    with pytest.raises(planner.PlannerError, match="topology_contract"):
        planner.plan_campaign(inventory, recovery, recovery_sha, matrix, matrix_sha, tmp_path / "out2", "20260829T000000Z")


def test_output_is_deterministic_and_mutation_rejected(tmp_path: Path) -> None:
    result, output, inventory, recovery, recovery_sha = _plan(tmp_path)
    _, _, _, matrix, matrix_sha = _authority(tmp_path / "authority")
    before = (output / "campaign-index.json").read_bytes()
    planner.plan_campaign(inventory, recovery, recovery_sha, matrix, matrix_sha, output, "20260829T000000Z")
    assert (output / "campaign-index.json").read_bytes() == before
    (output / "descriptors.jsonl").write_bytes(b"tampered\n")
    with pytest.raises(planner.PlannerError, match="output:mutation"):
        planner.plan_campaign(inventory, recovery, recovery_sha, matrix, matrix_sha, output, "20260829T000000Z")


def test_current_image_arguments_are_explicit_and_fail_closed(tmp_path: Path) -> None:
    inventory, recovery, recovery_sha, matrix, matrix_sha = _authority(tmp_path / "authority")
    with pytest.raises(planner.PlannerError, match="together"):
        planner.plan_campaign(inventory, recovery, recovery_sha, matrix, matrix_sha, tmp_path / "out", "20260829T000000Z",
                              current_image_name="image:tag")
    with pytest.raises(planner.PlannerError, match="content_id_invalid"):
        planner.plan_campaign(inventory, recovery, recovery_sha, matrix, matrix_sha, tmp_path / "out2", "20260829T000000Z",
                              current_image_name="image:tag", current_image_id="not-a-digest")


def test_result_path_collision_fails_closed(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    inventory, recovery, recovery_sha, matrix, matrix_sha = _authority(tmp_path / "authority")
    monkeypatch.setattr(planner, "_slug", lambda _: "collision")
    with pytest.raises(planner.PlannerError, match="result_path:collision"):
        planner.plan_campaign(inventory, recovery, recovery_sha, matrix, matrix_sha,
                              tmp_path / "out", "20260829T000000Z")


def test_source_mutation_during_streamed_snapshot_read_fails_closed(tmp_path: Path,
                                                                    monkeypatch: pytest.MonkeyPatch) -> None:
    inventory, recovery, recovery_sha, matrix, matrix_sha = _authority(tmp_path / "authority")
    target = tmp_path / "authority" / "snapshots" / "corpus" / "tu-0000.ii"
    original_read = planner.os.read
    changed = False

    def read_and_mutate(fd: int, size: int) -> bytes:
        nonlocal changed
        value = original_read(fd, size)
        if not changed:
            changed = True
            target.write_bytes(target.read_bytes() + b"mutated")
        return value

    monkeypatch.setattr(planner.os, "read", read_and_mutate)
    with pytest.raises(planner.PlannerError, match="mutated_during_read"):
        planner.plan_campaign(inventory, recovery, recovery_sha, matrix, matrix_sha,
                              tmp_path / "out", "20260829T000000Z")


def test_authenticated_capability_manifest_enables_only_implemented_profiles(tmp_path: Path) -> None:
    inventory, recovery, recovery_sha, matrix, matrix_sha = _authority(tmp_path / "authority")
    capability = {
        "schema": planner.CAPABILITY_SCHEMA, "status": "PASS",
        "capability": planner.CAPABILITY, "producer_version": "native-live-fixture-v1",
        "source": {"path": "/source/producer.py", "commit": "a" * 40, "sha256": "b" * 64},
        "binaries": {name: {"path": f"/build/{name}", "bytes": 1, "sha256": "c" * 64}
                     for name in ("client", "daemon", "scheduler", "cache-service", "simulator")},
    }
    capability_path = tmp_path / "authority" / "capability.json"
    capability_path.write_bytes(planner._canonical(capability))
    result = planner.plan_campaign(
        inventory, recovery, recovery_sha, matrix, matrix_sha,
        tmp_path / "out", "20260829T000000Z",
        current_image_name="image:tag", current_image_id="sha256:" + "a" * 64,
        capability_manifest=capability_path,
        capability_manifest_sha256=hashlib.sha256(capability_path.read_bytes()).hexdigest())
    assert result["counts"]["current_image_ready"] == 704
