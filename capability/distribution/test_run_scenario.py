#!/usr/bin/env python3
from __future__ import annotations

import copy
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


def write_v2_fixture(
    root: Path,
    durations: list[int],
    sizes: list[int],
    workers: int = 2,
    environment_state: str = "resident",
    assignment_source: str = "policy",
) -> Path:
    legacy_path = write_fixture(root, durations, sizes, workers)
    legacy = json.loads(legacy_path.read_text())
    (root / "trace.tsv").write_text(
        "logical\tjob_id\tii_relative\traw_bytes\tcompile_ns\tcompile_model\t"
        "raw_sha256\tcompile_provenance\n"
        + "".join(
            f"{logical}\tj{logical}\tj{logical}.ii\t{size}\t{duration}\ttest-model\t"
            f"{sim.sha256(root / f'j{logical}.ii')}\tmodeled\n"
            for logical, (duration, size) in enumerate(zip(durations, sizes))
        )
    )
    content_manifest = sim.workload_content_manifest(
        sim.read_trace(root / "trace.tsv", require_content_identity=True)
    )
    document = {
        "schema": "icecream-experiment-v2",
        "name": "v2-test",
        "seed": 7,
        "components": {
            name: {"release": "1.5.90", "main_protocol": 50, "commit": "1" * 40}
            for name in ("scheduler", "wrapper", "c_daemon", "f_daemon")
        },
        "capabilities": {
            "cache_wire": "v1",
            "codec_profile": "legacy",
            "experiment_control": "raw",
        },
        "topology": {
            "c_count": 1,
            "f_count": workers,
            "f_slots": 2,
            "input_staging_slots": 4,
            "bandwidth": legacy["network"],
            "scheduler_policy": "round_robin",
            "assignment_source": assignment_source,
        },
        "workload": {
            "release_policy": "all_at_once",
            "jobs": [
                {
                    "id": "test",
                    "c_index": 0,
                    "trace": "trace.tsv",
                    "corpus_root": ".",
                    "manifest_digest": sim.canonical_json_sha256(content_manifest),
                    "compile_profile": "test-model",
                    "build_epochs": 1,
                    "build_release": {"mode": "after-previous"},
                }
            ],
        },
        "environment": {
            "initial_state": environment_state,
            "image_digest": "2" * 64,
            "transfer_bytes": 100,
            "install_verify_ns": 5,
        },
        "cache": {
            "initial_state": "cold",
            "capacity_bytes": 1_000_000,
            "lifecycle_events": [],
        },
        "expected": {
            "selected_main_protocol": 50,
            "selected_cache_wire": "v1",
            "selected_codec_profile": "legacy",
            "compile_result": "pass",
        },
    }
    path = root / "experiment-v2.json"
    path.write_text(json.dumps(document, indent=2) + "\n")
    return path


def execution_for(path: Path) -> dict[str, object]:
    scenario = json.loads(path.read_text())
    return {
        "schema": "icecream-execution-v2",
        "scenario_digest": sim.canonical_json_sha256(scenario),
        "mode": "simulated",
        "runner_commit": "3" * 40,
        "host_manifest": "4" * 64,
        "started_at": "2026-08-22T00:00:00Z",
        "source_commit": "5" * 40,
        "simulator_commit": "6" * 40,
        "codec_executable_digest": "7" * 64,
        "input_manifest_digests": [scenario["workload"]["jobs"][0]["manifest_digest"]],
        "image_identifiers": ["test-image@sha256:" + "2" * 64],
        "compiler_versions": ["test-compiler 1"],
        "library_versions": [],
        "host_identities": ["test-host"],
    }


