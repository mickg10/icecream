from __future__ import annotations

import copy
import hashlib
import io
import json
import os
import shutil
import subprocess
import tarfile
from pathlib import Path

import pytest

from farmharness.integration.tests import farm_fixture

from farmharness.integration import farmtest
from farmharness.integration.farm_spec import FarmSpec, FarmSpecError, load_farm_spec
from farmharness.integration.scenario_spec import ScenarioSpec, load_scenario_spec
from farmharness.integration.system_source_snapshot import (
    SystemSourceSnapshotError,
    verify_local_archive,
    private_root,
)
from farmharness.integration.lifecycle import (
    PreflightRefusal,
    SYSTEM_SOURCE_MATERIALIZE_SCRIPT,
    SYSTEM_SOURCE_VERIFY_SCRIPT,
    _materialize_system_source_snapshot,
)
from farmharness.integration.images import CommandFactory
from farmharness.integration.remote import CommandResult


INTEGRATION = Path(__file__).resolve().parents[1]


def test_private_snapshot_paths_isolate_runs_workers_and_shared_source() -> None:
    from farmharness.integration.system_source_snapshot import derived_root

    digest = "a" * 64
    roots = {
        private_root("/scratch", digest, run, worker)
        for run in ("run-a", "run-b") for worker in ("F1", "F2")
    }
    assert len(roots) == 4
    assert derived_root("/scratch", digest) not in roots
    assert str(private_root("/scratch", digest, "run-a", "F2")) == (
        "/scratch/icefarm/run-a/F2/system-source/" + digest
    )


@pytest.mark.parametrize("unsafe", ["", ".", "..", "a/b", "../escape", "/absolute", "a\n", "a" * 81])
def test_private_snapshot_rejects_unsafe_ownership(unsafe: str) -> None:
    with pytest.raises(SystemSourceSnapshotError):
        private_root("/scratch", "a" * 64, unsafe, "F2")
    with pytest.raises(SystemSourceSnapshotError):
        private_root("/scratch", "a" * 64, "run-a", unsafe)


def _snapshot_farm() -> FarmSpec:
    farm = load_farm_spec(farm_fixture.example_farm_path())
    data = copy.deepcopy(farm.data)
    data["system_source_snapshots"] = {
        "stable-f-source": {
            "schema": "icefarm-system-source-snapshot-v1",
            "source": "runtime",
            "source_runtime_reference": data["runtime_image"]["reference"],
            "source_runtime_id": data["runtime_image"]["id"],
            "source_runtime_closure_sha256": data["runtime_image"]["closure_sha256"],
            "manifest_sha256": "a" * 64,
            "file_count": 16805,
            "enumeration": {
                "schema": "icefarm-system-source-manifest-v1",
                "roots": ["/usr/include", "/usr/lib/gcc", "/usr/local/include"],
                "symlink_policy": "no-directory-follow;regular-file-symlinks-regularized",
            },
            "archive": {
                "path": "/tmp/icefarm-system-source.tar.zst",
                "sha256": "b" * 64,
                "archive_bytes": 100,
                "unpacked_bytes": 200,
                "compression": {
                    "checksum": True, "codec": "zstd", "level": 19,
                    "long": 31, "threads": 8,
                },
            },
        }
    }
    return FarmSpec(path=farm.path, data=data)


def _snapshot_scenario(farm: FarmSpec, filename: str = "S80-p29v1.json") -> ScenarioSpec:
    value = json.loads(
        (INTEGRATION / "scenarios" / filename).read_text(encoding="utf-8")
    )
    for instance in value["instances"]:
        if instance["role"] == "C":
            instance["system_source_snapshot"] = "stable-f-source"
    path = INTEGRATION / "scenarios" / filename
    # The loader validates against the in-memory farm; no scenario file is written.
    temporary = path.with_name(".unit-snapshot-s80.json")
    temporary.write_text(json.dumps(value), encoding="utf-8")
    try:
        return load_scenario_spec(temporary, farm)
    finally:
        temporary.unlink()


