from __future__ import annotations

import hashlib
import json
import subprocess
from pathlib import Path
from typing import Sequence

import pytest

import s8_external_farm_executor as executor


def _authority(tmp_path: Path) -> dict[str, object]:
    hosts: dict[str, object] = {}
    for host in executor.HOSTS:
        descriptor = tmp_path / f"{host}.descriptor"
        descriptor.write_text(json.dumps({"host": host}))
        sha, size = executor._sha(descriptor)
        hosts[host] = {
            "target": f"mickg@{host}", "descriptor": {"path": str(descriptor), "sha256": sha, "bytes": size},
            "physical_host_digest": hashlib.sha256(host.encode()).hexdigest(),
            "cpu_count": executor.CPU_COUNTS[host],
            "idle": {"status": "PASS", "load_1m": 0.1},
            "image": {"image_id": "sha256:" + "a" * 64, "architecture": "amd64", "os": "linux"},
            "binaries": {role: hashlib.sha256((host + role).encode()).hexdigest() for role in {
                "scheduler/icecc-scheduler", "daemon/iceccd", "client/icecc",
                "client/icecc-create-env", "cache/icecc-cache-service"}},
        }
    return {"schema": executor.AUTHORITY_SCHEMA, "hosts": hosts}


def test_placement_has_no_q3_f_and_disjoint_physical_ids(tmp_path: Path) -> None:
    authority = _authority(tmp_path)
    placement = executor.role_placement(authority, "C1F20/40", ["q2"] * 15 + ["research7"] * 5)
    assert placement["c_host_digest"] not in placement["f_host_digests"]
    with pytest.raises(executor.ExternalFarmError, match="q3_f_forbidden"):
        executor.role_placement(authority, "C1F1/100000", ["q3"])


def test_parallel_gate_requires_all_lanes_and_overlap() -> None:
    with pytest.raises(executor.ExternalFarmError, match="parallel_overlap_missing"):
        executor.overlap_required("C1F20/40", {"planned_lanes": 40, "max_concurrent": 1})
    executor.overlap_required("C1F20/40", {"planned_lanes": 40, "max_concurrent": 2})


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


def test_concrete_transport_invokes_q3_c_scheduler_and_non_q3_f(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    authority = _authority(tmp_path)
    transport = executor.SSHTransport(authority)
    calls: list[tuple[str, str]] = []
    def fake_run(host: str, script: str, args: Sequence[object] = ()) -> subprocess.CompletedProcess[str]:
        calls.append((host, script))
        stdout = ("PASS: all-P50 C1F1\nS8_BATCH_METRICS max_concurrent_admitted_or_compiling_jobs=2\n"
                  if script.startswith("exec env ") else "")
        return subprocess.CompletedProcess([], 0, stdout=stdout, stderr="")
    monkeypatch.setattr(transport, "run", fake_run)
    monkeypatch.setattr(executor.s4, "copy_remote_tree", lambda *args, **kwargs: None)
    result = transport.execute_command(topology="C1F1/100000", relationship_hosts=["q2"],
                                       profile="ZSTD_ROUTE", product_root_remote="/product",
                                       batch_command=["env", "ICECC_P50_EXTERNAL_FARM=1", "/product/run"],
                                       output=tmp_path / "out")
    assert result["status"] == "PASS"
    assert any(host == "q3" and "icecc-scheduler" in script for host, script in calls)
    assert any(host == "q2" and "iceccd" in script for host, script in calls)
    assert any(host == "q3" and "--no-remote -m 0" in script for host, script in calls)
    assert all(not (host == "q3" and "-N p50-f" in script) for host, script in calls)
