from __future__ import annotations

import json
import socket
from pathlib import Path

import pytest

import s8_protected_launcher as launcher


def _fixture(tmp_path: Path) -> tuple[dict[str, str], dict[str, Path], socket.socket]:
    paths = {}
    for name in ("workspace", "experiments", "temp"):
        path = tmp_path / name
        path.mkdir()
        paths[name] = path
    machine_id = tmp_path / "machine-id"
    cpuinfo = tmp_path / "cpuinfo"
    dmi = tmp_path / "product_uuid"
    machine_id.write_text("machine\n")
    cpuinfo.write_text("cpu\n")
    dmi.write_text("dmi\n")
    paths.update(machine_id=machine_id, cpuinfo=cpuinfo, dmi=dmi)
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.bind(str(tmp_path / "docker.sock"))
    paths["socket"] = tmp_path / "docker.sock"
    raw = {"reference": "supervisor:tag", "image_id": "sha256:" + "a" * 64,
           "architecture": "amd64", "os": "linux", "created": "now"}
    return raw, paths, sock


def test_build_command_is_host_visible_pinned_and_owner_bound(tmp_path: Path) -> None:
    identity, paths, sock = _fixture(tmp_path)
    try:
        command = launcher.build_command(
            ["python3", "farmharness/s8_campaign_driver.py", "--mode", "all"],
            workspace=paths["workspace"], experiment_root=paths["experiments"],
            container_temp_root=paths["temp"], image_identity=identity,
            docker_socket=paths["socket"], machine_id=paths["machine_id"],
            cpuinfo=paths["cpuinfo"], dmi_paths=(paths["dmi"],),
            owner_uid=123, owner_gid=456, docker_gid=789,
            docker_group_supported=True)
    finally:
        sock.close()
    assert "--pid=host" in command
    assert "--network=host" in command
    assert command[command.index("--entrypoint"):command.index("--entrypoint") + 2] == [
        "--entrypoint", "/bin/sh"]
    assert "--oom-score-adj=-1000" in command
    assert ["--user", "123:456"] == command[command.index("--user"):command.index("--user") + 2]
    assert ["--group-add", "789"] == command[command.index("--group-add"):command.index("--group-add") + 2]
    for path in (paths["workspace"], paths["experiments"], paths["temp"]):
        assert f"{path}:{path}:rw" in command
    assert f"{paths['socket']}:{paths['socket']}:rw" in command
    assert f"{paths['machine_id']}:{paths['machine_id']}:ro" in command
    assert f"{paths['cpuinfo']}:{paths['cpuinfo']}:ro" in command
    assert f"{paths['dmi']}:{paths['dmi']}:ro" in command
    assert identity["image_id"] in command
    assert "command -v \"$tool\"" in command[-1]
    assert all(tool in command[-1] for tool in ("python3", "docker", "git"))


def test_build_rejects_unauthenticated_group_or_missing_authority(tmp_path: Path) -> None:
    identity, paths, sock = _fixture(tmp_path)
    try:
        with pytest.raises(launcher.LauncherError, match="docker_group:support_not_authenticated"):
            launcher.build_command(
                ["python3", "--mode", "all"], workspace=paths["workspace"],
                experiment_root=paths["experiments"], container_temp_root=paths["temp"],
                image_identity=identity, docker_socket=paths["socket"],
                machine_id=paths["machine_id"], cpuinfo=paths["cpuinfo"],
                dmi_paths=(paths["dmi"],))
        paths["cpuinfo"].unlink()
        with pytest.raises(launcher.LauncherError, match="host_cpuinfo:unavailable"):
            launcher.build_command(
                ["python3", "--mode", "all"], workspace=paths["workspace"],
                experiment_root=paths["experiments"], container_temp_root=paths["temp"],
                image_identity=identity, docker_socket=paths["socket"],
                machine_id=paths["machine_id"], cpuinfo=paths["cpuinfo"],
                dmi_paths=(paths["dmi"],),
                docker_group_supported=True)
    finally:
        sock.close()


def test_resolve_requires_exact_content_id_without_launching(tmp_path: Path) -> None:
    expected = "sha256:" + "b" * 64
    calls: list[str] = []

    def inspect(reference: str) -> dict[str, object]:
        calls.append(reference)
        return {"Id": expected, "Architecture": "amd64", "Os": "linux", "Created": "now"}

    identity = launcher.resolve_supervisor_image("supervisor:tag", expected, inspect)
    assert identity["image_id"] == expected
    assert calls == ["supervisor:tag"]
    with pytest.raises(launcher.LauncherError, match="content_id_mismatch"):
        launcher.resolve_supervisor_image("supervisor:tag", "sha256:" + "c" * 64, inspect)


def test_relative_paths_and_invalid_image_are_rejected(tmp_path: Path) -> None:
    identity, paths, sock = _fixture(tmp_path)
    try:
        with pytest.raises(launcher.LauncherError, match="workspace:unavailable"):
            launcher.build_command(
                ["python3", "--mode", "all"], workspace=Path("relative"),
                experiment_root=paths["experiments"], container_temp_root=paths["temp"],
                image_identity=identity, docker_socket=paths["socket"],
                machine_id=paths["machine_id"], cpuinfo=paths["cpuinfo"],
                dmi_paths=(paths["dmi"],), docker_group_supported=True)
    finally:
        sock.close()


def test_main_is_dry_run_by_default_and_retains_argv(monkeypatch: pytest.MonkeyPatch,
                                                     tmp_path: Path,
                                                     capsys: pytest.CaptureFixture[str]) -> None:
    argv_file = tmp_path / "argv.json"
    campaign_argv = ["python3", "driver.py", "--mode", "all"]
    argv_file.write_text(json.dumps(campaign_argv))
    identity = {"reference": "supervisor:tag", "image_id": "sha256:" + "a" * 64,
                "architecture": "amd64", "os": "linux", "created": "now"}
    command = ["docker", "run", identity["image_id"]]
    monkeypatch.setattr(launcher, "resolve_supervisor_image", lambda *args: identity)
    monkeypatch.setattr(launcher, "build_command", lambda *args, **kwargs: command)
    monkeypatch.setattr(launcher.subprocess, "run",
                        lambda *args, **kwargs: (_ for _ in ()).throw(
                            AssertionError("dry run executed")))
    result = launcher.main([
        "--campaign-argv-json", str(argv_file), "--workspace", str(tmp_path),
        "--experiment-root", str(tmp_path), "--container-temp-root", str(tmp_path),
        "--supervisor-image", "supervisor:tag",
        "--supervisor-image-id", identity["image_id"], "--docker-group-supported"])
    assert result == 0
    envelope = json.loads(capsys.readouterr().out)
    assert envelope["status"] == "DRY_RUN"
    assert envelope["campaign_argv"] == campaign_argv
