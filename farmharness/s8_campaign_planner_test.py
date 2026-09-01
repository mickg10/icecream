from __future__ import annotations

import hashlib
import json
import os
import subprocess
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


def _capability_fixture(tmp_path: Path) -> tuple[Path, str, dict[str, object]]:
    source_root = tmp_path / "producer-source"
    source_root.mkdir(parents=True)
    source_path = source_root / "producer.py"
    source_path.write_bytes(b"# authenticated native/live producer fixture\n")
    subprocess.run(["git", "init", "-q", str(source_root)], check=True)
    subprocess.run(["git", "-C", str(source_root), "config", "user.email", "fixture@example.invalid"], check=True)
    subprocess.run(["git", "-C", str(source_root), "config", "user.name", "fixture"], check=True)
    subprocess.run(["git", "-C", str(source_root), "add", "producer.py"], check=True)
    subprocess.run(["git", "-C", str(source_root), "commit", "-q", "-m", "fixture"], check=True)
    commit = subprocess.check_output(["git", "-C", str(source_root), "rev-parse", "HEAD"], text=True).strip()
    tree = subprocess.check_output(["git", "-C", str(source_root), "rev-parse", "HEAD^{tree}"], text=True).strip()
    binaries_root = tmp_path / "producer-binaries"
    binaries_root.mkdir()
    binaries = {}
    for name in ("client", "daemon", "scheduler", "cache-service", "simulator"):
        binary = binaries_root / name
        binary.write_bytes((name + "\n").encode())
        binary.chmod(0o755)
        raw = binary.read_bytes()
        binaries[name] = {"path": str(binary), "bytes": len(raw),
                          "sha256": hashlib.sha256(raw).hexdigest()}
    source_raw = source_path.read_bytes()
    capability = {
        "schema": planner.CAPABILITY_SCHEMA, "status": "PASS",
        "capability": planner.CAPABILITY, "producer_version": "native-live-fixture-v1",
        "source": {"path": str(source_path), "bytes": len(source_raw), "commit": commit,
                   "tree": tree, "sha256": hashlib.sha256(source_raw).hexdigest()},
        "binaries": binaries,
    }
    capability_path = tmp_path / "capability.json"
    capability_path.write_bytes(planner._canonical(capability))
    return capability_path, hashlib.sha256(capability_path.read_bytes()).hexdigest(), capability


