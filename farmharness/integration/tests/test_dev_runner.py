from __future__ import annotations

import json
import os
import subprocess
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[3]
RUNNER = ROOT / "dev" / "run-qa.sh"


def _executable(path: Path, contents: str) -> None:
    path.write_text(contents, encoding="utf-8")
    path.chmod(0o755)


@pytest.mark.parametrize("native_status", [9, 0])
def test_dev_qa_separates_unprivileged_checks_and_root_only_gate(
    tmp_path: Path, native_status: int
) -> None:
    source_mount = tmp_path / "snapshot"
    work_root = tmp_path / "scratch"
    fake_bin = tmp_path / "fake-bin"
    source_mount.mkdir()
    work_root.mkdir()
    fake_bin.mkdir()
    sdk_metadata = tmp_path / "sdk-metadata"
    sdk_metadata.mkdir()
    for name in ("pyproject.toml", "uv.lock", ".python-version"):
        (source_mount / name).write_text("metadata\n", encoding="utf-8")
        (sdk_metadata / name).write_text("metadata\n", encoding="utf-8")

    runner_text = RUNNER.read_text(encoding="utf-8")
    runner_text = runner_text.replace("SOURCE_MOUNT=/source", f"SOURCE_MOUNT={source_mount}")
    runner_text = runner_text.replace("WORK_ROOT=/work", f"WORK_ROOT={work_root}")
    runner_text = runner_text.replace("SDK_METADATA_ROOT=/opt/icecream-python-src",
                                      f"SDK_METADATA_ROOT={sdk_metadata}")
    runner = tmp_path / "run-qa.sh"
    _executable(runner, runner_text)

    _executable(fake_bin / "findmnt", "#!/bin/sh\nprintf 'ro,relatime\\n'\n")
    _executable(
        fake_bin / "stat",
        "#!/bin/sh\n"
        'printf "%s\\n" "$*" >> "$STAT_LOG"\n'
        "echo 12:34\n",
    )
    _executable(
        fake_bin / "id",
        "#!/bin/sh\n"
        'case "$*" in\n'
        '  "-u") echo 0 ;;\n'
        '  "-u nobody"|"-g nobody") echo 65534 ;;\n'
        '  *) exit 2 ;;\n'
        "esac\n",
    )
    _executable(
        fake_bin / "runuser",
        "#!/bin/sh\n"
        'printf "%s\\n" "$*" >> "$RUNUSER_LOG"\n'
        '[ "$1" = "-u" ] && [ "$2" = "nobody" ] && [ "$3" = "--" ] || exit 2\n'
        "shift 3\n"
        'exec "$@"\n',
    )
    _executable(
        fake_bin / "chown",
        "#!/bin/sh\n"
        'printf "%s\\n" "$*" >> "$CHOWN_LOG"\n'
        "exit 0\n",
    )
    _executable(
        source_mount / "autogen.sh",
        "#!/bin/sh\nexit 0\n",
    )
    _executable(
        source_mount / "configure",
        "#!/bin/sh\n"
        'printf "%s\\n" "$PWD" > "$WORK_ROOT/configure.cwd"\n'
        'printf "%s\\n" "$*" > "$WORK_ROOT/configure.args"\n'
        "exit 0\n",
    )
    _executable(
        fake_bin / "make",
        "#!/bin/sh\n"
        'printf "%s\\n" "$*" >> "$MAKE_LOG"\n'
        'printf "%s|%s\\n" "$TMPDIR" "$ICEFARM_TMPDIR" >> "$TMP_LOG"\n'
        'case " $* " in *" check "*)\n'
        '  if [ "$MAKE_CHECK_STATUS" = 0 ]; then\n'
        '    mkdir -p "$WORK_ROOT/build/unittests"\n'
        '    : > "$WORK_ROOT/build/unittests/p50cacheservice"\n'
        '    chmod +x "$WORK_ROOT/build/unittests/p50cacheservice"\n'
        '  fi\n'
        '  exit "$MAKE_CHECK_STATUS" ;; esac\n'
        "exit 0\n",
    )
    _executable(
        fake_bin / "uv",
        "#!/bin/sh\n"
        'printf "%s|%s|%s|%s\\n" "$*" "$UV_OFFLINE" "$UV_CACHE_DIR" "$UV_PROJECT_ENVIRONMENT" >> "$UV_LOG"\n'
        "exit 0\n",
    )
    _executable(
        fake_bin / "python3",
        "#!/bin/sh\n"
        'printf "%s\\n" "$*" > "$PYTEST_LOG"\n'
        'printf "%s|%s|%s\\n" "$VIRTUAL_ENV" "$UV_OFFLINE" "$UV_PYTHON_DOWNLOADS" >> "$PYTEST_LOG"\n'
        'printf "%s\\n" "${ICECC_P50CACHESERVICE_BIN-unset}" >> "$PYTEST_LOG"\n'
        "exit 0\n",
    )

    env = {
        **os.environ,
        "PATH": f"{fake_bin}:{os.environ['PATH']}",
        "WORK_ROOT": str(work_root),
        "MAKE_LOG": str(tmp_path / "make.log"),
        "MAKE_CHECK_STATUS": "9",
        "ICECC_P50CACHESERVICE_BIN": "/stale/inherited/p50cacheservice",
        "RUNUSER_LOG": str(tmp_path / "runuser.log"),
        "STAT_LOG": str(tmp_path / "stat.log"),
        "PYTEST_LOG": str(tmp_path / "pytest.log"),
        "CHOWN_LOG": str(tmp_path / "chown.log"),
        "TMP_LOG": str(tmp_path / "tmp.log"),
        "UV_LOG": str(tmp_path / "uv.log"),
        "ICEFARM_TMPDIR": str(work_root / "tmp"),
        "ICEFARM_OUTPUT_UID": "1234",
        "ICEFARM_OUTPUT_GID": "5678",
    }
    env["MAKE_CHECK_STATUS"] = str(native_status)
    result = subprocess.run(
        ("bash", str(runner), "qa", "2"),
        env=env,
        capture_output=True,
        text=True,
        check=False,
    )

    assert result.returncode == (1 if native_status else 0), result.stdout + result.stderr
    assert (work_root / "configure.cwd").read_text().strip() == str(work_root / "build")
    configure_args = (work_root / "configure.args").read_text()
    assert f"--prefix={work_root}/install" in configure_args

    make_calls = (tmp_path / "make.log").read_text().splitlines()
    assert len(make_calls) == (4 if native_status == 0 else 3)
    assert make_calls[0].endswith(" all")
    assert make_calls[1].endswith(" install")
    assert make_calls[2].endswith(" check")
    if native_status == 0:
        assert make_calls[3].endswith(
            " check TESTS=p50cacheservice p50cacheservice-sanitize.sh"
        )
    user_stages = (tmp_path / "runuser.log").read_text().splitlines()
    assert len(user_stages) == 7
    assert all(line.startswith("-u nobody -- ") for line in user_stages)
    assert not any("native-root-check" in line for line in user_stages)
    assert (tmp_path / "tmp.log").read_text().splitlines() == [
        "/tmp|/tmp"
    ] * (4 if native_status == 0 else 3)
    stat_calls = (tmp_path / "stat.log").read_text().splitlines()
    assert any(line.endswith("/tmp") for line in stat_calls)
    assert any(line.endswith(f"{work_root}/tmp") for line in stat_calls)
    chown_calls = (tmp_path / "chown.log").read_text().splitlines()
    assert chown_calls == [
        f"-R --no-dereference 65534:65534 {work_root}/source {work_root}/build "
        f"{work_root}/install /tmp {work_root}/artifacts {work_root}/uv-cache "
        f"{work_root}/python-env",
        f"-R --no-dereference 1234:5678 {work_root}",
    ]

    pytest_args = (tmp_path / "pytest.log").read_text()
    assert "-m pytest" in pytest_args
    assert str(work_root / "artifacts" / "pytest.xml") in pytest_args
    assert f"{work_root}/python-env|1|never" in pytest_args
    expected_live_binary = (
        f"{work_root}/build/unittests/p50cacheservice"
        if native_status == 0
        else ""
    )
    assert pytest_args.splitlines()[-1] == expected_live_binary
    assert (tmp_path / "uv.log").read_text().startswith(
        f"sync --locked --offline --managed-python --python metadata|1|"
        f"{work_root}/uv-cache|{work_root}/python-env"
    )
    summary = json.loads((work_root / "artifacts" / "summary.json").read_text())
    assert summary == {
        "mode": "qa",
        "jobs": 2,
        "autogen_exit": 0,
        "configure_exit": 0,
        "build_exit": 0,
        "install_exit": 0,
        "python_sync_exit": 0,
        "native_check_exit": native_status,
        "native_root_check_exit": None if native_status else 0,
        "pytest_exit": 0,
        "overall_exit": 1 if native_status else 0,
    }
