from __future__ import annotations

import copy
import hashlib
import json
import os
import shutil
from pathlib import Path

import pytest

from farmharness.integration import farmtest
from farmharness.integration.farm_spec import FarmSpec, load_farm_spec
from farmharness.integration.remote import CommandResult
from farmharness.integration.system_source_authority import (
    SystemSourceAuthorityError,
    capture_system_source_snapshot,
    capture_system_source_from_runtime,
    runtime_identity_command,
    validate_runtime_identity_result,
)
import farmharness.integration.system_source_authority as authority_module
from farmharness.integration.schema_validation import canonical_bytes


INTEGRATION = Path(__file__).resolve().parents[1]


def _farm():
    return load_farm_spec(INTEGRATION / "farm.example.json")


def _source(tmp_path: Path, *, identity: dict | None = None) -> Path:
    root = tmp_path / "runtime"
    for relative, value in {
        "usr/include/a.h": b"alpha",
        "usr/lib/gcc/b.h": b"beta",
        "usr/local/include/c.h": b"gamma",
    }.items():
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(value)
    runtime = _farm().data["runtime_image"]
    (root / ".icefarm-runtime-identity.json").write_text(
        json.dumps(identity or {
            "schema": "icefarm-runtime-identity-v1",
            "reference": runtime["reference"],
            "id": runtime["id"],
            "closure_sha256": runtime["closure_sha256"],
        }),
        encoding="utf-8",
    )
    return root


@pytest.mark.skipif(shutil.which("zstd") is None, reason="zstd is required")
def test_capture_is_pinned_deterministic_and_streamed(tmp_path: Path, monkeypatch) -> None:
    monkeypatch.setenv("ICEFARM_TMPDIR", str(tmp_path / "tmp"))
    source = _source(tmp_path)
    output_root = tmp_path / "tmp"
    output_root.mkdir()
    first = capture_system_source_snapshot(
        _farm(), source_root=source, output=output_root / "one.json", archive_dir=output_root / "archives1"
    )
    second = capture_system_source_snapshot(
        _farm(), source_root=source, output=output_root / "two.json", archive_dir=output_root / "archives2"
    )
    assert first["source_runtime_id"] == _farm().data["runtime_image"]["id"]
    assert first["file_count"] == 3
    assert first["manifest_sha256"] == second["manifest_sha256"]
    assert first["archive"]["sha256"] == second["archive"]["sha256"]
    assert first["archive"]["compression"] == {
        "checksum": True, "codec": "zstd", "level": 19, "long": 31, "threads": 8
    }
    assert not list((tmp_path / "tmp").glob(".system-source-stage-*"))


@pytest.mark.skipif(shutil.which("zstd") is None, reason="zstd is required")
@pytest.mark.parametrize("member", ("directory", "special", "dangling"))
def test_capture_rejects_unsafe_source_members(tmp_path: Path, member: str) -> None:
    source = _source(tmp_path)
    if member == "directory":
        (source / "usr/include/bad-dir").symlink_to(source / "usr/include")
    elif member == "special":
        os.mkfifo(source / "usr/include/fifo")
    else:
        (source / "usr/include/bad-link").symlink_to(source / "missing")
    with pytest.raises(SystemSourceAuthorityError):
        capture_system_source_snapshot(_farm(), source_root=source, output=tmp_path / "out.json")
    assert not (tmp_path / "out.json").exists()


def test_capture_rejects_runtime_identity_tamper(tmp_path: Path) -> None:
    runtime = _farm().data["runtime_image"]
    identity = {
        "schema": "icefarm-runtime-identity-v1",
        "reference": runtime["reference"],
        "id": runtime["id"],
        "closure_sha256": "0" * 64,
    }
    with pytest.raises(SystemSourceAuthorityError, match="differs"):
        capture_system_source_snapshot(
            _farm(), source_root=_source(tmp_path, identity=identity), output=tmp_path / "out.json"
        )


