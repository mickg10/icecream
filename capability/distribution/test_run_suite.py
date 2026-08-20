#!/usr/bin/env python3
from __future__ import annotations

import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("run_suite.py")
SPEC = importlib.util.spec_from_file_location("run_suite", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
suite = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = suite
SPEC.loader.exec_module(suite)


def scenario_document(name: str, workers: int, slots: int) -> dict[str, object]:
    return {
        "schema": "icecream-distribution-scenario-v1",
        "name": name,
        "seed": 1,
        "environments": {
            "env_count": 1,
            "job_selection": {
                "mode": "explicit",
                "jobs": [
                    {
                        "id": "work",
                        "environment": 0,
                        "trace": "trace.tsv",
                        "corpus_root": ".",
                        "builds": 2,
                        "start_ns": 0,
                        "build_release": {
                            "mode": "after-previous",
                            "gap_ns": 600,
                        },
                        "tu_release": {"mode": "all-at-zero"},
                    }
                ],
            },
        },
        "workers": {
            "f_count": workers,
            "template": {
                "slots": slots,
                "compile_profile": "test-model",
                "initial_cache": "cold",
            },
        },
        "network": {
            "c_to_f": {
                "bits_per_second": 800,
                "one_way_latency_ns": 0,
                "lanes_per_endpoint": 1,
            },
            "f_to_c": {
                "bits_per_second": 800,
                "one_way_latency_ns": 0,
                "lanes_per_endpoint": 1,
            },
            "shared_fabric_bps": 800,
        },
        "scheduler": {
            "ready_job_policy": "fifo-release",
            "placement_policy": "round-robin",
        },
        "experiment": {"codecs": ["grz", "p29"], "routing_mode": "online"},
    }


class SuiteTest(unittest.TestCase):
    def test_checked_in_topology_v2_suite_has_expected_physical_limits(self) -> None:
        root = Path(__file__).parent
        with tempfile.TemporaryDirectory() as directory:
            matrix, _ = suite.run_suite(
                root / "topology-v2-smoke-suite.json",
                Path(directory),
                require_payload=True,
            )
        self.assertEqual(
            [row["topology"] for row in matrix],
            [
                "P2A1E1F2_1E72X144",
                "P2A1E2F2_1E72X144",
                "P2A1E2F2_1A72E72X144",
            ],
        )
        self.assertEqual(
            [row["wall_makespan_ns"] for row in matrix],
            [2_000_000_001, 1_000_000_001, 2_000_000_001],
        )

    def test_requested_firefox_suite_shape(self) -> None:
        root = Path(__file__).parent
        suite_document, scenario_paths = suite.load_suite(
            root / "firefox-5build-topology-suite.json"
        )
        self.assertEqual(suite_document["diagnostic_codecs"], ["compile-only"])
        expected = {
            "firefox-1c-1f-1000000cores.json": (1, 1, 1_000_000),
            "firefox-1c-20f-50cores.json": (1, 20, 50),
            "firefox-1c-50f-50cores.json": (1, 50, 50),
            "firefox-10c-50f-60cores.json": (10, 50, 60),
        }
        self.assertEqual({path.name for path in scenario_paths}, set(expected))
        for path in scenario_paths:
            document = json.loads(path.read_text())
            env_count, worker_count, slots = expected[path.name]
            self.assertEqual(document["environments"]["env_count"], env_count)
            self.assertEqual(document["workers"]["f_count"], worker_count)
            self.assertEqual(document["workers"]["template"]["slots"], slots)
            jobs = document["environments"]["job_selection"]["jobs"]
            self.assertEqual(len(jobs), env_count)
            self.assertEqual(
                {job["environment"] for job in jobs}, set(range(env_count))
            )
            for job in jobs:
                self.assertEqual(job["builds"], 5)
                self.assertEqual(job["start_ns"], 0)
                self.assertEqual(job["build_release"]["mode"], "after-previous")
                self.assertEqual(job["build_release"]["gap_ns"], 600_000_000_000)
                self.assertEqual(job["tu_release"]["mode"], "all-at-zero")
                self.assertEqual(job["trace"], "../firefox-corrected.compile-trace.tsv")

    def test_one_command_runs_every_scenario_and_writes_build_matrix(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "trace.tsv").write_text(
                "logical\tjob_id\tii_relative\traw_bytes\tcompile_ns\tcompile_model\n"
                "0\tj0\tj0.ii\t1\t5\ttest-model\n"
            )
            scenario_paths = []
            for name, workers, slots in (("small", 2, 1), ("giant", 1, 1_000_000)):
                path = root / f"{name}.json"
                path.write_text(json.dumps(scenario_document(name, workers, slots)))
                scenario_paths.append(path.name)
            suite_path = root / "suite.json"
            suite_path.write_text(
                json.dumps(
                    {
                        "schema": "icecream-distribution-suite-v1",
                        "name": "test-suite",
                        "scenarios": scenario_paths,
                        "diagnostic_codecs": ["compile-only"],
                    }
                )
            )
            output = root / "out"
            matrix, builds = suite.run_suite(suite_path, output)
            self.assertEqual(len(matrix), 2)
            self.assertEqual(len(builds), 4)
            self.assertEqual({row["cold_builds"] for row in matrix}, {1})
            self.assertEqual({row["warm_builds"] for row in matrix}, {1})
            self.assertEqual({row["summed_generation_ns"] for row in matrix}, {10})
            self.assertEqual({row["wall_makespan_ns"] for row in matrix}, {610})
            self.assertEqual(
                {row["gap_from_previous_finish_ns"] for row in builds}, {"", 600}
            )
            self.assertTrue((output / "matrix.tsv").is_file())
            self.assertTrue((output / "builds.tsv").is_file())
            self.assertTrue((output / "generations.tsv").is_file())
            self.assertTrue((output / "runner-timings.tsv").is_file())
            self.assertTrue((output / "suite-summary.json").is_file())
            self.assertTrue(
                (output / "giant" / "compile-only" / "assignments.tsv").is_file()
            )
            second_output = root / "out-second"
            suite.run_suite(suite_path, second_output)
            for relative in (
                "matrix.tsv",
                "builds.tsv",
                "generations.tsv",
                "suite-summary.json",
            ):
                self.assertEqual(
                    (output / relative).read_bytes(),
                    (second_output / relative).read_bytes(),
                )


if __name__ == "__main__":
    unittest.main()