def _raw_authority_fixture(tmp_path: Path, inventory: Path, *, rows: str = "full",
                           wrong_cell: bool = False) -> tuple[Path, str]:
    """Create one authenticated RAW_II corpus/regime authority entry."""
    tmp_path.mkdir(parents=True, exist_ok=True)
    inventory_value = json.loads(inventory.read_text())
    corpus = inventory_value["corpus_manifests"][0]
    manifest = Path(corpus["manifest"]["path"])
    source_rows = []
    for ordinal, source_path in enumerate(manifest.read_text().splitlines()):
        path = Path(source_path)
        raw = path.read_bytes()
        source_rows.append({
            "ordinal": ordinal,
            "source_relative": str(path.resolve().relative_to(manifest.parent.resolve())),
            "source_sha256": hashlib.sha256(raw).hexdigest(),
            "source_bytes": len(raw),
        })
    if rows == "missing":
        source_rows = source_rows[:-1]
    witness_rows = [{**row, "c_to_f": {"compile_file_bytes": 10, "file_chunk_bytes": 20,
                                       "end_bytes": 3, "total_bytes": 33}}
                    for row in source_rows]
    engine_rows = [{**row, "f_to_c_bytes": 17, "elapsed_ns": 100}
                   for row in source_rows]
    if rows == "duplicate":
        witness_rows.append(witness_rows[0])
    producer_corpus = planner._producer_corpus_for_manifest({
        "manifest_id": corpus["manifest_id"], "project": corpus["project"],
        "tu_count": corpus["tu_count"]})
    assert producer_corpus is not None
    cell = {"corpus": producer_corpus, "profile": "RAW_II", "regime": "cold"}
    file_cell = dict(cell)
    if wrong_cell:
        file_cell["regime"] = "warm"
    witness_value = {"schema": planner.RAW_II_WITNESS_SCHEMA,
                     "semantics": planner.RAW_II_SEMANTICS, "cell": file_cell,
                     "split": "held_out_validation",
                     "formula": planner.RAW_II_FORMULA, "rows": witness_rows}
    engine_value = {"schema": planner.RAW_II_ENGINE_SCHEMA,
                    "semantics": planner.RAW_II_SEMANTICS, "cell": file_cell,
                    "split": "held_out_validation",
                    "control_baseline": planner.RAW_II_BASELINE,
                    "engine_scope": "raw_ii_control_engine", "model_id": "fixture-v1",
                    "rows": engine_rows}
    witness_path = tmp_path / "witness.json"
    engine_path = tmp_path / "engine.json"
    witness_path.write_bytes(planner._canonical(witness_value))
    engine_path.write_bytes(planner._canonical(engine_value))
    source_root = tmp_path / "producer-source"
    source_root.mkdir()
    source_path = source_root / planner.RAW_II_PRODUCER_SOURCE
    source_path.write_text("# dedicated RAW_II producer fixture\n")
    subprocess.run(["git", "init", "-q", str(source_root)], check=True)
    subprocess.run(["git", "-C", str(source_root), "config", "user.email", "fixture@example.invalid"], check=True)
    subprocess.run(["git", "-C", str(source_root), "config", "user.name", "fixture"], check=True)
    subprocess.run(["git", "-C", str(source_root), "add", source_path.name], check=True)
    subprocess.run(["git", "-C", str(source_root), "commit", "-q", "-m", "fixture"], check=True)
    source_raw = source_path.read_bytes()
    source = {"path": str(source_path), "bytes": len(source_raw),
              "sha256": hashlib.sha256(source_raw).hexdigest(),
              "commit": subprocess.check_output(["git", "-C", str(source_root), "rev-parse", "HEAD"], text=True).strip(),
              "tree": subprocess.check_output(["git", "-C", str(source_root), "rev-parse", "HEAD^{tree}"], text=True).strip()}
    def descriptor(path: Path) -> dict[str, object]:
        raw = path.read_bytes()
        return {"path": str(path), "bytes": len(raw), "sha256": hashlib.sha256(raw).hexdigest()}
    authority = {"schema": planner.RAW_II_AUTHORITY_SCHEMA, "status": "PASS",
                 "capability": planner.RAW_II_CAPABILITY,
                 "producer": "farmharness.s8_raw_ii_predictive_producer",
                 "producer_source": source,
                 "cells": [{"cell": cell, "witness": descriptor(witness_path),
                            "engine": descriptor(engine_path)}]}
    authority_path = tmp_path / "raw-ii-authority.json"
    authority_path.write_bytes(planner._canonical(authority))
    return authority_path, hashlib.sha256(authority_path.read_bytes()).hexdigest()


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
        "IMPLEMENTED", "IMPLEMENTED", "IMPLEMENTED", "IMPLEMENTED", "IMPLEMENTED",
        "NOT_IMPLEMENTED", "NOT_IMPLEMENTED"]
    raw_method = result["methods"][0]
    assert raw_method["arm_kind"] == "control_baseline"
    assert raw_method["required_inputs"] == [
        "raw_ii_legacy_wire_witness", "raw_ii_engine_template"]
    assert result["topologies"][0]["stream_capacity_tus"] == 100000
    assert result["topologies"][1]["global_execution_slots"] == 40
    assert result["topologies"][1]["stream_capacity_tus"] is None
    assert result["depths"] == ["100", "200", "full-1", "state-carrying full-2"]
    assert result["counts"]["historical_image_seven_method_grid"] == 4928
    assert result["counts"]["current_image_implemented_subset"] == 880
    assert result["counts"]["descriptor_status_counts"] == {
        "READY": 0, "NOT_READY": 1232, "MISSING_EXTERNAL_AUTHORITY": 4928}
    assert result["counts"]["current_image_ready"] == 0
    lines = (output / "descriptors.jsonl").read_text().splitlines()
    assert all("docker run" not in line and "simulator" not in line for line in lines)
    current_implemented = [json.loads(line) for line in lines
                           if json.loads(line)["image"]["key"] == "current-pinned"
                           and json.loads(line)["method"] in planner.IMPLEMENTED_ROOT_METHODS]
    assert len(current_implemented) == 880
    assert {item["status"] for item in current_implemented} == {"NOT_READY"}
    assert {item["reason"] for item in current_implemented} == {
        "required_producer_capability_not_integrated_on_planner_source",
        "raw_ii_control_authority_not_integrated_on_planner_source"}
    assert result["corpus_authority"]["snapshots"]["count"] == 8261
    first = json.loads(lines[0])
    assert first["arm_kind"] == "control_baseline"
    assert first["required_inputs"] == [
        "raw_ii_legacy_wire_witness", "raw_ii_engine_template"]
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
    capability_path, capability_sha, _ = _capability_fixture(tmp_path / "capability-authority")
    result = planner.plan_campaign(
        inventory, recovery, recovery_sha, matrix, matrix_sha,
        tmp_path / "out", "20260829T000000Z",
        current_image_name="image:tag", current_image_id="sha256:" + "a" * 64,
        capability_manifest=capability_path,
        capability_manifest_sha256=capability_sha)
    assert result["counts"]["current_image_ready"] == 704


