from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[3]
WRAPPER = ROOT / "dev" / "python.sh"


def _fake_uv(bin_dir: Path) -> Path:
    uv = bin_dir / "uv"
    uv.write_text(
        "#!/bin/sh\n"
        "set -eu\n"
        "printf '%s\\n' \"$*\" > \"$UV_LOG\"\n"
        "printf 'offline=%s\\n' \"${UV_OFFLINE:-}\" >> \"$UV_LOG\"\n"
        "printf 'project=%s\\n' \"$UV_PROJECT_ENVIRONMENT\" >> \"$UV_LOG\"\n"
        "if [ \"${UV_FAIL:-0}\" = 1 ]; then exit 17; fi\n"
        "case \"$UV_PROJECT_ENVIRONMENT\" in \"$FAKE_ROOT\"/*) ;; *) exit 91 ;; esac\n"
        "mkdir -p \"$UV_PROJECT_ENVIRONMENT/bin\"\n"
        "if [ \"${FAKE_NO_CHILD:-0}\" = 1 ]; then exit 0; fi\n"
        "cat > \"$UV_PROJECT_ENVIRONMENT/bin/python.tmp.$$\" <<'PYTHON'\n"
        "#!/bin/sh\n"
        "printf '%s\\n' \"$*\" > \"$FAKE_CHILD_LOG\"\n"
        "printf '%s\\n' \"$VIRTUAL_ENV\" >> \"$FAKE_CHILD_LOG\"\n"
        "printf '%s\\n' \"$PATH\" >> \"$FAKE_CHILD_LOG\"\n"
        "exit \"${FAKE_CHILD_EXIT:-0}\"\n"
        "PYTHON\n"
        "chmod +x \"$UV_PROJECT_ENVIRONMENT/bin/python.tmp.$$\"\n"
        "mv -f \"$UV_PROJECT_ENVIRONMENT/bin/python.tmp.$$\" \"$UV_PROJECT_ENVIRONMENT/bin/python\"\n",
        encoding="utf-8",
    )
    uv.chmod(0o755)
    return uv


def _env(tmp_path: Path, *, fake_uv: bool = True) -> tuple[dict[str, str], Path]:
    scratch = tmp_path / "scratch root"
    scratch.mkdir()
    bin_dir = tmp_path / "bin"
    bin_dir.mkdir()
    if fake_uv:
        _fake_uv(bin_dir)
    child_log = tmp_path / "child.log"
    uv_log = tmp_path / "uv.log"
    env = {
        **os.environ,
        "ICEFARM_TMPDIR": str(scratch),
        "PATH": f"{bin_dir}:{os.environ['PATH']}",
        "FAKE_CHILD_LOG": str(child_log),
        "UV_LOG": str(uv_log),
        "FAKE_ROOT": str(tmp_path),
    }
    # Every fake-UV invocation is isolated from the outer test runner's live
    # environment, so it can never overwrite a real managed interpreter.
    env["UV_CACHE_DIR"] = str(tmp_path / "uv-cache")
    env["UV_PYTHON_INSTALL_DIR"] = str(tmp_path / "uv-python")
    env["UV_PROJECT_ENVIRONMENT"] = str(tmp_path / "managed env")
    env.pop("VIRTUAL_ENV", None)
    return env, scratch


