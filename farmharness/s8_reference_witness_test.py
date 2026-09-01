from __future__ import annotations

import json
import shutil
from pathlib import Path

import pytest

import s8_reference_witness as witness
import s8_external_farm_executor as executor


def _fixture(tmp_path: Path) -> tuple[dict[str, object], Path, Path, Path, Path, Path]:
    source = tmp_path / "src" / "N.cpp"
    source.parent.mkdir()
    source.write_bytes(b"int n() { return 50; }\n")
    payload = tmp_path / "payload.ii"
    payload.write_bytes(b"# 1 \"N.cpp\"\nint n() { return 50; }\n")
    output = tmp_path / "build" / "N.o"
    output.parent.mkdir()
    command = f"g++ -std=c++17 -O2 -c {source} -o {output}"
    compile_db = tmp_path / "compile_commands.json"
    compile_db.write_text(json.dumps([{"directory": str(source.parent), "file": str(source),
                                       "command": command}]) + "\n", encoding="utf-8")
    authority = tmp_path / "authority.json"
    authority.write_text(json.dumps({"schema": "authority-v1", "hosts": {
                              "q3": {"image": "image", "idle": {"status": "PASS"}},
                              "q2": {"image": "image", "idle": {"status": "PASS"}}}}) + "\n", encoding="utf-8")
    source_manifest = tmp_path / "source-manifest.json"
    source_manifest.write_text("{\"schema\":\"source-v1\"}\n", encoding="utf-8")
    plan = tmp_path / "plan.json"
    plan.write_text(json.dumps({"schema": witness.PLAN_SCHEMA,
                                "cell": {"corpus": "DuckDB", "profile": "ZSTD_ROUTE", "regime": "cold"},
                                "request": {"depth": 1},
                                "source_manifest": {"path": str(source_manifest),
                                                     "sha256": witness.descriptor(source_manifest, "fixture")["sha256"],
                                                     "bytes": source_manifest.stat().st_size},
                                "inputs": []}) + "\n", encoding="utf-8")
    row = {"tu_id": "tu-0", "source": str(source), "source_relative": "N.cpp",
           "sha256": witness.descriptor(source, "fixture")["sha256"],
           "predictive_input": {"ordinal": 0, "path": str(payload),
                                 "source_relative": "N.ii",
                                 "sha256": witness.descriptor(payload, "fixture")["sha256"],
                                 "bytes": payload.stat().st_size},
           "compile_db": str(compile_db),
           "compile_db_sha256": witness.descriptor(compile_db, "fixture")["sha256"],
           "compile_source": str(source), "compile_output": str(output)}
    batch = tmp_path / "batch.jsonl"
    batch.write_text(json.dumps(row) + "\n", encoding="utf-8")
    direct = tmp_path / "direct.o"; direct.write_bytes(b"object-bytes")
    remote = tmp_path / "remote.o"; remote.write_bytes(direct.read_bytes())
    return row, authority, plan, batch, direct, remote


def _make(tmp_path: Path):
    row, authority, plan, batch, direct, remote = _fixture(tmp_path)
    cell_output = tmp_path / "cell"
    evidence_dir = cell_output / "product-evidence"
    evidence_dir.mkdir(parents=True)
    retained_batch = evidence_dir / "batch-manifest.jsonl"
    retained_plan = evidence_dir / "predictive-plan.json"
    shutil.copy2(batch, retained_batch)
    shutil.copy2(plan, retained_plan)
    shutil.copy2(direct, evidence_dir / "local-full-1-0.o")
    shutil.copy2(remote, evidence_dir / "remote-full-1-0.o")
    cell = {"corpus": "DuckDB", "profile": "ZSTD_ROUTE", "regime": "cold"}
    image = {"reference": "image", "image_id": "sha256:" + "b" * 64,
             "architecture": "amd64", "os": "linux", "created": "now"}
    binaries = {"usr/bin/g++": "a" * 64}
    experiment = {"schema": "icecream-s7-live-experiment-v1", "cell": cell,
                  "runtime_image": image, "binary_sha256": binaries,
                  "source_commit": "c" * 40, "source_tree": "d" * 40,
                  "runner_sha256": "e" * 64, "remote_compile_required": True,
                  "execution_environment": "external_farm_product_build",
                  "artifact_retention": {"mode": "all"}, "depth": 1,
                  "topology": "single"}
    results_raw = b'{"status":"PASS"}\n'
    (cell_output / "results.jsonl").write_bytes(results_raw)
    remote_desc = witness.descriptor(evidence_dir / "remote-full-1-0.o", "fixture.remote")
    timing_raw = (json.dumps({"ordinal": 0, "tu_id": row["tu_id"], "cell": cell,
                              "remote_compile": True,
                              "object_sha256": remote_desc["sha256"],
                              "returned_object_bytes": remote_desc["bytes"]},
                             sort_keys=True, separators=(",", ":")) + "\n").encode()
    (cell_output / "timing.jsonl").write_bytes(timing_raw)
    batch_desc = witness.descriptor(retained_batch, "fixture.batch")
    plan_desc = witness.descriptor(retained_plan, "fixture.plan")
    evidence = {"schema": "icecream-s7-live-evidence-v2", "cell": cell,
                "source_commit": experiment["source_commit"],
                "source_tree": experiment["source_tree"],
                "runner": {"name": "p50compilee2e-run.sh", "sha256": "e" * 64},
                "remote_compile_required": True,
                "execution_environment": "external_farm_product_build",
                "runtime_image": image, "binary_sha256": binaries,
                "input_manifest": {"path": "product-evidence/batch-manifest.jsonl",
                                    "sha256": batch_desc["sha256"], "bytes": batch_desc["bytes"]},
                "predictive_plan": {"path": "product-evidence/predictive-plan.json",
                                    "sha256": plan_desc["sha256"], "bytes": plan_desc["bytes"]},
                "evidence": {"results": {"path": "results.jsonl",
                                           "sha256": witness.hashlib.sha256(results_raw).hexdigest(),
                                           "bytes": len(results_raw)},
                             "timing": {"path": "timing.jsonl",
                                        "sha256": witness.hashlib.sha256(timing_raw).hexdigest(),
                                        "bytes": len(timing_raw)}}}
    evidence["evidence_sha256"] = witness.hashlib.sha256(
        witness._canonical({k: v for k, v in evidence.items() if k != "evidence_sha256"})
    ).hexdigest()
    (cell_output / "evidence.json").write_bytes(witness._canonical(evidence) + b"\n")
    (cell_output / "experiment_manifest.json").write_bytes(witness._canonical(experiment) + b"\n")
    package = tmp_path / "witness"
    manifest = witness.create_from_cell(cell_output, authority=authority, package_dir=package)
    return row, authority, plan, batch, direct, remote, manifest


