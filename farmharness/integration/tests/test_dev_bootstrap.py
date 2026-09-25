from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

import pytest

from dev import bootstrap
from farmharness.integration.tests import test_distribution_manifest


def _spec(tmp_path: Path, **fields: object) -> Path:
    path = tmp_path / "farm.json"
    path.write_text(json.dumps({"version": 1, **fields}), encoding="utf-8")
    return path


def _recipe(spec: dict) -> str:
    return hashlib.sha256(
        b"".join((bootstrap.ROOT / name).read_bytes() for name in (
            "dev/Dockerfile", "dev/run-qa.sh", "pyproject.toml", "uv.lock",
            ".python-version",
        ))
        + spec["base_image"].encode()
        + spec["profile"].encode()
    ).hexdigest()


class FakeRun:
    def __init__(self, root: Path, images: list[dict | None] | None = None) -> None:
        self.root = root
        self.commands: list[tuple[str, list[str]]] = []
        self.images = list(images or [])

    def command(self, name: str, argv: list[str], **_kwargs: object) -> str:
        self.commands.append((name, argv))
        return ""

    def inspect(self, _reference: str) -> dict | None:
        return self.images.pop(0) if self.images else None


class GateCommandRun(FakeRun):
    def __init__(self, root: Path, gate_error: Exception | None = None) -> None:
        super().__init__(root)
        self.gate_error = gate_error
        self.command_timeouts: list[tuple[str, int | None]] = []
        self.gate_id: str | None = None

    def command(self, name: str, argv: list[str], **kwargs: object) -> str:
        super().command(name, argv, **kwargs)
        self.command_timeouts.append((name, kwargs.get("timeout")))
        if name.startswith("gate-") and name != "gate-network-create":
            self.gate_id = next(
                value.split("=", 1)[1] for value in argv
                if value.startswith("icecream.dev.gate.id=")
            )
        if name.startswith("gate-") and name != "gate-network-create" and self.gate_error:
            raise self.gate_error
        return ""


def _observed(spec: dict) -> dict:
    return {
        "Id": "sha256:" + "a" * 64,
        "Config": {
            "Labels": {
                "org.icecream.dev.recipe": _recipe(spec),
                "org.icecream.dev.profile": spec["profile"],
            }
        },
    }


@pytest.mark.parametrize("version", [True, "1", 2])
def test_spec_version_requires_integer_one(tmp_path: Path, version: object) -> None:
    path = tmp_path / "farm.json"
    path.write_text(json.dumps({"version": version}), encoding="utf-8")
    with pytest.raises(bootstrap.BootstrapError, match="version must be 1"):
        bootstrap.load_spec(path)


@pytest.mark.parametrize("profile", [[], {}, None, "fedora"])
def test_spec_profile_values_are_rejected_as_bootstrap_errors(
    tmp_path: Path, profile: object
) -> None:
    path = _spec(tmp_path, profile=profile)
    with pytest.raises(bootstrap.BootstrapError, match="unsupported prepared profile"):
        bootstrap.load_spec(path)


def test_cli_repository_override_wins_and_never_accepts_a_tag(
    tmp_path: Path,
) -> None:
    path = _spec(tmp_path, image_repository="not a valid repo")
    loaded = bootstrap.load_spec(path, "registry.example/dev/sdk")
    assert loaded["image_repository"] == "registry.example/dev/sdk"

    with pytest.raises(bootstrap.BootstrapError, match="without a tag"):
        bootstrap.load_spec(_spec(tmp_path, image_repository="registry.example/dev/sdk:v1"))


@pytest.mark.parametrize("bundle", [17, {}, True])
def test_invalid_image_bundle_type_is_a_spec_error(
    tmp_path: Path, bundle: object
) -> None:
    path = _spec(tmp_path, image_bundle=bundle)
    with pytest.raises(bootstrap.BootstrapError, match="image_bundle"):
        bootstrap.load_spec(path)


