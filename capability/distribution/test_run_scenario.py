#!/usr/bin/env python3
from __future__ import annotations

import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("run_scenario.py")
SPEC = importlib.util.spec_from_file_location("run_scenario", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
sim = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = sim
SPEC.loader.exec_module(sim)


def write_fixture(
    root: Path, durations: list[int], sizes: list[int], workers: int = 2
) -> Path:
    trace = root / "trace.tsv"
    trace.write_text(
        "logical\tjob_id\tii_relative\traw_bytes\tcompile_ns\tcompile_model\n"
        + "".join(
            f"{logical}\tj{logical}\tj{logical}.ii\t{size}\t{duration}\ttest-model\n"
            for logical, (duration, size) in enumerate(zip(durations, sizes))
        )
    )
    scenario = {
        "schema": "icecream-distribution-scenario-v1",
        "name": "test",
        "seed": 1,
        "environments": {
            "env_count": 1,
            "job_selection": {
                "mode": "explicit",
                "jobs": [
                    {
                        "id": "test",
                        "environment": 0,
                        "trace": "trace.tsv",
                        "corpus_root": str(root),
                        "builds": 1,
                        "start_ns": 0,
                        "build_release": {"mode": "after-previous"},
                        "tu_release": {"mode": "all-at-zero"},
                    }
                ],
            },
        },
        "workers": {
            "f_count": workers,
            "template": {
                "slots": 1,
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
    path = root / "scenario.json"
    path.write_text(json.dumps(scenario))
    return path


class SimulatorTest(unittest.TestCase):
    def test_round_robin_only_dispatches_to_free_slots(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = write_fixture(root, [4, 1, 1], [1, 1, 1])
            result = sim.Simulator(
                sim.load_scenario(path), sim.CompileOnlyAdapter()
            ).run()
            self.assertEqual([row["worker"] for row in result.assignments], [0, 1, 1])
            self.assertEqual(
                [row["dispatch_ns"] for row in result.assignments], [0, 0, 1]
            )
            self.assertEqual(result.summary["makespan_ns"], 4)

    def test_shared_fabric_is_divided_between_active_routes(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = write_fixture(root, [1, 1], [100, 100])
            result = sim.Simulator(sim.load_scenario(path), sim.RawAdapter()).run()
            self.assertEqual(result.summary["c_to_f_bytes"], 200)
            # Two 800-bit payloads share an 800-bit/s fabric, then compile for 1 ns.
            self.assertEqual(result.summary["makespan_ns"], 2_000_000_001)
            self.assertEqual(
                [row["transfer_done_ns"] for row in result.assignments],
                [2_000_000_000, 2_000_000_000],
            )

    def test_after_previous_build_is_a_completion_barrier(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = write_fixture(root, [5], [1], workers=1)
            document = json.loads(path.read_text())
            document["environments"]["job_selection"]["jobs"][0]["builds"] = 2
            document["environments"]["job_selection"]["jobs"][0]["build_release"][
                "gap_ns"
            ] = 600
            path.write_text(json.dumps(document))
            result = sim.Simulator(
                sim.load_scenario(path), sim.CompileOnlyAdapter()
            ).run()
            self.assertEqual(
                [row["dispatch_ns"] for row in result.assignments], [0, 605]
            )
            self.assertEqual(result.summary["makespan_ns"], 610)
            self.assertEqual(
                [row["gap_from_previous_finish_ns"] for row in result.builds],
                ["", 600],
            )

    def test_million_slot_worker_is_sparse(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = write_fixture(root, [4, 1, 1], [1, 1, 1], workers=1)
            document = json.loads(path.read_text())
            document["workers"]["template"]["slots"] = 1_000_000
            path.write_text(json.dumps(document))
            simulator = sim.Simulator(sim.load_scenario(path), sim.CompileOnlyAdapter())
            result = simulator.run()
            self.assertEqual([row["slot"] for row in result.assignments], [0, 1, 2])
            self.assertEqual(result.summary["total_worker_slots"], 1_000_000)
            self.assertEqual(simulator.free_slots[0].next_unused, 3)
            self.assertEqual(simulator.free_slots[0].in_use, set())

    def test_environments_advance_builds_independently_and_fairly(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = write_fixture(root, [5], [1], workers=2)
            document = json.loads(path.read_text())
            first = document["environments"]["job_selection"]["jobs"][0]
            first["builds"] = 2
            first["build_release"]["gap_ns"] = 600
            second = json.loads(json.dumps(first))
            second["id"] = "test-c1"
            second["environment"] = 1
            document["environments"]["env_count"] = 2
            document["environments"]["job_selection"]["jobs"].append(second)
            document["scheduler"]["ready_job_policy"] = "environment-round-robin"
            path.write_text(json.dumps(document))
            result = sim.Simulator(
                sim.load_scenario(path), sim.CompileOnlyAdapter()
            ).run()
            self.assertEqual(
                [(row["environment"], row["build"]) for row in result.assignments],
                [(0, 0), (1, 0), (0, 1), (1, 1)],
            )
            self.assertEqual(
                [row["dispatch_ns"] for row in result.assignments],
                [0, 0, 605, 605],
            )
            self.assertEqual(result.summary["cold_builds"], 2)
            self.assertEqual(result.summary["warm_builds"], 2)
            self.assertEqual(
                [row["gap_from_previous_finish_ns"] for row in result.builds],
                ["", 600, "", 600],
            )

    def test_dialogue_waits_for_each_arrival_and_scores_only_c_to_f(self) -> None:
        class ThreePhase(sim.CodecAdapter):
            name = "three-phase-test"

            def begin(self, item: sim.WorkItem, worker: int):
                del item, worker
                return (
                    sim.Phase("a", "c_to_f", 100),
                    sim.Phase("b", "f_to_c", 100),
                    sim.Phase("c", "c_to_f", 100),
                )

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = write_fixture(root, [1], [1], workers=1)
            document = json.loads(path.read_text())
            document["network"]["c_to_f"]["one_way_latency_ns"] = 10
            document["network"]["f_to_c"]["one_way_latency_ns"] = 10
            path.write_text(json.dumps(document))
            result = sim.Simulator(sim.load_scenario(path), ThreePhase()).run()
            self.assertEqual(result.summary["c_to_f_bytes"], 200)
            self.assertEqual(result.summary["f_to_c_bytes"], 100)
            self.assertEqual(result.summary["scored_outgoing_bytes"], 200)
            # Three serial one-second phases, three one-way delays, then 1 ns compile.
            self.assertEqual(result.summary["makespan_ns"], 3_000_000_031)


if __name__ == "__main__":
    unittest.main()
