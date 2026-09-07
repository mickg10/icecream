from __future__ import annotations

import os
import subprocess
from pathlib import Path

import pytest

from farmharness.integration import farmtest


ROOT = Path(__file__).resolve().parents[3]


def test_one_farm_temp_variable_routes_all_standard_temp_variables(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    routed = tmp_path / "scratch-temp"
    monkeypatch.setenv("ICEFARM_TMPDIR", str(routed))
    for variable in ("TMPDIR", "TMP", "TEMP", "TEMPDIR"):
        monkeypatch.delenv(variable, raising=False)

    assert farmtest._configure_host_temp_environment() == routed
    assert routed.is_dir()
    for variable in ("TMPDIR", "TMP", "TEMP", "TEMPDIR"):
        assert os.environ[variable] == str(routed)


def test_farm_temp_variable_must_be_absolute(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setenv("ICEFARM_TMPDIR", "relative-temp")

    with pytest.raises(farmtest.PlanError, match="must be an absolute path"):
        farmtest._configure_host_temp_environment()


def test_required_integration_make_targets_are_scratch_routed() -> None:
    suites = {
        "integration_smoke": "smoke.json",
        "integration_controls": "controls.json",
        "integration_ladder": "ladder.json",
        "integration_twobuild": "twobuild.json",
        "integration_full": "full.json",
    }
    for target, suite in suites.items():
        result = subprocess.run(
            ("make", "--no-print-directory", "-n", target),
            cwd=ROOT,
            check=True,
            capture_output=True,
            text=True,
        )
        command = result.stdout
        assert 'ICEFARM_TMPDIR="/tmp/i"' in command
        for variable in ("TMPDIR", "TMP", "TEMP", "TEMPDIR"):
            assert f'{variable}="/tmp/i"' in command
        assert "farmharness.integration.farmtest suite" in command
        assert f"farmharness/integration/suites/{suite}" in command
        assert ("--stop-on-fail" in command) is (
            target in {"integration_ladder", "integration_twobuild", "integration_full"}
        )


def test_image_make_target_is_scratch_routed() -> None:
    result = subprocess.run(
        ("make", "--no-print-directory", "-n", "integration_images"),
        cwd=ROOT,
        check=True,
        capture_output=True,
        text=True,
    )
    assert 'ICEFARM_TMPDIR="/tmp/i"' in result.stdout
    for variable in ("TMPDIR", "TMP", "TEMP", "TEMPDIR"):
        assert f'{variable}="/tmp/i"' in result.stdout
    assert result.stdout.count("farmharness.integration.farmtest images") == 2
    assert result.stdout.count("--foundations") == 1
    assert (
        '--labels "p43-1.4.0,p50s2-5b2e5801,p50s4-89917385,'
        'p50s4-b42d65e8,p50s4-0c820e79,p50s4-2deb91d6,'
        'p50s4-57a1e336,p50s4-h3-tail-57a1e336,p50s4-a82d72d8,'
        'p50s90-f-revision-2-a82d72d8,p50s90-f-hidden-skew-a82d72d8"'
    ) in result.stdout
    assert "p50s30-f-refusal-mutant-candidate" not in result.stdout
    assert "p50s30-f-refusal-0c820e79" not in result.stdout
    assert "p50s30-f-refusal-2deb91d6" not in result.stdout
    assert "p50s30-f-refusal-57a1e336" not in result.stdout
    assert "p50s90-f-revision-2-candidate" not in result.stdout
    assert "p50s90-f-revision-2-57a1e336" not in result.stdout
    assert '--repo "' in result.stdout

    retained = subprocess.run(
        (
            "make",
            "--no-print-directory",
            "-n",
            "integration_images",
            "ICEFARM_IMAGE_RECEIPT_DIR=/tanksmall/scratch/ictmp/operator-receipts",
        ),
        cwd=ROOT,
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    assert '--output "/tanksmall/scratch/ictmp/operator-receipts/foundations.json"' in retained
    assert (
        '--output "/tanksmall/scratch/ictmp/operator-receipts/sealed-products.json"'
        in retained
    )

    source_backed = subprocess.run(
        (
            "make",
            "--no-print-directory",
            "-n",
            "integration_images",
            "ICEFARM_SOURCE_ARCHIVE_DIR=/tanksmall/scratch/ictmp/source-inventory",
        ),
        cwd=ROOT,
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    assert (
        '--source-archive-dir "/tanksmall/scratch/ictmp/source-inventory"'
        in source_backed
    )


def test_source_archive_make_target_is_explicit_and_scratch_routed() -> None:
    result = subprocess.run(
        (
            "make",
            "--no-print-directory",
            "-n",
            "integration_source_archives",
            "ICEFARM_SOURCE_ARCHIVE_DIR=/tanksmall/scratch/ictmp/source-inventory",
        ),
        cwd=ROOT,
        check=True,
        capture_output=True,
        text=True,
    )
    assert 'ICEFARM_TMPDIR="/tmp/i"' in result.stdout
    assert "farmharness.integration.farmtest source-archives" in result.stdout
    assert '--output-dir "/tanksmall/scratch/ictmp/source-inventory"' in result.stdout
    assert (
        '--labels "p43-1.4.0,p50s2-5b2e5801,p50s4-89917385,'
        'p50s4-b42d65e8,p50s4-0c820e79,p50s4-2deb91d6,'
        'p50s4-57a1e336,p50s4-h3-tail-57a1e336,p50s4-a82d72d8,'
        'p50s90-f-revision-2-a82d72d8,p50s90-f-hidden-skew-a82d72d8,'
        'p50s30-f-refusal-mutant-candidate,'
        'p50s30-f-refusal-0c820e79,'
        'p50s30-f-refusal-2deb91d6,'
        'p50s30-f-refusal-57a1e336,'
        'p50s90-f-revision-2-candidate,'
        'p50s90-f-revision-2-57a1e336"'
    ) in result.stdout


def test_autotools_source_exposes_the_same_required_targets() -> None:
    text = (ROOT / "Makefile.am").read_text(encoding="utf-8")
    for target in (
        "integration_source_archives",
        "integration_images",
        "integration_smoke",
        "integration_controls",
        "integration_ladder",
        "integration_twobuild",
        "integration_full",
    ):
        assert f"{target}:" in text
    assert "ICEFARM_TMPDIR ?= /tmp/i" in text
    assert 'ICEFARM_TMPDIR="$(ICEFARM_TMPDIR)"' in text
    for variable in ("TMPDIR", "TMP", "TEMP", "TEMPDIR"):
        assert f'{variable}="$(ICEFARM_TMPDIR)"' in text
    image_target = text.split("integration_images:", 1)[1].split(
        "integration_smoke:", 1
    )[0]
    assert image_target.count("farmharness.integration.farmtest images") == 2
    assert image_target.count("--foundations") == 1
    assert "ICEFARM_SEALED_LABELS" in text
    assert "ICEFARM_SOURCE_LABELS" in text
    assert "ICEFARM_SOURCE_ARCHIVE_DIR" in text
    assert "p50s30-f-refusal-mutant-candidate" not in image_target
    assert "p50s30-f-refusal-57a1e336" not in image_target
    assert "p50s90-f-revision-2-candidate" not in image_target
    assert "foundations.json" in image_target
    assert "sealed-products.json" in image_target
