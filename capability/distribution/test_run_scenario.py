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
    for logical, size in enumerate(sizes):
        (root / f"j{logical}.ii").write_bytes(bytes([logical % 251]) * size)
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


def write_physical_ledger(path: Path, scenario_path: Path) -> Path:
    rows = [
        {
            "record": "physical-ledger",
            "schema": "icecream-physical-codec-ledger-v1",
            "codec": "p29",
            "scenario_sha256": sim.sha256(scenario_path),
            "reconstruction": {
                "status": "pass",
                "method": "test byte comparison",
            },
        },
        {
            "record": "tu",
            "workload": "test",
            "build": 0,
            "logical": 0,
            "worker": 0,
            "tu_seq": 0,
            "rel_seq": 0,
            "route_sequence": 0,
            "raw_bytes": 10,
            "raw_sha256": sim.sha256(scenario_path.parent / "j0.ii"),
            "phases": [
                {"name": "dict", "direction": "c_to_f", "bytes": 10},
                {"name": "need", "direction": "f_to_c", "bytes": 2},
                {"name": "fill", "direction": "c_to_f", "bytes": 3},
            ],
            "state_after": {"known_objects": 4},
            "exact": True,
        },
        {
            "record": "tu",
            "workload": "test",
            "build": 0,
            "logical": 1,
            "worker": 0,
            "tu_seq": 1,
            "rel_seq": 1,
            "route_sequence": 1,
            "raw_bytes": 11,
            "raw_sha256": sim.sha256(scenario_path.parent / "j1.ii"),
            "phases": [
                {"name": "dict", "direction": "c_to_f", "bytes": 5},
                {"name": "need", "direction": "f_to_c", "bytes": 1},
                {"name": "fill", "direction": "c_to_f", "bytes": 1},
            ],
            "state_after": {"known_objects": 5},
            "exact": True,
        },
        {
            "record": "physical-summary",
            "totals": {"tus": 2, "c_to_f_bytes": 19, "f_to_c_bytes": 3},
        },
    ]
    path.write_text("".join(json.dumps(row) + "\n" for row in rows))
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

    def test_raw_protocol_uses_one_ordered_dialogue_per_f_relationship(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = write_fixture(root, [3_000_000_000, 1], [100, 100], workers=1)
            document = json.loads(path.read_text())
            document["workers"]["template"]["slots"] = 2
            path.write_text(json.dumps(document))
            result = sim.Simulator(sim.load_scenario(path), sim.RawAdapter()).run()
            self.assertEqual(
                [(row["tu_seq"], row["rel_seq"]) for row in result.assignments],
                [(0, 0), (1, 1)],
            )
            self.assertEqual(
                [row["transfer_done_ns"] for row in result.assignments],
                [1_000_000_000, 2_000_000_000],
            )
            # TU1 transfers while TU0 is still compiling; only the cache dialogue is ordered.
            self.assertEqual(result.assignments[0]["compile_finish_ns"], 4_000_000_000)
            self.assertEqual(result.assignments[1]["compile_start_ns"], 2_000_000_000)
            self.assertEqual(result.summary["makespan_ns"], 4_000_000_000)
            self.assertEqual(
                result.generations[0]["capacity_c_to_f_floor_ns"],
                2_000_000_000,
            )
            self.assertEqual(
                result.generations[0]["capacity_compiler_floor_ns"],
                3_000_000_000,
            )
            self.assertEqual(
                result.generations[0]["capacity_overlap_floor_ns"],
                3_000_000_000,
            )
            self.assertAlmostEqual(
                result.generations[0]["duration_over_capacity_floor"], 4 / 3
            )

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

    def test_c_authority_bandwidth_is_shared_across_its_f_routes(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = write_fixture(root, [1, 1], [100, 100])
            document = json.loads(path.read_text())
            document["network"]["shared_fabric_bps"] = 1_600
            document["network"]["c_to_f"][
                "per_environment_bits_per_second"
            ] = 800
            path.write_text(json.dumps(document))
            result = sim.Simulator(sim.load_scenario(path), sim.RawAdapter()).run()
            self.assertEqual(
                [row["transfer_done_ns"] for row in result.assignments],
                [2_000_000_000, 2_000_000_000],
            )
            network_samples = [
                row
                for row in result.timeline
                if row["record"] == "snapshot" and row["metrics"]["routes"]
            ]
            self.assertEqual(
                network_samples[0]["metrics"]["environments"][0][
                    "c_to_f_average_bps"
                ],
                800.0,
            )
            self.assertEqual(
                network_samples[0]["metrics"]["environments"][0][
                    "c_to_f_utilization"
                ],
                1.0,
            )

    def test_f_bandwidth_is_shared_across_c_authorities(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = write_fixture(root, [1], [100], workers=1)
            document = json.loads(path.read_text())
            first = document["environments"]["job_selection"]["jobs"][0]
            second = json.loads(json.dumps(first))
            second["id"] = "test-c1"
            second["environment"] = 1
            document["environments"]["env_count"] = 2
            document["environments"]["job_selection"]["jobs"].append(second)
            document["workers"]["template"]["slots"] = 2
            document["network"]["shared_fabric_bps"] = 1_600
            document["network"]["c_to_f"]["per_worker_bits_per_second"] = 800
            path.write_text(json.dumps(document))
            result = sim.Simulator(sim.load_scenario(path), sim.RawAdapter()).run()
            self.assertEqual(
                [row["transfer_done_ns"] for row in result.assignments],
                [2_000_000_000, 2_000_000_000],
            )
            network_samples = [
                row
                for row in result.timeline
                if row["record"] == "snapshot" and row["metrics"]["routes"]
            ]
            self.assertEqual(
                network_samples[0]["metrics"]["workers"][0][
                    "c_to_f_average_bps"
                ],
                800.0,
            )
            self.assertEqual(
                network_samples[0]["metrics"]["workers"][0][
                    "c_to_f_utilization"
                ],
                1.0,
            )

    def test_directional_fabrics_allow_full_duplex_flows(self) -> None:
        class FullDuplex(sim.CodecAdapter):
            name = "full-duplex-test"

            def begin(self, item: sim.WorkItem, worker: int):
                raise AssertionError("DAG adapter must not enter the linear path")

            def transaction_plan(
                self, item: sim.WorkItem, worker: int
            ) -> sim.TransactionPlan | None:
                del item, worker
                return sim.TransactionPlan(
                    (
                        sim.DagNode("forward", "c_to_f", 100),
                        sim.DagNode("return", "f_to_c", 100),
                    ),
                    ("attachment:accepted",),
                    (
                        "attachment:accepted",
                        "forward:delivered",
                        "return:delivered",
                    ),
                    ("forward:delivered", "return:delivered"),
                )

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = write_fixture(root, [1], [1], workers=1)
            document = json.loads(path.read_text())
            del document["network"]["shared_fabric_bps"]
            document["network"]["c_to_f"]["fabric_bits_per_second"] = 800
            document["network"]["f_to_c"]["fabric_bits_per_second"] = 800
            path.write_text(json.dumps(document))
            result = sim.Simulator(sim.load_scenario(path), FullDuplex()).run()
            self.assertEqual(result.summary["makespan_ns"], 1_000_000_001)
            sample = next(row for row in result.timeline if row["record"] == "snapshot")
            self.assertIsNone(sample["metrics"]["fabric_capacity_bps"])
            self.assertIsNone(sample["metrics"]["fabric_utilization"])
            self.assertEqual(
                [row["average_bps"] for row in sample["metrics"]["direction_fabrics"]],
                [800.0, 800.0],
            )
            self.assertEqual(
                [row["utilization"] for row in sample["metrics"]["direction_fabrics"]],
                [1.0, 1.0],
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
            self.assertEqual(result.summary["summed_generation_ns"], 10)
            self.assertEqual(result.summary["wall_minus_summed_generation_ns"], 600)
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
            self.assertEqual(result.summary["summed_generation_ns"], 4)
            self.assertEqual(simulator.free_slots[0].next_unused, 3)
            self.assertEqual(simulator.free_slots[0].in_use, set())

    def test_input_staging_admits_work_without_reserving_compiler_slots(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = write_fixture(root, [10, 1, 1], [1, 1, 1], workers=1)
            document = json.loads(path.read_text())
            document["workers"]["template"]["input_staging_slots"] = 2
            path.write_text(json.dumps(document))
            result = sim.Simulator(
                sim.load_scenario(path), sim.CompileOnlyAdapter()
            ).run()
            self.assertEqual(
                [row["dispatch_ns"] for row in result.assignments], [0, 0, 0]
            )
            self.assertEqual(
                [row["compile_start_ns"] for row in result.assignments], [0, 10, 11]
            )
            self.assertEqual(
                [row["compiler_slot"] for row in result.assignments], [0, 0, 0]
            )
            self.assertEqual(result.summary["makespan_ns"], 12)
            self.assertEqual(result.summary["input_staging_slots_per_worker"], 2)
            self.assertEqual(
                result.summary["compiler_slot_assignment"], "at input readiness"
            )

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
                [
                    (row["environment"], row["tu_seq"], row["rel_seq"])
                    for row in result.assignments
                ],
                [(0, 0, 0), (1, 0, 0), (0, 1, 1), (1, 1, 1)],
            )
            self.assertEqual(
                [row["dispatch_ns"] for row in result.assignments],
                [0, 0, 605, 605],
            )
            self.assertEqual(result.summary["cold_builds"], 2)
            self.assertEqual(result.summary["warm_builds"], 2)
            self.assertEqual(result.summary["summed_generation_ns"], 10)
            self.assertEqual([row["duration_ns"] for row in result.generations], [5, 5])
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

    def test_dag_need_overlaps_lines_and_fill_preempts_at_writer_quantum(self) -> None:
        class ForkJoin(sim.CodecAdapter):
            name = "fork-join-test"

            def begin(self, item: sim.WorkItem, worker: int):
                raise AssertionError("DAG adapter must not enter the linear path")

            def transaction_plan(
                self, item: sim.WorkItem, worker: int
            ) -> sim.TransactionPlan | None:
                del worker
                if item.logical == 0:
                    return sim.TransactionPlan(
                        (
                            sim.DagNode("dict", "c_to_f", 100, (), 3),
                            sim.DagNode(
                                "need", "f_to_c", 100, ("dict:delivered",), 1
                            ),
                            sim.DagNode(
                                "fill", "c_to_f", 100, ("need:delivered",), 2
                            ),
                        ),
                        ("attachment:accepted",),
                        ("attachment:accepted", "fill:delivered"),
                        ("fill:delivered",),
                    )
                return sim.TransactionPlan(
                    (sim.DagNode("lines", "c_to_f", 1_000, (), 4),),
                    ("attachment:accepted",),
                    ("attachment:accepted", "lines:delivered"),
                    ("lines:delivered",),
                )

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = write_fixture(root, [1, 1], [1, 1], workers=1)
            document = json.loads(path.read_text())
            document["workers"]["template"]["slots"] = 2
            document["network"]["shared_fabric_bps"] = 1_600
            document["network"]["c_to_f"]["writer_quantum_bytes"] = 100
            document["network"]["f_to_c"]["writer_quantum_bytes"] = 100
            path.write_text(json.dumps(document))
            result = sim.Simulator(
                sim.load_scenario(path),
                ForkJoin(),
                snapshot_interval_ns=1_000_000_000,
            ).run()
            self.assertEqual(result.assignments[0]["transfer_done_ns"], 3_000_000_000)
            self.assertEqual(result.assignments[1]["transfer_done_ns"], 12_000_000_000)
            fill_start = next(
                row
                for row in result.events
                if row["event"] == "flow-start" and row["phase"] == "fill"
            )
            lines_resume = [
                row
                for row in result.events
                if row["event"] == "flow-resume" and row["phase"] == "lines"
            ]
            self.assertEqual(fill_start["time_ns"], 2_000_000_000)
            self.assertEqual(lines_resume[0]["time_ns"], 3_000_000_000)
            self.assertEqual(result.summary["c_to_f_bytes"], 1_200)
            self.assertEqual(result.summary["f_to_c_bytes"], 100)

    def test_priority_burst_bound_eventually_runs_the_oldest_flow(self) -> None:
        class PriorityBurst(sim.CodecAdapter):
            name = "priority-burst-test"

            def begin(self, item: sim.WorkItem, worker: int):
                del worker
                priority = 4 if item.logical == 0 else 0
                return (sim.Phase(f"flow-{item.logical}", "c_to_f", 300, priority),)

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = write_fixture(root, [1] * 5, [1] * 5, workers=1)
            document = json.loads(path.read_text())
            document["workers"]["template"]["slots"] = 5
            document["network"]["c_to_f"]["writer_quantum_bytes"] = 100
            document["network"]["c_to_f"]["max_priority_burst_quanta"] = 2
            path.write_text(json.dumps(document))
            result = sim.Simulator(
                sim.load_scenario(path),
                PriorityBurst(),
                snapshot_interval_ns=1_000_000_000,
            ).run()
            oldest_start = next(
                row
                for row in result.events
                if row["event"] == "flow-start" and row["phase"] == "flow-0"
            )
            self.assertEqual(oldest_start["time_ns"], 2_000_000_000)
            self.assertEqual(oldest_start["detail"], "bounded-priority-turn")

    def test_build_finishes_at_compile_and_transaction_join(self) -> None:
        class LateCommit(sim.CodecAdapter):
            name = "late-commit-test"

            def begin(self, item: sim.WorkItem, worker: int):
                raise AssertionError("DAG adapter must not enter the linear path")

            def transaction_plan(
                self, item: sim.WorkItem, worker: int
            ) -> sim.TransactionPlan | None:
                del item, worker
                return sim.TransactionPlan(
                    (sim.DagNode("ack-path", "c_to_f", 100),),
                    ("attachment:accepted",),
                    ("attachment:accepted",),
                    ("ack-path:delivered",),
                )

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = write_fixture(root, [1], [1], workers=1)
            result = sim.Simulator(sim.load_scenario(path), LateCommit()).run()
            assignment = result.assignments[0]
            self.assertEqual(assignment["compile_finish_ns"], 1)
            self.assertEqual(assignment["transaction_commit_ns"], 1_000_000_000)
            self.assertEqual(assignment["complete_ns"], 1_000_000_000)
            self.assertEqual(result.builds[0]["last_compile_finish_ns"], 1)
            self.assertEqual(
                result.builds[0]["last_transaction_commit_ns"], 1_000_000_000
            )
            self.assertEqual(result.builds[0]["finish_ns"], 1_000_000_000)
            self.assertEqual(result.generations[0]["stop_ns"], 1_000_000_000)
            self.assertEqual(result.summary["makespan_ns"], 1_000_000_000)

    def test_timeline_uses_ten_ms_active_samples_and_compresses_idle_gap(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = write_fixture(root, [25_000_000], [1], workers=1)
            document = json.loads(path.read_text())
            document["environments"]["job_selection"]["jobs"][0]["builds"] = 2
            document["environments"]["job_selection"]["jobs"][0]["build_release"][
                "gap_ns"
            ] = 600_000_000
            path.write_text(json.dumps(document))
            scenario = sim.load_scenario(path)
            spool = root / "timeline-spool.jsonl"
            event_spool = root / "event-spool.jsonl"
            result = sim.Simulator(
                scenario,
                sim.CompileOnlyAdapter(),
                timeline_spool_path=spool,
                event_spool_path=event_spool,
            ).run()
            self.assertEqual(result.timeline.records, [])
            self.assertEqual(result.events.records, [])
            self.assertTrue(spool.is_file())
            self.assertTrue(event_spool.is_file())
            snapshots = [x for x in result.timeline if x["record"] == "snapshot"]
            gaps = [x for x in result.timeline if x["record"] == "gap"]
            self.assertEqual(
                [x["active_duration_ns"] for x in snapshots],
                [10_000_000, 10_000_000, 5_000_000] * 2,
            )
            self.assertEqual(len(gaps), 1)
            self.assertEqual(gaps[0]["wall_duration_ns"], 600_000_000)
            self.assertEqual(result.summary["timeline_active_ns"], 50_000_000)
            self.assertEqual(snapshots[-1]["state"]["scheduler"]["completed_tus"], 2)

            output = root / "out"
            sim.write_result(scenario, result, output)
            self.assertFalse(spool.exists())
            self.assertFalse(event_spool.exists())
            rows = [json.loads(line) for line in (output / "experiment.jsonl").read_text().splitlines()]
            self.assertEqual(rows[0]["record"], "experiment")
            self.assertEqual(rows[0]["clock"]["snapshot_interval_ns"], 10_000_000)
            self.assertEqual(
                rows[0]["topology_dimensions"],
                {
                    "schema_semantics": (
                        "v1 maps one producer agent, one logical C authority, and one C "
                        "egress group to each environment"
                    ),
                    "producer_agents": 1,
                    "logical_c_authorities": 1,
                    "c_egress_groups": 1,
                    "f_stores": 1,
                    "compiler_slots_per_f": 1,
                    "input_staging_slots_per_f": 1,
                },
            )
            self.assertEqual(rows[-1]["record"], "summary")
            event_sequences = [
                event["sequence"]
                for row in rows[1:-1]
                for event in row["events"]
            ]
            self.assertEqual(event_sequences, list(range(len(result.events))))
            report = (output / "report.html").read_text()
            self.assertIn("Network serialization rate", report)
            self.assertIn("icecream-distribution-timeline-v1", report)

    def test_timeline_integrates_route_and_fabric_rate(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = write_fixture(root, [1], [100], workers=1)
            result = sim.Simulator(
                sim.load_scenario(path),
                sim.RawAdapter(),
                snapshot_interval_ns=500_000_000,
            ).run()
            network_samples = [
                row
                for row in result.timeline
                if row["record"] == "snapshot" and row["metrics"]["routes"]
            ]
            self.assertEqual(len(network_samples), 2)
            for row in network_samples:
                self.assertEqual(row["metrics"]["fabric_average_bps"], 800.0)
                self.assertEqual(row["metrics"]["routes"][0]["average_bps"], 800.0)
                self.assertEqual(row["metrics"]["routes"][0]["route_utilization"], 1.0)

    def test_report_compaction_keeps_boundaries_and_every_gap(self) -> None:
        timeline = [
            {"record": "snapshot", "sequence": sequence}
            for sequence in range(2_005)
        ]
        timeline.insert(100, {"record": "gap", "sequence": 10_000})
        view, metadata = sim.compact_report_timeline(timeline, limit=2_000)
        snapshots = [row for row in view if row["record"] == "snapshot"]
        gaps = [row for row in view if row["record"] == "gap"]
        self.assertEqual(len(snapshots), 2_000)
        self.assertEqual(snapshots[0]["sequence"], 0)
        self.assertEqual(snapshots[-1]["sequence"], 2_004)
        self.assertEqual(gaps, [{"record": "gap", "sequence": 10_000}])
        self.assertEqual(metadata["source_snapshots"], 2_005)
        self.assertEqual(metadata["embedded_snapshots"], 2_000)

    def test_physical_ledger_runs_in_common_engine_with_committed_route_order(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            scenario_path = write_fixture(root, [1, 1], [10, 11], workers=1)
            document = json.loads(scenario_path.read_text())
            document["workers"]["template"]["slots"] = 2
            scenario_path.write_text(json.dumps(document))
            scenario = sim.load_scenario(scenario_path)
            ledger_path = write_physical_ledger(root / "physical.jsonl", scenario_path)
            adapter = sim.PhysicalLedgerAdapter(ledger_path, scenario, "p29")
            result = sim.Simulator(scenario, adapter).run()
            self.assertTrue(result.summary["physical_codec_result"])
            self.assertEqual(result.summary["c_to_f_bytes"], 19)
            self.assertEqual(result.summary["f_to_c_bytes"], 3)
            starts = [
                row for row in result.events if row["event"] == "dialogue-start"
            ]
            finishes = [
                row for row in result.events if row["event"] == "dialogue-finish"
            ]
            self.assertEqual(len(starts), 2)
            self.assertEqual(starts[1]["time_ns"], finishes[0]["time_ns"])
            self.assertEqual(adapter.committed_by_route[(0, 0)], 2)
            self.assertEqual(adapter.last_route_state[(0, 0)]["known_objects"], 5)

    def test_multi_f_ledger_may_group_sparse_tu_seq_by_relationship(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            scenario_path = write_fixture(root, [1] * 4, [1] * 4, workers=2)
            document = json.loads(scenario_path.read_text())
            document["workers"]["template"]["slots"] = 2
            scenario_path.write_text(json.dumps(document))
            rows = [
                {
                    "record": "physical-ledger",
                    "schema": "icecream-physical-codec-ledger-v1",
                    "codec": "grz",
                    "scenario_sha256": sim.sha256(scenario_path),
                    "reconstruction": {"status": "pass", "method": "test"},
                }
            ]
            # Physical builders group rows by route. TU_SEQ is therefore sparse in each
            # group even though its C-global domain remains exactly [0, 1, 2, 3].
            for worker, logicals in ((0, (0, 2)), (1, (1, 3))):
                for rel_seq, logical in enumerate(logicals):
                    rows.append(
                        {
                            "record": "tu",
                            "workload": "test",
                            "build": 0,
                            "logical": logical,
                            "worker": worker,
                            "tu_seq": logical,
                            "rel_seq": rel_seq,
                            "route_sequence": rel_seq,
                            "raw_bytes": 1,
                            "raw_sha256": sim.sha256(root / f"j{logical}.ii"),
                            "phases": [
                                {
                                    "name": "body",
                                    "direction": "c_to_f",
                                    "bytes": 1,
                                }
                            ],
                            "state_after": {"rel_seq": rel_seq + 1},
                            "exact": True,
                        }
                    )
            rows.append(
                {
                    "record": "physical-summary",
                    "totals": {"tus": 4, "c_to_f_bytes": 4, "f_to_c_bytes": 0},
                }
            )
            ledger_path = root / "multi-f.jsonl"
            ledger_path.write_text("".join(json.dumps(row) + "\n" for row in rows))
            scenario = sim.load_scenario(scenario_path)
            result = sim.Simulator(
                scenario,
                sim.PhysicalLedgerAdapter(ledger_path, scenario, "grz"),
            ).run()
            self.assertEqual(
                [
                    (row["tu_seq"], row["worker"], row["rel_seq"])
                    for row in result.assignments
                ],
                [(0, 0, 0), (1, 1, 0), (2, 0, 1), (3, 1, 1)],
            )

    def test_physical_ledger_refuses_unverified_or_mismatched_input(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            scenario_path = write_fixture(root, [1, 1], [10, 11], workers=1)
            scenario = sim.load_scenario(scenario_path)
            ledger_path = write_physical_ledger(root / "physical.jsonl", scenario_path)
            rows = [json.loads(line) for line in ledger_path.read_text().splitlines()]
            rows[0]["reconstruction"]["status"] = "not-run"
            ledger_path.write_text("".join(json.dumps(row) + "\n" for row in rows))
            with self.assertRaisesRegex(ValueError, "reconstruction result"):
                sim.PhysicalLedgerAdapter(ledger_path, scenario, "p29")
            rows[0]["reconstruction"]["status"] = "pass"
            ledger_path.write_text("".join(json.dumps(row) + "\n" for row in rows))
            (root / "j0.ii").write_bytes(b"x" * 10)
            with self.assertRaisesRegex(ValueError, "raw content digest"):
                sim.PhysicalLedgerAdapter(ledger_path, scenario, "p29")


if __name__ == "__main__":
    unittest.main()