def add_route_trace(path: Path, workers: list[int], sizes: list[int]) -> Path:
    document = json.loads(path.read_text())
    document["topology"]["assignment_source"] = "route_trace"
    document["topology"]["route_trace"] = "input-route-trace.jsonl"
    path.write_text(json.dumps(document, indent=2) + "\n")
    digest = sim.canonical_json_sha256(document)
    rel_next: dict[int, int] = {}
    rows = [
        {
            "record": "route_trace",
            "schema": "icecream-route-trace-v1",
            "scenario_digest": digest,
            "codec_profile": "raw",
            "provenance": "modeled",
        }
    ]
    for logical, (worker, size) in enumerate(zip(workers, sizes)):
        item = sim.WorkItem(
            logical,
            0,
            "test",
            0,
            logical,
            f"j{logical}",
            path.parent / f"j{logical}.ii",
            size,
            1,
            0,
            sim.sha256(path.parent / f"j{logical}.ii"),
        )
        logical_id = sim.logical_job_identity(digest, item)
        rel_seq = rel_next.get(worker, 0)
        rel_next[worker] = rel_seq + 1
        rows.append(
            {
                "record": "assignment",
                "logical_job_id": logical_id,
                "attempt_id": f"{logical_id}:attempt-0",
                "workload": "test",
                "build": 0,
                "logical": logical,
                "worker": worker,
                "C_STORE_GUID": sim.stable_hex("trace-c", digits=32),
                "physical_endpoint": f"physical-F{worker}",
                "RouteLaneId": f"lane-C0-F{worker}",
                "F_STORE_GUID": sim.stable_hex("trace-f", worker, digits=32),
                "session_serial": 9,
                "HISTORY_NONCE": 11,
                "TU_SEQ": logical,
                "REL_SEQ": rel_seq,
                "c_to_f_bytes": size,
                "f_to_c_bytes": 0,
            }
        )
    rows.append(
        {
            "record": "route_summary",
            "assignments": len(workers),
            "c_to_f_bytes": sum(sizes),
            "f_to_c_bytes": 0,
        }
    )
    route_path = path.parent / "input-route-trace.jsonl"
    route_path.write_text("".join(json.dumps(row) + "\n" for row in rows))
    return route_path


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

    def test_static_rendezvous_reuses_tu_destinations_inside_dense_frontier(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            jobs = 20
            path = write_fixture(
                root,
                [1 + (logical % 4) for logical in range(jobs)],
                [1] * jobs,
                workers=5,
            )
            document = json.loads(path.read_text())
            document["environments"]["job_selection"]["jobs"][0]["builds"] = 2
            document["scheduler"]["placement_policy"] = "rendezvous"
            document["scheduler"]["dense_frontier_workers"] = 2
            path.write_text(json.dumps(document))

            result = sim.Simulator(
                sim.load_scenario(path), sim.CompileOnlyAdapter()
            ).run()
            first = {
                int(row["logical"]): int(row["worker"])
                for row in result.assignments
                if row["build"] == 0
            }
            second = {
                int(row["logical"]): int(row["worker"])
                for row in result.assignments
                if row["build"] == 1
            }
            self.assertEqual(first, second)
            routing = result.summary["routing"]
            homes = set(routing["home_sets"][0]["workers"])
            self.assertEqual(len(homes), 2)
            self.assertLessEqual(set(first.values()), homes)
            self.assertEqual(routing["algorithm"], "stable-rendezvous-v1")
            self.assertEqual(routing["seed"], 1)
            self.assertEqual(routing["binding"], "release-time-static")
            self.assertFalse(routing["build_number_in_identity"])
            by_identity = {
                (int(row["build"]), int(row["logical"])): int(row["tu_seq"])
                for row in result.assignments
            }
            self.assertEqual(
                by_identity,
                {
                    (build, logical): build * jobs + logical
                    for build in range(2)
                    for logical in range(jobs)
                },
            )
            route_events = [
                row for row in result.events if row["event"] == "route-bound"
            ]
            self.assertEqual(len(route_events), jobs * 2)
            self.assertEqual(
                {
                    (int(row["build"]), int(row["logical"])): int(row["worker"])
                    for row in route_events
                },
                {
                    (int(row["build"]), int(row["logical"])): int(row["worker"])
                    for row in result.assignments
                },
            )
            for worker in homes:
                rel_seq = [
                    int(row["rel_seq"])
                    for row in result.assignments
                    if row["worker"] == worker
                ]
                self.assertEqual(rel_seq, list(range(len(rel_seq))))

    def test_tu_seq_is_preparation_order_not_static_route_dispatch_order(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = write_fixture(root, [10, 1, 5], [1, 1, 1], workers=1)
            document = json.loads(path.read_text())
            document["scheduler"]["ready_job_policy"] = "shortest-known"
            document["scheduler"]["placement_policy"] = "rendezvous"
            document["scheduler"]["dense_frontier_workers"] = 1
            path.write_text(json.dumps(document))

            result = sim.Simulator(
                sim.load_scenario(path), sim.CompileOnlyAdapter()
            ).run()
            self.assertEqual(
                [
                    (row["logical"], row["tu_seq"], row["rel_seq"])
                    for row in result.assignments
                ],
                [(1, 1, 0), (2, 2, 1), (0, 0, 2)],
            )
            self.assertEqual(
                result.summary["relationship_ordering"],
                "TU_SEQ is allocated in prepared-input release order per C; REL_SEQ is "
                "allocated independently in route-local dispatch order",
            )

    def test_dense_frontier_validation_is_explicit(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = write_fixture(root, [1], [1], workers=2)
            document = json.loads(path.read_text())
            document["scheduler"]["dense_frontier_workers"] = 1
            path.write_text(json.dumps(document))
            with self.assertRaisesRegex(ValueError, "requires rendezvous"):
                sim.load_scenario(path)
            document["scheduler"]["placement_policy"] = "rendezvous"
            document["scheduler"]["dense_frontier_workers"] = 3
            path.write_text(json.dumps(document))
            with self.assertRaisesRegex(ValueError, "exceeds workers.f_count"):
                sim.load_scenario(path)

    def test_static_routing_seed_is_a_validated_nonnegative_integer(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = write_fixture(root, [1], [1], workers=2)
            document = json.loads(path.read_text())
            document["scheduler"]["placement_policy"] = "rendezvous"
            document["seed"] = "1"
            path.write_text(json.dumps(document))
            with self.assertRaisesRegex(
                ValueError, "seed must be a non-negative integer"
            ):
                sim.load_scenario(path)

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
            document["network"]["c_to_f"]["per_environment_bits_per_second"] = 800
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
                network_samples[0]["metrics"]["environments"][0]["c_to_f_average_bps"],
                800.0,
            )
            self.assertEqual(
                network_samples[0]["metrics"]["environments"][0]["c_to_f_utilization"],
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
                network_samples[0]["metrics"]["workers"][0]["c_to_f_average_bps"],
                800.0,
            )
            self.assertEqual(
                network_samples[0]["metrics"]["workers"][0]["c_to_f_utilization"],
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
                            sim.DagNode("need", "f_to_c", 100, ("dict:delivered",), 1),
                            sim.DagNode("fill", "c_to_f", 100, ("need:delivered",), 2),
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
            rows = [
                json.loads(line)
                for line in (output / "experiment.jsonl").read_text().splitlines()
            ]
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
                event["sequence"] for row in rows[1:-1] for event in row["events"]
            ]
            self.assertEqual(event_sequences, list(range(len(result.events))))
            report = (output / "report.html").read_text()
            self.assertIn("Network serialization rate", report)
            self.assertIn("icecream-distribution-timeline-v1", report)

    def test_fractional_timeline_intervals_tile_integer_active_time(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = write_fixture(root, [1], [1], workers=1)
            document = json.loads(path.read_text())
            job = document["environments"]["job_selection"]["jobs"][0]
            job["builds"] = 2
            job["build_release"]["gap_ns"] = 1
            for direction in ("c_to_f", "f_to_c"):
                document["network"][direction]["bits_per_second"] = 6
            document["network"]["shared_fabric_bps"] = 6
            path.write_text(json.dumps(document))

            result = sim.Simulator(
                sim.load_scenario(path),
                sim.RawAdapter(),
            ).run()
            snapshots = [x for x in result.timeline if x["record"] == "snapshot"]

            self.assertEqual(
                sum(row["active_duration_ns"] for row in snapshots),
                result.summary["timeline_active_ns"],
            )
            self.assertTrue(
                all(
                    row["active_duration_ns"]
                    == row["active_end_ns"] - row["active_start_ns"]
                    for row in snapshots
                )
            )

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
            {"record": "snapshot", "sequence": sequence} for sequence in range(2_005)
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

    def test_physical_ledger_runs_in_common_engine_with_committed_route_order(
        self,
    ) -> None:
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
            starts = [row for row in result.events if row["event"] == "dialogue-start"]
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

    def test_compatible_ledger_reuse_requires_exact_inputs_and_runtime_order(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source_path = write_fixture(root, [1, 1], [10, 11], workers=1)
            source_document = json.loads(source_path.read_text())
            source_document["workers"]["template"]["slots"] = 2
            source_path.write_text(json.dumps(source_document))
            ledger_path = write_physical_ledger(root / "physical.jsonl", source_path)

            timing_document = json.loads(source_path.read_text())
            timing_document["name"] = "test-faster-link"
            timing_document["network"]["c_to_f"]["bits_per_second"] = 1_600
            timing_path = root / "timing-variant.json"
            timing_path.write_text(json.dumps(timing_document))
            timing_scenario = sim.load_scenario(timing_path)
            with self.assertRaisesRegex(ValueError, "different scenario"):
                sim.PhysicalLedgerAdapter(ledger_path, timing_scenario, "p29")

            digest_cache = {}
            adapter = sim.PhysicalLedgerAdapter(
                ledger_path,
                timing_scenario,
                "p29",
                allow_compatible_scenario=True,
                payload_digest_cache=digest_cache,
            )
            self.assertEqual(len(digest_cache), 2)
            result = sim.Simulator(timing_scenario, adapter).run()
            self.assertEqual(result.summary["c_to_f_bytes"], 19)
            self.assertEqual(
                result.summary["codec_metadata"]["scenario_binding"],
                "compatible-inputs-and-runtime-route-order",
            )
            (root / "j0.ii").write_bytes(b"q" * 10)
            with self.assertRaisesRegex(ValueError, "raw content digest"):
                sim.PhysicalLedgerAdapter(
                    ledger_path,
                    timing_scenario,
                    "p29",
                    allow_compatible_scenario=True,
                    payload_digest_cache=digest_cache,
                )

    def test_v2_resident_run_has_separate_execution_header_and_exact_closure(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = write_v2_fixture(root, [3, 2], [10, 11], workers=2)
            scenario = sim.load_scenario(path)
            self.assertTrue(scenario.is_v2)
            self.assertEqual(
                scenario.scenario_digest,
                sim.canonical_json_sha256(json.loads(path.read_text())),
            )
            result = sim.Simulator(scenario, sim.RawAdapter()).run()
            self.assertEqual(result.summary["scored_outgoing_bytes"], 21)
            self.assertEqual(result.summary["environment_c_to_f_bytes"], 0)
            self.assertEqual(result.summary["network_c_to_f_bytes"], 21)
            self.assertEqual(result.summary["exact_byte_ledger"]["closure"], "pass")
            self.assertEqual(
                result.summary["replay_closure"]["semantics"],
                "aggregate-directional",
            )
            self.assertEqual({row["release_ns"] for row in result.assignments}, {0})
            for event in result.events:
                self.assertFalse(set(sim.EVENT_IDENTITY_FIELDS) - set(event))

            execution = execution_for(path)
            output = root / "out"
            sim.write_result(scenario, result, output, execution)
            rows = [
                json.loads(line)
                for line in (output / "experiment.jsonl").read_text().splitlines()
            ]
            self.assertEqual(rows[0]["record"], "execution")
            self.assertEqual(rows[0]["mode"], "simulated")
            self.assertNotIn("mode", rows[0]["scenario_manifest"])
            self.assertEqual(
                sim.validate_experiment_jsonl(output / "experiment.jsonl"),
                {"events": result.events.count, "c_to_f_bytes": 21, "f_to_c_bytes": 0},
            )
            self.assertTrue((output / "route-trace.jsonl").is_file())

    def test_v2_absent_environment_is_single_flight_and_not_in_source_score(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = write_v2_fixture(
                root, [1, 1], [1, 1], workers=1, environment_state="absent"
            )
            document = json.loads(path.read_text())
            document["topology"]["bandwidth"]["c_to_f"]["lanes_per_endpoint"] = 2
            document["topology"]["bandwidth"]["shared_fabric_bps"] = 1_600
            path.write_text(json.dumps(document, indent=2) + "\n")
            result = sim.Simulator(sim.load_scenario(path), sim.RawAdapter()).run()
            events = list(result.events)
            self.assertEqual(
                sum(
                    event["event"] == "env_transfer" and event["transition"] == "start"
                    for event in events
                ),
                1,
            )
            ready = [event for event in events if event["event"] == "environment_ready"]
            self.assertEqual(len(ready), 1)
            self.assertTrue(
                all(
                    row["compile_start_ns"] >= ready[0]["time_ns"]
                    for row in result.assignments
                )
            )
            self.assertEqual(result.summary["scored_outgoing_bytes"], 2)
            self.assertEqual(result.summary["environment_c_to_f_bytes"], 100)
            self.assertEqual(result.summary["network_c_to_f_bytes"], 102)
            accounts = result.summary["exact_byte_ledger"]["directions"]["c_to_f"][
                "accounts"
            ]
            self.assertEqual(accounts, {"environment": 100, "source": 2})

    def test_v2_route_trace_replay_closes_every_assignment_and_route_byte(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = write_v2_fixture(root, [1, 1, 1], [2, 3, 4], workers=2)
            add_route_trace(path, [1, 0, 1], [2, 3, 4])
            scenario = sim.load_scenario(path)
            result = sim.Simulator(scenario, sim.RawAdapter()).run()
            self.assertEqual(
                [
                    row["worker"]
                    for row in sorted(
                        result.assignments, key=lambda row: row["logical"]
                    )
                ],
                [1, 0, 1],
            )
            closure = result.summary["replay_closure"]
            self.assertEqual(closure["status"], "pass")
            self.assertEqual(closure["semantics"], "exact-route")
            self.assertEqual(sum(row["tus"] for row in closure["routes"]), 3)

            route_rows = [
                json.loads(line)
                for line in scenario.route_trace_path.read_text().splitlines()
            ]
            route_rows[1]["c_to_f_bytes"] += 1
            route_rows[-1]["c_to_f_bytes"] += 1
            scenario.route_trace_path.write_text(
                "".join(json.dumps(row) + "\n" for row in route_rows)
            )
            changed = sim.load_scenario(path)
            with self.assertRaisesRegex(RuntimeError, "route-trace closure differs"):
                sim.Simulator(changed, sim.RawAdapter()).run()

    def test_policy_only_physical_replay_requires_aggregate_not_route_closure(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = write_v2_fixture(root, [1, 1], [10, 11], workers=2)
            scenario = sim.load_scenario(path)
            ledger_path = write_physical_ledger(root / "physical.jsonl", path)
            rows = [json.loads(line) for line in ledger_path.read_text().splitlines()]
            rows[0]["scenario_sha256"] = scenario.scenario_digest
            ledger_path.write_text("".join(json.dumps(row) + "\n" for row in rows))
            adapter = sim.PhysicalLedgerAdapter(
                ledger_path, scenario, "p29", assignment_closure="aggregate"
            )
            result = sim.Simulator(scenario, adapter).run()
            self.assertEqual([row["worker"] for row in result.assignments], [0, 1])
            self.assertEqual(result.summary["c_to_f_bytes"], 19)
            self.assertEqual(result.summary["f_to_c_bytes"], 3)
            self.assertEqual(
                result.summary["replay_closure"]["semantics"],
                "aggregate-directional",
            )

    def test_v2_schema_keeps_future_z3_profiles_without_implementing_them(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = write_v2_fixture(root, [1], [1], workers=1)
            for profile in ("z3_long", "z3_shared_long"):
                document = json.loads(path.read_text())
                document["capabilities"].pop("experiment_control", None)
                document["capabilities"]["codec_profile"] = profile
                document["expected"]["selected_codec_profile"] = profile
                sim.validate_json_schema(
                    document, "experiment.schema.json", f"profile-{profile}"
                )
            document["capabilities"]["experiment_control"] = "z3_shared_long_b1"
            sim.validate_json_schema(document, "experiment.schema.json", "b1-control")
            document["mode"] = "simulated"
            with self.assertRaisesRegex(ValueError, "schema error"):
                sim.validate_json_schema(document, "experiment.schema.json", "bad-mode")

    def test_v2_schema_rejects_obsolete_stream_labels(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = write_v2_fixture(root, [1], [1], workers=1)
            for profile in ("stream_a", "stream_b"):
                document = json.loads(path.read_text())
                document["capabilities"].pop("experiment_control", None)
                document["capabilities"]["codec_profile"] = profile
                document["expected"]["selected_codec_profile"] = profile
                with self.subTest(profile=profile), self.assertRaisesRegex(
                    ValueError, "schema error"
                ):
                    sim.validate_json_schema(
                        document, "experiment.schema.json", f"obsolete-{profile}"
                    )

    def test_v2_content_manifest_and_execution_bind_actual_payloads(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = write_v2_fixture(root, [1, 1], [4, 4], workers=1)
            scenario = sim.load_scenario(path)
            execution = execution_for(path)
            execution["input_manifest_digests"] = ["f" * 64]
            execution_path = root / "execution.json"
            execution_path.write_text(json.dumps(execution))
            with self.assertRaisesRegex(ValueError, "input_manifest_digests"):
                sim.load_execution(execution_path, scenario)

            original = (root / "j0.ii").read_bytes()
            (root / "j0.ii").write_bytes(b"z" * len(original))
            with self.assertRaisesRegex(ValueError, "payload digest"):
                sim.load_scenario(path)

            trace_rows = (root / "trace.tsv").read_text().splitlines()
            fields = trace_rows[1].split("\t")
            fields[-2] = sim.sha256(root / "j0.ii")
            trace_rows[1] = "\t".join(fields)
            (root / "trace.tsv").write_text("\n".join(trace_rows) + "\n")
            with self.assertRaisesRegex(ValueError, "manifest_digest"):
                sim.load_scenario(path)

            fields[-2] = ""
            trace_rows[1] = "\t".join(fields)
            (root / "trace.tsv").write_text("\n".join(trace_rows) + "\n")
            with self.assertRaisesRegex(ValueError, "lacks raw_sha256"):
                sim.load_scenario(path)

    def test_v2_expected_selection_and_compile_outcome_are_reconciled(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = write_v2_fixture(root, [1], [1], workers=1)
            document = json.loads(path.read_text())
            document["expected"]["selected_main_protocol"] = 43
            path.write_text(json.dumps(document))
            with self.assertRaisesRegex(ValueError, "selected_main_protocol"):
                sim.load_scenario(path)
            document["expected"]["selected_main_protocol"] = 50
            document["expected"]["compile_result"] = "definitive-failure"
            path.write_text(json.dumps(document))
            with self.assertRaisesRegex(ValueError, "schema error"):
                sim.load_scenario(path)

    def test_route_trace_binds_codec_and_stable_route_identity(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = write_v2_fixture(root, [1, 1], [2, 3], workers=1)
            route_path = add_route_trace(path, [0, 0], [2, 3])
            rows = [json.loads(line) for line in route_path.read_text().splitlines()]
            rows[0]["codec_profile"] = "p29"
            route_path.write_text("".join(json.dumps(row) + "\n" for row in rows))
            with self.assertRaisesRegex(ValueError, "route trace codec"):
                sim.load_scenario(path)

            rows[0]["codec_profile"] = "raw"
            rows[2]["session_serial"] += 1
            route_path.write_text("".join(json.dumps(row) + "\n" for row in rows))
            with self.assertRaisesRegex(ValueError, "route identity drifted"):
                sim.load_scenario(path)

    def test_transaction_free_route_event_uses_trace_session_nonce_and_rel(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = write_v2_fixture(root, [1, 1], [2, 3], workers=1)
            add_route_trace(path, [0, 0], [2, 3])
            scenario = sim.load_scenario(path)
            result = sim.Simulator(scenario, sim.RawAdapter()).run()
            for event in result.events:
                if event["event"] != "route-bound":
                    continue
                entry = scenario.route_trace[event["logical"]]
                self.assertEqual(event["session_serial"], entry.session_serial)
                self.assertEqual(event["HISTORY_NONCE"], entry.history_nonce)
                self.assertEqual(event["REL_SEQ"], entry.rel_seq)
                self.assertEqual(event["RouteLaneId"], entry.route_lane_id)

    def test_exact_route_replay_includes_environment_but_not_source_score(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = write_v2_fixture(
                root, [1, 1], [1, 1], workers=1, environment_state="absent"
            )
            document = json.loads(path.read_text())
            document["topology"]["bandwidth"]["c_to_f"]["lanes_per_endpoint"] = 2
            document["topology"]["bandwidth"]["shared_fabric_bps"] = 1_600
            path.write_text(json.dumps(document, indent=2) + "\n")
            add_route_trace(path, [0, 0], [1, 1])
            scenario = sim.load_scenario(path)
            result = sim.Simulator(scenario, sim.RawAdapter()).run()
            route = result.summary["replay_closure"]["routes"][0]
            self.assertEqual(route["accounts"]["source"]["c_to_f_bytes"], 2)
            self.assertEqual(route["accounts"]["environment"]["c_to_f_bytes"], 100)
            self.assertEqual(route["c_to_f_bytes"], 102)
            self.assertEqual(result.summary["scored_outgoing_bytes"], 2)
            self.assertEqual(result.summary["network_c_to_f_bytes"], 102)
            output = root / "out"
            sim.write_result(scenario, result, output, execution_for(path))
            self.assertEqual(
                sim.validate_experiment_jsonl(output / "experiment.jsonl"),
                {"events": result.events.count, "c_to_f_bytes": 102, "f_to_c_bytes": 0},
            )

    def test_physical_byte_provenance_keeps_simulated_timing_separate(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = write_v2_fixture(root, [1, 1], [10, 11], workers=1)
            document = json.loads(path.read_text())
            document["capabilities"] = {
                "cache_wire": "v1",
                "codec_profile": "p29",
            }
            document["expected"]["selected_codec_profile"] = "p29"
            path.write_text(json.dumps(document, indent=2) + "\n")
            scenario = sim.load_scenario(path)
            ledger_path = write_physical_ledger(root / "physical.jsonl", path)
            ledger_rows = [
                json.loads(line) for line in ledger_path.read_text().splitlines()
            ]
            ledger_rows[0]["scenario_sha256"] = scenario.scenario_digest
            ledger_path.write_text(
                "".join(json.dumps(row) + "\n" for row in ledger_rows)
            )
            result = sim.Simulator(
                scenario,
                sim.PhysicalLedgerAdapter(
                    ledger_path, scenario, "p29", assignment_closure="aggregate"
                ),
            ).run()
            byte_events = [
                event
                for event in result.events
                if event["c_to_f_byte_delta"] or event["f_to_c_byte_delta"]
            ]
            self.assertTrue(byte_events)
            self.assertTrue(
                all(
                    event["provenance"]["byte_delta"] == "observed"
                    for event in byte_events
                )
            )
            self.assertTrue(
                all(event["provenance"]["timing"] == "modeled" for event in byte_events)
            )

    def test_retained_stream_validator_rejects_each_independent_drift(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = write_v2_fixture(root, [3, 2], [10, 11], workers=2)
            add_route_trace(path, [1, 0], [10, 11])
            scenario = sim.load_scenario(path)
            result = sim.Simulator(scenario, sim.RawAdapter()).run()
            output = root / "out"
            sim.write_result(scenario, result, output, execution_for(path))
            original = [
                json.loads(line)
                for line in (output / "experiment.jsonl").read_text().splitlines()
            ]

            def reject(
                label: str,
                mutate: object,
                pattern: str,
                *,
                synchronize_summary: bool = False,
            ) -> None:
                candidate = copy.deepcopy(original)
                mutate(candidate)
                if synchronize_summary:
                    candidate[0]["expected_summary"] = copy.deepcopy(
                        candidate[-1]["summary"]
                    )
                candidate_path = output / f"bad-{label}.jsonl"
                candidate_path.write_text(
                    "".join(json.dumps(row) + "\n" for row in candidate)
                )
                with self.subTest(label=label), self.assertRaisesRegex(
                    ValueError, pattern
                ):
                    sim.validate_experiment_jsonl(candidate_path)

            reject(
                "manifest",
                lambda rows: rows[0]["scenario_manifest"].__setitem__(
                    "name", "changed"
                ),
                "embedded scenario manifest digest",
            )
            reject(
                "scenario-digest",
                lambda rows: rows[0].__setitem__("scenario_digest", "f" * 64),
                "embedded scenario manifest digest",
            )
            reject(
                "input-digests",
                lambda rows: rows[0].__setitem__("input_manifest_digests", ["f" * 64]),
                "input manifest digests",
            )

            def mutate_provenance(rows: list[dict[str, object]]) -> None:
                rows[1]["events"][0]["provenance"]["timing"] = "invalid"

            reject("provenance", mutate_provenance, "schema error")

            def reroute_byte(rows: list[dict[str, object]]) -> None:
                for timeline in rows[1:-1]:
                    for event in timeline["events"]:
                        if event["c_to_f_byte_delta"]:
                            event["worker"] = 1 - event["worker"]
                            return

            reject("direction-route", reroute_byte, "route identity|per-route")

            def drift_resource(rows: list[dict[str, object]]) -> None:
                rows[-1]["summary"]["exact_byte_ledger"]["resources"][0][
                    "peak_bytes"
                ] += 1

            reject(
                "resource-history",
                drift_resource,
                "resource history summary",
                synchronize_summary=True,
            )
            reject(
                "event-count",
                lambda rows: rows[-1].__setitem__(
                    "event_count", rows[-1]["event_count"] + 1
                ),
                "event count",
            )

            def drift_replay(rows: list[dict[str, object]]) -> None:
                rows[-1]["summary"]["replay_closure"]["routes"][0]["c_to_f_bytes"] += 1

            reject(
                "replay-route",
                drift_replay,
                "exact replay route totals",
                synchronize_summary=True,
            )

            def first_event(
                rows: list[dict[str, object]], name: str
            ) -> dict[str, object]:
                return next(
                    event
                    for timeline in rows[1:-1]
                    for event in timeline["events"]
                    if event["event"] == name
                )

            def move_event_outside_interval(rows: list[dict[str, object]]) -> None:
                event = rows[1]["events"][0]
                value = rows[1]["wall_end_ns"] + 1
                event["start_ns"] = value
                event["end_ns"] = value
                event["time_ns"] = value

            reject(
                "event-containment",
                move_event_outside_interval,
                "outside its timeline interval",
            )
            reject(
                "timeline-sequence",
                lambda rows: rows[1].__setitem__("sequence", 7),
                "timeline sequence",
            )

            def drift_release_c(rows: list[dict[str, object]]) -> None:
                first_event(rows, "release")["C_STORE_GUID"] = "a" * 32

            reject("release-c", drift_release_c, "C identity differs")

            def drift_profile(rows: list[dict[str, object]]) -> None:
                first_event(rows, "release")["negotiated_profiles"] = ["invented"]

            reject("selected-profile", drift_profile, "profiles omit")

            def drift_transaction_digest(rows: list[dict[str, object]]) -> None:
                target = first_event(rows, "dispatch")["logical_job_id"]
                for timeline in rows[1:-1]:
                    for event in timeline["events"]:
                        if (
                            event["logical_job_id"] == target
                            and event["transaction_digest"] is not None
                        ):
                            event["transaction_digest"] = "f" * 64
                            event["InputRecord_identity"] = "input-forged"

            reject(
                "transaction-digest",
                drift_transaction_digest,
                "transaction/InputRecord digest",
            )

            def forge_job_count(rows: list[dict[str, object]]) -> None:
                rows[-1]["summary"]["jobs"] = 999

            reject(
                "job-cardinality",
                forge_job_count,
                "final jobs differs",
                synchronize_summary=True,
            )

            def forge_trace_sha(rows: list[dict[str, object]]) -> None:
                forged = "f" * 64
                rows[0]["route_trace_evidence"]["sha256"] = forged
                rows[-1]["summary"]["routing"]["route_trace_sha256"] = forged
                rows[-1]["summary"]["replay_closure"]["route_trace_sha256"] = forged

            reject(
                "route-trace-digest",
                forge_trace_sha,
                "digest differs from exact bytes",
                synchronize_summary=True,
            )

            def forge_route_identity(rows: list[dict[str, object]]) -> None:
                target = first_event(rows, "dispatch")["logical_job_id"]
                forged_f = "a" * 32
                for timeline in rows[1:-1]:
                    for event in timeline["events"]:
                        if event["logical_job_id"] == target:
                            event["F_STORE_GUID"] = forged_f
                            event["physical_endpoint"] = "forged-F"
                            event["RouteLaneId"] = "forged-lane"
                for route in rows[-1]["summary"]["replay_closure"]["routes"]:
                    if route["worker"] == first_event(rows, "dispatch")["worker"]:
                        route["F_STORE_GUID"] = forged_f
                        route["physical_endpoint"] = "forged-F"
                        route["RouteLaneId"] = "forged-lane"

            reject(
                "route-identity-vs-trace",
                forge_route_identity,
                "retained route trace|route identity drifted",
                synchronize_summary=True,
            )

            def drift_phase_extent(rows: list[dict[str, object]]) -> None:
                first_event(rows, "flow-sent")["bytes"] += 1

            reject("phase-byte-extent", drift_phase_extent, "phase bytes differ")

            def remove_completion(rows: list[dict[str, object]]) -> None:
                for timeline in rows[1:-1]:
                    for index, event in enumerate(timeline["events"]):
                        if event["event"] == "transaction-complete":
                            del timeline["events"][index]
                            for later in rows[1:-1]:
                                for candidate in later["events"]:
                                    if candidate["sequence"] > event["sequence"]:
                                        candidate["sequence"] -= 1
                            timeline["event_sequence_end"] -= 1
                            rows[-1]["event_count"] -= 1
                            return

            reject(
                "tu-completion-cardinality",
                remove_completion,
                "event extent|transaction-complete|event sequence",
            )

    def test_v2_stream_is_identical_in_two_checkout_roots(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            parent = Path(directory)
            streams = []
            for checkout in (parent / "checkout-a", parent / "checkout-b"):
                checkout.mkdir()
                path = write_v2_fixture(checkout, [3, 2], [10, 11], workers=2)
                add_route_trace(path, [1, 0], [10, 11])
                scenario = sim.load_scenario(path)
                result = sim.Simulator(scenario, sim.RawAdapter()).run()
                output = checkout / "out"
                sim.write_result(scenario, result, output, execution_for(path))
                streams.append((output / "experiment.jsonl").read_bytes())
            self.assertEqual(streams[0], streams[1])

    def test_transaction_digest_binds_route_state_profiles(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = write_v2_fixture(root, [3], [10], workers=1)
            scenario = sim.load_scenario(path)
            baseline = sim.Simulator(scenario, sim.RawAdapter()).run()
            changed_simulator = sim.Simulator(scenario, sim.RawAdapter())
            changed_simulator.route_state_profiles.append("retained-window-v2")
            changed = changed_simulator.run()
            self.assertNotEqual(
                baseline.assignments[0]["transaction_digest"],
                changed.assignments[0]["transaction_digest"],
            )

    def test_v2_validator_rejects_nonzero_release_offset_at_build_boundary(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = write_v2_fixture(root, [3, 2], [10, 11], workers=2)
            scenario = sim.load_scenario(path)
            output = root / "out"
            sim.write_result(
                scenario,
                sim.Simulator(scenario, sim.RawAdapter()).run(),
                output,
                execution_for(path),
            )
            rows = [
                json.loads(line)
                for line in (output / "experiment.jsonl").read_text().splitlines()
            ]
            release = next(
                event
                for timeline in rows[1:-1]
                for event in timeline["events"]
                if event["event"] == "release"
            )
            release["start_ns"] = 1
            release["end_ns"] = 1
            release["time_ns"] = 1
            candidate = output / "bad-release-offset.jsonl"
            candidate.write_text("".join(json.dumps(row) + "\n" for row in rows))
            with self.assertRaisesRegex(ValueError, "event times|build .* boundary"):
                sim.validate_experiment_jsonl(candidate)

    def test_retained_r4_sample_validates_and_replays(self) -> None:
        root = MODULE_PATH.parent / "samples" / "r4-minimum"
        scenario = sim.load_scenario(root / "experiment.json")
        execution = sim.load_execution(root / "execution.json", scenario)
        self.assertEqual(execution["mode"], "simulated")
        result = sim.Simulator(scenario, sim.RawAdapter()).run()
        self.assertEqual(result.summary["replay_closure"]["semantics"], "exact-route")
        self.assertEqual(result.summary["scored_outgoing_bytes"], 37)
        self.assertEqual(
            sim.validate_experiment_jsonl(root / "sample-experiment.jsonl"),
            {"events": 30, "c_to_f_bytes": 37, "f_to_c_bytes": 0},
        )
        self.assertEqual(
            sim.sha256(root / "sample-experiment.jsonl"),
            "9a23a42bddf43ff2606472594bfd98126f8c550cda92e65407b265e049d99071",
        )


if __name__ == "__main__":
    unittest.main()
