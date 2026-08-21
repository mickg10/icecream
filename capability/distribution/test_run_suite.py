#!/usr/bin/env python3
from __future__ import annotations

import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


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


def write_physical_ledger(path: Path, scenario_path: Path) -> Path:
    scenario = suite.sim.load_scenario(scenario_path)
    diagnostic = suite.sim.Simulator(
        scenario, suite.sim.CompileOnlyAdapter(), snapshot_interval_ns=10**30
    ).run()
    by_key = {
        item.key: item for items in scenario.work_items.values() for item in items
    }
    rows: list[dict[str, object]] = [
        {
            "record": "physical-ledger",
            "schema": "icecream-physical-codec-ledger-v1",
            "codec": "p29",
            "scenario_sha256": suite.sim.sha256(scenario_path),
            "reconstruction": {"status": "pass", "method": "test"},
        }
    ]
    for assignment in diagnostic.assignments:
        key = (
            str(assignment["workload"]),
            int(assignment["build"]),
            int(assignment["logical"]),
        )
        item = by_key[key]
        rows.append(
            {
                "record": "tu",
                "workload": item.workload,
                "build": item.build,
                "logical": item.logical,
                "worker": int(assignment["worker"]),
                "tu_seq": int(assignment["tu_seq"]),
                "rel_seq": int(assignment["rel_seq"]),
                "route_sequence": int(assignment["rel_seq"]),
                "raw_bytes": item.raw_bytes,
                "raw_sha256": suite.sim.sha256(item.payload),
                "phases": [
                    {"name": "p29-test", "direction": "c_to_f", "bytes": 1}
                ],
                "state_after": {"route_commits": int(assignment["rel_seq"]) + 1},
                "exact": True,
            }
        )
    rows.append(
        {
            "record": "physical-summary",
            "totals": {
                "tus": len(diagnostic.assignments),
                "c_to_f_bytes": len(diagnostic.assignments),
                "f_to_c_bytes": 0,
            },
        }
    )
    path.write_text("".join(json.dumps(row) + "\n" for row in rows))
    return path