def test_snapshot_is_authority_bound_and_mounts_are_in_topology_digest() -> None:
    farm = _snapshot_farm()
    scenario = _snapshot_scenario(farm)
    plan = farmtest.build_plan(farm, scenario, run_id="snapshot-unit")
    client = next(item for item in plan["topology"]["instances"] if item["role"] == "C")
    assert client["system_source_snapshot"]["manifest_sha256"] == "a" * 64
    starts = [
        command for command in plan["commands"]
        if command["phase"] == "up.start-c" and command["instance"] == client["name"]
    ]
    assert len(starts) == 1
    argv = starts[0]["argv"]
    for destination in ("/usr/include", "/usr/lib/gcc", "/usr/local/include"):
        assert any(destination in value and "readonly" in value for value in argv)

    altered = copy.deepcopy(farm.data)
    altered["system_source_snapshots"]["stable-f-source"]["manifest_sha256"] = "b" * 64
    altered_farm = FarmSpec(path=farm.path, data=altered)
    altered_scenario = _snapshot_scenario(altered_farm)
    altered_plan = farmtest.build_plan(altered_farm, altered_scenario, run_id="snapshot-unit")
    assert altered_plan["topology_digest"] != plan["topology_digest"]


def test_worker_snapshot_plan_is_private_writable_and_replay_derivable() -> None:
    farm = _snapshot_farm()
    original = load_scenario_spec(INTEGRATION / "scenarios/S40-full-newgen-engagement.json", farm)
    assert all(item.get("system_source_snapshot") == "stable-f-source" for item in original.data["instances"] if item["role"] == "F")
    data = copy.deepcopy(original.data)
    for instance in data["instances"]:
        if instance["role"] == "F":
            instance["system_source_snapshot"] = "stable-f-source"
    scenario = ScenarioSpec(path=original.path, data=data)
    plan = farmtest.build_plan(farm, scenario, run_id="private-s40")
    next_plan = farmtest.build_plan(farm, scenario, run_id="private-s40-next")
    assert plan["topology_digest"] != next_plan["topology_digest"]
    workers = [item for item in plan["topology"]["instances"] if item["role"] == "F"]
    assert len({item["system_source_snapshot"]["root"] for item in workers}) == len(workers)
    for worker in workers:
        snapshot = worker["system_source_snapshot"]
        assert snapshot["scope"] == "run-instance"
        commands = [item for item in plan["commands"] if item["phase"] == "up.start-f" and item["instance"] == worker["name"]]
        assert len(commands) == 1
        for destination, source in snapshot["mounts"].items():
            assert f"type=bind,src={source},dst={destination}" in commands[0]["argv"]
            assert f"/private-s40/{worker['name']}/system-source/" in source
    rebound = farmtest._bind_system_source_snapshots(farm, data, farmtest.resolve_topology(farm, scenario), "private-s40")
    assert rebound == plan["topology"]
    tampered = copy.deepcopy(plan["topology"])
    target = next(item for item in tampered["instances"] if item["role"] == "F")
    target["system_source_snapshot"]["mounts"]["/usr/include"] = "/shared/usr/include"
    with pytest.raises(farmtest.PlanError, match="not run-private"):
        farmtest._planned_commands(farm, scenario, tampered, plan["ports"], "private-s40")


def test_snapshot_refuses_bad_enumeration_and_extra_archive_fields() -> None:
    farm = _snapshot_farm()
    farm.data["system_source_snapshots"]["stable-f-source"]["enumeration"]["roots"] = ["/etc"]
    path = Path("/tmp/i-snapshot-invalid.json")
    path.write_text(json.dumps(farm.data), encoding="utf-8")
    try:
        with pytest.raises(FarmSpecError):
            load_farm_spec(path)
    finally:
        path.unlink(missing_ok=True)

    farm = _snapshot_farm()
    farm.data["system_source_snapshots"]["stable-f-source"]["archive"]["compression"]["level"] = 18
    with pytest.raises(FarmSpecError):
        path = Path("/tmp/i-snapshot-extra.json")
        path.write_text(json.dumps(farm.data), encoding="utf-8")
        try:
            load_farm_spec(path)
        finally:
            path.unlink(missing_ok=True)


