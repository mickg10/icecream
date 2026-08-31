from pathlib import Path

import pytest

from s8_derived_experiment_packager import (
    _authority_calibration_metadata, _authority_role_placement, _derived_depth_class,
    _derived_manifest, package, PackagingError,
)


PREDICTIVE = Path("/tanksmall/scratch/ictmp/experiments/icecream/s8-fmt-zstd-tu-a90dc4f9-20260830T080127Z/depth-200/s8-fmt-ZSTD_TU-cold-C1F1-200-20260830T080127Z/predictive_curve_manifest.json")
LIVE = Path("/tanksmall/scratch/ictmp/experiments/icecream/s8-fmt-zstd-tu-a90dc4f9-20260830T080127Z/depth-200/live-output/C1F1/icecream/C1F1-100000/20260830T080127Z/ZSTD_TU")


def test_repeat_full_depth_label_preserves_source_depth() -> None:
    assert _derived_depth_class("200", "full-1") == "200"
    assert _derived_depth_class("full", "full-1") == "full"
    assert _derived_depth_class("full", "full-2") == "repeat-full"


def test_authority_calibration_metadata_requires_exact_captured_identity() -> None:
    metadata = {
        "product_image_digest": "a" * 64,
        "toolchain_digest": "b" * 64,
        "output_contract_digest": "c" * 64,
        "host_digest": "d" * 64,
        "ordered_input_class": "ordered",
    }
    assert _authority_calibration_metadata({"calibration_metadata": metadata}) == metadata
    missing_host = dict(metadata)
    del missing_host["host_digest"]
    with pytest.raises(PackagingError, match="host_digest_missing"):
        _authority_calibration_metadata({"calibration_metadata": missing_host})


def test_authority_requires_explicit_external_farm_placement() -> None:
    with pytest.raises(PackagingError, match="role_placement:(invalid|not_timing_eligible)"):
        _authority_role_placement({})
    placement = {
        "schema": "icecream-s8-role-placement-v1", "mode": "external_farm",
        "c_host_digest": "1" * 64, "scheduler_host_digest": "1" * 64,
        "f_host_digests": ["2" * 64], "roles_disjoint": True,
        "timing_eligible": True,
    }
    assert _authority_role_placement({"role_placement": placement}) == placement


def test_derived_manifest_propagates_authenticated_timing_authority() -> None:
    placement = {
        "schema": "icecream-s8-role-placement-v1", "mode": "external_farm",
        "c_host_digest": "1" * 64, "scheduler_host_digest": "1" * 64,
        "f_host_digests": ["2" * 64], "roles_disjoint": True,
        "timing_eligible": True,
    }
    source = {
        "cell": ("fmt", "ZSTD_TU", "cold"), "split": "calibration",
        "topology": "C1F1/100000", "suite": "C1F1/100000", "depth": "full",
        "runs": ["full-1"], "declared_count": 3,
        "path": Path("source/experiment_manifest.json"),
        "facts": {"sha256": "3" * 64, "bytes": 17},
        "plan_sha256": "4" * 64, "calibration_metadata": {},
        "execution_scope": "external_farm_timing", "role_placement": placement,
    }
    manifest = _derived_manifest(
        source, "full-1", Path("predictive.json"), {"sha256": "5" * 64, "bytes": 19},
        Path("live.json"), {"sha256": "6" * 64, "bytes": 23},
        {"sha256": "7" * 64, "bytes": 29})
    assert manifest["execution_scope"] == "external_farm_timing"
    assert manifest["role_placement"] == placement


@pytest.mark.skipif(not PREDICTIVE.exists() or not (LIVE / "experiment_manifest.json").exists(),
                    reason="retained exact-a90 fmt200 artifacts unavailable")
def test_retained_fmt200_is_packaged_and_auditable(tmp_path: Path) -> None:
    with pytest.raises(PackagingError, match="calibration_metadata:host_digest_missing"):
        package(PREDICTIVE, LIVE, "full-1", tmp_path)