class SuiteTest(unittest.TestCase):
    def test_physical_builder_launcher_forwards_binary_root_and_codec_options(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            scenario = root / "scenario.json"
            scenario.write_text("{}")
            binary = root / "grz2g"
            binary.write_bytes(b"binary")
            run_directory = root / "run"
            run_directory.mkdir()

            def fake_run(
                command: list[str], stdout_path: Path, stderr_path: Path
            ) -> None:
                self.assertIn(str(binary), command)
                self.assertIn("--prefix-stride", command)
                self.assertIn("17", command)
                self.assertIn("work=/payload", command)
                self.assertIn("--codec-option=--extra", command)
                Path(command[command.index("--out") + 1]).write_text("ledger\n")
                stdout_path.write_text("pass\n")
                stderr_path.write_text("")

            with mock.patch.object(
                suite, "checked_builder_run", side_effect=fake_run
            ) as checked:
                ledger, _ = suite.build_physical_ledger(
                    "grz",
                    scenario,
                    binary,
                    run_directory,
                    {"work": Path("/payload")},
                    ["--extra"],
                    17,
                )
            self.assertEqual(checked.call_count, 1)
            self.assertTrue(ledger.is_file())

    def test_c1f1_capacity_bandwidth_suite_shape(self) -> None:
        root = Path(__file__).parent
        suite_document, scenario_paths = suite.load_suite(
            root / "firefox-c1f1-capacity-bandwidth-suite.json"
        )
        self.assertEqual(suite_document["diagnostic_codecs"], ["compile-only", "raw"])
        self.assertEqual(len(scenario_paths), 4)
        observed = {}
        for path in scenario_paths:
            document = json.loads(path.read_text())
            observed[document["name"]] = (
                document["workers"]["template"]["slots"],
                document["network"]["c_to_f"][
                    "per_environment_bits_per_second"
                ],
                suite.sim.topology_label(document),
            )
            self.assertEqual(document["workers"]["f_count"], 1)
            self.assertEqual(
                document["workers"]["template"]["input_staging_slots"],
                document["workers"]["template"]["slots"],
            )
            job = document["environments"]["job_selection"]["jobs"][0]
            self.assertEqual(job["id"], "firefox-c0")
            self.assertEqual(job["builds"], 5)
            self.assertEqual(job["build_release"]["gap_ns"], 0)
        self.assertEqual(
            observed,
            {
                "firefox-corrected-C1F1_1000000B_BW1G": (
                    1_000_000, 1_000_000_000, "C1F1_1000000B1G"
                ),
                "firefox-corrected-C1F1_1000000B_BW10000G": (
                    1_000_000, 10_000_000_000_000, "C1F1_1000000B10T"
                ),
                "firefox-corrected-C1F1_10000B_BW1G": (
                    10_000, 1_000_000_000, "C1F1_10000B1G"
                ),
                "firefox-corrected-C1F1_10000B_BW10G": (
                    10_000, 10_000_000_000, "C1F1_10000B10G"
                ),
            },
        )

    def test_shared_uplink_f_width_suite_shape(self) -> None:
        root = Path(__file__).parent
        suite_document, scenario_paths = suite.load_suite(
            root / "firefox-f-width-suite.json"
        )
        self.assertEqual(
            suite_document["diagnostic_codecs"], ["compile-only", "raw"]
        )
        self.assertEqual(
            [json.loads(path.read_text())["workers"]["f_count"] for path in scenario_paths],
            [1, 2, 3, 4, 20],
        )
        self.assertEqual(
            [
                suite.sim.topology_label(json.loads(path.read_text()))
                for path in scenario_paths
            ],
            [
                "C1F1_200B1G",
                "C1F2_200B1G",
                "C1F3_200B1G",
                "C1F4_200B1G",
                "C1F20_200B1G",
            ],
        )
        for path in scenario_paths:
            document = json.loads(path.read_text())
            self.assertEqual(document["environments"]["env_count"], 1)
            self.assertEqual(document["workers"]["template"]["slots"], 200)
            self.assertEqual(
                document["workers"]["template"]["input_staging_slots"], 400
            )
            job = document["environments"]["job_selection"]["jobs"][0]
            self.assertEqual(job["builds"], 5)
            self.assertEqual(job["build_release"], {"mode": "after-previous", "gap_ns": 0})
            for direction in ("c_to_f", "f_to_c"):
                link = document["network"][direction]
                self.assertEqual(link["per_environment_bits_per_second"], 1_000_000_000)
                self.assertEqual(link["bits_per_second"], 40_000_000_000)
                self.assertEqual(link["per_worker_bits_per_second"], 40_000_000_000)
                self.assertEqual(link["fabric_bits_per_second"], 40_000_000_000)

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
            self.assertEqual({row["summed_capacity_floor_ns"] for row in matrix}, {10})
            self.assertEqual(
                {row["summed_generation_over_capacity_floor"] for row in matrix},
                {1.0},
            )
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

    def test_suite_replays_prebuilt_physical_codec_and_writes_phase_breakdown(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "trace.tsv").write_text(
                "logical\tjob_id\tii_relative\traw_bytes\tcompile_ns\tcompile_model\n"
                "0\tj0\tj0.ii\t1\t5\ttest-model\n"
            )
            (root / "j0.ii").write_bytes(b"x")
            scenario_path = root / "scenario.json"
            scenario_path.write_text(json.dumps(scenario_document("physical", 1, 2)))
            suite_path = root / "suite.json"
            suite_path.write_text(
                json.dumps(
                    {
                        "schema": "icecream-distribution-suite-v1",
                        "name": "physical-suite",
                        "scenarios": [scenario_path.name],
                        "diagnostic_codecs": ["compile-only"],
                    }
                )
            )
            ledger = write_physical_ledger(root / "p29.jsonl", scenario_path)
            output = root / "out"
            matrix, builds = suite.run_suite(
                suite_path,
                output,
                codecs=["p29"],
                physical_ledgers={"p29": ledger},
                require_payload=True,
            )
            self.assertEqual(len(matrix), 1)
            self.assertEqual(len(builds), 2)
            self.assertTrue(matrix[0]["physical_codec_result"])
            self.assertEqual(matrix[0]["c_to_f_bytes"], 2)
            self.assertEqual(matrix[0]["cold_c_to_f_bytes"], 1)
            self.assertEqual(matrix[0]["warm_c_to_f_bytes"], 1)
            phase_rows = (output / "physical-phases.tsv").read_text().splitlines()
            self.assertEqual(len(phase_rows), 3)
            self.assertTrue(all("p29-test" in row for row in phase_rows[1:]))
            self.assertIn("\tcold\t", phase_rows[1])
            self.assertIn("\twarm\t", phase_rows[2])
            self.assertTrue(
                (output / "physical" / "p29" / "report.html").is_file()
            )


if __name__ == "__main__":
    unittest.main()
