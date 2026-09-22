from __future__ import annotations

import json
import os
import re
import subprocess
from pathlib import Path

import pytest

from farmharness.integration import farmtest


ROOT = Path(__file__).resolve().parents[3]


def _catalog_image_labels() -> set[str]:
    return {
        label
        for path in (ROOT / "farmharness/integration/scenarios").glob("*.json")
        for label in json.loads(path.read_text(encoding="utf-8"))["images"].values()
    }


def _command_labels(command: str) -> list[str]:
    labels = re.findall(r'--labels "([^"]+)"', command)
    assert len(labels) == 1
    return labels[0].split(",")


def test_image_defaults_match_current_catalog_in_both_makefiles() -> None:
    expected = _catalog_image_labels()
    # The old-generation image and H1's deliberately wrong image are both
    # necessary; "old" is not the same thing as "unused".
    assert {"p43-1.4.0", "p50s2-5b2e5801"} <= expected
    for name in ("GNUmakefile", "Makefile.am"):
        text = (ROOT / name).read_text(encoding="utf-8")
        labels = re.search(r"^ICEFARM_SEALED_LABELS = (.+)$", text, re.MULTILINE)
        assert labels is not None
        actual = labels[1].split(",")
        assert len(actual) == len(set(actual))
        assert set(actual) == expected, name
        assert "ICEFARM_SOURCE_LABELS = $(ICEFARM_SEALED_LABELS)\n" in text


@pytest.mark.parametrize("target", ["integration_images", "integration_source_archives"])
def test_historical_image_work_requires_but_still_accepts_explicit_labels(target: str) -> None:
    result = subprocess.run(
        (
            "make", "--no-print-directory", "-n", target,
            "LABELS=p50s4-89917385",
            "ICEFARM_SOURCE_ARCHIVE_DIR=/tanksmall/scratch/ictmp/source-inventory",
        ),
        cwd=ROOT, check=True, capture_output=True, text=True,
    )
    assert _command_labels(result.stdout) == ["p50s4-89917385"]


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


def test_required_integration_make_targets_are_scratch_routed(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.delenv("ICEFARM_TMPDIR", raising=False)
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


def test_image_make_target_is_scratch_routed(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.delenv("ICEFARM_TMPDIR", raising=False)
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
    assert set(_command_labels(result.stdout)) == _catalog_image_labels()
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


def test_source_archive_make_target_is_explicit_and_scratch_routed(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.delenv("ICEFARM_TMPDIR", raising=False)
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
    assert set(_command_labels(result.stdout)) == _catalog_image_labels()


@pytest.mark.parametrize(
    "target",
    [
        "integration_smoke",
        "integration_controls",
        "integration_ladder",
        "integration_twobuild",
        "integration_full",
        "integration_images",
        "integration_source_archives",
    ],
)
def test_make_targets_preserve_operator_temp_override(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path, target: str,
) -> None:
    routed = str(tmp_path / "scratch temp")
    monkeypatch.setenv("ICEFARM_TMPDIR", routed)
    command = subprocess.run(
        (
            "make", "--no-print-directory", "-n", target,
            f"ICEFARM_SOURCE_ARCHIVE_DIR={tmp_path / 'source-inventory'}",
        ),
        cwd=ROOT, check=True, capture_output=True, text=True,
    ).stdout
    for variable in ("ICEFARM_TMPDIR", "TMPDIR", "TMP", "TEMP", "TEMPDIR"):
        assert f'{variable}="{routed}"' in command


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