def test_repository_sdk_is_pulled_and_missing_offline_sdk_has_no_fallback(
    tmp_path: Path,
) -> None:
    online = bootstrap.load_spec(_spec(tmp_path, image_repository="registry.example/dev/sdk"))
    run = FakeRun(tmp_path, [_observed(online)])
    image = bootstrap.sdk_image(run, online)
    assert image == "sha256:" + "a" * 64
    assert [name for name, _ in run.commands] == ["pull-sdk"]
    assert run.commands[0][1] == [
        "docker", "image", "pull", "registry.example/dev/sdk:sdk-ubuntu24.04"
    ]

    offline = {**online, "offline": True}
    missing = FakeRun(tmp_path, [None])
    with pytest.raises(bootstrap.BootstrapError, match="no fallback was attempted"):
        bootstrap.sdk_image(missing, offline)
    assert missing.commands == []


def test_missing_online_repository_sdk_does_not_fall_back_to_local_build(
    tmp_path: Path,
) -> None:
    spec = bootstrap.load_spec(_spec(tmp_path, image_repository="registry.example/dev/sdk"))
    run = FakeRun(tmp_path, [None])
    with pytest.raises(bootstrap.BootstrapError, match="no fallback was attempted"):
        bootstrap.sdk_image(run, spec)
    assert [name for name, _ in run.commands] == ["pull-sdk"]


def test_repository_sdk_recipe_mismatch_is_rejected(tmp_path: Path) -> None:
    spec = bootstrap.load_spec(_spec(tmp_path, image_repository="registry.example/dev/sdk"))
    observed = _observed(spec)
    observed["Config"]["Labels"]["org.icecream.dev.recipe"] = "wrong"
    with pytest.raises(bootstrap.BootstrapError, match="recipe mismatch"):
        bootstrap.sdk_image(FakeRun(tmp_path, [observed]), spec)


def test_repository_sdk_profile_mismatch_is_rejected(tmp_path: Path) -> None:
    spec = bootstrap.load_spec(_spec(tmp_path, image_repository="registry.example/dev/sdk"))
    observed = _observed(spec)
    observed["Config"]["Labels"]["org.icecream.dev.profile"] = "ubuntu22.04"
    with pytest.raises(bootstrap.BootstrapError, match="profile mismatch"):
        bootstrap.sdk_image(FakeRun(tmp_path, [observed]), spec)


def test_local_sdk_build_uses_profile_base_recipe_args_and_repo_context(
    tmp_path: Path,
) -> None:
    spec = bootstrap.load_spec(_spec(tmp_path, base_image="mirror.example/ubuntu:24.04"))
    run = FakeRun(tmp_path, [None, _observed(spec)])
    bootstrap.sdk_image(run, spec)
    assert [name for name, _ in run.commands] == ["build-sdk"]
    argv = run.commands[0][1]
    assert "BASE_IMAGE=mirror.example/ubuntu:24.04" in argv
    assert "DEV_PROFILE=ubuntu24.04" in argv
    assert f"RECIPE_REVISION={_recipe(spec)}" in argv
    context = Path(argv[-1])
    assert context.name == "sdk-context"
    assert argv[argv.index("--file") + 1] == str(context / "Dockerfile")
    assert {path.name for path in context.iterdir()} == {
        "Dockerfile", "run-qa.sh", "pyproject.toml", "uv.lock", ".python-version"
    }


def _git(source: Path, *args: str) -> None:
    subprocess.run(("git", "-C", str(source), *args), check=True, capture_output=True)


def test_snapshot_captures_edits_untracked_files_and_deletions_but_not_ignored(
    tmp_path: Path,
) -> None:
    source = tmp_path / "checkout"
    source.mkdir()
    _git(source, "init", "--quiet")
    _git(source, "config", "user.email", "qa@example.invalid")
    _git(source, "config", "user.name", "QA")
    (source / "edited.txt").write_text("original", encoding="utf-8")
    (source / "deleted.txt").write_text("remove me", encoding="utf-8")
    (source / ".gitignore").write_text("ignored.out\n", encoding="utf-8")
    _git(source, "add", ".")
    _git(source, "commit", "--quiet", "-m", "base")

    (source / "edited.txt").write_text("modified", encoding="utf-8")
    (source / "deleted.txt").unlink()
    (source / "new file.txt").write_text("untracked", encoding="utf-8")
    (source / "ignored.out").write_text("private", encoding="utf-8")
    destination = tmp_path / "snapshot"
    digest = bootstrap.snapshot(source, destination)

    assert len(digest) == 64
    assert (destination / "edited.txt").read_text() == "modified"
    assert (destination / "new file.txt").read_text() == "untracked"
    assert not (destination / "deleted.txt").exists()
    assert not (destination / "ignored.out").exists()
    assert (destination / ".gitignore").is_file()
    assert not (destination / ".git").exists()


