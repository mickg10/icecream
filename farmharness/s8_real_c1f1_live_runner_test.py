from __future__ import annotations

import hashlib
import json
import os
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
    payload_root = tmp_path / "translation-units"
    payload_root.mkdir(exist_ok=True)
    for index in range(count):
        source = payload_root / f"tu-{index:03d}.ii"
        raw = f"int duck_{index}() {{ return {index}; }}\n".encode()
        source.write_bytes(raw)
        source_relative = f"translation-units/tu-{index:03d}.ii"
        rows.append({"tu_id": f"duck-tu-{index:03d}", "source": str(source),
                     "source_relative": source_relative,
                     "sha256": hashlib.sha256(raw).hexdigest(),
                     "predictive_input": {"ordinal": index, "path": str(source),
                                          "source_relative": source_relative,
                                          "sha256": hashlib.sha256(raw).hexdigest(),
                                          "bytes": len(raw)}})
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


def _plan(tmp_path: Path, batch: Path, count: int = 100, *, profile: str = "ZSTD_TU",
          regime: str = "cold") -> Path:
    rows = [json.loads(line) for line in batch.read_text().splitlines()]
    source_manifest = tmp_path / "source-manifest.txt"
    source_manifest.write_text("".join(row["source"] + "\n" for row in rows))
    source_raw = source_manifest.read_bytes()
    inputs = [row["predictive_input"] for row in rows]
    value = {"schema": "icecream-s8-depth-run-plan-v1", "semantics": "s8-current-semantics-v1",
             "cell": {"corpus": "DuckDB", "profile": profile, "regime": regime},
             "split": "held_out_validation", "request": {"depth": count, "requested_curve_points": count},
             "source_manifest": {"path": str(source_manifest), "sha256": hashlib.sha256(source_raw).hexdigest(),
                                 "bytes": len(source_raw), "entries": count},
             "source_root": str(tmp_path),
             "inputs": inputs}
    path = tmp_path / "predictive-plan.json"
    path.write_text(json.dumps(value, sort_keys=True))
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
    rows[0]["predictive_input"]["bytes"] += 1
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


def test_predictive_plan_path_swap_is_rejected(tmp_path: Path) -> None:
    batch = _batch(tmp_path)
    plan_path = _plan(tmp_path, batch)
    _plan_value, plan_inputs, _sha = runner.load_predictive_plan(
        plan_path, corpus="DuckDB", profile="ZSTD_TU", regime="cold", depth="100")
    rows = runner.load_batch_manifest(batch)
    rows[0]["predictive_input"], rows[1]["predictive_input"] = (
        rows[1]["predictive_input"], rows[0]["predictive_input"])
    with pytest.raises(runner.LiveRunnerError, match="batch_input_mismatch:0"):
        runner.bind_batch_to_plan(rows, plan_inputs)


def test_predictive_plan_source_relative_mutation_is_rejected(tmp_path: Path) -> None:
    batch = _batch(tmp_path)
    plan_path = _plan(tmp_path, batch)
    value = json.loads(plan_path.read_text())
    value["inputs"][0]["source_relative"] = "other/tu-000.ii"
    plan_path.write_text(json.dumps(value, sort_keys=True))
    with pytest.raises(runner.LiveRunnerError, match="input_root_mismatch:0"):
        runner.load_predictive_plan(plan_path, corpus="DuckDB", profile="ZSTD_TU",
                                    regime="cold", depth="100")


def _action_row(actor: str, seq: int, *, history: int = 9) -> dict[str, object]:
    digest = "a" * 32
    return {
        "action": "TX_BEGIN", "actor": actor, "c_store_guid": "b" * 32,
        "f_store_guid": "c" * 32, "previous_f_store_guid": "d" * 32,
        "session_serial": 1, "history_nonce": history, "rel_seq": seq,
        "tu_seq": seq, "transaction_digest": digest, "raw_digest": "e" * 32,
        "state_digest": "f" * 32, "content_digest": "1" * 32, "key64": 1,
        "need_keys": [], "remaining_need": 0, "duplicate": False, "stage_bytes": 17,
    }