def test_capability_source_and_binary_descriptors_are_authenticated(tmp_path: Path) -> None:
    capability_path, capability_sha, capability = _capability_fixture(tmp_path)
    loaded = planner._load_capability(capability_path, capability_sha)
    assert loaded["source"]["git"]["head"] == capability["source"]["commit"]
    assert loaded["source"]["git"]["tree"] == capability["source"]["tree"]
    assert loaded["source"]["git"]["tracked_path"] == "producer.py"
    assert set(loaded["binaries"]) == {"client", "daemon", "scheduler", "cache-service", "simulator"}


def test_capability_missing_binary_fails_closed(tmp_path: Path) -> None:
    capability_path, capability_sha, capability = _capability_fixture(tmp_path)
    Path(capability["binaries"]["simulator"]["path"]).unlink()
    with pytest.raises(planner.PlannerError, match="binary:simulator:unavailable"):
        planner._load_capability(capability_path, capability_sha)


def test_capability_mutated_binary_fails_closed(tmp_path: Path) -> None:
    capability_path, capability_sha, capability = _capability_fixture(tmp_path)
    binary = Path(capability["binaries"]["daemon"]["path"])
    binary.write_bytes(binary.read_bytes() + b"mutated")
    with pytest.raises(planner.PlannerError, match="binary_(bytes|sha256)_mismatch:daemon"):
        planner._load_capability(capability_path, capability_sha)


def test_capability_mutated_source_fails_closed(tmp_path: Path) -> None:
    capability_path, capability_sha, capability = _capability_fixture(tmp_path)
    source = Path(capability["source"]["path"])
    source.write_bytes(source.read_bytes() + b"mutated")
    with pytest.raises(planner.PlannerError, match="source_(bytes|sha256)_mismatch"):
        planner._load_capability(capability_path, capability_sha)


def test_capability_path_swap_fails_closed(tmp_path: Path) -> None:
    capability_path, capability_sha, capability = _capability_fixture(tmp_path)
    binary = Path(capability["binaries"]["scheduler"]["path"])
    replacement = binary.with_name("replacement")
    replacement.write_bytes(b"replacement\n")
    binary.unlink()
    binary.symlink_to(replacement)
    with pytest.raises(planner.PlannerError, match="binary:scheduler:(unavailable|not_private_regular_file)"):
        planner._load_capability(capability_path, capability_sha)


