from __future__ import annotations

import re
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]


def test_package_builder_aggregates_only_include_supported_rows() -> None:
    for target in ("docker_build", "docker_test"):
        result = subprocess.run(
            ("make", "--no-print-directory", "-n", target),
            cwd=ROOT,
            check=True,
            capture_output=True,
            text=True,
        )
        commands = result.stdout
        assert "package_builder/ubuntu22.04" in commands
        assert "package_builder/ubuntu24.04" in commands
        assert "package_builder/fedora-latest" in commands
        assert "package_builder/fedora28" not in commands


def test_local_harness_make_targets_select_fast_and_complete_sets() -> None:
    fast = subprocess.run(
        ("make", "--no-print-directory", "-n", "test-harness-fast", "ICEFARM_PYTHON=python3-custom", "ICEFARM_TMPDIR=/tmp/custom-scratch"),
        cwd=ROOT,
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    thorough = subprocess.run(
        ("make", "--no-print-directory", "-n", "test-harness-thorough"),
        cwd=ROOT,
        check=True,
        capture_output=True,
        text=True,
    ).stdout

    assert "-m 'not thorough'" in fast
    assert "farmharness/integration/tests" in fast
    assert "python3-custom -B -m pytest" in fast
    assert "check_scratch.sh" in fast
    assert 'TMPDIR="$ICEFARM_TMPDIR"' in fast
    assert "PYTHONDONTWRITEBYTECODE=1" in fast
    assert "-m 'not thorough'" not in thorough
    assert "farmharness/integration/tests" in thorough

    autotools = (ROOT / "Makefile.am").read_text(encoding="utf-8")
    assert "test-harness-fast:" in autotools
    assert "test-harness-thorough:" in autotools


def test_compose_image_installs_all_declared_build_dependencies() -> None:
    dockerfile = (ROOT / "tests/compose/Dockerfile").read_text(encoding="utf-8")

    for package in ("libboost-dev", "libxxhash-dev", "liblzo2-dev", "libzstd-dev", "libarchive-dev"):
        assert re.search(rf"\b{re.escape(package)}\b", dockerfile)


def test_compose_image_bootstraps_autotools_before_configuration() -> None:
    dockerfile = (ROOT / "tests/compose/Dockerfile").read_text(encoding="utf-8")

    autogen = dockerfile.index("./autogen.sh")
    configure = dockerfile.index("./configure")
    build = dockerfile.index('make -j"${BUILD_JOBS}"')
    assert autogen < configure < build
    assert "COPY . /src" in dockerfile


def test_compose_build_job_default_is_bounded_and_overridable() -> None:
    dockerfile = (ROOT / "tests/compose/Dockerfile").read_text(encoding="utf-8")

    assert re.search(r"(?m)^ARG BUILD_JOBS=2$", dockerfile)
    assert 'make -j"${BUILD_JOBS}"' in dockerfile
    assert "$(nproc)" not in dockerfile
