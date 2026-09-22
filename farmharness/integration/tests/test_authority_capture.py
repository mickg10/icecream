from __future__ import annotations

import hashlib
import json
from datetime import datetime, timezone
from pathlib import Path

import pytest

from farmharness.integration.tests import farm_fixture

from farmharness.integration import authority, farmtest
from farmharness.integration.farm_spec import FarmSpecError, load_farm_spec
from farmharness.integration.remote import CommandResult, decode_ssh_payload
from farmharness.integration.schema_validation import canonical_bytes


INTEGRATION = Path(__file__).resolve().parents[1]
NOW = datetime(2026, 9, 4, 20, 0, 0, tzinfo=timezone.utc)
DOCKER_INFO = {
    "Architecture": "amd64",
    "DockerRootDir": "/var/lib/docker",
    "MemTotal": 512 * 1024**3,
    "NCPU": 32,
    "Name": "fixture",
    "OSType": "linux",
    "ServerVersion": "28.0.0",
}


def _farm(tmp_path: Path):
    document = json.loads((farm_fixture.example_farm_path()).read_text(encoding="utf-8"))
    path = tmp_path / "farm-template.json"
    path.write_bytes(canonical_bytes(document))
    return path, load_farm_spec(path)


def _host_payload(host: str, farm, *, identity_host: str | None = None) -> str:
    policy = farm.hosts[host]
    identity_host = identity_host or host
    value = {
        "arch": policy["arch"],
        "boot_id_sha256": hashlib.sha256(f"boot:{identity_host}".encode()).hexdigest(),
        "captured_at": NOW.strftime("%Y-%m-%dT%H:%M:%SZ"),
        "cpu_count": policy["cores"],
        "hostname": host,
        "ipv4": [policy["lan_ip"]],
        "machine_id_sha256": hashlib.sha256(f"machine:{identity_host}".encode()).hexdigest(),
        "mem_bytes": policy["mem_gb"] * 1024**3,
        "nic_identity_sha256": hashlib.sha256(f"nic:{identity_host}".encode()).hexdigest(),
        "schema": "icecream-newgen-host-capture-v2",
    }
    return json.dumps(value, sort_keys=True, separators=(",", ":"))


class _Recorder:
    def __init__(self, farm, *, bad_arch: bool = False, duplicate_host: bool = False):
        self.farm = farm
        self.bad_arch = bad_arch
        self.duplicate_host = duplicate_host
        self.commands = []

    def invoke(self, command):
        self.commands.append(command)
        if command.phase == "authority.probe-host":
            identity_host = sorted(self.farm.hosts)[0] if self.duplicate_host else command.host
            return CommandResult(
                0,
                _host_payload(command.host, self.farm, identity_host=identity_host),
                "",
            )
        docker_info = dict(DOCKER_INFO)
        docker_info["NCPU"] = self.farm.hosts[command.host]["cores"]
        if self.bad_arch:
            docker_info["Architecture"] = "arm64"
        return CommandResult(0, json.dumps(docker_info), "")


def test_authority_dry_run_is_pure_stable_and_argv_only(tmp_path: Path) -> None:
    farm_path, farm = _farm(tmp_path)
    output = tmp_path / "farm.local.json"
    descriptors = tmp_path / "descriptors"
    argv = [
        "authority",
        "capture",
        "--farm",
        str(farm_path),
        "--output",
        str(output),
        "--descriptor-dir",
        str(descriptors),
        "--dry-run",
    ]
    assert farmtest.main(argv) == 0
    first = authority.render_capture_plan(farm)
    assert farmtest.main(argv) == 0
    assert authority.render_capture_plan(farm) == first
    assert not output.exists()
    assert not descriptors.exists()
    plan = json.loads(first)
    assert plan["schema"] == authority.PLAN_SCHEMA
    assert plan["commands"]
    probe = next(item for item in plan["commands"] if item["phase"] == "authority.probe-host")
    assert decode_ssh_payload(probe["argv"])[0] == "env"