def test_raw_ii_authority_makes_only_exact_cells_ready(tmp_path: Path) -> None:
    inventory, recovery, recovery_sha, matrix, matrix_sha = _authority(tmp_path / "authority")
    authority, authority_sha = _raw_authority_fixture(tmp_path / "raw", inventory)
    result = planner.plan_campaign(
        inventory, recovery, recovery_sha, matrix, matrix_sha, tmp_path / "out",
        "20260829T000000Z", current_image_name="image:tag",
        current_image_id="sha256:" + "a" * 64,
        raw_ii_authority_manifest=authority,
        raw_ii_authority_manifest_sha256=authority_sha)
    raw_ready = [json.loads(line) for line in (tmp_path / "out" / "descriptors.jsonl").read_text().splitlines()
                 if json.loads(line)["image"]["key"] == "current-pinned" and
                 json.loads(line)["method"] == "RAW_II" and json.loads(line)["status"] == "READY"]
    assert len(raw_ready) == 8  # one corpus/regime, two topologies, four depths
    assert {item["producer_capability"] for item in raw_ready} == {planner.RAW_II_CAPABILITY}
    assert result["counts"]["current_image_ready"] == 8
    assert planner.RAW_II_CAPABILITY != planner.CAPABILITY


def test_raw_ii_authority_missing_cell_stays_not_ready(tmp_path: Path) -> None:
    inventory, recovery, recovery_sha, matrix, matrix_sha = _authority(tmp_path / "authority")
    authority, authority_sha = _raw_authority_fixture(tmp_path / "raw", inventory)
    planner.plan_campaign(
        inventory, recovery, recovery_sha, matrix, matrix_sha, tmp_path / "out",
        "20260829T000000Z", current_image_name="image:tag",
        current_image_id="sha256:" + "a" * 64,
        raw_ii_authority_manifest=authority,
        raw_ii_authority_manifest_sha256=authority_sha)
    raw = [json.loads(line) for line in (tmp_path / "out" / "descriptors.jsonl").read_text().splitlines()
           if json.loads(line)["image"]["key"] == "current-pinned" and json.loads(line)["method"] == "RAW_II"]
    assert {item["status"] for item in raw} == {"READY", "NOT_READY"}
    assert any(item["reason"] == "raw_ii_exact_cell_inputs_unavailable" for item in raw)


@pytest.mark.parametrize("fixture_kwargs,match", [
    ({"wrong_cell": True}, "scope_invalid"),
    ({"rows": "missing"}, "coverage_incomplete"),
    ({"rows": "duplicate"}, "duplicate_occurrence"),
])
def test_raw_ii_authority_rejects_wrong_or_incomplete_inputs(tmp_path: Path,
                                                              fixture_kwargs: dict[str, object],
                                                              match: str) -> None:
    inventory, recovery, recovery_sha, matrix, matrix_sha = _authority(tmp_path / "authority")
    authority, authority_sha = _raw_authority_fixture(tmp_path / "raw", inventory, **fixture_kwargs)
    with pytest.raises(planner.PlannerError, match=match):
        planner.plan_campaign(
            inventory, recovery, recovery_sha, matrix, matrix_sha, tmp_path / "out",
            "20260829T000000Z", current_image_name="image:tag",
            current_image_id="sha256:" + "a" * 64,
            raw_ii_authority_manifest=authority,
            raw_ii_authority_manifest_sha256=authority_sha)


def test_raw_ii_authority_mutated_file_fails_closed(tmp_path: Path) -> None:
    inventory, recovery, recovery_sha, matrix, matrix_sha = _authority(tmp_path / "authority")
    authority, authority_sha = _raw_authority_fixture(tmp_path / "raw", inventory)
    value = json.loads(authority.read_text())
    witness = Path(value["cells"][0]["witness"]["path"])
    witness.write_bytes(witness.read_bytes() + b"mutated")
    with pytest.raises(planner.PlannerError, match="witness_(bytes|sha256)_mismatch"):
        planner.plan_campaign(
            inventory, recovery, recovery_sha, matrix, matrix_sha, tmp_path / "out",
            "20260829T000000Z", current_image_name="image:tag",
            current_image_id="sha256:" + "a" * 64,
            raw_ii_authority_manifest=authority,
            raw_ii_authority_manifest_sha256=authority_sha)


