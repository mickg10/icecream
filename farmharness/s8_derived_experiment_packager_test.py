from pathlib import Path

import pytest

from s8_derived_experiment_packager import (
    _authority_calibration_metadata, _derived_depth_class, package, PackagingError,
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


@pytest.mark.skipif(not PREDICTIVE.exists() or not (LIVE / "experiment_manifest.json").exists(),
                    reason="retained exact-a90 fmt200 artifacts unavailable")
def test_retained_fmt200_is_packaged_and_auditable(tmp_path: Path) -> None:
    with pytest.raises(PackagingError, match="calibration_metadata:host_digest_missing"):
        package(PREDICTIVE, LIVE, "full-1", tmp_path)
