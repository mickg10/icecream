from pathlib import Path

import pytest

from s8_derived_experiment_packager import package
import s8_matrix_auditor as auditor


PREDICTIVE = Path("/tanksmall/scratch/ictmp/experiments/icecream/s8-fmt-zstd-tu-a90dc4f9-20260830T080127Z/depth-200/s8-fmt-ZSTD_TU-cold-C1F1-200-20260830T080127Z/predictive_curve_manifest.json")
LIVE = Path("/tanksmall/scratch/ictmp/experiments/icecream/s8-fmt-zstd-tu-a90dc4f9-20260830T080127Z/depth-200/live-output/C1F1/icecream/C1F1-100000/20260830T080127Z/ZSTD_TU")


@pytest.mark.skipif(not PREDICTIVE.exists() or not (LIVE / "experiment_manifest.json").exists(),
                    reason="retained exact-a90 fmt200 artifacts unavailable")
def test_retained_fmt200_is_packaged_and_auditable(tmp_path: Path) -> None:
    output = package(PREDICTIVE, LIVE, "full-1", tmp_path)
    assert sorted(path.name for path in output.iterdir()) == ["experiment_manifest.json", "records.jsonl"]
    report = auditor.audit(output)
    assert report["matrix"]["completed_cells"] == 1
    assert not report["matrix"]["invalid_candidates"]