@pytest.mark.parametrize(("kind", "field", "match"), [
    ("witness", "split", "scope_invalid"),
    ("engine", "split", "scope_invalid"),
    ("engine", "model_id", "model_id_invalid"),
])
def test_raw_ii_authority_matches_producer_contract(tmp_path: Path, kind: str,
                                                     field: str, match: str) -> None:
    inventory, recovery, recovery_sha, matrix, matrix_sha = _authority(tmp_path / "authority")
    authority, authority_sha = _raw_authority_fixture(tmp_path / "raw", inventory)
    value = json.loads(authority.read_text())
    file_path = Path(value["cells"][0][kind]["path"])
    file_value = json.loads(file_path.read_text())
    file_value.pop(field)
    file_path.write_bytes(planner._canonical(file_value))
    value["cells"][0][kind] = {
        "path": str(file_path), "bytes": file_path.stat().st_size,
        "sha256": hashlib.sha256(file_path.read_bytes()).hexdigest(),
    }
    authority.write_bytes(planner._canonical(value))
    with pytest.raises(planner.PlannerError, match=match):
        planner.plan_campaign(
            inventory, recovery, recovery_sha, matrix, matrix_sha, tmp_path / "out",
            "20260829T000000Z", current_image_name="image:tag",
            current_image_id="sha256:" + "a" * 64,
            raw_ii_authority_manifest=authority,
            raw_ii_authority_manifest_sha256=hashlib.sha256(authority.read_bytes()).hexdigest())


def test_raw_ii_authority_rejects_witness_engine_split_substitution(tmp_path: Path) -> None:
    inventory, recovery, recovery_sha, matrix, matrix_sha = _authority(tmp_path / "authority")
    authority, _ = _raw_authority_fixture(tmp_path / "raw", inventory)
    value = json.loads(authority.read_text())
    engine_path = Path(value["cells"][0]["engine"]["path"])
    engine_value = json.loads(engine_path.read_text())
    engine_value["split"] = "calibration"
    engine_path.write_bytes(planner._canonical(engine_value))
    value["cells"][0]["engine"] = {
        "path": str(engine_path), "bytes": engine_path.stat().st_size,
        "sha256": hashlib.sha256(engine_path.read_bytes()).hexdigest(),
    }
    authority.write_bytes(planner._canonical(value))
    with pytest.raises(planner.PlannerError, match="scope_invalid"):
        planner.plan_campaign(
            inventory, recovery, recovery_sha, matrix, matrix_sha, tmp_path / "out",
            "20260829T000000Z", current_image_name="image:tag",
            current_image_id="sha256:" + "a" * 64,
            raw_ii_authority_manifest=authority,
            raw_ii_authority_manifest_sha256=hashlib.sha256(authority.read_bytes()).hexdigest())


@pytest.mark.parametrize("mutation", ["unexpected", "non_finite"])
def test_raw_ii_authority_top_level_is_exact_and_finite(tmp_path: Path,
                                                        mutation: str) -> None:
    inventory, recovery, recovery_sha, matrix, matrix_sha = _authority(tmp_path / "authority")
    authority, _ = _raw_authority_fixture(tmp_path / "raw", inventory)
    value = json.loads(authority.read_text())
    if mutation == "unexpected":
        value["unexpected"] = True
        match = "schema_or_status_invalid"
        authority.write_bytes(planner._canonical(value))
    else:
        value["status"] = float("nan")
        match = "non_finite_json"
        authority.write_text(json.dumps(value, sort_keys=True) + "\n")
    with pytest.raises(planner.PlannerError, match=match):
        planner.plan_campaign(
            inventory, recovery, recovery_sha, matrix, matrix_sha, tmp_path / "out",
            "20260829T000000Z", current_image_name="image:tag",
            current_image_id="sha256:" + "a" * 64,
            raw_ii_authority_manifest=authority,
            raw_ii_authority_manifest_sha256=hashlib.sha256(authority.read_bytes()).hexdigest())