def test_runtime_identity_probe_is_exact_and_fail_closed() -> None:
    farm = _farm()
    command = runtime_identity_command(farm)
    assert command.argv == ("docker", "image", "inspect", "--format", "{{json .}}", farm.data["runtime_image"]["reference"])
    inspected = {
        "Id": farm.data["runtime_image"]["id"],
        "Architecture": "amd64",
        "Os": "linux",
        "Created": "2026-01-01T00:00:00Z",
        "Config": {"Env": ["PATH=/usr/bin"]},
        "RootFS": {"Layers": ["sha256:layer"]},
    }
    farm_data = copy.deepcopy(farm.data)
    closure = hashlib.sha256(canonical_bytes({key: inspected[key] for key in ("Architecture", "Os", "Created", "Config", "RootFS")})).hexdigest()
    farm_data["runtime_image"]["closure_sha256"] = closure
    farm = FarmSpec(path=farm.path, data=farm_data)
    validate_runtime_identity_result(
        farm,
        CommandResult(0, json.dumps(inspected), ""),
    )
    with pytest.raises(SystemSourceAuthorityError):
        validate_runtime_identity_result(farm, CommandResult(1, "", "failed"))
    with pytest.raises(SystemSourceAuthorityError):
        validate_runtime_identity_result(farm, CommandResult(0, json.dumps({**inspected, "Id": "sha256:" + "0" * 64}), ""))


def test_cli_capture_system_source_uses_identity_probe_and_does_not_mutate_farm(monkeypatch, tmp_path: Path) -> None:
    farm = _farm()
    calls = {"probe": 0, "capture": 0}

    def capture(*args, **kwargs):
        calls["capture"] += 1
        return {"schema": "icefarm-system-source-snapshot-v1"}

    monkeypatch.setattr(farmtest, "load_farm_spec", lambda _path: farm)
    monkeypatch.setattr(farmtest, "capture_system_source_from_runtime", capture)
    assert farmtest.main(["authority", "capture-system-source", "--farm", "farm.json", "--output", str(tmp_path / "receipt.json")]) == 0
    assert calls == {"probe": 0, "capture": 1}


def _inspected_farm() -> tuple[FarmSpec, dict]:
    farm = _farm()
    inspected = {
        "Id": farm.data["runtime_image"]["id"],
        "Architecture": "amd64",
        "Os": "linux",
        "Created": "2026-01-01T00:00:00Z",
        "Config": {"Env": ["PATH=/usr/bin"]},
        "RootFS": {"Layers": ["sha256:layer"]},
    }
    data = copy.deepcopy(farm.data)
    data["runtime_image"]["closure_sha256"] = hashlib.sha256(
        canonical_bytes({key: inspected[key] for key in ("Architecture", "Os", "Created", "Config", "RootFS")})
    ).hexdigest()
    return FarmSpec(path=farm.path, data=data), inspected


@pytest.mark.skipif(shutil.which("zstd") is None, reason="zstd is required")
def test_runtime_capture_uses_verified_native_id_and_cleans_staging(tmp_path: Path, monkeypatch) -> None:
    farm, inspected = _inspected_farm()
    monkeypatch.setenv("ICEFARM_TMPDIR", str(tmp_path / "tmp"))
    calls = []

    class Recorder:
        def invoke(self, command):
            calls.append(command)
            if command.phase == "authority.system-source-extract":
                stage = Path(next(item.split("src=", 1)[1].split(",", 1)[0] for item in command.argv if item.startswith("type=bind,src=")))
                source = _source(tmp_path / "source")
                destination = stage / "root"
                for root_name in ("usr/include", "usr/lib/gcc", "usr/local/include"):
                    target = destination / root_name
                    target.mkdir(parents=True, exist_ok=True)
                    for item in (source / root_name).iterdir():
                        shutil.copy2(item, target / item.name)
            return CommandResult(0, json.dumps(inspected) if command.phase == "authority.system-source-runtime-inspect" else "", "")

    output = tmp_path / "tmp" / "receipt.json"
    receipt = capture_system_source_from_runtime(
        farm, output=output, recorder=Recorder(), timeout_s=30
    )
    assert receipt["source_runtime_id"] == farm.data["runtime_image"]["id"]
    extract = next(command for command in calls if command.phase == "authority.system-source-extract")
    assert farm.data["runtime_image"]["id"] in extract.argv
    assert "--pull=never" in extract.argv and "--network" in extract.argv
    assert extract.argv[extract.argv.index("--user") + 1] == f"{os.getuid()}:{os.getgid()}"
    assert not list((tmp_path / "tmp").glob(".system-source-capture-*"))


