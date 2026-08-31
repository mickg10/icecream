from __future__ import annotations

import hashlib
import json
import re
import shutil
import subprocess
import datetime as dt
from pathlib import Path
from typing import Sequence

import pytest

import s8_external_farm_executor as executor
import s8_real_c1f1_live_runner as finalizer


def _authority(tmp_path: Path) -> dict[str, object]:
    hosts: dict[str, object] = {}
    for host in executor.HOSTS:
        machine = hashlib.sha256((host + ":machine").encode()).hexdigest()
        nic = hashlib.sha256((host + ":nic").encode()).hexdigest()
        physical_input = {"machine_id_sha256": machine, "nic_identity_sha256": nic}
        physical = hashlib.sha256(json.dumps(
            physical_input, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
        boot = hashlib.sha256((host + ":boot").encode()).hexdigest()
        facts = {"machine_id_sha256": machine, "boot_id_sha256": boot,
                 "nic_identity_sha256": nic, "cpu_vendor_sha256": "2" * 64,
                 "cpu_model_sha256": "3" * 64, "cpu_count": executor.CPU_COUNTS[host],
                 "physical_host_digest": physical}
        descriptor = tmp_path / f"{host}.descriptor"
        descriptor.write_text(json.dumps(
            {"schema": "icecream-s8-external-host-descriptor-v1", "facts": facts},
            sort_keys=True, separators=(",", ":")) + "\n")
        sha, size = executor._sha(descriptor)
        sample = {"before": "cpu before", "after": "cpu after",
                  "duration_seconds": 1.0, "idle_percent": 100.0}
        hosts[host] = {
            "target": executor.s4.HOSTS[host]["target"], "lan": executor.s4.HOSTS[host]["lan"],
            "hostname": host, "descriptor": {"path": str(descriptor), "sha256": sha, "bytes": size},
            "physical_host_digest": physical, "boot_id_digest": boot,
            "cpu_count": executor.CPU_COUNTS[host],
            "idle": {"status": "PASS", "load_1m": 0.1,
                     "captured_at": dt.datetime.now(dt.timezone.utc).replace(
                         microsecond=0).strftime("%Y-%m-%dT%H:%M:%SZ"),
                     "baseline_digest": hashlib.sha256((host + ":baseline").encode()).hexdigest()},
            "cpu_sample": sample,
            "cpu_sample_digest": hashlib.sha256((json.dumps(
                sample, sort_keys=True, separators=(",", ":")) + "\n").encode()).hexdigest(),
            "image": {"reference": executor.s4.PINNED_IMAGE,
                      "image_id": (executor.s4.EXPECTED_IMAGE_CONFIG_ID if host == "research7"
                                   else executor.s4.EXPECTED_IMAGE_ID),
                      "architecture": "amd64", "os": "linux", "created": "now"},
            "binaries": {role: hashlib.sha256(role.encode()).hexdigest() for role in {
                "scheduler/icecc-scheduler", "daemon/iceccd", "client/icecc",
                "client/icecc-create-env", "cache/icecc-cache-service"}},
        }
    return {"schema": executor.AUTHORITY_SCHEMA, "hosts": hosts,
            "placements": {"C1F1/100000": {"relationship_hosts": ["q2"]},
                           "C1F20/40": {"relationship_hosts": ["q2"] * 15 + ["research7"] * 5}}}


def test_placement_has_no_q3_f_and_disjoint_physical_ids(tmp_path: Path) -> None:
    authority = _authority(tmp_path)
    placement = executor.role_placement(authority, "C1F20/40", ["q2"] * 15 + ["research7"] * 5)
    assert placement["c_host_digest"] not in placement["f_host_digests"]
    with pytest.raises(executor.ExternalFarmError, match="q3_f_forbidden"):
        executor.role_placement(authority, "C1F1/100000", ["q3"])


def test_research6_is_captured_but_not_an_executable_f_host(tmp_path: Path) -> None:
    authority = _authority(tmp_path)
    authority["placements"]["C1F1/100000"]["relationship_hosts"] = ["research6"]
    with pytest.raises(executor.ExternalFarmError, match="placements:C1F1/100000:invalid"):
        executor._validate_authority(authority)
    with pytest.raises(executor.ExternalFarmError, match="q3_f_forbidden"):
        executor.role_placement(_authority(tmp_path), "C1F1/100000", ["research6"])


def test_rotation_uses_ready_pids_and_worker_root_allows_daemon_outputs() -> None:
    source = Path(executor.__file__).read_text(encoding="utf-8")
    assert source.count(r'before_pid=$(field \"$before_ready\" pid)') >= 2
    assert source.count(r'after_pid=$(field \"$after_ready\" pid)') >= 2
    assert "before_pid=$(cat {client_work}/c-rotation-before-pid)" not in source
    assert 'chmod 1777 {worker_root} {worker_root}/envs' in source


def test_parallel_gate_requires_all_lanes_and_overlap() -> None:
    with pytest.raises(executor.ExternalFarmError, match="parallel_overlap_missing"):
        executor.overlap_required("C1F20/40", {"planned_lanes": 40, "max_concurrent": 1})
    executor.overlap_required("C1F20/40", {"planned_lanes": 40, "max_concurrent": 2})


def test_interference_witness_allows_keepalive_tick_but_holds_material_work() -> None:
    assert executor.interference_delta(10, 110) == 100
    with pytest.raises(executor.ExternalFarmError, match="background_farm_activity"):
        executor.interference_delta(10, 111)


def test_f_to_c_is_returned_object_for_compressed_and_legacy_ledger_for_raw() -> None:
    assert executor.f_to_c_bytes("ZSTD_ROUTE", remote_object_bytes=41, f_action_stage_bytes=999) == 41
    assert executor.f_to_c_bytes("RAW_II", remote_object_bytes=41, f_action_stage_bytes=999,
                                 legacy_wire_bytes=88) == 88
    with pytest.raises(executor.ExternalFarmError, match="raw_wire"):
        executor.f_to_c_bytes("RAW_II", remote_object_bytes=41, f_action_stage_bytes=999)


def test_scheduler_identity_is_job_join_not_tu_grep() -> None:
    client = "Have to use host p50-f - Job ID: 17 - env: x\n"
    scheduler = "BEGIN: 17 client=abc server=p50-f\n"
    assert executor.scheduler_assignment(client, scheduler, service="p50-f") == (17, "abc")
    with pytest.raises(executor.ExternalFarmError, match="scheduler_assignment"):
        executor.scheduler_assignment(client, "BEGIN: 16 client=abc server=p50-f\n", service="p50-f")


def test_compile_database_argv_preserves_flags_and_stages_ii() -> None:
    argv = executor.compile_database_argv(
        {"command": "g++ -std=c++17 -DVALUE=7 -c src/unit.cpp -o build/unit.o"},
        staged_input=Path("/run/input/unit.ii"), output=Path("/run/out/unit.o"))
    assert argv == ["g++", "-std=c++17", "-DVALUE=7", "-c", "/run/input/unit.ii", "-o", "/run/out/unit.o"]
    assert "-O2" not in argv


def test_profile_modes_parameterize_cache_and_raw_has_no_sidecar() -> None:
    assert "--cache-service" in executor.profile_arguments("ZSTD_TU", root="/r", work="/w", role="f-0")
    assert executor.profile_arguments("RAW_II", root="/r", work="/w", role="f-0") == []
    assert executor.profile_environment("GRZ_RESIDUAL") == "grz"


def test_authority_requires_environment_builder_and_descriptor(tmp_path: Path) -> None:
    authority = _authority(tmp_path)
    authority["hosts"]["q2"]["binaries"].pop("client/icecc-create-env")
    with pytest.raises(executor.ExternalFarmError, match="binary_identity_incomplete"):
        executor._validate_authority(authority)


def test_authority_binds_routing_and_staging_rejects_outside_input(tmp_path: Path) -> None:
    authority = _authority(tmp_path)
    authority["hosts"]["q3"]["lan"] = "192.0.2.1"
    with pytest.raises(executor.ExternalFarmError, match="identity_invalid"):
        executor._validate_authority(authority)
    inside = Path("/tanksmall/MICKG2")
    outside = Path("/proc/cpuinfo")
    with pytest.raises(executor.ExternalFarmError, match="outside_staged_product"):
        executor._stage_input_paths(outside, outside, outside, inside, [])


def test_staging_requires_inputs_but_not_the_replaced_compile_output(tmp_path: Path) -> None:
    product = tmp_path / "product"; product.mkdir()
    batch = tmp_path / "batch.jsonl"; batch.write_text("batch\n")
    plan = tmp_path / "plan.json"; plan.write_text("{}\n")
    topology = tmp_path / "topology.json"; topology.write_text("{}\n")
    source = tmp_path / "source.cc"; source.write_text("int source();\n")
    payload = tmp_path / "source.ii"; payload.write_text("int source();\n")
    compile_db = tmp_path / "compile_commands.json"; compile_db.write_text("[]\n")
    missing_output = tmp_path / "build" / "source.o"
    paths = executor._stage_input_paths(
        batch, plan, topology, product,
        [{"source": str(source), "compile_db": str(compile_db),
          "compile_source": str(source), "compile_output": str(missing_output),
          "predictive_input": {"path": str(payload)}}])
    assert missing_output not in paths
    assert {source.resolve(), payload.resolve(), compile_db.resolve()}.issubset(paths)


def test_external_command_preserves_two_pass_repeat_input(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(executor.live, "build_command", lambda *args, **kwargs: [
        "env", "ICECC_P50_C1F1_PASSES=2", "/tanksmall/unittests/p50compilee2e-run.sh"])
    command = executor.build_external_command(
        Path("/tanksmall/batch.jsonl"), Path("/tanksmall/plan.json"),
        Path("/tanksmall/topology.json"), Path("/tanksmall/product"),
        profile="ZSTD_TU", corpus="DuckDB", regime="cold", depth="100",
        suite="C1F1/100000", workdir=Path("/tmp/p50compilee2e.external"),
        timeout_seconds=900, passes=2,
        repeat_predictive_plan=Path("/tanksmall/plan-full-2.json"))
    assert "ICECC_P50_C1F1_PASSES=2" in command
    assert "ICECC_P50_REPEAT_PREDICTIVE_PLAN=/tanksmall/plan-full-2.json" in command


def test_external_shell_branch_skips_every_local_role_start() -> None:
    shell = (Path(__file__).resolve().parents[1] / "unittests" /
             "p50compilee2e-run.sh").read_text(encoding="utf-8")
    marker = 'if test "$external_mode" = 0; then'
    start = shell.index(marker)
    external_marker = '\nelse\n    # The external transport owns these processes.'
    external_start = shell.index(external_marker, start)
    branch = shell[start:external_start]
    external_end = shell.index("\nfi\n", shell.index("\nfi\n", external_start) + 1)
    external = shell[external_start:external_end]
    assert '"$build/scheduler/icecc-scheduler"' in branch
    assert '"$build/daemon/iceccd"' in branch
    assert '"$build/scheduler/icecc-scheduler"' not in external
    assert '"$build/daemon/iceccd"' not in external
    assert "ICECC_P50_EXTERNAL_RESET_HOOK" in external
    assert "relationship_count" in external
    assert "remote_batch_end_ns=$batch_end_ns" in shell
    assert "local_witness_start_ns=$(date +%s%N)" in shell


def test_concrete_transport_invokes_q3_c_scheduler_and_non_q3_f(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    authority = _authority(tmp_path)
    transport = executor.SSHTransport(authority)
    calls: list[tuple[str, str]] = []
    def fake_run(host: str, script: str, args: Sequence[object] = ()) -> subprocess.CompletedProcess[str]:
        calls.append((host, script))
        if "S8_F_INTERFERENCE" in script:
            stdout = "S8_F_INTERFERENCE before=1 after=2\n"
        else:
            workdir = re.search(
                r"ICECC_P50_C1F1_WORKDIR=(/tmp/p50compilee2e\.external\.[A-Za-z0-9]+)",
                script)
            stdout = (("PASS: all-P50 C1F1\n"
                       f"S7_WORKDIR={workdir.group(1)}\n"
                       "S8_BATCH_METRICS max_concurrent_admitted_or_compiling_jobs=2\n")
                      if "-batch" in script and workdir is not None else "")
        return subprocess.CompletedProcess([], 0, stdout=stdout, stderr="")
    monkeypatch.setattr(transport, "run", fake_run)
    monkeypatch.setattr(executor.s4, "run_script",
                        lambda *args, **kwargs: subprocess.CompletedProcess([], 1, "", ""))
    def fake_copy(host: str, remote: str, destination: Path, timeout: float) -> None:
        destination.mkdir(parents=True, exist_ok=False)
        if host == "q3":
            (destination / "scheduler.container-id").write_text("a" * 64)
            (destination / "scheduler.pid").write_text("100")
            (destination / "c.container-id").write_text("b" * 64)
            (destination / "c.pid").write_text("101")
        else:
            (destination / "container-id").write_text("c" * 64)
            (destination / "container-pid").write_text("102")
            (destination / "f.log").write_text("")
            (destination / "f-measured-log-offset").write_text("0")
    monkeypatch.setattr(executor, "_copy_remote_tree", fake_copy)
    result = transport.execute_command(topology="C1F1/100000", relationship_hosts=["q2"],
                                       profile="ZSTD_ROUTE", product_root_remote="/product",
                                       batch_command=["env", "ICECC_P50_EXTERNAL_FARM=1", "/product/unittests/p50compilee2e-run.sh"],
                                       output=tmp_path / "out")
    assert result["status"] == "PASS"
    assert any(host == "q3" and "icecc-scheduler" in script for host, script in calls)
    assert any(host == "q2" and "iceccd" in script for host, script in calls)
    assert any(host == "q3" and "--no-remote -m 0" in script for host, script in calls)
    assert all(not (host == "q3" and "-N p50-f" in script) for host, script in calls)
    receipt = json.loads((tmp_path / "out" / "external-farm-receipt.json").read_text())
    assert receipt["schema"] == "icecream-s8-external-farm-receipt-v1"
    assert receipt["execution"]["artifacts"] == {"collection": "complete", "cleanup": "complete"}
    assert receipt["execution"]["start_marker"].startswith("S8_EXTERNAL_FARM_EXECUTION ")
    assert receipt["workdir"].startswith("/tmp/p50compilee2e.external.")
    stdout = (tmp_path / "out" / "product-output.log").read_text()
    assert stdout.count("S8_EXTERNAL_FARM_EXECUTION ") == 2
    assert result["finalizer_input"]["receipt_path"].endswith("external-farm-receipt.json")
    assert result["finalizer_input"]["receipt_sha256"] == executor._sha(
        tmp_path / "out" / "external-farm-receipt.json")[0]
    finalizer_input = result["finalizer_input"]
    retained_workdir = Path(finalizer_input["workdir"])
    try:
        external = finalizer.ExternalFarmFinalization(
            Path(finalizer_input["manifest_path"]), finalizer_input["manifest_sha256"],
            Path(finalizer_input["authority_path"]), finalizer_input["authority_sha256"],
            retained_workdir, Path(finalizer_input["stdout_path"]),
            finalizer_input["stdout_sha256"], finalizer_input["stdout_bytes"],
            Path(finalizer_input["receipt_path"]), finalizer_input["receipt_sha256"],
            finalizer_input["receipt_bytes"])
        binding = finalizer._external_farm_binding(external, finalizer.TOPOLOGY)
        assert binding["manifest"]["sha256"] == finalizer_input["manifest_sha256"]
        assert binding["authority"]["sha256"] == finalizer_input["authority_sha256"]
    finally:
        shutil.rmtree(retained_workdir, ignore_errors=True)


def test_arbitrary_true_command_cannot_be_admitted(tmp_path: Path) -> None:
    authority = _authority(tmp_path)
    with pytest.raises(executor.ExternalFarmError, match="authenticated_batch_command"):
        executor.SSHTransport(authority).execute_command(
            topology="C1F1/100000", relationship_hosts=["q2"], profile="ZSTD_ROUTE",
            product_root_remote="/product",
            batch_command=["env", "ICECC_P50_EXTERNAL_FARM=1", "/bin/true"],
            output=tmp_path / "out")