def test_warm_prewarm_trace_requires_continuing_product_state(tmp_path: Path) -> None:
    prewarm_c = tmp_path / "pre-c.jsonl"
    prewarm_f = tmp_path / "pre-f.jsonl"
    measured_c = tmp_path / "measured-c.jsonl"
    measured_f = tmp_path / "measured-f.jsonl"
    prewarm_c.write_text(json.dumps(_action_row("C", 0)) + "\n")
    prewarm_f.write_text(json.dumps(_action_row("F", 0)) + "\n")
    measured_c.write_text(json.dumps(_action_row("C", 1)) + "\n")
    measured_f.write_text(json.dumps(_action_row("F", 1)) + "\n")
    prewarm = runner._action_stage_paths(prewarm_c, prewarm_f, 1)
    measured = runner._action_stage_paths(measured_c, measured_f, 1)
    runner._validate_warm_continuation(prewarm, measured, 1)
    measured[0]["history_nonce"] += 1
    with pytest.raises(runner.LiveRunnerError, match="does_not_continue_prewarm"):
        runner._validate_warm_continuation(prewarm, measured, 1)


def test_per_tu_profile_evidence_is_required(tmp_path: Path) -> None:
    log = tmp_path / "client-compile-full-1-0.log"
    log.write_text("P29 CACHE_SESSION\n")
    (tmp_path / "f-0.log").write_text("CACHE_SESSION\n")
    observation = [{"run": "full-1", "ordinal": 0, "relationship": 0}]
    runner._validate_product_log_evidence(tmp_path, observation, "P29")
    log.write_text("CACHE_SESSION\n")
    with pytest.raises(runner.LiveRunnerError, match="profile_evidence_missing"):
        runner._validate_product_log_evidence(tmp_path, observation, "P29")


@pytest.mark.parametrize("profile", ["ZSTD_TU", "ZSTD_ROUTE", "P29", "GRZ_RESIDUAL"])
def test_dry_run_command_targets_real_single_lifecycle_for_all_profiles(tmp_path: Path,
                                                                          profile: str) -> None:
    batch = _batch(tmp_path)
    plan = _plan(tmp_path, batch, profile=profile, regime="warm")
    product, script = _product(tmp_path)
    command = runner.build_command(batch, profile,
                                   product_root=product, script=script, corpus="DuckDB",
                                   regime="warm", depth="100", passes=2,
                                   predictive_plan=plan)
    assert command[0] == "env"
    assert "ICECC_CARET_WORKAROUND=0" in command
    assert f"ICECC_P50_PROFILE={profile}" in command
    assert "ICECC_P50_C1F1_WARM=1" in command
    assert "ICECC_P50_C1F1_PASSES=2" in command
    assert "ICECC_P50_C1F1_EXPECTED_COUNT=100" in command
    assert any(item.startswith("ICECC_P50_PREDICTIVE_PLAN=") for item in command)
    assert "ICECC_P50_C1F1_TIMEOUT=3630" in command
    assert "ICECC_P50_C1F1_KEEP_WORK=1" in command
    assert any(item.startswith("ICECC_P50_C1F1_BATCH_MANIFEST=") for item in command)
    assert command[-1].endswith("unittests/p50compilee2e-run.sh")


def test_timeout_scales_and_is_capped() -> None:
    assert runner.derive_timeout(100, 1, False) < runner.derive_timeout(100, 2, True)
    assert runner.derive_timeout(100000, 2, True) == runner.MAX_TIMEOUT_SECONDS