def test_snapshot_archive_verification_rejects_missing_and_wrong_content(tmp_path: Path) -> None:
    farm = _snapshot_farm()
    snapshot = copy.deepcopy(farm.data["system_source_snapshots"]["stable-f-source"])
    archive = tmp_path / "source.tar.zst"
    archive.write_bytes(b"authoritative-bytes")
    snapshot["archive"]["path"] = str(archive)
    snapshot["archive"]["archive_bytes"] = archive.stat().st_size
    import hashlib

    snapshot["archive"]["sha256"] = hashlib.sha256(archive.read_bytes()).hexdigest()
    assert verify_local_archive(snapshot)["archive_sha256"] == snapshot["archive"]["sha256"]
    archive.write_bytes(b"tampered")
    with pytest.raises(SystemSourceSnapshotError, match="(byte count|digest) differs"):
        verify_local_archive(snapshot)


class _SnapshotRecorder:
    def __init__(self, archive_sha: str, manifest_sha: str, count: int, statuses=None, materialize_value=None) -> None:
        self.archive_sha = archive_sha
        self.manifest_sha = manifest_sha
        self.count = count
        self.commands = []
        self.verify_calls = 0
        self.statuses = list(statuses or ("absent", "ready"))
        self.materialize_value = materialize_value

    def invoke(self, command):
        self.commands.append(command)
        if command.phase == "preflight.system-source-verify":
            self.verify_calls += 1
            status = self.statuses[min(self.verify_calls - 1, len(self.statuses) - 1)]
            if isinstance(status, dict):
                output = json.dumps(status)
            elif status == "absent":
                output = '{"status":"absent"}\n'
            elif status in {"unsafe", "unknown"}:
                output = json.dumps({"status": status})
            elif status == "mismatch":
                output = json.dumps({"status": "mismatch", "bytes": 1, "sha256": "0" * 64})
            else:
                output = json.dumps({"status": "ready", "bytes": 7, "sha256": self.archive_sha})
            return CommandResult(0, output, "")
        if command.phase == "preflight.system-source-materialize":
            output = json.dumps(self.materialize_value or {"file_count": self.count, "manifest_sha256": self.manifest_sha})
            return CommandResult(0, output, "")
        return CommandResult(0, "", "")


@pytest.mark.parametrize("role", ["C", "F"])
def test_snapshot_preflight_syncs_and_materializes_from_derived_paths(tmp_path: Path, role: str) -> None:
    farm = _snapshot_farm()
    scenario = _snapshot_scenario(farm)
    plan = farmtest.build_plan(farm, scenario, run_id="snapshot-preflight")
    instance = next(item for item in plan["topology"]["instances"] if item["role"] == "C")
    instance = copy.deepcopy(instance)
    instance["role"] = role
    snapshot = farm.data["system_source_snapshots"]["stable-f-source"]
    archive = tmp_path / "source.tar.zst"
    archive.write_bytes(b"archive")
    import hashlib
    snapshot["archive"]["path"] = str(archive)
    snapshot["archive"]["archive_bytes"] = archive.stat().st_size
    snapshot["archive"]["sha256"] = hashlib.sha256(archive.read_bytes()).hexdigest()
    recorder = _SnapshotRecorder(snapshot["archive"]["sha256"], snapshot["manifest_sha256"], snapshot["file_count"])
    receipt = _materialize_system_source_snapshot(
        farm, instance, snapshot, "snapshot-preflight", recorder, CommandFactory(), 30
    )
    assert receipt["manifest_sha256"] == snapshot["manifest_sha256"]
    assert receipt["root"].endswith(snapshot["manifest_sha256"])
    if role == "F":
        expected = private_root(farm.hosts[instance["host"]]["scratch_root"], snapshot["manifest_sha256"], "snapshot-preflight", instance["name"])
        assert receipt["root"] == str(expected)
        assert receipt["scope"] == "run-instance"
        assert receipt["instance"] == instance["name"]
        assert all(str(expected) in path for path in receipt["mounts"].values())
        assert f"src={expected.parent},dst=/icefarm-system-source" in " ".join(recorder.commands[-1].argv)
    phases = [command.phase for command in recorder.commands]
    assert phases == [
        "preflight.system-source-mkdir",
        "preflight.system-source-verify",
        "preflight.system-source-sync",
        "preflight.system-source-verify",
        "preflight.system-source-materialize",
    ]
    materialize = recorder.commands[-1]
    assert "readonly" in " ".join(materialize.argv)
    assert materialize.argv[-3] == snapshot["archive"]["sha256"]
    assert materialize.argv[-2] == snapshot["manifest_sha256"]
    archive.unlink()
    with pytest.raises(SystemSourceSnapshotError, match="absent"):
        verify_local_archive(snapshot)