def _run(tmp_path: Path, *args: str, env: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
    if env is None:
        env, _ = _env(tmp_path)
    return subprocess.run(
        ["sh", str(WRAPPER), *args], env=env, capture_output=True, text=True, check=False
    )


def test_missing_scratch_fails_before_uv(tmp_path: Path) -> None:
    env, scratch = _env(tmp_path)
    scratch.rmdir()
    result = _run(tmp_path, "--sync", env=env)
    assert result.returncode != 0
    assert "ICEFARM_TMPDIR" in result.stderr
    assert not (tmp_path / "uv.log").exists()


def test_missing_uv_reports_clear_requirement(tmp_path: Path) -> None:
    env, _ = _env(tmp_path, fake_uv=False)
    env["PATH"] = "/usr/bin:/bin"
    result = _run(tmp_path, "--sync", env=env)
    assert result.returncode == 2
    assert "uv 0.9.21 is required" in result.stderr


def test_sync_is_locked_managed_and_uses_scratch_storage(tmp_path: Path) -> None:
    env, scratch = _env(tmp_path)
    result = _run(tmp_path, "--sync", env=env)
    assert result.returncode == 0, result.stderr
    command = (tmp_path / "uv.log").read_text(encoding="utf-8").strip()
    assert command.startswith("sync --locked --managed-python ")
    assert "--project " in command
    assert str(ROOT) in command
    assert " lock" not in f" {command}"
    assert env["ICEFARM_TMPDIR"] == str(scratch)


def test_default_uv_storage_is_under_scratch_not_checkout(tmp_path: Path) -> None:
    env, scratch = _env(tmp_path)
    env.pop("UV_PROJECT_ENVIRONMENT")
    env["FAKE_NO_CHILD"] = "1"
    result = _run(tmp_path, "--sync", env=env)
    assert result.returncode == 0, result.stderr
    log = (tmp_path / "uv.log").read_text(encoding="utf-8")
    assert f"project={scratch}/icecream-uv-" in log
    assert f"project={ROOT}" not in log


def test_child_uses_managed_python_and_wrapper_path(tmp_path: Path) -> None:
    env, _ = _env(tmp_path)
    result = _run(tmp_path, "-c", "print('ignored')", env=env)
    assert result.returncode == 0, result.stderr
    child = (tmp_path / "child.log").read_text(encoding="utf-8").splitlines()
    assert child[0] == "-c print('ignored')"
    assert child[1] == str(tmp_path / "managed env")
    assert child[1] != sys.prefix
    assert child[2].split(":", 1)[0] == child[1] + "/bin"


def test_exec_forwards_without_replacing_arguments(tmp_path: Path) -> None:
    env, _ = _env(tmp_path)
    result = _run(tmp_path, "--exec", "sh", "-c", "printf '%s' \"$1\"", "_", "forwarded", env=env)
    assert result.returncode == 0
    assert result.stdout == "forwarded"


def test_sync_failure_blocks_child(tmp_path: Path) -> None:
    env, _ = _env(tmp_path)
    env["UV_FAIL"] = "1"
    result = _run(tmp_path, "-c", "exit 0", env=env)
    assert result.returncode == 17
    assert not (tmp_path / "child.log").exists()


def test_offline_environment_reaches_uv_and_child(tmp_path: Path) -> None:
    env, _ = _env(tmp_path)
    env["UV_OFFLINE"] = "1"
    result = _run(tmp_path, "--sync", env=env)
    assert result.returncode == 0, result.stderr
    assert "offline=1" in (tmp_path / "uv.log").read_text(encoding="utf-8")


def test_real_uv_locked_sync_rejects_stale_isolated_copy(tmp_path: Path) -> None:
    uv = shutil.which("uv")
    if uv is None:
        pytest.skip("uv executable is unavailable")
    project = tmp_path / "stale-project"
    project.mkdir()
    pyproject = (ROOT / "pyproject.toml").read_text(encoding="utf-8")
    pyproject = pyproject.replace('name = "icecream-dev-tools"',
                                  'name = "icecream-dev-tools-stale"')
    (project / "pyproject.toml").write_text(pyproject, encoding="utf-8")
    (project / "uv.lock").write_bytes((ROOT / "uv.lock").read_bytes())
    env = {
        **os.environ,
        "UV_PROJECT_ENVIRONMENT": str(tmp_path / "isolated-env"),
        "UV_OFFLINE": "1",
        "UV_PYTHON_DOWNLOADS": "never",
    }
    env.pop("VIRTUAL_ENV", None)
    result = subprocess.run(
        [uv, "sync", "--locked", "--offline", "--python", sys.executable,
         "--project", str(project)],
        env=env, capture_output=True, text=True, check=False,
    )
    assert result.returncode != 0
    assert "lockfile" in result.stderr.lower(), result.stderr
    assert "needs to be updated" in result.stderr.lower(), result.stderr


def test_python_wrapper_and_shell_compiler_routes_are_distributed() -> None:
    top = (ROOT / "Makefile.am").read_text(encoding="utf-8")
    assert "pyproject.toml" in top and "uv.lock" in top
    assert ".python-version" in top and "dev/python.sh" in top
    unittests = (ROOT / "unittests/Makefile.am").read_text(encoding="utf-8")
    assert 'LOG_COMPILER = sh "$(abs_top_srcdir)/dev/python.sh" --exec' in unittests
    assert "SH_LOG_COMPILER = $(LOG_COMPILER) $(SHELL)" in unittests
    tests = (ROOT / "tests/Makefile.am").read_text(encoding="utf-8")
    assert 'LOG_COMPILER = sh "$(abs_top_srcdir)/dev/python.sh" --exec' in tests
    compose = (ROOT / "tests/compose/run.sh").read_text(encoding="utf-8")
    assert 'sh "$python_runner" --sync' in compose
    assert 'sh "$python_runner" "${script_dir}/verify.py"' in compose