def test_distribution_manifest_accepts_a_gitless_source_snapshot(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    manifest = bootstrap.ROOT / "farmharness/DISTFILES"
    snapshot = tmp_path / "source-snapshot"
    snapshot.mkdir()
    paths = manifest.read_text(encoding="utf-8").splitlines()
    for relative in [*paths, "farmharness/DISTFILES"]:
        source = bootstrap.ROOT / relative
        target = snapshot / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)

    assert not (snapshot / ".git").exists()
    monkeypatch.setattr(test_distribution_manifest, "ROOT", snapshot)
    assert test_distribution_manifest._source_files("farmharness") is None
    test_distribution_manifest.test_farmharness_distribution_manifest_is_complete_and_private_free()


def test_snapshot_rejects_symlink_escape(tmp_path: Path) -> None:
    source = tmp_path / "checkout"
    source.mkdir()
    outside = tmp_path / "outside"
    outside.write_text("outside", encoding="utf-8")
    link = source / "escape"
    link.symlink_to(outside)
    _git(source, "init", "--quiet")
    _git(source, "add", "escape")

    with pytest.raises(bootstrap.BootstrapError, match="points outside checkout"):
        bootstrap.snapshot(source, tmp_path / "snapshot")


def test_snapshot_does_not_leave_absolute_symlink_pointing_into_checkout(
    tmp_path: Path,
) -> None:
    source = tmp_path / "checkout"
    source.mkdir()
    target = source / "target.txt"
    target.write_text("in checkout", encoding="utf-8")
    (source / "absolute-link").symlink_to(target)
    _git(source, "init", "--quiet")
    _git(source, "add", "target.txt", "absolute-link")

    snapshot = tmp_path / "snapshot"
    try:
        bootstrap.snapshot(source, snapshot)
    except bootstrap.BootstrapError:
        return
    copied_link = snapshot / "absolute-link"
    assert copied_link.resolve().is_relative_to(snapshot.resolve())
    assert copied_link.read_text() == "in checkout"