def test_snapshot_scripts_bind_distinct_archive_manifest_keys_and_fail_closed() -> None:
    assert "archive_key=$1" in SYSTEM_SOURCE_MATERIALIZE_SCRIPT
    assert "key=$2" in SYSTEM_SOURCE_MATERIALIZE_SCRIPT
    assert 'archive="/icefarm-system-source-archives/$archive_key.tar.zst"' in SYSTEM_SOURCE_MATERIALIZE_SCRIPT
    assert 'target="$base/$key"' in SYSTEM_SOURCE_MATERIALIZE_SCRIPT
    assert 'case "$archive_key"' in SYSTEM_SOURCE_MATERIALIZE_SCRIPT
    assert 'case "$key"' in SYSTEM_SOURCE_MATERIALIZE_SCRIPT
    assert "verify_existing" in SYSTEM_SOURCE_MATERIALIZE_SCRIPT
    assert "directory symlink is unsafe" in SYSTEM_SOURCE_MATERIALIZE_SCRIPT
    assert 'if path.is_symlink()' in SYSTEM_SOURCE_VERIFY_SCRIPT
    assert '"status": "unsafe"' in SYSTEM_SOURCE_VERIFY_SCRIPT


def _make_system_source_archive(tmp_path: Path, kind: str = "valid") -> tuple[Path, str, str]:
    raw = tmp_path / "source.tar"
    entries = {
        "usr/include/a.h": b"alpha\n",
        "usr/lib/gcc/compiler.h": b"beta\n",
        "usr/local/include/c.h": b"gamma\n",
    }
    with tarfile.open(raw, "w") as archive:
        if kind == "valid":
            for name, data in entries.items():
                info = tarfile.TarInfo(name)
                info.size = len(data)
                archive.addfile(info, io.BytesIO(data))
        elif kind == "traversal":
            info = tarfile.TarInfo("usr/include/../../escape")
            info.size = 1
            archive.addfile(info, io.BytesIO(b"x"))
        elif kind == "outside":
            info = tarfile.TarInfo("etc/passwd")
            info.size = 1
            archive.addfile(info, io.BytesIO(b"x"))
        elif kind == "link":
            info = tarfile.TarInfo("usr/include/link")
            info.type = tarfile.SYMTYPE
            info.linkname = "/etc/passwd"
            archive.addfile(info)
        elif kind == "special":
            info = tarfile.TarInfo("usr/include/device")
            info.type = tarfile.CHRTYPE
            info.devmajor, info.devminor = 1, 3
            archive.addfile(info)
        elif kind == "backslash":
            info = tarfile.TarInfo(r"usr/include\\outside.h")
            info.size = 1
            archive.addfile(info, io.BytesIO(b"x"))
        else:
            raise AssertionError(kind)
    compressed = tmp_path / "source.tar.zst"
    subprocess.run(
        ["zstd", "-q", "-19", "--long=31", "--check", "-f", str(raw), "-o", str(compressed)],
        check=True,
    )
    manifest_rows = sorted(
        "/" + name + "\t" + hashlib.sha256(data).hexdigest() + "\n"
        for name, data in entries.items()
    )
    manifest = hashlib.sha256("".join(manifest_rows).encode()).hexdigest()
    return compressed, hashlib.sha256(compressed.read_bytes()).hexdigest(), manifest


def _run_local_materializer(tmp_path: Path, archive: Path, archive_sha: str, manifest: str, count: int = 3):
    archive_parent = tmp_path / "archives"
    snapshot_parent = tmp_path / "snapshots"
    archive_parent.mkdir(exist_ok=True)
    snapshot_parent.mkdir(exist_ok=True)
    remote_archive = archive_parent / f"{archive_sha}.tar.zst"
    shutil.copyfile(archive, remote_archive)
    script = SYSTEM_SOURCE_MATERIALIZE_SCRIPT.replace(
        "/icefarm-system-source-archives", str(archive_parent)
    ).replace(
        "/icefarm-system-source", str(snapshot_parent)
    )
    return subprocess.run(
        ["bash", "-c", script, "materialize", archive_sha, manifest, str(count)],
        text=True,
        capture_output=True,
        env={**os.environ, "ICEFARM_TMPDIR": str(tmp_path)},
    ), snapshot_parent / manifest