def test_capture_writes_complete_valid_newgen_farm(tmp_path: Path) -> None:
    _farm_path, farm = _farm(tmp_path)
    recorder = _Recorder(farm)
    output = tmp_path / "farm.local.json"
    descriptors = tmp_path / "descriptors"
    receipt = authority.capture_authority(
        farm,
        output=output,
        descriptor_dir=descriptors,
        recorder=recorder,
        now=NOW,
    )
    loaded = load_farm_spec(output)
    assert loaded.data["authority"]["schema"] == "icecream-newgen-farm-authority-v1"
    assert loaded.data["authority_capture"]["schema"] == authority.CAPTURE_SCHEMA
    assert set(loaded.data["authority_capture"]["hosts"]) == set(farm.hosts)
    assert "placements" not in loaded.data["authority"]
    assert receipt["hosts"] == sorted(farm.hosts)
    assert len(list(descriptors.glob("*.json"))) == len(farm.hosts)
    assert len(recorder.commands) == len(authority.authority_capture_plan(farm)["commands"])


def test_capture_refuses_docker_identity_mismatch_before_writes(tmp_path: Path) -> None:
    _farm_path, farm = _farm(tmp_path)
    output = tmp_path / "farm.local.json"
    descriptors = tmp_path / "descriptors"
    with pytest.raises(authority.AuthorityCaptureError, match="architecture differs"):
        authority.capture_authority(
            farm,
            output=output,
            descriptor_dir=descriptors,
            recorder=_Recorder(farm, bad_arch=True),
            now=NOW,
        )
    assert not output.exists()
    assert not descriptors.exists()


def test_capture_refuses_duplicate_physical_hosts(tmp_path: Path) -> None:
    _farm_path, farm = _farm(tmp_path)
    with pytest.raises(authority.AuthorityCaptureError, match="one physical host"):
        authority.capture_authority(
            farm,
            output=tmp_path / "farm.local.json",
            descriptor_dir=tmp_path / "descriptors",
            recorder=_Recorder(farm, duplicate_host=True),
            now=NOW,
        )


@pytest.mark.parametrize("existing", ("output", "descriptors"))
def test_capture_is_non_overwriting(existing: str, tmp_path: Path) -> None:
    _farm_path, farm = _farm(tmp_path)
    output = tmp_path / "farm.local.json"
    descriptors = tmp_path / "descriptors"
    if existing == "output":
        output.write_text("keep", encoding="utf-8")
    else:
        descriptors.mkdir()
    with pytest.raises(authority.AuthorityCaptureError, match="already exists"):
        authority.capture_authority(
            farm,
            output=output,
            descriptor_dir=descriptors,
            recorder=_Recorder(farm),
            now=NOW,
        )


def test_capture_refuses_symlinked_output_ancestor(tmp_path: Path) -> None:
    _farm_path, farm = _farm(tmp_path)
    real = tmp_path / "real"
    real.mkdir()
    alias = tmp_path / "alias"
    alias.symlink_to(real, target_is_directory=True)
    with pytest.raises(authority.AuthorityCaptureError, match="symlink ancestor"):
        authority.capture_authority(
            farm,
            output=alias / "farm.local.json",
            descriptor_dir=tmp_path / "descriptors",
            recorder=_Recorder(farm),
            now=NOW,
        )


def test_loaded_capture_rejects_tampered_physical_digest(tmp_path: Path) -> None:
    _farm_path, farm = _farm(tmp_path)
    output = tmp_path / "farm.local.json"
    authority.capture_authority(
        farm,
        output=output,
        descriptor_dir=tmp_path / "descriptors",
        recorder=_Recorder(farm),
        now=NOW,
    )
    document = json.loads(output.read_text(encoding="utf-8"))
    first = sorted(document["authority_capture"]["hosts"])[0]
    document["authority_capture"]["hosts"][first]["physical_host_sha256"] = "f" * 64
    tampered = tmp_path / "tampered.json"
    tampered.write_bytes(canonical_bytes(document))
    with pytest.raises(FarmSpecError, match="does not bind machine and NIC identity"):
        load_farm_spec(tampered)


def test_s8_authority_cannot_be_used_as_newgen_source(tmp_path: Path) -> None:
    farm_path, _farm_spec = _farm(tmp_path)
    document = json.loads(farm_path.read_text(encoding="utf-8"))
    document["authority"] = {
        "schema": "icecream-s8-external-farm-authority-v1",
        "placements": {"relationship_hosts": {}},
    }
    farm_path.write_bytes(canonical_bytes(document))
    assert farmtest.main(
        [
            "authority",
            "capture",
            "--farm",
            str(farm_path),
            "--output",
            str(tmp_path / "farm.local.json"),
            "--descriptor-dir",
            str(tmp_path / "descriptors"),
            "--dry-run",
        ]
    ) == 3