def test_create_and_reuse_resnapshots_authority_and_every_binding(tmp_path: Path) -> None:
    row, authority, plan, batch, _direct, remote, manifest = _make(tmp_path)
    # Idle observations are volatile, but the authority identity remains the
    # same. Reuse therefore records the new full authority snapshot safely.
    authority.write_text(authority.read_text(encoding="utf-8").replace('"status": "PASS"', '"status": "HOLD"'), encoding="utf-8")
    plan_data = witness.validate_reuse(manifest, batch_manifest=batch, predictive_plan=plan,
                                      authority=authority,
                                      image_identity={"reference": "image", "image_id": "sha256:" + "b" * 64,
                                                      "architecture": "amd64", "os": "linux", "created": "now"})
    assert plan_data["mode"] == "reuse"


def test_reuse_accepts_volatile_authority_and_rejects_remote_bytes(tmp_path: Path) -> None:
    row, authority, plan, batch, _direct, remote, manifest = _make(tmp_path)
    value = json.loads(authority.read_text(encoding="utf-8")); value["new_fact"] = "changed"
    authority.write_text(json.dumps(value) + "\n", encoding="utf-8")
    # The fixture authority has an explicitly volatile top-level idle field;
    # the stable projection only removes per-host observations, so this is a
    # deliberate fail-closed check for an unrecognized authority mutation.
    with pytest.raises(witness.ReferenceWitnessError, match="authority_identity_mismatch"):
        witness.validate_reuse(manifest, batch_manifest=batch, predictive_plan=plan,
                               authority=authority,
                               image_identity={"reference": "image", "image_id": "sha256:image",
                                               "architecture": "amd64", "os": "linux", "created": "now"})

    # Rebuild with the original authority and exercise the output comparator.
    authority.write_text(json.dumps({"schema": "authority-v1", "hosts": {
                              "q3": {"image": "image", "idle": {"status": "PASS"}},
                              "q2": {"image": "image", "idle": {"status": "PASS"}}}}) + "\n", encoding="utf-8")
    plan_data = witness.validate_reuse(manifest, batch_manifest=batch, predictive_plan=plan,
                                      authority=authority,
                                      image_identity={"reference": "image", "image_id": "sha256:" + "b" * 64,
                                                      "architecture": "amd64", "os": "linux", "created": "now"})
    assert witness.validate_remote_objects(plan_data, [remote])[0]["status"] == "PASS"
    remote.write_bytes(b"different")
    with pytest.raises(witness.ReferenceWitnessError, match="remote_object_mismatch"):
        witness.validate_remote_objects(plan_data, [remote])