@pytest.mark.skipif(shutil.which("zstd") is None, reason="zstd is required for executable archive tests")
def test_independent_extractions_do_not_share_header_inodes(tmp_path: Path) -> None:
    archive, archive_sha, manifest = _make_system_source_archive(tmp_path)
    headers = []
    for owner in ("shared", "run-a-F1", "run-a-F2", "run-b-F2"):
        directory = tmp_path / owner
        directory.mkdir()
        result, target = _run_local_materializer(directory, archive, archive_sha, manifest)
        assert result.returncode == 0, result.stderr
        headers.append(target / "usr/include/a.h")
    assert len({(path.stat().st_dev, path.stat().st_ino) for path in headers}) == 4
    headers[2].chmod(0o600)
    headers[2].write_bytes(b"alpha\n/* S40 header edit */\n")
    for index in (0, 1, 3):
        assert headers[index].read_bytes() == b"alpha\n"
    assert hashlib.sha256(archive.read_bytes()).hexdigest() == archive_sha


@pytest.mark.skipif(shutil.which("zstd") is None, reason="zstd is required for executable archive tests")
def test_materializer_executes_first_existing_tamper_missing_extra_and_symlink_paths(tmp_path: Path) -> None:
    archive, archive_sha, manifest = _make_system_source_archive(tmp_path)
    first, target = _run_local_materializer(tmp_path, archive, archive_sha, manifest)
    assert first.returncode == 0, first.stderr
    assert json.loads(first.stdout) == {"file_count": 3, "manifest_sha256": manifest}
    second, _ = _run_local_materializer(tmp_path, archive, archive_sha, manifest)
    assert second.returncode == 0, second.stderr
    assert json.loads(second.stdout) == {"file_count": 3, "manifest_sha256": manifest}

    tampered = target / "usr/include/a.h"
    tampered.chmod(0o600)
    tampered.write_bytes(b"tampered")
    failed, _ = _run_local_materializer(tmp_path, archive, archive_sha, manifest)
    assert failed.returncode != 0
    tampered.write_bytes(b"alpha\n")
    (target / "usr/lib/gcc").chmod(0o700)
    (target / "usr/lib/gcc/compiler.h").unlink()
    failed, _ = _run_local_materializer(tmp_path, archive, archive_sha, manifest)
    assert failed.returncode != 0
    (target / "usr/lib/gcc").mkdir(parents=True, exist_ok=True)
    (target / "usr/lib/gcc/compiler.h").write_bytes(b"beta\n")
    (target / "usr/lib/gcc/compiler.h").chmod(0o600)
    (target / "usr/lib/gcc/extra.h").write_bytes(b"extra")
    failed, _ = _run_local_materializer(tmp_path, archive, archive_sha, manifest)
    assert failed.returncode != 0
    for path in target.rglob("*"):
        path.chmod(0o700)
    target.chmod(0o700)
    shutil.rmtree(target)
    target.symlink_to(tmp_path / "outside")
    failed, _ = _run_local_materializer(tmp_path, archive, archive_sha, manifest)
    assert failed.returncode != 0


@pytest.mark.skipif(shutil.which("zstd") is None, reason="zstd is required for executable archive tests")
@pytest.mark.parametrize("kind", ("traversal", "outside", "link", "special", "backslash"))
def test_materializer_rejects_unsafe_archive_members_and_cleans_temp(tmp_path: Path, kind: str) -> None:
    archive, archive_sha, manifest = _make_system_source_archive(tmp_path, kind)
    result, target = _run_local_materializer(tmp_path, archive, archive_sha, manifest, count=3)
    assert result.returncode != 0
    assert not target.exists()
    assert not list(target.parent.glob(f".materialize-{manifest}-*"))


