from __future__ import annotations

import hashlib
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import time
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
    assert source.count('before_pid=$(field "$before_ready" pid)') >= 2
    assert source.count('after_pid=$(field "$after_ready" pid)') >= 2
    assert "before_pid=$(cat {client_work}/c-rotation-before-pid)" not in source
    assert 'chmod 1777 {worker_root} {worker_root}/envs' in source


def test_remote_failure_retains_bounded_diagnostic(
        tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    transport = executor.SSHTransport(_authority(tmp_path))
    monkeypatch.setattr(
        executor.s4, "run_script",
        lambda *args, **kwargs: subprocess.CompletedProcess(
            [], 17, "first signal\n", "specific failure\n"))
    with pytest.raises(
            executor.ExternalFarmError,
            match=r"q3:remote_command_failed:17:script=[0-9a-f]{12}:.*first signal.*specific failure"):
        transport.run("q3", "set -eu\nprintf phase-name\n")


def test_cleanup_does_not_replace_primary_error_and_handles_container_owned_files() -> None:
    primary = executor.ExternalFarmError("primary failure")
    executor._finish_cleanup(["q2:permission"], primary)
    assert any("cleanup:incomplete:q2:permission" in note
               for note in getattr(primary, "__notes__", []))
    with pytest.raises(executor.ExternalFarmError, match="cleanup:incomplete:q2:permission"):
        executor._finish_cleanup(["q2:permission"], None)
    source = Path(executor.__file__).read_text(encoding="utf-8")
    assert "docker run --rm --network none --user 0" in source
    assert "find /probe/cleanup -mindepth 1 -delete" in source
    assert "failure-diagnostics" in source
    assert "icecream-s8-external-farm-failure-v1" in source
    assert source.index("failure-diagnostics") < source.index("cleanup_paths =")


def test_external_batch_executes_inside_c_time_namespace_and_stops_scheduler_last() -> None:
    source = Path(executor.__file__).read_text(encoding="utf-8")
    assert source.count("-c --pid=host --network host --user 0") == 1
    assert source.count("-v {client_work}:/probe/work:rw -v {client_work}:{client_work}:rw") == 1
    assert "docker exec --user 0 {token}-c" in source
    assert "docker run --rm --name {token}-batch" not in source
    assert 'docker exec --user 0 "$container_id" /bin/sh -c' in source
    assert "kill -9 \"$pid\"" in source
    assert "docker rm -f {token}-c" not in source
    targets = source[source.index("targets = [(\"q3\""):
                     source.index("cleanup = '''set -eu")]
    assert targets.index("reset-worker.pid") < targets.index("c.container-id")
    assert targets.index("c.container-id") < targets.index("worker_work(i)")
    assert targets.index("worker_work(i)") < targets.index("scheduler.container-id")


def test_preflight_and_authority_capture_hash_identical_nic_rows() -> None:
    executor_source = Path(executor.__file__).read_text(encoding="utf-8")
    authority_source = (Path(executor.__file__).with_name(
        "s8_external_farm_authority.py").read_text(encoding="utf-8"))
    pattern = re.compile(r"^nic_rows=\$\(for p in /sys/class/net/\*;.*done \| sort\)$", re.M)
    executor_row = pattern.search(executor_source)
    authority_row = pattern.search(authority_source)
    assert executor_row is not None and authority_row is not None
    assert executor_row.group(0) == authority_row.group(0)
    hash_pattern = re.compile(r"^nic=\$\(printf .*sha256sum.*$", re.M)
    executor_hash = hash_pattern.search(executor_source)
    authority_hash = hash_pattern.search(authority_source)
    assert executor_hash is not None and authority_hash is not None
    assert executor_hash.group(0) == authority_hash.group(0)


def test_preflight_rechecks_current_idle_after_staging() -> None:
    source = Path(executor.__file__).read_text(encoding="utf-8")
    assert "for _ in $(seq 1 10); do" in source
    assert 'idle_ready=1' in source
    assert 'fail cpu_idle_percent "$idle" "$min_idle"' in source
    assert executor.IDLE_LOAD_THRESHOLD == 0.50
    assert executor.MIN_IDLE_PERCENT == 95.0


def test_parallel_gate_requires_all_lanes_and_overlap() -> None:
    with pytest.raises(executor.ExternalFarmError, match="parallel_overlap_missing"):
        executor.overlap_required("C1F20/40", {"planned_lanes": 40, "max_concurrent": 1})
    executor.overlap_required("C1F20/40", {"planned_lanes": 40, "max_concurrent": 2})


def test_interference_witness_allows_keepalive_tick_but_holds_material_work() -> None:
    assert executor.interference_delta(10, 110) == 100
    with pytest.raises(executor.ExternalFarmError, match="background_farm_activity"):
        executor.interference_delta(10, 111)


def _local_monitor_identity() -> tuple[str, str]:
    boot = hashlib.sha256(Path("/proc/sys/kernel/random/boot_id").read_bytes()).hexdigest()
    machine = hashlib.sha256(Path("/etc/machine-id").read_bytes()).hexdigest()
    rows: list[str] = []
    for path in sorted(Path("/sys/class/net").glob("*")):
        if path.name == "lo":
            continue
        try:
            real, mac = str(path.resolve()), (path / "address").read_text().strip()
        except OSError:
            continue
        if "/virtual/" in real or not mac or mac == "00:00:00:00:00:00":
            continue
        rows.append(f"{path.name}:{mac}:{real}")
    nic = hashlib.sha256(("\n".join(rows) + "\n").encode()).hexdigest()
    physical = hashlib.sha256(json.dumps(
        {"machine_id_sha256": machine, "nic_identity_sha256": nic},
        sort_keys=True, separators=(",", ":")).encode()).hexdigest()
    return physical, boot


def _run_local_monitor(tmp_path: Path, competitor: str | None,
                       *, kill_owner: bool = False,
                       competitor_before_monitor: bool = False,
                       settle_seconds: float = 0.5) -> tuple[dict[str, object], int]:
    root = tmp_path / "monitor-root"
    root.mkdir()
    owner = subprocess.Popen(["sleep", "10"])
    (root / "container-pid").write_text(str(owner.pid))
    physical, boot = _local_monitor_identity()
    child = None
    if competitor is not None and competitor_before_monitor:
        child = subprocess.Popen([sys.executable, "-c", competitor])
    start = subprocess.run(
        ["bash", "-s", "--", str(root), physical, boot],
        input=executor.interference_start_script(), text=True,
        capture_output=True, check=False, timeout=5)
    assert start.returncode == 0, start.stderr
    if competitor is not None and not competitor_before_monitor:
        child = subprocess.Popen([sys.executable, "-c", competitor])
        time.sleep(settle_seconds)
    elif competitor_before_monitor:
        time.sleep(settle_seconds)
    if kill_owner:
        owner.terminate()
        owner.wait(timeout=5)
        time.sleep(0.1)
    stop = subprocess.run(
        ["bash", "-s", "--", str(root)],
        input=executor.interference_stop_script(), text=True,
        capture_output=True, check=False, timeout=5)
    if child is not None:
        child.wait(timeout=5)
    owner.terminate(); owner.wait(timeout=5)
    return json.loads((root / "interference-result.json").read_text()), stop.returncode


def _ordinary_daemon_program(payload: str) -> str:
    return (
        "import os\n"
        f"os.execv({sys.executable!r}, "
        f"['iceccd', '-c', {payload!r}, '-N', 'farm-qbox'])\n"
    )


def test_interference_monitor_rejects_short_lived_competing_child(tmp_path: Path) -> None:
    payload = (
        "import subprocess,time\n"
        "time.sleep(.30)\n"
        f"subprocess.run([{sys.executable!r}, '-c', "
        "'x=0\\nwhile x < 10000000: x += 1'])\n"
        "time.sleep(1.5)\n"
    )
    result, stop_rc = _run_local_monitor(
        tmp_path, _ordinary_daemon_program(payload),
        competitor_before_monitor=True, settle_seconds=2.0)
    assert stop_rc != 0
    assert result["status"] == "FAIL"
    assert result["timing_eligible"] is False
    # The child is deliberately gone before the first useful stop boundary;
    # this is the reaped-work witness, not a live-process discovery race.
    assert result["reason"] == "ordinary_child_cpu"
    assert [row["phase"] for row in result["phases"]] == ["before", "during", "after"]


def _proc_cpu_ticks(pid: int) -> int:
    text = Path(f"/proc/{pid}/stat").read_text()
    tail = text[text.rfind(")") + 2:].split()
    return int(tail[11]) + int(tail[12])


def test_interference_monitor_uses_cached_roots_and_edge_identity_checks() -> None:
    source = executor.INTERFERENCE_MONITOR_PROGRAM
    assert executor.INTERFERENCE_POLL_SECONDS >= 0.5
    assert executor.INTERFERENCE_ROOT_REDISCOVERY_SECONDS >= 5.0
    assert 'pathlib.Path("/proc").glob("[0-9]*")' in source
    assert "time.sleep(poll_seconds)" in source
    assert "child_cpu_ticks" in source
    assert "host_identity() != (expected_physical, expected_boot)" not in source
    assert source.count("host_identity()") == 3  # definition + before + after


def test_interference_monitor_idle_cpu_is_below_two_percent(tmp_path: Path) -> None:
    root = tmp_path / "monitor-root"
    root.mkdir()
    owner = subprocess.Popen(["sleep", "10"])
    (root / "container-pid").write_text(str(owner.pid))
    physical, boot = _local_monitor_identity()
    start = subprocess.run(
        ["bash", "-s", "--", str(root), physical, boot],
        input=executor.interference_start_script(), text=True,
        capture_output=True, check=False, timeout=5)
    assert start.returncode == 0, start.stderr
    monitor_pid = int((root / "interference-monitor.pid").read_text())
    ticks_before = _proc_cpu_ticks(monitor_pid)
    sample_seconds = 5.0
    time.sleep(sample_seconds)
    ticks_after = _proc_cpu_ticks(monitor_pid)
    stop = subprocess.run(
        ["bash", "-s", "--", str(root)],
        input=executor.interference_stop_script(), text=True,
        capture_output=True, check=False, timeout=5)
    owner.terminate(); owner.wait(timeout=5)
    assert stop.returncode == 0, stop.stderr
    clock_ticks = int(os.sysconf("SC_CLK_TCK"))
    cpu_seconds = (ticks_after - ticks_before) / clock_ticks
    assert cpu_seconds / sample_seconds < 0.02, \
        f"idle interference monitor used {cpu_seconds:.3f}s CPU in {sample_seconds:.1f}s"


def test_interference_monitor_allows_idle_ordinary_daemon(tmp_path: Path) -> None:
    result, stop_rc = _run_local_monitor(
        tmp_path, _ordinary_daemon_program("import time\ntime.sleep(.5)\n"))
    assert stop_rc == 0
    assert result["status"] == "PASS"
    assert result["timing_eligible"] is True


def test_interference_monitor_ignores_diagnostic_command_text(tmp_path: Path) -> None:
    result, stop_rc = _run_local_monitor(
        tmp_path,
        "import time\n# diagnostic mentions iceccd -N farm-qbox\ntime.sleep(6)\n",
        settle_seconds=5.5)
    assert stop_rc == 0
    assert result["status"] == "PASS"
    assert result["timing_eligible"] is True


def test_interference_result_rejects_executor_identity_or_pid_reuse() -> None:
    valid = {"schema": executor.INTERFERENCE_SCHEMA, "status": "PASS",
             "timing_eligible": True, "reason": None, "events": [],
             "phases": [{"phase": phase, "path": f"/tmp/interference-{phase}.jsonl"}
                        for phase in ("before", "during", "after")]}
    for reason in ("executor_identity_lost_or_pid_reused", "executor_pid_reused"):
        invalid = dict(valid, status="FAIL", timing_eligible=False, reason=reason)
        with pytest.raises(executor.ExternalFarmError, match="background_farm_activity"):
            executor.validate_interference_result(invalid)


def test_interference_result_rejects_pass_with_hidden_event() -> None:
    invalid = {"schema": executor.INTERFERENCE_SCHEMA, "status": "PASS",
               "timing_eligible": True, "reason": None,
               "events": ["ordinary_child_cpu"],
               "phases": [{"phase": phase,
                            "path": f"/tmp/interference-{phase}.jsonl"}
                           for phase in ("before", "during", "after")]}
    with pytest.raises(executor.ExternalFarmError, match="background_farm_activity"):
        executor.validate_interference_result(invalid)


def test_interference_monitor_rejects_executor_identity_loss(tmp_path: Path) -> None:
    result, stop_rc = _run_local_monitor(tmp_path, None, kill_owner=True)
    assert stop_rc != 0
    assert result["status"] == "FAIL"
    assert result["timing_eligible"] is False
    assert result["reason"] == "executor_identity_lost_or_pid_reused"


def test_interference_monitor_cleanup_reaps_only_its_monitor(tmp_path: Path) -> None:
    result, stop_rc = _run_local_monitor(tmp_path, None)
    assert stop_rc == 0 and result["timing_eligible"] is True
    root = tmp_path / "monitor-root"
    monitor_pid = int((root / "interference-monitor.pid").read_text())
    with pytest.raises(ProcessLookupError):
        os.kill(monitor_pid, 0)


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


def test_external_command_places_s2_process_loss_before_runner(
        monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(executor.live, "build_command", lambda *args, **kwargs: [
        "env", "ICECC_P50_C1F1_PASSES=1",
        "/tanksmall/unittests/p50compilee2e-run.sh"])
    command = executor.build_external_command(
        Path("/tanksmall/batch.jsonl"), Path("/tanksmall/plan.json"),
        Path("/tanksmall/topology.json"), Path("/tanksmall/product"),
        profile="ZSTD_TU", corpus="DuckDB", regime="cold", depth="100",
        suite="C1F1/100000", workdir=Path("/tmp/p50compilee2e.external"),
        timeout_seconds=900, s2_process_loss=True)
    flag = command.index("ICECC_P50_S2_PROCESS_LOSS=1")
    runner = command.index("/tanksmall/unittests/p50compilee2e-run.sh")
    assert flag < runner
    assert command.count("ICECC_P50_S2_PROCESS_LOSS=1") == 1
    with pytest.raises(executor.ExternalFarmError,
                       match="s2_process_loss:flag_invalid"):
        executor.build_external_command(
            Path("/tanksmall/batch.jsonl"), Path("/tanksmall/plan.json"),
            Path("/tanksmall/topology.json"), Path("/tanksmall/product"),
            profile="ZSTD_TU", corpus="DuckDB", regime="cold", depth="100",
            suite="C1F1/100000", workdir=Path("/tmp/p50compilee2e.external"),
            timeout_seconds=900, s2_process_loss=1)  # type: ignore[arg-type]


def test_s2_external_seam_is_f_only_and_requires_original_compile() -> None:
    source = Path(executor.__file__).read_text(encoding="utf-8")
    shell = (Path(__file__).resolve().parents[1] / "unittests" /
             "p50compilee2e-run.sh").read_text(encoding="utf-8")
    assert "-e ICECC_P50_TEST_ACTION_HOLD=F:TX_BEGIN" in source
    client_start = source.index("client_daemon =")
    client_end = source.index("reset_path =", client_start)
    assert "ICECC_P50_TEST_ACTION_HOLD" not in source[client_start:client_end]
    assert "s2_process_loss:requires_cached_c1f1" in source
    assert "s2_process_loss:requires_q2_f" in source
    assert 'current_c_guid=$(field "$current_ready" C_STORE_GUID)' in source
    assert 'current_f_guid=$(field "$current_ready" F_STORE_GUID)' in source
    assert 'test "$current_c_guid" = "$after_c_guid"' in source
    assert 'test "$current_f_guid" = "$after_f_guid"' in source
    assert "S2_KILL before_pid=" in source
    assert "external-s2-process-loss.json" in source
    assert "compile_once env-warm" in shell
    assert "wait \"$s2_compile_pid\"" in shell
    assert "original compile did not recover after F sidecar loss" in shell
    assert "original_compile_completed" in shell


def test_s2_external_supervisor_completes_exact_kill_and_verify_control_path(
        tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    authority = _authority(tmp_path)
    transport = executor.SSHTransport(authority, timeout=30)
    before = {"pid": 101, "parent_pid": 77,
              "c_store_guid": "1" * 32, "f_store_guid": "2" * 32}
    after = {"pid": 202, "parent_pid": 77,
             "c_store_guid": "3" * 32, "f_store_guid": "4" * 32}
    marker_sha = "5" * 64
    state = {"batch": False, "kill_ready": False, "verify_ready": False}
    calls: list[tuple[str, str]] = []

    def fake_run(host: str, script: str,
                 args: Sequence[object] = ()) -> subprocess.CompletedProcess[str]:
        calls.append((host, script))
        stdout = ""
        if "stat -c %s" in script and "/scheduler.log" in script:
            stdout = "0\n"
        elif "S2_KILL before_pid=%s" in script:
            assert host == "q2"
            assert subprocess.run(["bash", "-n"], input=script, text=True).returncode == 0
            stdout = (f"S2_KILL before_pid={before['pid']} after_pid={after['pid']} "
                      f"parent_pid={before['parent_pid']} before_c={before['c_store_guid']} "
                      f"after_c={after['c_store_guid']} before_f={before['f_store_guid']} "
                      f"after_f={after['f_store_guid']} marker_sha={marker_sha}\n")
        elif "S2_VERIFY before_pid=%s" in script:
            assert host == "q2"
            assert subprocess.run(["bash", "-n"], input=script, text=True).returncode == 0
            assert 'current_c_guid=$(field "$current_ready" C_STORE_GUID)' in script
            assert 'current_f_guid=$(field "$current_ready" F_STORE_GUID)' in script
            assert 'test "$current_c_guid" = "$after_c_guid"' in script
            assert 'test "$current_f_guid" = "$after_f_guid"' in script
            stdout = (f"S2_VERIFY before_pid={before['pid']} after_pid={after['pid']} "
                      f"parent_pid={before['parent_pid']} before_c={before['c_store_guid']} "
                      f"after_c={after['c_store_guid']} before_f={before['f_store_guid']} "
                      f"after_f={after['f_store_guid']} marker_sha={marker_sha} tx_count=3\n")
        elif "touch " in script and "external-s2-kill.ready" in script:
            state["kill_ready"] = True
        elif "external-s2-process-loss.json.tmp" in script:
            state["verify_ready"] = True
        elif "interference-result.json" in script:
            stdout = json.dumps({
                "schema": executor.INTERFERENCE_SCHEMA, "status": "PASS",
                "timing_eligible": True,
                "phases": [{"phase": phase, "path": f"/tmp/interference-{phase}.jsonl"}
                           for phase in ("before", "during", "after")],
                "events": [],
            }) + "\n"
        else:
            workdir = re.search(
                r"ICECC_P50_C1F1_WORKDIR=(/tmp/p50compilee2e\.external\.[A-Za-z0-9]+)",
                script)
            if ("docker exec --user 0" in script and workdir is not None and
                    "ICECC_P50_S2_PROCESS_LOSS=1" in script):
                state["batch"] = True
                deadline = time.monotonic() + 5
                while not state["verify_ready"] and time.monotonic() < deadline:
                    time.sleep(0.01)
                assert state["verify_ready"]
                stdout = ("PASS: all-P50 C1F1\n"
                          f"S7_WORKDIR={workdir.group(1)}\n"
                          "S8_BATCH_METRICS max_concurrent_admitted_or_compiling_jobs=2\n")
        return subprocess.CompletedProcess([], 0, stdout=stdout, stderr="")

    def fake_probe(_host: str, script: str, args: Sequence[object] = (),
                   **_kwargs: object) -> subprocess.CompletedProcess[str]:
        path = str(args[0]) if args else ""
        present = (state["batch"] and
                   ((path.endswith("external-s2-kill.request") and
                     not state["kill_ready"]) or
                    (path.endswith("external-s2-verify.request") and
                     state["kill_ready"] and not state["verify_ready"])))
        return subprocess.CompletedProcess([], 0 if present else 1, "", "")

    evidence = {
        "schema": "icecream-s2-process-loss-v1", "status": "PASS",
        "role": "F", "relationship": 0, "action": "TX_BEGIN",
        "before": before, "after": after, "marker_sha256": marker_sha,
        "marker_unchanged": True, "release_absent": True,
        "replacement_ready": True, "scheduler_relogin": True,
        "original_compile_completed": True, "tx_begin_records": 3,
    }

    def fake_copy(host: str, _remote: str, destination: Path,
                  _image: str, _timeout: float) -> None:
        destination.mkdir(parents=True, exist_ok=False)
        if host == "q3":
            (destination / "scheduler.container-id").write_text("a" * 64)
            (destination / "scheduler.pid").write_text("100")
            (destination / "c.container-id").write_text("b" * 64)
            (destination / "c.pid").write_text("101")
            (destination / "external-s2-process-loss.json").write_text(
                json.dumps(evidence, sort_keys=True, separators=(",", ":")) + "\n")
        else:
            (destination / "container-id").write_text("c" * 64)
            (destination / "container-pid").write_text("102")
            (destination / "f.log").write_text("")
            (destination / "f-measured-log-offset").write_text("0")
            for phase in ("before", "during", "after"):
                (destination / f"interference-{phase}.jsonl").write_text(
                    json.dumps({"phase": phase}) + "\n")

    monkeypatch.setattr(transport, "run", fake_run)
    monkeypatch.setattr(executor.s4, "run_script", fake_probe)
    monkeypatch.setattr(executor, "_copy_remote_tree", fake_copy)
    result = transport.execute_command(
        topology="C1F1/100000", relationship_hosts=["q2"], profile="ZSTD_TU",
        product_root_remote="/product",
        batch_command=["env", "ICECC_P50_EXTERNAL_FARM=1",
                       "ICECC_P50_S2_PROCESS_LOSS=1",
                       "/product/unittests/p50compilee2e-run.sh"],
        output=tmp_path / "out")
    retained_workdir = Path(result["finalizer_input"]["workdir"])
    try:
        assert result["status"] == "PASS"
        assert state == {"batch": True, "kill_ready": True, "verify_ready": True}
        assert "s2-process-loss.json" in result["retained_artifacts"]
        assert json.loads((tmp_path / "out" / "s2-process-loss.json").read_text()) == evidence
        worker_launch = next(script for host, script in calls
                             if host == "q2" and "docker run -d" in script)
        client_launch = next(script for host, script in calls
                             if host == "q3" and "--no-remote -m 0" in script)
        assert "ICECC_P50_TEST_ACTION_HOLD=F:TX_BEGIN" in worker_launch
        assert "ICECC_P50_TEST_ACTION_HOLD" not in client_launch
    finally:
        shutil.rmtree(retained_workdir, ignore_errors=True)


def test_external_timeout_includes_post_measurement_references() -> None:
    assert executor.external_timeout_seconds(100, 1, False) == 2430
    assert executor.external_timeout_seconds(100, 2, False) == 4830
    assert executor.external_timeout_seconds(100, 2, True) == 7230
    assert executor.external_timeout_seconds(100000, 2, True) == \
        executor.live.MAX_TIMEOUT_SECONDS
    for invalid in (0, -1, True):
        with pytest.raises(executor.ExternalFarmError, match="timeout:arguments_invalid"):
            executor.external_timeout_seconds(invalid, 1, False)


def test_warm_cell_expands_default_transport_to_lifecycle_budget(
        tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    transport = executor.SSHTransport(_authority(tmp_path), timeout=900)
    rows = [{"input": str(index)} for index in range(100)]
    observed: dict[str, int] = {}
    monkeypatch.setattr(executor, "validate_batch_inputs",
                        lambda *args, **kwargs: (rows, []))
    monkeypatch.setattr(executor, "build_external_command",
                        lambda *args, **kwargs: [
                            "env", "ICECC_P50_EXTERNAL_FARM=1",
                            "/product/unittests/p50compilee2e-run.sh"])

    def fake_execute(**_kwargs: object) -> dict[str, object]:
        observed["timeout"] = int(transport.timeout)
        return {"status": "PASS", "finalizer_input": {"manifest_path": "fixture"}}

    transport.execute = fake_execute  # type: ignore[method-assign]
    monkeypatch.setattr(executor.live, "finalize", lambda *args, **kwargs: tmp_path / "done")
    result = executor.execute_and_finalize_external_cell(
        transport, topology="C1F1/100000", relationship_hosts=["q2"],
        profile="ZSTD_ROUTE", batch_manifest=tmp_path / "batch.jsonl",
        predictive_plan=tmp_path / "plan.json", topology_file=tmp_path / "topology.json",
        corpus="DuckDB", regime="warm", depth="100", output=tmp_path / "output",
        product_root=tmp_path / "product")
    assert result == tmp_path / "done"
    assert observed["timeout"] == executor.external_timeout_seconds(100, 1, True)
    assert observed["timeout"] > 900


def test_service_map_heredoc_is_composable(tmp_path: Path) -> None:
    guid = "ab" * 16
    (tmp_path / "f-trace-0.jsonl").write_text(json.dumps(
        {"action": "TX_BEGIN", "actor": "F", "f_store_guid": guid}) + "\n")
    marker = tmp_path / "after-heredoc"
    script = executor.f_service_map_script(str(tmp_path), 1)
    script += f"touch {shlex.quote(str(marker))}\n"
    subprocess.run(["bash", "-c", script], check=True, timeout=5)
    assert marker.is_file()
    assert (tmp_path / "s8-f-service-map.tsv").read_text() == \
        f"0\tp50-f\t{guid}\n"


def test_marker_hook_reports_supervisor_failure_without_timeout(tmp_path: Path) -> None:
    request = tmp_path / "request"
    ready = tmp_path / "ready"
    failed = tmp_path / "failed"
    failed.write_text("collection failed\n")
    result = subprocess.run(
        ["sh", "-c", executor.marker_wait_hook(
            str(request), str(ready), str(failed))],
        text=True, capture_output=True, timeout=2)
    assert result.returncode != 0
    assert request.is_file()
    assert "collection failed" in result.stderr


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
        if "interference-result.json" in script:
            stdout = json.dumps({
                "schema": executor.INTERFERENCE_SCHEMA, "status": "PASS",
                "timing_eligible": True,
                "phases": [{"phase": phase, "path": f"/tmp/interference-{phase}.jsonl"}
                           for phase in ("before", "during", "after")],
                "events": [],
            }) + "\n"
        else:
            workdir = re.search(
                r"ICECC_P50_C1F1_WORKDIR=(/tmp/p50compilee2e\.external\.[A-Za-z0-9]+)",
                script)
            stdout = (("PASS: all-P50 C1F1\n"
                       f"S7_WORKDIR={workdir.group(1)}\n"
                       "S8_BATCH_METRICS max_concurrent_admitted_or_compiling_jobs=2\n")
                      if "docker exec --user 0" in script and workdir is not None else "")
        return subprocess.CompletedProcess([], 0, stdout=stdout, stderr="")
    monkeypatch.setattr(transport, "run", fake_run)
    monkeypatch.setattr(executor.s4, "run_script",
                        lambda *args, **kwargs: subprocess.CompletedProcess([], 1, "", ""))
    copied_images: list[str] = []
    def fake_copy(host: str, remote: str, destination: Path,
                  image: str, timeout: float) -> None:
        copied_images.append(image)
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
            for phase in ("before", "during", "after"):
                (destination / f"interference-{phase}.jsonl").write_text(
                    json.dumps({"phase": phase}) + "\n")
    monkeypatch.setattr(executor, "_copy_remote_tree", fake_copy)
    result = transport.execute_command(topology="C1F1/100000", relationship_hosts=["q2"],
                                       profile="ZSTD_ROUTE", product_root_remote="/product",
                                       batch_command=["env", "ICECC_P50_EXTERNAL_FARM=1", "/product/unittests/p50compilee2e-run.sh"],
                                       output=tmp_path / "out")
    assert result["status"] == "PASS"
    assert any(host == "q3" and "icecc-scheduler" in script for host, script in calls)
    assert any(host == "q2" and "iceccd" in script for host, script in calls)
    assert copied_images == [authority["hosts"]["q3"]["image"]["reference"],
                             authority["hosts"]["q2"]["image"]["reference"]]
    assert any(host == "q3" and "--no-remote -m 0" in script for host, script in calls)
    assert any(host == "q3" and "2>>/probe/work/c-service.stderr" in script
               for host, script in calls)
    assert any(host == "q2" and "2>>/probe/work/f-service.stderr" in script
               for host, script in calls)
    assert any(host == "q3" and "s7-warm-c-action-trace.jsonl" in script
               and "chmod 0666" in script for host, script in calls)
    assert any(host == "q2" and "s7-warm-f-action-trace-0.jsonl" in script
               and "chmod 0666" in script for host, script in calls)
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


@pytest.mark.parametrize(("topology", "profile"), (
    ("C1F20/40", "ZSTD_TU"),
    ("C1F1/100000", "RAW_II"),
))
def test_s2_process_loss_rejects_ambiguous_or_cacheless_cells(
        tmp_path: Path, topology: str, profile: str) -> None:
    authority = _authority(tmp_path)
    hosts = authority["placements"][topology]["relationship_hosts"]
    with pytest.raises(executor.ExternalFarmError,
                       match="s2_process_loss:requires_cached_c1f1"):
        executor.SSHTransport(authority).execute_command(
            topology=topology, relationship_hosts=hosts, profile=profile,
            product_root_remote="/product",
            batch_command=["env", "ICECC_P50_EXTERNAL_FARM=1",
                           "ICECC_P50_S2_PROCESS_LOSS=1",
                           "/product/unittests/p50compilee2e-run.sh"],
            output=tmp_path / "out")


def test_s2_process_loss_requires_exact_q2_f(tmp_path: Path) -> None:
    authority = _authority(tmp_path)
    authority["placements"]["C1F1/100000"]["relationship_hosts"] = ["research7"]
    with pytest.raises(executor.ExternalFarmError,
                       match="s2_process_loss:requires_q2_f"):
        executor.SSHTransport(authority).execute_command(
            topology="C1F1/100000", relationship_hosts=["research7"],
            profile="ZSTD_TU", product_root_remote="/product",
            batch_command=["env", "ICECC_P50_EXTERNAL_FARM=1",
                           "ICECC_P50_S2_PROCESS_LOSS=1",
                           "/product/unittests/p50compilee2e-run.sh"],
            output=tmp_path / "out")


def test_external_cell_adapter_finalizes_immediately_after_execution(
        tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    image = {"reference": executor.s4.PINNED_IMAGE,
             "image_id": executor.s4.EXPECTED_IMAGE_ID,
             "architecture": "amd64", "os": "linux", "created": "now"}
    authority = {"placements": {"C1F1/100000": {"relationship_hosts": ["q2"]}},
                 "hosts": {"q3": {"image": image}}}
    events: list[str] = []
    calls: dict[str, object] = {}
    finalizer_input = {"manifest_path": str(tmp_path / "manifest.json")}

    class FakeTransport:
        def __init__(self) -> None:
            self.authority = authority

        def execute(self, **kwargs: object) -> dict[str, object]:
            events.append("execute")
            calls["execute"] = kwargs
            return {"status": "PASS", "finalizer_input": finalizer_input}

    monkeypatch.setattr(
        executor, "validate_batch_inputs",
        lambda *args, **kwargs: ([{"input": "one"}], []))
    monkeypatch.setattr(
        executor, "build_external_command",
        lambda *args, **kwargs: ["env", "ICECC_P50_EXTERNAL_FARM=1",
                                 "/product/unittests/p50compilee2e-run.sh"])

    def fake_finalize(*args: object, **kwargs: object) -> Path:
        events.append("finalize")
        calls["finalize_args"] = args
        calls["finalize"] = kwargs
        return tmp_path / "finalized"

    monkeypatch.setattr(executor.live, "finalize", fake_finalize)
    transport = FakeTransport()
    result = executor.execute_and_finalize_external_cell(
        transport, topology="C1F1/100000", relationship_hosts=None,
        profile="ZSTD_ROUTE", batch_manifest=tmp_path / "batch.jsonl",
        predictive_plan=tmp_path / "plan.json", topology_file=tmp_path / "topology.json",
        corpus="DuckDB", regime="cold", depth="100", output=tmp_path / "output",
        product_root=tmp_path / "product", timestamp="20260901T120000Z")

    assert result == tmp_path / "finalized"
    assert events == ["execute", "finalize"]
    execute_kwargs = calls["execute"]
    assert isinstance(execute_kwargs, dict)
    assert execute_kwargs["relationship_hosts"] == ["q2"]
    assert execute_kwargs["batch_command"] == [
        "env", "ICECC_P50_EXTERNAL_FARM=1", "/product/unittests/p50compilee2e-run.sh"]
    finalize_kwargs = calls["finalize"]
    assert isinstance(finalize_kwargs, dict)
    assert finalize_kwargs["external_farm"] is finalizer_input
    assert finalize_kwargs["execution_environment"] == "external_farm_product_build"
    assert finalize_kwargs["output"] == tmp_path / "output"
    assert finalize_kwargs["suite"] == "C1F1/100000"
    assert finalize_kwargs["runtime_image"] == image


@pytest.mark.parametrize("mutation", [
    lambda image: image.pop("image_id"),
    lambda image: image.update({"image_id": executor.s4.EXPECTED_IMAGE_CONFIG_ID}),
])
def test_external_cell_adapter_rejects_missing_or_mismatched_compiler_image(
        mutation: object, tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    image = {"reference": executor.s4.PINNED_IMAGE,
             "image_id": executor.s4.EXPECTED_IMAGE_ID,
             "architecture": "amd64", "os": "linux", "created": "now"}
    mutation(image)  # type: ignore[operator]
    authority = {"placements": {"C1F1/100000": {"relationship_hosts": ["q2"]}},
                 "hosts": {"q3": {"image": image}}}

    calls = {"execute": 0, "finalize": 0}

    class FakeTransport:
        def __init__(self) -> None:
            self.authority = authority

        def execute(self, **_kwargs: object) -> dict[str, object]:
            calls["execute"] += 1
            return {"status": "PASS", "finalizer_input": {"fixture": True}}

    monkeypatch.setattr(executor, "validate_batch_inputs",
                        lambda *args, **kwargs: ([{"input": "one"}], []))
    monkeypatch.setattr(executor, "build_external_command",
                        lambda *args, **kwargs: ["env", "ICECC_P50_EXTERNAL_FARM=1",
                                                 "/product/unittests/p50compilee2e-run.sh"])
    monkeypatch.setattr(executor.live, "finalize",
                        lambda *args, **kwargs: calls.__setitem__("finalize", calls["finalize"] + 1))
    with pytest.raises(executor.ExternalFarmError, match="authority:q3:image_invalid"):
        executor.execute_and_finalize_external_cell(
            FakeTransport(), topology="C1F1/100000", relationship_hosts=["q2"],
            profile="ZSTD_ROUTE", batch_manifest=tmp_path / "batch.jsonl",
            predictive_plan=tmp_path / "plan.json", topology_file=tmp_path / "topology.json",
            corpus="DuckDB", regime="cold", depth="100", output=tmp_path / "output",
            product_root=tmp_path / "product")
    assert calls == {"execute": 0, "finalize": 0}


def test_external_cell_adapter_rejects_execution_without_finalizer_input(
        tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    image = {"reference": executor.s4.PINNED_IMAGE,
             "image_id": executor.s4.EXPECTED_IMAGE_ID,
             "architecture": "amd64", "os": "linux", "created": "now"}
    class FakeTransport:
        authority = {"placements": {"C1F1/100000": {"relationship_hosts": ["q2"]}},
                     "hosts": {"q3": {"image": image}}}

        def execute(self, **_kwargs: object) -> dict[str, object]:
            return {"status": "PASS"}

    monkeypatch.setattr(
        executor, "validate_batch_inputs",
        lambda *args, **kwargs: ([{"input": "one"}], []))
    monkeypatch.setattr(
        executor, "build_external_command",
        lambda *args, **kwargs: ["env", "ICECC_P50_EXTERNAL_FARM=1",
                                 "/product/unittests/p50compilee2e-run.sh"])
    with pytest.raises(executor.ExternalFarmError, match="finalizer_input_missing"):
        executor.execute_and_finalize_external_cell(
            FakeTransport(), topology="C1F1/100000", relationship_hosts=["q2"],
            profile="ZSTD_ROUTE", batch_manifest=tmp_path / "batch.jsonl",
            predictive_plan=tmp_path / "plan.json", topology_file=tmp_path / "topology.json",
            corpus="DuckDB", regime="cold", depth="100", output=tmp_path / "output",
            product_root=tmp_path / "product")