def test_raw_ii_authority_top_level_symlink_fails_closed(tmp_path: Path) -> None:
    inventory, recovery, recovery_sha, matrix, matrix_sha = _authority(tmp_path / "authority")
    authority, _ = _raw_authority_fixture(tmp_path / "raw", inventory)
    replacement = authority.with_name("authority-copy.json")
    replacement.write_bytes(authority.read_bytes())
    authority.unlink()
    authority.symlink_to(replacement)
    with pytest.raises(planner.PlannerError, match="raw_ii_authority:not_private_regular_file"):
        planner.plan_campaign(
            inventory, recovery, recovery_sha, matrix, matrix_sha, tmp_path / "out",
            "20260829T000000Z", current_image_name="image:tag",
            current_image_id="sha256:" + "a" * 64,
            raw_ii_authority_manifest=authority,
            raw_ii_authority_manifest_sha256="0" * 64)


def test_raw_ii_authority_deleted_top_level_fails_closed(tmp_path: Path) -> None:
    inventory, recovery, recovery_sha, matrix, matrix_sha = _authority(tmp_path / "authority")
    authority, authority_sha = _raw_authority_fixture(tmp_path / "raw", inventory)
    authority.unlink()
    with pytest.raises(planner.PlannerError, match="raw_ii_authority:unavailable"):
        planner.plan_campaign(
            inventory, recovery, recovery_sha, matrix, matrix_sha, tmp_path / "out",
            "20260829T000000Z", current_image_name="image:tag",
            current_image_id="sha256:" + "a" * 64,
            raw_ii_authority_manifest=authority,
            raw_ii_authority_manifest_sha256=authority_sha)


@pytest.mark.parametrize("kind", ["witness", "engine"])
def test_raw_ii_authority_deleted_cell_artifact_fails_closed(tmp_path: Path,
                                                              kind: str) -> None:
    inventory, recovery, recovery_sha, matrix, matrix_sha = _authority(tmp_path / "authority")
    authority, authority_sha = _raw_authority_fixture(tmp_path / "raw", inventory)
    value = json.loads(authority.read_text())
    Path(value["cells"][0][kind]["path"]).unlink()
    with pytest.raises(planner.PlannerError, match=f"raw_ii_{kind}:unavailable"):
        planner.plan_campaign(
            inventory, recovery, recovery_sha, matrix, matrix_sha, tmp_path / "out",
            "20260829T000000Z", current_image_name="image:tag",
            current_image_id="sha256:" + "a" * 64,
            raw_ii_authority_manifest=authority,
            raw_ii_authority_manifest_sha256=authority_sha)


def test_raw_ii_authority_unmapped_manifest_cannot_be_ready(tmp_path: Path) -> None:
    inventory, recovery, recovery_sha, matrix, matrix_sha = _authority(tmp_path / "authority")
    authority, _ = _raw_authority_fixture(tmp_path / "raw", inventory)
    value = json.loads(authority.read_text())
    value["cells"][0]["cell"]["corpus"] = "corpus4"
    authority.write_bytes(planner._canonical(value))
    with pytest.raises(planner.PlannerError, match="cell_scope_invalid"):
        planner.plan_campaign(
            inventory, recovery, recovery_sha, matrix, matrix_sha, tmp_path / "out",
            "20260829T000000Z", current_image_name="image:tag",
            current_image_id="sha256:" + "a" * 64,
            raw_ii_authority_manifest=authority,
            raw_ii_authority_manifest_sha256=hashlib.sha256(authority.read_bytes()).hexdigest())
    assert planner._producer_corpus_for_manifest({
        "manifest_id": "corpus4", "project": "abseil+protobuf", "tu_count": 700}) is None


def test_raw_ii_manifest_mapping_matches_shared_producer_contract() -> None:
    expected = {
        "corpus": "LLVM-1238", "corpus2": "RocksDB",
        "corpus3": "DuckDB", "corpus7": "fmt",
    }
    observed = {
        manifest_id: planner._producer_corpus_for_manifest({
            "manifest_id": manifest_id, "project": project, "tu_count": count})
        for manifest_id, project, count in planner.CORPUS_AUTHORITY
    }
    assert {key: observed[key] for key in expected} == expected
    assert all(observed[key] is None for key in observed if key not in expected)
    assert set(expected.values()) == set(planner.RAW_II_SUPPORTED_CORPORA)
    assert set(expected) == {"corpus", "corpus2", "corpus3", "corpus7"}


