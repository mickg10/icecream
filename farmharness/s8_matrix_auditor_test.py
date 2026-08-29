"""Fixture tests for the read-only S8 matrix auditor."""

from __future__ import annotations

import hashlib
import json
import tempfile
import unittest
from pathlib import Path

from s8_matrix_auditor import CELLS, audit, markdown
from s8_schema import SPLITS


def _canonical(value: object) -> bytes:
    return (json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n").encode()


def _records(cell: tuple[str, str, str], points: int = 2) -> bytes:
    corpus, profile, regime = cell
    identity = {
        "corpus": corpus, "profile": profile, "regime": regime,
        "split": SPLITS[corpus], "run_id": "run-001",
        "source_commit": "a" * 40, "source_tree": "b" * 40,
        "input_digest": "c" * 64, "topology_digest": "d" * 64,
        "model_id": "model-v1",
    }
    units = {"point": "step", "channel_bytes": "bytes", "elapsed_ns": "ns"}
    predicted, observed = [], []
    errors, losses = [], []
    total = 0.0
    for step in range(points):
        p = {"channel_bytes": 10 + step * 10, "elapsed_ns": 100 + step * 10}
        o = {"channel_bytes": 12 + step * 10, "elapsed_ns": 105 + step * 10}
        predicted.append({"step": step, "tu_id": f"tu-{step}", "cumulative": p})
        observed.append({"step": step, "tu_id": f"tu-{step}", "cumulative": o})
        point = {}
        squared = 0.0
        for key in p:
            signed = p[key] - o[key]
            sq = signed * signed
            squared += sq
            point[f"cumulative.{key}"] = {
                "signed": signed, "absolute": abs(signed),
                "relative": signed / abs(o[key]), "squared": sq,
            }
        total += squared
        errors.append({"step": step, "tu_id": f"tu-{step}", "errors": point})
        losses.append({"step": step, "tu_id": f"tu-{step}",
                       "squared_error": squared, "cumulative_loss": total})
    cell_value = {"corpus": corpus, "profile": profile, "regime": regime}
    common = {"schema": "icecream-s8-predictive-live-record-v1",
              "semantics": "s8-current-semantics-v1", "cell": cell_value,
              "split": SPLITS[corpus], "identity": identity, "units": units}
    pred = {**common, "record_type": "predictive_sim", "model_id": "model-v1",
            "provenance": {"mode": "predictive_sim", "producer": "predictor-v1", "trace_free": True,
                            "manifest_sha256": "3" * 64, "curve_sha256": "1" * 64},
            "raw_cumulative_curve": predicted}
    live_identity = {**identity, "model_id": "live-v1"}
    live = {**common, "record_type": "live", "model_id": "live-v1", "identity": live_identity,
            "provenance": {"mode": "live", "producer": "live-v1", "trace_free": False,
                            "manifest_sha256": "4" * 64, "curve_sha256": "2" * 64},
            "raw_cumulative_curve": observed}
    comparison = {**common, "record_type": "comparison", "model_id": "model-v1",
                  "provenance": {"predictive_manifest_sha256": "3" * 64,
                                  "live_manifest_sha256": "4" * 64,
                                  "predictive_curve_sha256": "1" * 64,
                                  "live_curve_sha256": "2" * 64},
                  "point_errors": errors, "loss_curve": losses}
    return b"".join(_canonical(item) for item in (pred, live, comparison))


def _experiment(root: Path, cell: tuple[str, str, str], *, points: int = 2,
                requested: object = "unspecified", split: str | None = None) -> Path:
    path = root / ("-".join(cell) + "-experiment")
    path.mkdir()
    records = _records(cell, points)
    records_path = path / "records.jsonl"
    records_path.write_bytes(records)
    manifest = {
        "schema": "icecream-s8-first-triple-driver-v2", "status": "PASS",
        "cell": {"corpus": cell[0], "profile": cell[1], "regime": cell[2]},
        "split": split or SPLITS[cell[0]],
        "records": {"path": "records.jsonl", "sha256": hashlib.sha256(records).hexdigest(),
                    "bytes": len(records)},
    }
    if requested != "unspecified":
        manifest["requested_curve_points"] = requested
    (path / "experiment_manifest.json").write_bytes(_canonical(manifest))
    return path


class MatrixAuditorTest(unittest.TestCase):
    def test_full_matrix_split_counts_and_readable_report(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for cell in CELLS:
                _experiment(root, cell)
            summary = audit(root)
            self.assertEqual(summary["status"], "PASS")
            self.assertEqual(summary["matrix"]["completed_cells"], 32)
            self.assertEqual(summary["matrix"]["calibration_cells"], 16)
            self.assertEqual(summary["matrix"]["held_out_validation_cells"], 16)
            row = next(item for item in summary["cells"] if item["cell"] == "fmt/ZSTD_TU/cold")
            self.assertEqual(row["live_values"], {"channel_bytes": 22, "elapsed_ns": 115})
            self.assertEqual(row["prediction_errors_final"]["cumulative.channel_bytes"]["signed"], -2)
            report = markdown(summary)
            self.assertIn("| fmt/ZSTD_TU/cold | calibration |", report)
            self.assertIn("| LLVM-1238/GRZ_RESIDUAL/warm | held_out_validation |", report)
            self.assertIn("Prediction errors (final; loss)", report)

    def test_one_point_requested_depth_and_supplemental_exclusion(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _experiment(root, ("fmt", "GRZ_RESIDUAL", "cold"), points=1, requested=100)
            supplemental = root / "supplemental"
            supplemental.mkdir()
            (supplemental / "records.jsonl").write_bytes(b"old\n")
            summary = audit(root)
            self.assertEqual(summary["status"], "INCOMPLETE")
            row = summary["cells"][0]
            self.assertEqual(row["curve_depth"], {"requested": 100, "observed": 1,
                                                     "classification": "one-point-underfilled"})
            self.assertEqual(summary["matrix"]["ignored_noncanonical_records"], 1)

    def test_depth_labels_preserve_requested_200_full_and_repeat_full(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for index, requested in enumerate((200, "full", "repeat-full")):
                cell = CELLS[index]
                _experiment(root, cell, points=1, requested=requested)
            summary = audit(root)
            labels = {row["cell"]: row["curve_depth"] for row in summary["cells"]}
            requested_values = [labels["/".join(CELLS[index])]["requested"] for index in range(3)]
            self.assertEqual(requested_values, [200, "full", "repeat-full"])
            labels = list(labels.values())
            self.assertTrue(all(item["classification"] == "one-point-underfilled" for item in labels))

    def test_duplicate_and_wrong_split_candidates_are_invalid(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _experiment(root, ("fmt", "P29", "warm"))
            duplicate = root / "duplicate"
            duplicate.mkdir()
            source = root / "fmt-P29-warm-experiment"
            (duplicate / "records.jsonl").write_bytes((source / "records.jsonl").read_bytes())
            _canonical_manifest = {
                "schema": "icecream-s8-first-triple-driver-v2", "status": "PASS",
                "cell": {"corpus": "fmt", "profile": "P29", "regime": "warm"},
                "split": "calibration",
                "records": {"path": "records.jsonl", "sha256": hashlib.sha256((duplicate / "records.jsonl").read_bytes()).hexdigest(),
                            "bytes": (duplicate / "records.jsonl").stat().st_size},
            }
            (duplicate / "experiment_manifest.json").write_bytes(_canonical(_canonical_manifest))
            wrong = _experiment(root, ("RocksDB", "P29", "cold"), split="held_out_validation")
            summary = audit(root)
            reasons = [item["reason"] for item in summary["matrix"]["invalid_candidates"]]
            self.assertIn("duplicate_canonical_cell", reasons)
            self.assertTrue(any("split_mismatch" in reason for reason in reasons))

    def test_mutated_records_fail_manifest_authentication(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            experiment = _experiment(root, CELLS[0])
            records_path = experiment / "records.jsonl"
            records_path.write_bytes(records_path.read_bytes().replace(b"model-v1", b"model-v2", 1))
            summary = audit(root)
            self.assertEqual(summary["status"], "INCOMPLETE")
            self.assertEqual(summary["matrix"]["completed_cells"], 0)
            self.assertIn("records_digest_mismatch", summary["matrix"]["invalid_candidates"][0]["reason"])


if __name__ == "__main__":
    unittest.main()