def test_runtime_capture_failure_publishes_nothing(tmp_path: Path, monkeypatch) -> None:
    farm, inspected = _inspected_farm()
    monkeypatch.setenv("ICEFARM_TMPDIR", str(tmp_path / "tmp"))

    class Recorder:
        def invoke(self, command):
            if command.phase == "authority.system-source-runtime-inspect":
                return CommandResult(0, json.dumps(inspected), "")
            return CommandResult(9, "", "extract failed")

    output = tmp_path / "tmp" / "receipt.json"
    with pytest.raises(SystemSourceAuthorityError, match="extraction failed"):
        capture_system_source_from_runtime(farm, output=output, recorder=Recorder())
    assert not output.exists()


def test_runtime_capture_timeout_still_attempts_exact_cleanup(tmp_path: Path, monkeypatch) -> None:
    farm, inspected = _inspected_farm()
    monkeypatch.setenv("ICEFARM_TMPDIR", str(tmp_path / "tmp"))
    phases = []

    class Recorder:
        def invoke(self, command):
            phases.append(command.phase)
            if command.phase == "authority.system-source-runtime-inspect":
                return CommandResult(0, json.dumps(inspected), "")
            if command.phase == "authority.system-source-extract":
                raise TimeoutError("bounded timeout")
            return CommandResult(0, "", "")

    with pytest.raises(TimeoutError, match="bounded timeout"):
        capture_system_source_from_runtime(farm, output=tmp_path / "tmp" / "receipt.json", recorder=Recorder())
    assert phases[-1] == "authority.system-source-cleanup-list"


def test_runtime_capture_surfaces_cleanup_failure_without_masking_primary(tmp_path: Path, monkeypatch) -> None:
    farm, inspected = _inspected_farm()
    monkeypatch.setenv("ICEFARM_TMPDIR", str(tmp_path / "tmp"))

    class Recorder:
        def invoke(self, command):
            if command.phase == "authority.system-source-runtime-inspect":
                return CommandResult(0, json.dumps(inspected), "")
            if command.phase == "authority.system-source-extract":
                return CommandResult(9, "", "extract root cause")
            return CommandResult(8, "", "cleanup root cause")

    with pytest.raises(SystemSourceAuthorityError, match="extraction failed") as error:
        capture_system_source_from_runtime(farm, output=tmp_path / "tmp" / "receipt.json", recorder=Recorder())
    assert "cleanup failed" in " ".join(error.value.__notes__ or [])


@pytest.mark.skipif(shutil.which("zstd") is None, reason="zstd is required")
def test_archive_publication_race_removes_new_archive(tmp_path: Path, monkeypatch) -> None:
    monkeypatch.setenv("ICEFARM_TMPDIR", str(tmp_path))
    source = _source(tmp_path)
    output = tmp_path / "receipt.json"
    original = authority_module._publish_bytes

    def race(path, payload, subject):
        if path == output:
            raise SystemSourceAuthorityError("simulated receipt publication race")
        return original(path, payload, subject)

    monkeypatch.setattr(authority_module, "_publish_bytes", race)
    with pytest.raises(SystemSourceAuthorityError, match="publication race"):
        capture_system_source_snapshot(_farm(), source_root=source, output=output)
    assert not output.exists()
    assert not list(tmp_path.glob("system-source-*.tar.zst"))


@pytest.mark.skipif(shutil.which("zstd") is None, reason="zstd is required")
def test_capture_accepts_trusted_symlink_temp_boundary(tmp_path: Path, monkeypatch) -> None:
    scratch = tmp_path / "scratch"
    scratch.mkdir()
    alias = tmp_path / "i"
    alias.symlink_to(scratch, target_is_directory=True)
    monkeypatch.setenv("ICEFARM_TMPDIR", str(alias))
    source = _source(tmp_path)

    receipt = capture_system_source_snapshot(
        _farm(),
        source_root=source,
        output=alias / "receipt.json",
        archive_dir=alias / "archives",
    )

    assert (scratch / "receipt.json").is_file()
    assert Path(receipt["archive"]["path"]).is_relative_to(scratch)
