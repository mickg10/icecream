#!/usr/bin/env python3
from __future__ import annotations

import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock


MODULE_PATH = Path(__file__).with_name("build_grz_ledger.py")
SPEC = importlib.util.spec_from_file_location("build_grz_ledger", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
builder = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = builder
SPEC.loader.exec_module(builder)


def write_scenario(root: Path) -> Path:
    sizes = [10, 11, 12, 13]
    (root / "trace.tsv").write_text(
        "logical\tjob_id\tii_relative\traw_bytes\tcompile_ns\tcompile_model\n"
        + "".join(
            f"{logical}\tj{logical}\tj{logical}.ii\t{size}\t1\ttest-model\n"
            for logical, size in enumerate(sizes)
        )
    )
    for logical, size in enumerate(sizes):
        (root / f"j{logical}.ii").write_bytes(bytes([logical + 1]) * size)
    document = {
        "schema": "icecream-distribution-scenario-v1",
        "name": "grz-static-route-test",
        "seed": 9,
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
                        "builds": 2,
                        "start_ns": 0,
                        "build_release": {"mode": "after-previous"},
                        "tu_release": {"mode": "all-at-zero"},
                    }
                ],
            },
        },
        "workers": {
            "f_count": 4,
            "template": {
                "slots": 2,
                "input_staging_slots": 2,
                "compile_profile": "test-model",
                "initial_cache": "cold",
            },
        },
        "network": {
            "c_to_f": {
                "bits_per_second": 1_000_000_000,
                "one_way_latency_ns": 0,
                "lanes_per_endpoint": 1,
            },
            "f_to_c": {
                "bits_per_second": 1_000_000_000,
                "one_way_latency_ns": 0,
                "lanes_per_endpoint": 1,
            },
            "shared_fabric_bps": 1_000_000_000,
        },
        "scheduler": {
            "ready_job_policy": "fifo-release",
            "placement_policy": "rendezvous",
            "dense_frontier_workers": 2,
        },
        "experiment": {"codecs": ["grz"], "routing_mode": "replay"},
    }
    path = root / "scenario.json"
    path.write_text(json.dumps(document))
    return path


def fake_grz_run(command: list[str], stdout_path: Path, stderr_path: Path) -> None:
    operation = command[1]
    route_directory = stdout_path.parent
    if operation == "tu":
        Path(command[3]).write_text("test-tu-map\n")
    elif operation == "enc":
        container = Path(command[3])
        curve = Path(command[command.index("--curve") + 1])
        payloads = [
            Path(value)
            for value in (route_directory / "manifest.txt").read_text().splitlines()
        ]
        groups = [bytes([index + 1, index + 1]) for index in range(len(payloads))]
        container.write_bytes(
            b"GRZ!" + b"".join(groups) + b"E" * builder.END_FRAME_BYTES
        )
        curve.write_text(
            "group\ttu_lo\ttu_hi\tclosed_by\tout_bytes\tcomp_bytes\n"
            + "".join(
                f"{index}\t{index}\t{index + 1}\ttu\t{payload.stat().st_size}\t2\n"
                for index, payload in enumerate(payloads)
            )
        )
    elif operation == "dec":
        Path(command[3]).write_bytes((route_directory / "input.ii").read_bytes())
    elif operation == "decprefix":
        point = int(command[command.index("-g") + 1])
        payloads = [
            Path(value)
            for value in (route_directory / "manifest.txt").read_text().splitlines()
        ]
        Path(command[3]).write_bytes(
            b"".join(payload.read_bytes() for payload in payloads[:point])
        )
    else:
        raise AssertionError(f"unexpected fake GRZ operation {operation}")
    stdout_path.write_text("")
    stderr_path.write_text("")


class GrzLedgerBuilderTest(unittest.TestCase):
    def test_prefix_points_cover_every_small_tu_and_large_build_boundaries(
        self,
    ) -> None:
        small = [SimpleNamespace(build=0) for _ in range(4)]
        self.assertEqual(builder.prefix_points(4, 64, small), [1, 2, 3, 4])
        large = [SimpleNamespace(build=0) for _ in range(100)] + [
            SimpleNamespace(build=1) for _ in range(100)
        ]
        self.assertEqual(
            builder.prefix_points(200, 64, large), [1, 64, 100, 128, 192, 200]
        )

    def test_streaming_prefix_copy_and_comparison(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source"
            prefix = root / "prefix"
            source.write_bytes(bytes(range(251)) * 100)
            builder.copy_prefix(source, prefix, 12_345)
            self.assertTrue(builder.file_equals_prefix(prefix, source, 12_345))
            prefix.write_bytes(prefix.read_bytes()[:-1] + b"x")
            self.assertFalse(builder.file_equals_prefix(prefix, source, 12_345))

    def test_static_dense_routes_build_and_replay_exact_grz_bytes(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            scenario_path = write_scenario(root)
            binary = root / "grz2g"
            binary.write_bytes(b"")
            output = root / "ledger.jsonl"
            with mock.patch.object(builder, "checked_run", side_effect=fake_grz_run):
                builder.build_ledger(
                    scenario_path,
                    binary,
                    output,
                    root / "work",
                    [],
                    64,
                )

            rows = [json.loads(line) for line in output.read_text().splitlines()]
            descriptor, tu_rows, final = rows[0], rows[1:-1], rows[-1]
            self.assertEqual(
                descriptor["routing"],
                {
                    "placement_policy": "rendezvous",
                    "dense_frontier_workers": 2,
                },
            )
            by_build = {
                build: {
                    int(row["logical"]): int(row["worker"])
                    for row in tu_rows
                    if row["build"] == build
                }
                for build in range(2)
            }
            self.assertEqual(by_build[0], by_build[1])
            self.assertLessEqual(len(set(by_build[0].values())), 2)
            self.assertEqual(
                final["totals"]["c_to_f_bytes"],
                sum(int(phase["bytes"]) for row in tu_rows for phase in row["phases"]),
            )

            scenario = builder.sim.load_scenario(scenario_path)
            adapter = builder.sim.PhysicalLedgerAdapter(output, scenario, "grz")
            result = builder.sim.Simulator(scenario, adapter).run()
            self.assertEqual(
                result.summary["c_to_f_bytes"], final["totals"]["c_to_f_bytes"]
            )
            self.assertEqual(result.summary["f_to_c_bytes"], 0)
            self.assertEqual(
                {
                    (int(row["build"]), int(row["logical"])): int(row["worker"])
                    for row in result.assignments
                },
                {
                    (int(row["build"]), int(row["logical"])): int(row["worker"])
                    for row in tu_rows
                },
            )


if __name__ == "__main__":
    unittest.main()