def test_scratch_is_required_absolute_existing_and_writable(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    monkeypatch.delenv("ICEFARM_TMPDIR", raising=False)
    with pytest.raises(bootstrap.BootstrapError, match="ICEFARM_TMPDIR is required"):
        bootstrap.scratch_root()
    monkeypatch.setenv("ICEFARM_TMPDIR", str(tmp_path))
    assert bootstrap.scratch_root() == tmp_path.resolve()
    monkeypatch.setenv("ICEFARM_TMPDIR", "/")
    with pytest.raises(bootstrap.BootstrapError, match="non-root directory"):
        bootstrap.scratch_root()


@pytest.mark.parametrize("mode", ["qa", "bootstrap"])
def test_build_source_mounts_read_only_snapshot_and_cleans_its_container(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path, mode: str
) -> None:
    source = tmp_path / "snapshot"
    source.mkdir()
    run = FakeRun(tmp_path)
    cleanup: list[list[str]] = []
    monkeypatch.setattr(
        bootstrap.subprocess,
        "run",
        lambda argv, **_kwargs: cleanup.append(argv)
        or subprocess.CompletedProcess(argv, 0),
    )
    spec = {"jobs": 2, "memory_gb": 8}

    work = bootstrap.build_source(run, "sha256:sdk", source, spec, "current", mode)

    assert work == tmp_path / "current"
    assert len(run.commands) == 1
    argv = run.commands[0][1]
    assert "--network=none" in argv
    assert "--pull=never" in argv
    if mode == "qa":
        assert argv[argv.index("--cap-add") + 1] == "SYS_PTRACE"
    else:
        assert "--cap-add" not in argv
    assert f"type=bind,src={source},dst=/source,readonly" in argv
    assert f"type=bind,src={work},dst=/work" in argv
    temporary = work / "tmp"
    assert temporary.stat().st_mode & 0o7777 == 0o1777
    assert f"type=bind,src={temporary},dst=/tmp" in argv
    assert argv[-2:] == [mode, "2"]
    container = argv[argv.index("--name") + 1]
    assert cleanup == [["docker", "rm", "-f", container]]


def test_build_source_cleans_its_container_after_docker_run_failure(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    source = tmp_path / "snapshot"
    source.mkdir()
    run = FakeRun(tmp_path)
    cleanup: list[list[str]] = []
    monkeypatch.setattr(
        bootstrap.subprocess,
        "run",
        lambda argv, **_kwargs: cleanup.append(argv)
        or subprocess.CompletedProcess(argv, 0),
    )

    def fail_command(*_args: object, **_kwargs: object) -> str:
        raise bootstrap.BootstrapError("mock docker run failed")

    run.command = fail_command  # type: ignore[method-assign]
    with pytest.raises(bootstrap.BootstrapError, match="mock docker run failed"):
        bootstrap.build_source(run, "sha256:sdk", source, {"jobs": 2, "memory_gb": 8}, "current", "qa")
    container = next(item for item in cleanup[0] if item.startswith("icecream-dev-"))
    assert cleanup == [["docker", "rm", "-f", container]]


@pytest.mark.parametrize("gate,target,timeout_s", [
    ("p51-arm-expiry", "p50daemonpositive-p51-arm-expiry-check", 240),
    ("p51-restart-w30", "p50daemonpositive-p51-restart-w30-check", 4200),
    ("p51-scheduler-restart-w30", "p51schedulerrestart-w30-check", 1800),
])
def test_opt_in_gate_names_are_fixed_and_bounded(
    gate: str, target: str, timeout_s: int,
) -> None:
    assert bootstrap.gate_spec(gate) == (target, timeout_s)


def test_opt_in_gate_rejects_arbitrary_make_targets() -> None:
    with pytest.raises(bootstrap.BootstrapError, match="unsupported opt-in gate"):
        bootstrap.gate_spec("check; touch /tmp/untrusted")


def test_gate_docker_command_uses_private_network_short_tmp_and_bounded_resources(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path,
) -> None:
    run = GateCommandRun(tmp_path)
    source = tmp_path / "snapshot"
    source.mkdir()
    work = tmp_path / "current"
    (work / "tmp").mkdir(parents=True)
    cleanup: list[list[str]] = []

    def fake_subprocess_run(argv: list[str], **_kwargs: object) -> subprocess.CompletedProcess:
        cleanup.append(argv)
        if argv[1] == "container" and argv[2] == "inspect":
            labels = {"icecream.dev.gate.id": run.gate_id}
            return subprocess.CompletedProcess(argv, 0, stdout=json.dumps(labels))
        if argv[1] == "network" and argv[2] == "inspect":
            labels = {"icecream.dev.gate.id": run.gate_id}
            return subprocess.CompletedProcess(argv, 0, stdout=json.dumps(labels))
        return subprocess.CompletedProcess(argv, 0)

    monkeypatch.setattr(bootstrap.subprocess, "run", fake_subprocess_run)
    result = bootstrap.run_gate(
        run, "sdk:test", source, work, {"jobs": 2, "memory_gb": 8},
        "p51-arm-expiry",
    )
    network_name, network_argv = run.commands[0]
    gate_name, gate_argv = run.commands[1]
    assert network_name == "gate-network-create"
    assert network_argv[1:5] == ["network", "create", "--driver", "bridge"]
    assert "--internal" in network_argv
    assert gate_name == "gate-p51-arm-expiry"
    assert gate_argv[gate_argv.index("--network") + 1] == result["network"]
    assert gate_argv[gate_argv.index("--cpus") + 1] == "2"
    assert gate_argv[gate_argv.index("--memory") + 1] == "8g"
    assert gate_argv[gate_argv.index("--cap-add") + 1] == "NET_ADMIN"
    assert "SYS_PTRACE" not in gate_argv
    env_values = {
        gate_argv[index + 1]
        for index, value in enumerate(gate_argv[:-1]) if value == "--env"
    }
    assert set(bootstrap.GATE_OFFLINE_ENV) <= env_values
    assert f"type=bind,src={source},dst=/source,readonly" in gate_argv
    assert f"type=bind,src={work},dst=/work" in gate_argv
    assert f"type=bind,src={work / 'tmp'},dst=/tmp" in gate_argv
    assert gate_argv[-2:] == ["/source/dev/run-gate.sh", "p51-arm-expiry"]
    assert run.command_timeouts == [("gate-network-create", 30), ("gate-p51-arm-expiry", 300)]
    assert result["target"] == "p50daemonpositive-p51-arm-expiry-check"
    assert result["timeout_s"] == 240
    assert [argv[1:4] for argv in cleanup] == [
        ["container", "inspect", "--format"],
        ["container", "rm", "-f"],
        ["network", "inspect", "--format"],
        ["network", "rm", result["network"]],
    ]


def test_scheduler_gate_gets_locked_offline_python_environment(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path,
) -> None:
    run = GateCommandRun(tmp_path)
    source = tmp_path / "snapshot"
    source.mkdir()
    work = tmp_path / "current"
    (work / "tmp").mkdir(parents=True)

    def fake_subprocess_run(argv: list[str], **_kwargs: object) -> subprocess.CompletedProcess:
        labels = {"icecream.dev.gate.id": run.gate_id}
        return subprocess.CompletedProcess(argv, 0, stdout=json.dumps(labels))

    monkeypatch.setattr(bootstrap.subprocess, "run", fake_subprocess_run)
    bootstrap.run_gate(
        run, "sdk:test", source, work, {"jobs": 2, "memory_gb": 8},
        "p51-scheduler-restart-w30",
    )
    gate_argv = run.commands[-1][1]
    env_values = {
        gate_argv[index + 1]
        for index, value in enumerate(gate_argv[:-1]) if value == "--env"
    }
    assert set(bootstrap.GATE_OFFLINE_ENV) <= env_values
    assert gate_argv[-2:] == [
        "/source/dev/run-gate.sh", "p51-scheduler-restart-w30",
    ]


@pytest.mark.parametrize("cleanup_failure", ["inspect-timeout", "remove-timeout"])
def test_gate_skip_is_failure_and_cleanup_continues_after_docker_timeouts(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path, cleanup_failure: str,
) -> None:
    run = GateCommandRun(tmp_path, bootstrap.BootstrapError("gate runner exit 77 (skip)"))
    source = tmp_path / "snapshot"
    source.mkdir()
    work = tmp_path / "current"
    (work / "tmp").mkdir(parents=True)
    cleanup: list[list[str]] = []

    def fake_subprocess_run(argv: list[str], **_kwargs: object) -> subprocess.CompletedProcess:
        cleanup.append(argv)
        if argv[1] in {"container", "network"} and argv[2] == "inspect":
            kind = argv[1]
            if cleanup_failure == "inspect-timeout" and kind == "container":
                raise subprocess.TimeoutExpired(argv, 30)
            labels = {"icecream.dev.gate.id": run.gate_id}
            return subprocess.CompletedProcess(argv, 0, stdout=json.dumps(labels))
        if (cleanup_failure == "remove-timeout" and argv[1:4] ==
                ["container", "rm", "-f"]):
            raise subprocess.TimeoutExpired(argv, 30)
        return subprocess.CompletedProcess(argv, 0)

    monkeypatch.setattr(bootstrap.subprocess, "run", fake_subprocess_run)
    with pytest.raises(bootstrap.BootstrapError, match="exit 77"):
        bootstrap.run_gate(
            run, "sdk:test", source, work, {"jobs": 2, "memory_gb": 8},
            "p51-arm-expiry",
        )
    assert any(argv[1:4] == ["network", "rm", run.commands[0][1][-1]]
               for argv in cleanup)


@pytest.mark.parametrize(("status", "log", "marker", "expected", "accepted"), [
    (0, "P51_REAL_SCHEDULER_RESTART_W30_PASS profile=1\n", "P51_REAL_SCHEDULER_RESTART_W30_PASS profile=", 1, True),
    (77, "SKIP: missing prerequisites\n", "P51_ARM_EXPIRY_WIRE_PASS=", 3, False),
    (0, "SKIP: selected test unavailable\n", "PASS", 1, False),
    (0, "P51_ARM_EXPIRY_WIRE_PASS=1\n", "P51_ARM_EXPIRY_WIRE_PASS=", 3, False),
    (1, "P51_ARM_EXPIRY_WIRE_PASS=1\n", "P51_ARM_EXPIRY_WIRE_PASS=", 1, False),
])
def test_gate_result_policy_rejects_skip_and_incomplete_logs(
    tmp_path: Path, status: int, log: str, marker: str, expected: int,
    accepted: bool,
) -> None:
    log_path = tmp_path / "gate.log"
    log_path.write_text(log, encoding="utf-8")
    result = subprocess.run(
        ["python3", str(bootstrap.ROOT / "dev/gate-result.py"), str(status),
         str(log_path), marker, str(expected)],
        capture_output=True, text=True, check=False,
    )
    assert (result.returncode == 0) is accepted


def test_product_image_build_uses_installed_tree_and_sdk_without_network(
    tmp_path: Path,
) -> None:
    run = FakeRun(tmp_path)
    work = tmp_path / "work"
    install = work / "install"
    install.mkdir(parents=True)
    result = bootstrap.product_image(run, "sha256:sdk", work, "current", "b" * 64)

    assert result == "icecream-dev:current-" + "b" * 16
    assert [name for name, _ in run.commands] == ["tag-sdk-current", "image-current"]
    tag_argv = run.commands[0][1]
    assert tag_argv == [
        "docker", "image", "tag", "sha256:sdk",
        "icecream-dev:sdk-runtime-sdk",
    ]
    name, argv = run.commands[1]
    assert name == "image-current"
    assert "--pull=false" in argv
    assert "--network=none" in argv
    assert "SDK_IMAGE=icecream-dev:sdk-runtime-sdk" in argv
    assert argv[-1] == str(install)
    assert argv[argv.index("-f") + 1] == str(bootstrap.ROOT / "dev/Dockerfile.runtime")


def _make_dev_bootstrap(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    farm: str | None,
    image_repo: str,
) -> tuple[list[str], str]:
    capture = tmp_path / "capture"
    fake_python = tmp_path / "fake-python"
    fake_python.write_text(
        "#!/bin/sh\n"
        'printf "%s\\n" "$@" > "$FAKE_ARGS"\n'
        'printf "%s" "${IMAGE_REPO-<unset>}" > "$FAKE_IMAGE_REPO"\n',
        encoding="utf-8",
    )
    fake_python.chmod(0o755)
    scratch = tmp_path / "scratch"
    scratch.mkdir(exist_ok=True)
    monkeypatch.delenv("FARM", raising=False)
    monkeypatch.delenv("IMAGE_REPO", raising=False)
    env = {
        **os.environ,
        "FAKE_ARGS": str(capture.with_suffix(".args")),
        "FAKE_IMAGE_REPO": str(capture.with_suffix(".repo")),
    }
    command = [
        "make", "--no-print-directory", "dev-bootstrap",
        f"ICEFARM_TMPDIR={scratch}", f"ICEFARM_PYTHON={fake_python}",
    ]
    if farm is not None:
        command.append(f"FARM={farm}")
    if image_repo:
        command.append(f"IMAGE_REPO={image_repo}")
    result = subprocess.run(
        command,
        cwd=bootstrap.ROOT,
        env=env,
        capture_output=True,
        text=True,
        check=False,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    args = capture.with_suffix(".args").read_text().splitlines()
    repo = capture.with_suffix(".repo").read_text()
    return args, repo


def test_make_dev_bootstrap_uses_root_farm_by_default_and_propagates_repo_override(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    args, image_repo = _make_dev_bootstrap(
        tmp_path, monkeypatch, None, "registry.example/team/sdk"
    )
    assert args[args.index("--farm") + 1] == str(bootstrap.ROOT / "farm.json")
    assert image_repo == "registry.example/team/sdk"


def test_make_dev_bootstrap_uses_custom_farm_argument(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    custom_farm = str(tmp_path / "custom farm.json")
    args, _image_repo = _make_dev_bootstrap(tmp_path, monkeypatch, custom_farm, "")
    assert args[args.index("--farm") + 1] == custom_farm