@pytest.mark.parametrize(
    "statuses, message",
    [
        (("unsafe",), "unsafe or mismatched"),
        (("mismatch",), "unsafe or mismatched"),
        (("unknown",), "unknown status"),
        (({"status": "absent", "bytes": 7},), "extra status fields"),
        (({"status": "ready", "bytes": 7},), "incomplete status fields"),
    ],
)
def test_snapshot_remote_verifier_refuses_non_absent_states_without_sync(
    tmp_path: Path, statuses, message: str
) -> None:
    farm = _snapshot_farm()
    scenario = _snapshot_scenario(farm)
    plan = farmtest.build_plan(farm, scenario, run_id="snapshot-status")
    instance = next(item for item in plan["topology"]["instances"] if item["role"] == "C")
    snapshot = farm.data["system_source_snapshots"]["stable-f-source"]
    archive = tmp_path / "source.tar.zst"
    archive.write_bytes(b"archive")
    import hashlib
    snapshot["archive"]["path"] = str(archive)
    snapshot["archive"]["archive_bytes"] = archive.stat().st_size
    snapshot["archive"]["sha256"] = hashlib.sha256(archive.read_bytes()).hexdigest()
    recorder = _SnapshotRecorder(snapshot["archive"]["sha256"], snapshot["manifest_sha256"], snapshot["file_count"], statuses)
    with pytest.raises(PreflightRefusal, match=message):
        _materialize_system_source_snapshot(
            farm, instance, snapshot, "snapshot-status", recorder, CommandFactory(), 30
        )
    assert "preflight.system-source-sync" not in [command.phase for command in recorder.commands]


def test_snapshot_materializer_receipt_requires_exact_keyset_and_types(tmp_path: Path) -> None:
    farm = _snapshot_farm()
    scenario = _snapshot_scenario(farm)
    plan = farmtest.build_plan(farm, scenario, run_id="snapshot-receipt")
    instance = next(item for item in plan["topology"]["instances"] if item["role"] == "C")
    snapshot = farm.data["system_source_snapshots"]["stable-f-source"]
    archive = tmp_path / "source.tar.zst"
    archive.write_bytes(b"archive")
    snapshot["archive"]["path"] = str(archive)
    snapshot["archive"]["archive_bytes"] = archive.stat().st_size
    snapshot["archive"]["sha256"] = hashlib.sha256(archive.read_bytes()).hexdigest()
    recorder = _SnapshotRecorder(
        snapshot["archive"]["sha256"], snapshot["manifest_sha256"], snapshot["file_count"],
        materialize_value={"file_count": snapshot["file_count"], "manifest_sha256": snapshot["manifest_sha256"], "extra": True},
    )
    with pytest.raises(PreflightRefusal, match="malformed materialization receipt"):
        _materialize_system_source_snapshot(
            farm, instance, snapshot, "snapshot-receipt", recorder, CommandFactory(), 30
        )


@pytest.mark.parametrize(
    "filename",
    (
        "S40-full-newgen-engagement.json",
        "S80-p29v1.json",
        "S80-zstd-tu.json",
        "S80-zstd-route.json",
        "S80-legacy.json",
    ),
)
def test_reuse_and_performance_scenarios_select_the_stable_f_snapshot(
    filename: str,
) -> None:
    farm = _snapshot_farm()
    scenario = load_scenario_spec(INTEGRATION / "scenarios" / filename, farm)
    plan = farmtest.build_plan(farm, scenario, run_id="snapshot-s80-unit")
    clients = [item for item in plan["topology"]["instances"] if item["role"] == "C"]
    assert len(clients) == 1
    assert clients[0]["system_source_snapshot"]["name"] == "stable-f-source"


def test_s40_fmt_records_conservative_cross_system_non_reuse() -> None:
    farm = _snapshot_farm()
    scenario = load_scenario_spec(
        INTEGRATION / "scenarios" / "S40-engagement-fmt.json", farm
    )
    plan = farmtest.build_plan(farm, scenario, run_id="snapshot-s40-fmt-unit")
    clients = [item for item in plan["topology"]["instances"] if item["role"] == "C"]
    assert len(clients) == 1
    assert "system_source_snapshot" not in clients[0]
    assert scenario.data["timeline"] == []
    assert scenario.data["expect"]["reuse"] == "all-false-when-p29v1"