def test_raw_ii_authority_validates_12490_occurrences_without_quadratic_scan(
        tmp_path: Path) -> None:
    count = 12490
    digest = "1" * 64
    expected = {
        (ordinal, f"tu-{ordinal:05d}.cc", digest, 1): {
            "ordinal": ordinal, "source_relative": f"tu-{ordinal:05d}.cc",
            "sha256": digest, "bytes": 1,
        }
        for ordinal in range(count)
    }
    rows = [{
        "ordinal": ordinal, "source_relative": f"tu-{ordinal:05d}.cc",
        "source_sha256": digest, "source_bytes": 1,
        "c_to_f": {"compile_file_bytes": 1, "file_chunk_bytes": 1,
                   "end_bytes": 1, "total_bytes": 3},
    } for ordinal in range(count)]
    value = {"schema": planner.RAW_II_WITNESS_SCHEMA,
             "semantics": planner.RAW_II_SEMANTICS,
             "cell": {"corpus": "fmt", "profile": "RAW_II", "regime": "cold"},
             "split": "calibration", "formula": planner.RAW_II_FORMULA,
             "rows": rows}
    path = tmp_path / "large-witness.json"
    path.write_bytes(planner._canonical(value))
    descriptor = {"path": str(path), "bytes": path.stat().st_size,
                  "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
    result = planner._raw_ii_cell_file(
        descriptor, value["cell"], "calibration", expected, "witness")
    assert result["coverage"] == count


@pytest.mark.parametrize(("mutation", "match"), [
    ("commit", "source_identity_invalid"),
    ("path", "producer_source_invalid"),
])
def test_raw_ii_authority_malformed_source_is_planner_error(tmp_path: Path,
                                                             mutation: str, match: str) -> None:
    inventory, recovery, recovery_sha, matrix, matrix_sha = _authority(tmp_path / "authority")
    authority, _ = _raw_authority_fixture(tmp_path / "raw", inventory)
    value = json.loads(authority.read_text())
    if mutation == "commit":
        value["producer_source"].pop("commit")
    else:
        value["producer_source"].pop("path")
    authority.write_bytes(planner._canonical(value))
    with pytest.raises(planner.PlannerError, match=match):
        planner.plan_campaign(
            inventory, recovery, recovery_sha, matrix, matrix_sha, tmp_path / "out",
            "20260829T000000Z", current_image_name="image:tag",
            current_image_id="sha256:" + "a" * 64,
            raw_ii_authority_manifest=authority,
            raw_ii_authority_manifest_sha256=hashlib.sha256(authority.read_bytes()).hexdigest())


@pytest.mark.parametrize("mutation", ["witness_descriptor", "cell_type"])
def test_raw_ii_authority_malformed_nested_types_are_planner_error(
        tmp_path: Path, mutation: str) -> None:
    inventory, recovery, recovery_sha, matrix, matrix_sha = _authority(tmp_path / "authority")
    authority, _ = _raw_authority_fixture(tmp_path / "raw", inventory)
    value = json.loads(authority.read_text())
    if mutation == "witness_descriptor":
        value["cells"][0]["witness"] = None
        match = "cell_file_descriptor_invalid"
    else:
        value["cells"][0]["cell"]["corpus"] = []
        match = "cell_scope_invalid"
    authority.write_bytes(planner._canonical(value))
    with pytest.raises(planner.PlannerError, match=match):
        planner.plan_campaign(
            inventory, recovery, recovery_sha, matrix, matrix_sha, tmp_path / "out",
            "20260829T000000Z", current_image_name="image:tag",
            current_image_id="sha256:" + "a" * 64,
            raw_ii_authority_manifest=authority,
            raw_ii_authority_manifest_sha256=hashlib.sha256(authority.read_bytes()).hexdigest())


def test_raw_ii_authority_symlink_swap_and_in_place_mutation_fail_closed(
        tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    inventory, recovery, recovery_sha, matrix, matrix_sha = _authority(tmp_path / "authority")
    authority, _ = _raw_authority_fixture(tmp_path / "raw", inventory)
    value = json.loads(authority.read_text())
    target = Path(value["cells"][0]["witness"]["path"])
    replacement = target.with_name("replacement.json")
    replacement.write_bytes(target.read_bytes())
    target.unlink()
    target.symlink_to(replacement)
    with pytest.raises(planner.PlannerError, match="raw_ii_witness:not_private_regular_file"):
        planner.plan_campaign(
            inventory, recovery, recovery_sha, matrix, matrix_sha, tmp_path / "out-symlink",
            "20260829T000000Z", current_image_name="image:tag",
            current_image_id="sha256:" + "a" * 64,
            raw_ii_authority_manifest=authority,
            raw_ii_authority_manifest_sha256=hashlib.sha256(authority.read_bytes()).hexdigest())

    authority, _ = _raw_authority_fixture(tmp_path / "raw2", inventory)
    value = json.loads(authority.read_text())
    target = Path(value["cells"][0]["witness"]["path"])
    original_open = planner.os.open
    original_read = planner.os.read
    opened: dict[int, Path] = {}
    changed = False

    def tracked_open(path: object, flags: int, *args: object) -> int:
        fd = original_open(path, flags, *args)
        opened[fd] = Path(path)
        return fd

    def mutate_read(fd: int, size: int) -> bytes:
        nonlocal changed
        result = original_read(fd, size)
        if not changed and opened.get(fd) == target and result:
            changed = True
            target.write_bytes(target.read_bytes() + b"mutated")
        return result

    monkeypatch.setattr(planner.os, "open", tracked_open)
    monkeypatch.setattr(planner.os, "read", mutate_read)
    with pytest.raises(planner.PlannerError,
                       match="raw_ii_witness:(mutated_during_read|not_private_regular_file)"):
        planner.plan_campaign(
            inventory, recovery, recovery_sha, matrix, matrix_sha, tmp_path / "out-mutated",
            "20260829T000000Z", current_image_name="image:tag",
            current_image_id="sha256:" + "a" * 64,
            raw_ii_authority_manifest=authority,
            raw_ii_authority_manifest_sha256=hashlib.sha256(authority.read_bytes()).hexdigest())


def test_raw_ii_authority_swap_after_open_fails_closed(
        tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    inventory, recovery, recovery_sha, matrix, matrix_sha = _authority(tmp_path / "authority")
    authority, _ = _raw_authority_fixture(tmp_path / "raw", inventory)
    value = json.loads(authority.read_text())
    target = Path(value["cells"][0]["witness"]["path"])
    replacement = target.with_name("replacement.json")
    replacement.write_bytes(target.read_bytes())
    original_open = planner.os.open
    swapped = False

    def swap_after_open(path: object, flags: int, *args: object) -> int:
        nonlocal swapped
        fd = original_open(path, flags, *args)
        if not swapped and Path(path) == target:
            swapped = True
            os.replace(replacement, target)
        return fd

    monkeypatch.setattr(planner.os, "open", swap_after_open)
    with pytest.raises(planner.PlannerError,
                       match="raw_ii_witness:(mutated_during_read|not_private_regular_file)"):
        planner.plan_campaign(
            inventory, recovery, recovery_sha, matrix, matrix_sha, tmp_path / "out",
            "20260829T000000Z", current_image_name="image:tag",
            current_image_id="sha256:" + "a" * 64,
            raw_ii_authority_manifest=authority,
            raw_ii_authority_manifest_sha256=hashlib.sha256(authority.read_bytes()).hexdigest())


def test_raw_ii_source_mutation_after_git_check_fails_closed(
        tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    capability_path, capability_sha, capability = _capability_fixture(tmp_path)
    source = Path(capability["source"]["path"])
    original = planner._git_output

    def mutate_after_status(repo: Path, args: list[str], label: str) -> str:
        result = original(repo, args, label)
        if label == "source_git_status_unavailable":
            source.write_bytes(source.read_bytes() + b"mutated-after-git-check")
        return result

    monkeypatch.setattr(planner, "_git_output", mutate_after_status)
    with pytest.raises(planner.PlannerError, match="source_mutated_during_authentication"):
        planner._load_capability(capability_path, capability_sha)