def test_direct_remote_mismatch_never_publishes_a_package(tmp_path: Path) -> None:
    row, authority, plan, batch, direct, remote = _fixture(tmp_path)
    remote.write_bytes(b"not-the-reference")
    package = tmp_path / "witness"
    with pytest.raises(witness.ReferenceWitnessError, match="authenticated_cell_required"):
        witness.create_package(
            package, cell={"corpus": "DuckDB", "profile": "ZSTD_ROUTE", "regime": "cold", "depth": 1},
            batch_manifest=batch, predictive_plan=plan, authority=authority,
            product={"image": {"reference": "image", "image_id": "sha256:" + "b" * 64,
                                "architecture": "amd64", "os": "linux", "created": "now"},
                     "toolchain": {"sha256": "a" * 64, "bytes": 10}},
            rows=[row], direct_objects=[direct], remote_objects=[remote])
    assert not package.exists()


def test_synthetic_equal_files_and_fabricated_metadata_cannot_publish(tmp_path: Path) -> None:
    _row, authority, plan, batch, direct, remote = _fixture(tmp_path)
    package = tmp_path / "synthetic-witness"
    with pytest.raises(witness.ReferenceWitnessError, match="authenticated_cell_required"):
        witness.create_package(
            package, cell={"corpus": "DuckDB", "profile": "ZSTD_ROUTE", "regime": "cold", "depth": 1},
            batch_manifest=batch, predictive_plan=plan, authority=authority,
            product={"image": {"reference": "fabricated", "image_id": "sha256:" + "b" * 64,
                                "architecture": "amd64", "os": "linux", "created": "fabricated"},
                     "toolchain": {"sha256": "a" * 64, "bytes": 10}},
            rows=[_row], direct_objects=[direct], remote_objects=[remote])
    assert not package.exists()


def test_external_command_exposes_explicit_reuse_inputs(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(executor.live, "build_command", lambda *args, **kwargs: [
        "env", "ICECC_P50_C1F1_PASSES=1", "/tanksmall/unittests/p50compilee2e-run.sh"])
    image = {"reference": "image", "image_id": "sha256:" + "b" * 64,
             "architecture": "amd64", "os": "linux", "created": "now"}
    command = executor.build_external_command(
        Path("/tanksmall/batch.jsonl"), Path("/tanksmall/plan.json"),
        Path("/tanksmall/topology.json"), Path("/tanksmall/product"),
        profile="ZSTD_ROUTE", corpus="DuckDB", regime="cold", depth="100",
        suite="C1F1/100000", workdir=Path("/tmp/p50compilee2e.external"),
        timeout_seconds=900, reference_witness=Path("/tanksmall/witness/manifest.jsonl"),
        reference_authority=Path("/tanksmall/authority.json"), reference_image=image,
        reference_toolchain={"sha256": "c" * 64, "bytes": 42})
    assert "ICECC_P50_REFERENCE_WITNESS=/tanksmall/witness/manifest.jsonl" in command
    assert "ICECC_P50_REFERENCE_AUTHORITY=/tanksmall/authority.json" in command
    assert "ICECC_P50_REFERENCE_IMAGE_ID=sha256:" + "b" * 64 in command
    runner = command.index("/tanksmall/unittests/p50compilee2e-run.sh")
    assert all(command.index(item) < runner for item in command
               if item.startswith("ICECC_P50_REFERENCE_"))


def test_reuse_has_no_direct_compiler_fallback_and_package_failures_cleanup(tmp_path: Path) -> None:
    runner_source = (Path(witness.__file__).parents[1] / "unittests" /
                     "p50compilee2e-run.sh").read_text(encoding="utf-8")
    assert 'elif test "$reference_reuse" -eq 0 && test -n "$db_ref"; then' in runner_source
    assert 'elif test "$reference_reuse" -eq 0; then' in runner_source
    assert 'FAIL: reference reuse did not produce a witness object' in runner_source
    _row, authority, plan, batch, direct, remote = _fixture(tmp_path)
    package = tmp_path / "witness"
    remote.write_bytes(b"different")
    with pytest.raises(witness.ReferenceWitnessError):
        witness.create_package(
            package, cell={"corpus": "DuckDB", "profile": "ZSTD_ROUTE", "regime": "cold", "depth": 1},
            batch_manifest=batch, predictive_plan=plan, authority=authority,
            product={"image": {"reference": "image", "image_id": "sha256:" + "b" * 64,
                                "architecture": "amd64", "os": "linux", "created": "now"},
                     "toolchain": {"sha256": "a" * 64, "bytes": 10}},
            rows=[_row], direct_objects=[direct], remote_objects=[remote])
    assert not package.exists()


def test_stable_compiler_identity_is_content_bound_not_archive_bound() -> None:
    image = {"reference": "image", "image_id": "sha256:" + "b" * 64,
             "architecture": "amd64", "os": "linux", "created": "now"}
    first = witness.stable_toolchain_identity(image, {"usr/bin/g++": "a" * 64})
    second = witness.stable_toolchain_identity(image, {"usr/bin/g++": "a" * 64})
    changed = witness.stable_toolchain_identity(image, {"usr/bin/g++": "c" * 64})
    assert first == second
    assert first["sha256"] != changed["sha256"]
    assert "content" in first and "image" in first["content"]