def test_container_command_uses_resolved_image_and_read_only_product_mount(
        tmp_path: Path) -> None:
    bind_root = tmp_path / "tanksmall"
    product = bind_root / "product"
    product.mkdir(parents=True)
    required = product / "batch.jsonl"
    required.write_text("{}\n")
    work_parent = Path("/tmp") / f"p5.pytest-{os.getpid()}-{tmp_path.name}"
    work_parent.mkdir()
    try:
        identity = {"reference": runner.PINNED_IMAGE,
                    "image_id": "sha256:" + "a" * 64,
                    "architecture": "amd64", "os": "linux",
                    "created": "2026-08-21T21:11:43Z"}
        command = runner.build_container_command(
            ["env", "ICECC_P50_PROFILE=ZSTD_TU", str(product / "run.sh")],
            image_identity=identity, bind_root=bind_root,
            work_parent=work_parent, required_paths=[required, product])
        assert command[:7] == ["docker", "run", "--rm", "--user", "0", "--network", "host"]
        assert command[7:9] == ["--name", runner.container_name(work_parent)]
        assert identity["image_id"] in command
        assert identity["reference"] not in command
        assert f"{bind_root.resolve()}:{bind_root.resolve()}:ro" in command
        assert f"{work_parent}:{work_parent}:rw" in command
        assert "ICECC_TEST_DAEMON_UID=nobody" in command
        assert "ICECC_TEST_DAEMON_GID=nogroup" in command
        assert f"chown -R {os.geteuid()}:{os.getegid()}" in command[-1]
    finally:
        work_parent.rmdir()


def test_container_cleanup_is_exact_and_tolerates_already_removed(
        monkeypatch: pytest.MonkeyPatch) -> None:
    work_parent = Path("/tmp") / f"p5.cleanup-{os.getpid()}"
    calls: list[list[str]] = []

    def fake_run(command: list[str], **_kwargs: object) -> subprocess.CompletedProcess[str]:
        calls.append(command)
        return subprocess.CompletedProcess(command, 1, "", "Error: No such container")

    monkeypatch.setattr(runner.subprocess, "run", fake_run)
    name = runner.container_name(work_parent)
    runner.stop_container(name)
    assert calls == [["docker", "container", "rm", "--force", name]]
    with pytest.raises(runner.LiveRunnerError, match="name_invalid"):
        runner.stop_container("unrelated-container")


