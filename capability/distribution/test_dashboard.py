from __future__ import annotations

import importlib.util
import json
import shutil
import sys
import tempfile
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("dashboard.py")
SPEC = importlib.util.spec_from_file_location("dashboard_under_test", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
dashboard = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = dashboard
SPEC.loader.exec_module(dashboard)


def descriptor() -> dict[str, object]:
    return {
        "record": "experiment",
        "schema": "icecream-distribution-timeline-v1",
        "scenario": "small",
        "topology": "C1F2",
        "codec_adapter": "raw",
        "physical_codec_result": False,
        "clock": {"snapshot_interval_ns": 10},
        "topology_dimensions": {"logical_c_authorities": 1, "f_stores": 2},
    }


def snapshot(
    i: int, events: list[dict[str, object]] | None = None
) -> dict[str, object]:
    return {
        "record": "snapshot",
        "sequence": i,
        "wall_start_ns": i,
        "wall_end_ns": i + 1,
        "active_start_ns": i,
        "active_end_ns": i + 1,
        "events": events or [],
        "state": {
            "scheduler": {"ready_tus": 0, "active_tus": 1, "completed_tus": i},
            "network": {"active_flows": 1},
            "c": [
                {
                    "environment": 0,
                    "ready_tus": 0,
                    "active_tus": 1,
                    "completed_tus": i,
                    "targets": [],
                }
            ],
            "f": [
                {"worker": 0, "compiling_slots": 1},
                {"worker": 1, "compiling_slots": 0},
            ],
        },
        "metrics": {"routes": [], "workers": [], "environments": []},
    }


class DashboardTest(unittest.TestCase):
    def test_streaming_projection_is_bounded_and_retains_gaps(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "experiment.jsonl"
            event = {
                "sequence": 1,
                "time_ns": 5,
                "event": "flow-queued",
                "build": 0,
                "worker": 0,
                "phase": "dict",
                "direction": "c_to_f",
                "bytes": 17,
            }
            rows = [descriptor()]
            rows.extend(
                snapshot(i, [event] if i in (0, 5_000, 9_999) else [])
                for i in range(10_000)
            )
            rows.insert(
                500,
                {
                    "record": "gap",
                    "wall_start_ns": 500,
                    "wall_end_ns": 600,
                    "wall_duration_ns": 100,
                    "reason": "workload-release",
                    "events": [],
                },
            )
            rows.append(
                {
                    "record": "summary",
                    "event_count": 3,
                    "summary": {
                        "workers": 2,
                        "environments": 1,
                        "build_epochs": 1,
                        "jobs": 1,
                        "makespan_ns": 10_000,
                        "timeline_active_ns": 9_900,
                    },
                }
            )
            path.write_text("\n".join(json.dumps(row) for row in rows) + "\n")
            d, view, final = dashboard.read_experiment(path, limit=100)
            self.assertEqual(final["record"], "summary")
            self.assertEqual(d["report_view"]["source_snapshots"], 10_000)
            self.assertEqual(d["report_view"]["embedded_snapshots"], 100)
            self.assertEqual(sum(row["record"] == "gap" for row in view), 1)
            out = dashboard.render_experiment_html(d, view, final, compact=False)
            self.assertLess(len(out), 1_000_000)
            self.assertNotIn('"codec_namespaces"', out)

    def test_gap_events_and_canonical_phase_extent_are_counted_once(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "experiment.jsonl"
            e = {
                "sequence": 2,
                "time_ns": 4,
                "event": "flow-queued",
                "build": 0,
                "worker": 0,
                "phase": "dict",
                "direction": "c_to_f",
                "bytes": 11,
            }
            finish = {**e, "sequence": 3, "event": "flow-finish"}
            path.write_text(
                "\n".join(
                    json.dumps(x)
                    for x in [
                        descriptor(),
                        {
                            "record": "gap",
                            "wall_start_ns": 0,
                            "wall_end_ns": 2,
                            "events": [e, finish],
                        },
                        {
                            "record": "summary",
                            "event_count": 2,
                            "summary": {
                                "workers": 1,
                                "environments": 1,
                                "build_epochs": 1,
                            },
                        },
                    ]
                )
                + "\n"
            )
            d, _, _ = dashboard.read_experiment(path)
            self.assertEqual(d["dashboard_aggregates"]["event_count"], 2)
            self.assertEqual(d["dashboard_aggregates"]["sampled_events"], 2)
            self.assertEqual(d["dashboard_aggregates"]["phases"][0]["bytes"], 11)
            self.assertEqual(d["report_view"]["source_records"], 1)
            self.assertEqual(d["report_view"]["source_event_count"], 2)

    def test_empty_trace_renders(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "experiment.jsonl"
            path.write_text(
                json.dumps(descriptor())
                + "\n"
                + json.dumps(
                    {
                        "record": "summary",
                        "event_count": 0,
                        "summary": {"workers": 0, "environments": 0, "build_epochs": 0},
                    }
                )
                + "\n"
            )
            d, view, final = dashboard.read_experiment(path)
            self.assertEqual(view, [])
            self.assertIn(
                "No active snapshots",
                dashboard.render_experiment_html(d, view, final, compact=False),
            )

    def test_authoritative_fixture_and_output_alias(self) -> None:
        fixture = (
            MODULE_PATH.with_name("samples") / "r4-minimum" / "sample-experiment.jsonl"
        )
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "report.html"
            dashboard.render_experiment_file(fixture, output)
            self.assertIn("icecream-execution-v2", output.read_text())
        before = fixture.read_bytes()
        with self.assertRaises(ValueError):
            dashboard.render_experiment_file(fixture, fixture)
        self.assertEqual(fixture.read_bytes(), before)

    def test_authoritative_validator_rejects_changed_summary(self) -> None:
        fixture_dir = MODULE_PATH.with_name("samples") / "r4-minimum"
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "experiment.jsonl"
            shutil.copy2(fixture_dir / "sample-experiment.jsonl", source)
            shutil.copy2(fixture_dir / "route-trace.jsonl", root / "route-trace.jsonl")
            rows = [json.loads(line) for line in source.read_text().splitlines()]
            rows[-1]["summary"]["scored_outgoing_bytes"] = 999
            source.write_text("\n".join(json.dumps(row) for row in rows) + "\n")
            with self.assertRaises(ValueError):
                dashboard.render_experiment_file(source, root / "report.html")
            self.assertFalse((root / "report.html").exists())

    def test_optional_state_projection_has_an_explicit_byte_cap(self) -> None:
        row = snapshot(0)
        row["state"]["f"][0].update(
            {f"cache_state_{index}": "x" * 100 for index in range(20)}
        )
        projected = dashboard._slim_row(row)
        selected_f = projected["state"]["f"][0]
        self.assertTrue(selected_f["state_projection_truncated"])
        self.assertLess(len(json.dumps(selected_f, separators=(",", ":"))), 700)


if __name__ == "__main__":
    unittest.main()