def test_interrupted_product_removes_its_exact_container(
        tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    work_parent = Path("/tmp") / f"p5.cleanup-main-{os.getpid()}"
    work_parent.mkdir()
    stopped: list[str] = []
    image = {"reference": runner.PINNED_IMAGE,
             "image_id": "sha256:" + "a" * 64,
             "architecture": "amd64", "os": "linux",
             "created": "2026-08-21T21:11:43Z"}

    monkeypatch.setattr(runner.tempfile, "mkdtemp", lambda **_kwargs: str(work_parent))
    monkeypatch.setattr(runner, "load_predictive_plan",
                        lambda *_args, **_kwargs: ({}, [], "b" * 64))
    monkeypatch.setattr(runner, "load_batch_manifest", lambda *_args, **_kwargs: [])
    monkeypatch.setattr(runner, "load_topology", lambda *_args, **_kwargs: "c" * 64)
    monkeypatch.setattr(runner, "container_image_identity", lambda *_args: image)
    monkeypatch.setattr(
        runner, "product_identity",
        lambda *_args: ("d" * 40, "e" * 40, {"client/icecc": "f" * 64}, "1" * 64))
    monkeypatch.setattr(runner, "build_command", lambda *_args, **_kwargs: ["env", "true"])
    monkeypatch.setattr(runner, "build_container_command",
                        lambda *_args, **_kwargs: ["docker", "run"])

    def interrupted(_command: list[str], _timeout: int) -> tuple[str, int]:
        raise runner.LiveRunnerError("product_run:interrupted")

    monkeypatch.setattr(runner, "_run_product", interrupted)
    monkeypatch.setattr(runner, "stop_container", stopped.append)
    try:
        status = runner.main([
            "--batch-manifest", str(tmp_path / "batch.jsonl"),
            "--predictive-plan", str(tmp_path / "plan.json"),
            "--topology", str(tmp_path / "topology.json"),
            "--profile", "ZSTD_TU", "--product-root", str(tmp_path / "product"),
            "--output", str(tmp_path / "output"), "--execute",
        ])
        assert status == 77
        assert stopped == [runner.container_name(work_parent)]
    finally:
        work_parent.rmdir()


def test_container_command_rejects_input_outside_read_only_root(tmp_path: Path) -> None:
    bind_root = tmp_path / "root"
    bind_root.mkdir()
    work_parent = Path("/tmp") / f"p5.pytest-outside-{os.getpid()}-{tmp_path.name}"
    work_parent.mkdir()
    identity = {"reference": runner.PINNED_IMAGE,
                "image_id": "sha256:" + "b" * 64,
                "architecture": "amd64", "os": "linux",
                "created": "2026-08-21T21:11:43Z"}
    try:
        with pytest.raises(runner.LiveRunnerError, match="required_path_outside"):
            runner.build_container_command(
                ["env", "true"], image_identity=identity, bind_root=bind_root,
                work_parent=work_parent, required_paths=[tmp_path.parent])
    finally:
        work_parent.rmdir()


def test_topology_requires_exact_tu_identity(tmp_path: Path) -> None:
    batch = _batch(tmp_path)
    rows = runner.load_batch_manifest(batch)
    topology = _topology(tmp_path, batch)
    value = json.loads(topology.read_text())
    value["assignments"][0].pop("tu_id")
    topology.write_text(json.dumps(value))
    with pytest.raises(runner.LiveRunnerError, match="identity_mismatch"):
        runner.load_topology(topology, rows)


def test_predictive_schedule_slot_mutation_fails_before_live_descriptor(tmp_path: Path) -> None:
    batch = _batch(tmp_path, 1)
    rows = runner.load_batch_manifest(batch, 1)
    topology = _topology(tmp_path, batch)
    scheduling = {"topology": "C1F1", "assignments": [
        {"ordinal": 0, "global_slot": 1, "f_relationship": 0, "per_f_slot": 0}
    ]}
    with pytest.raises(runner.LiveRunnerError, match="predictive_schedule_mismatch"):
        runner.load_topology(topology, rows, scheduling=scheduling)


def test_product_identity_rejects_tracked_edit(tmp_path: Path) -> None:
    product, script = _product(tmp_path)
    (product / "unittests/p50compilee2e-run.sh").write_text("#!/bin/sh\nexit 1\n")
    with pytest.raises(runner.LiveRunnerError, match="tracked_or_index_dirty"):
        runner.product_identity(product, script)


def test_batch_shell_excludes_warm_prewarm_and_carries_optional_repeat() -> None:
    shell = (Path(__file__).resolve().parents[1] / "unittests/p50compilee2e-run.sh").read_text()
    cache_service = (Path(__file__).resolve().parents[1] /
                     "cache/p50_cache_service.cpp").read_text()
    sidecar_adapter = (Path(__file__).resolve().parents[1] /
                       "cache/p50_daemon_sidecar_adapter.cpp").read_text()
    assert "run_batch prewarm 0" in shell
    assert "run_batch full-1 1" in shell
    assert 'staged="$work/src/$run_label-$ordinal.ii"' in shell
    assert 'cp -- "$predictive_path" "$staged"' in shell
    assert 'compile_args_for "$item_compile_db" "$item_compile_source" "$item_compile_output" "$input_path" "$remote_obj"' in shell
    assert 'if test "$passes" = 2; then' in shell
    assert "s7-measured-c-action-trace.jsonl" in Path(__file__).resolve().parent.joinpath(
        "s8_real_c1f1_live_runner.py").read_text()
    assert "shutil.rmtree(work)" in Path(__file__).resolve().parent.joinpath(
        "s8_real_c1f1_live_runner.py").read_text()
    assert 'C1F20/40) relationship_count=20; slots_per_f=2; execution_slots=40' in shell
    assert '"$build/daemon/iceccd" "$@" -p "$worker_port" -m 2' in shell
    assert '"$work/envs-f-$relationship"' in shell
    assert 'S8_BATCH_WINDOW run=%s start_ns=%s end_ns=%s' in shell
    assert 'run_one "$run_label" "$ordinal" "$relationship" "$f_slot"' in shell
    assert "planned_assignment=%s" in shell
    assert "planned_admission_slot=%s" in shell
    assert "preferred_service_identity=p50-f-%s" in shell
    assert "source_admission=global_source_commit_gate" in shell
    assert "physical_slot_observed=0" in shell
    assert '"physical_slot_observed": False' in Path(__file__).resolve().parent.joinpath(
        "s8_real_c1f1_live_runner.py").read_text()
    assert 'staged="$work/src/$run_label-$ordinal.ii"' in shell
    assert 'cp -- "$predictive_path" "$staged"' in shell
    assert 'compile_args_for "$item_compile_db" "$item_compile_source" "$item_compile_output" "$input_path" "$remote_obj"' in shell
    assert 'input-ready/$run_label-$relationship-$ordinal' in shell
    assert "source committed for P50 CompileFile" in shell
    assert 'while test -e "$marker"; do sleep 0.005; done' in shell
    assert "grep -oE 'p50-f-[0-9]+' | sort -u | wc -l" in shell
    assert "fsession_owner_(64, config_.f_store_generation)" in cache_service
    assert "::listen(outer_launch_listener_fd_, 64)" in sidecar_adapter


def test_unsupported_topology_fails_closed(tmp_path: Path) -> None:
    batch = _batch(tmp_path)
    rows = runner.load_batch_manifest(batch)
    topology = tmp_path / "bad-topology.json"
    topology.write_text(json.dumps({"suite": "C1F20/40", "assignments": []}))
    with pytest.raises(runner.LiveRunnerError, match="unsupported_suite"):
        runner.load_topology(topology, rows)


def test_parallel_topology_binds_all_relationships_and_slots(tmp_path: Path) -> None:
    batch = _batch(tmp_path)
    rows = runner.load_batch_manifest(batch)
    topology = tmp_path / "parallel-topology.json"
    assignments = [{"ordinal": index, "tu_id": row["tu_id"],
                    "relationship": index % 20, "f_slot": (index // 20) % 2}
                   for index, row in enumerate(rows)]
    topology.write_text(json.dumps({"schema": "icecream-s8-topology-assignment-v1",
                                    "suite": runner.PARALLEL_TOPOLOGY,
                                    "assignments": assignments}))
    assert len(runner.load_topology(topology, rows, runner.PARALLEL_TOPOLOGY)) == 64
    assignments[0]["relationship"] = 20
    topology.write_text(json.dumps({"schema": "icecream-s8-topology-assignment-v1",
                                    "suite": runner.PARALLEL_TOPOLOGY,
                                    "assignments": assignments}))
    with pytest.raises(runner.LiveRunnerError, match="assignment_invalid"):
        runner.load_topology(topology, rows, runner.PARALLEL_TOPOLOGY)


def test_parallel_batch_window_requires_real_overlap() -> None:
    rows = [{} for _ in range(40)]
    observations = [{"run": "full-1", "relationship": index % 20,
                     "planned_admission_slot": (index // 20) % 2,
                     "compile_start_ns": 1_000 + index,
                     "compile_end_ns": 2_000 + index}
                    for index in range(40)]
    stdout = "S8_BATCH_WINDOW run=full-1 start_ns=900 end_ns=2100\n"
    windows = runner._batch_windows(stdout, observations, rows, 1,
                                    runner.PARALLEL_TOPOLOGY)
    assert windows["full-1"]["makespan_ns"] == 1_200
    serial = [{**row, "compile_start_ns": 1 + index * 100,
               "compile_end_ns": 1 + index * 100 + 10}
              for index, row in enumerate(observations)]
    serial_stdout = "S8_BATCH_WINDOW run=full-1 start_ns=1 end_ns=5000\n"
    with pytest.raises(runner.LiveRunnerError, match="no_observed_compile_overlap"):
        runner._batch_windows(serial_stdout, serial, rows, 1, runner.PARALLEL_TOPOLOGY)


def test_source_mutation_is_rejected_before_launch(tmp_path: Path) -> None:
    batch = _batch(tmp_path)
    rows = runner.load_batch_manifest(batch)
    Path(rows[0]["source"]).write_bytes(b"changed\n")
    with pytest.raises(runner.LiveRunnerError, match="source_digest_mismatch"):
        runner.load_batch_manifest(batch)


def test_predictive_payload_descriptor_is_required(tmp_path: Path) -> None:
    batch = _batch(tmp_path)
    values = [json.loads(line) for line in batch.read_text().splitlines()]
    values[0].pop("predictive_input")
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
