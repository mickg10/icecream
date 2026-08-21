#!/usr/bin/env python3
from __future__ import annotations

import importlib.util
import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


MODULE_PATH = Path(__file__).with_name("build_p29_ledger.py")
SPEC = importlib.util.spec_from_file_location("build_p29_ledger", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
builder = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = builder
SPEC.loader.exec_module(builder)


def frame(kind: int, payload: bytes) -> bytes:
    return bytes([kind]) + struct.pack("<I", len(payload)) + payload


def write_scenario(root: Path, workers: int = 2) -> Path:
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
        "name": "p29-multi-route-test",
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
                "slots": 2,
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
            "placement_policy": "round-robin",
        },
        "experiment": {"codecs": ["p29"], "routing_mode": "online"},
    }
    path = root / "scenario.json"
    path.write_text(json.dumps(document))
    return path


def fake_codec_run(command: list[str], stdout_path: Path, stderr_path: Path) -> None:
    def option(name: str) -> str:
        return command[command.index(name) + 1]

    worker_count = int(option("--route-s1"))
    route_lines = Path(option("--route-map")).read_text().splitlines()
    assignment = [int(value) for value in route_lines[1:]]
    manifest = [Path(value) for value in Path(option("--manifest")).read_text().splitlines()]
    output_root = Path(option("--materialize-routes-dir"))
    populated = sorted(set(assignment))
    for worker in populated:
        active = [tu for tu, route in enumerate(assignment) if route == worker]
        active_set = set(active)
        c_data = bytearray()
        f_data = bytearray()
        curve_rows = []
        selector_rows = []
        component_rows = []
        rel_seq = 0
        c_frames = f_frames = 0
        for tu, payload in enumerate(manifest):
            is_active = tu in active_set
            if is_active:
                c_chunk = (
                    frame(1, bytes([tu]))
                    + frame(8, b"L")
                    + frame(0xFE, b"")
                )
                f_chunk = frame(3, b"") + frame(0xFD, b"")
                c_data.extend(c_chunk)
                f_data.extend(f_chunk)
                c_frames += 3
                f_frames += 2
                selector_rows.append(
                    f"{tu}\t{rel_seq}\t{len(c_chunk)}\t0\tROUTE_S1\n"
                )
            curve_rows.append(
                f"{tu + 1}\t{int(is_active)}\t{rel_seq if is_active else 0}\t"
                f"{payload.stat().st_size if is_active else 0}\t{len(c_data)}\t"
                f"{len(f_data)}\t{c_frames}\t{f_frames}\t0\n"
            )
            component_rows.append(
                f"{tu + 1}\t{int(is_active)}\t{rel_seq if is_active else 0}\t"
                f"{'true' if is_active else 'false'}\n"
            )
            if is_active:
                rel_seq += 1

        route_directory = output_root / f"C0-F{worker}"
        route_directory.mkdir(parents=True, exist_ok=True)
        (route_directory / "p29.c-to-f.bin").write_bytes(c_data)
        (route_directory / "p29.f-to-c.bin").write_bytes(f_data)
        (route_directory / "sink-curve.tsv").write_text(
            "tu\tactive\trel_seq\traw_bytes\tcf_offset\tfc_offset\tcf_frames\tfc_frames\tbuild_close\n"
            + "".join(curve_rows)
        )
        (route_directory / "selector.tsv").write_text(
            "tu\trel_seq\tactual_delta\temitted_blockdefs\twinner\n"
            + "".join(selector_rows)
        )
        (route_directory / "components.tsv").write_text(
            "tu\tactive\trel_seq\texact\n" + "".join(component_rows)
        )
        markers = [
            f"MULTIROUTE_PLAN routes={worker_count} target={worker} "
            f"active_tus={len(active)} blocks=7 digest={'12' * 16}",
            "byte-exact=OK",
            "SELECTOR closure:",
            "SELECTOR manifest:",
            "SELECTOR full total:",
        ]
        if "--sink-replay" in command:
            markers.append("SINK REPLAY OK:")
        log_name = "replay.stdout" if "--sink-replay" in command else "encode.stdout"
        (route_directory / log_name).write_text("\n".join(markers) + "\n")
        error_name = "replay.stderr" if "--sink-replay" in command else "encode.stderr"
        (route_directory / error_name).write_text("")

    stdout_path.write_text(
        f"MULTIROUTE_SUPERVISOR routes={worker_count} "
        f"completed={len(populated)} blocks=7 digest={'12' * 16} status=PASS\n"
    )
    stderr_path.write_text("")


class P29LedgerBuilderTest(unittest.TestCase):
    def test_shared_plan_record_is_unique_and_exact(self) -> None:
        marker = (
            "MULTIROUTE_PLAN routes=2 target=1 active_tus=3 blocks=7 "
            f"digest={'ab' * 16}\n"
        )
        self.assertEqual(
            builder.shared_plan_record(marker),
            {
                "routes": 2,
                "target": 1,
                "active_tus": 3,
                "blocks": 7,
                "digest": "ab" * 16,
            },
        )
        with self.assertRaisesRegex(RuntimeError, "exactly one"):
            builder.shared_plan_record(marker + marker)
        with self.assertRaisesRegex(RuntimeError, "exactly one"):
            builder.shared_plan_record(marker.replace("ab", "AB"))

    def test_shared_supervisor_record_is_unique_and_exact(self) -> None:
        marker = (
            "MULTIROUTE_SUPERVISOR routes=20 completed=3 blocks=7 "
            f"digest={'ab' * 16} status=PASS\n"
        )
        self.assertEqual(
            builder.shared_supervisor_record(marker),
            {
                "routes": 20,
                "completed": 3,
                "blocks": 7,
                "digest": "ab" * 16,
                "status": "PASS",
            },
        )
        with self.assertRaisesRegex(RuntimeError, "exactly one"):
            builder.shared_supervisor_record(marker + marker)

    def test_multi_route_builder_preserves_global_and_relationship_order(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            scenario_path = write_scenario(root)
            scenario = builder.sim.load_scenario(scenario_path)
            assigned = builder.scenario_items(scenario)
            self.assertEqual([entry.tu_seq for entry in assigned], list(range(4)))
            self.assertEqual([entry.worker for entry in assigned], [0, 1, 0, 1])
            self.assertEqual([entry.rel_seq for entry in assigned], [0, 0, 1, 1])

            codec = root / "codec50-sink"
            codec.write_bytes(b"")
            output = root / "ledger.jsonl"
            with mock.patch.object(
                builder, "checked_run", side_effect=fake_codec_run
            ) as checked:
                builder.build_ledger(
                    scenario_path,
                    codec,
                    output,
                    root / "work",
                    [],
                )
            self.assertEqual(checked.call_count, 2)
            self.assertTrue(
                all(
                    "--materialize-routes-dir" in call.args[0]
                    for call in checked.call_args_list
                )
            )
            rows = [json.loads(line) for line in output.read_text().splitlines()]
            descriptor, tu_rows, summary = rows[0], rows[1:-1], rows[-1]
            self.assertEqual(descriptor["shared_plan_digest"], "12" * 16)
            self.assertEqual(descriptor["shared_block_count"], 7)
            self.assertEqual(
                descriptor["materializer"]["encode_shared_preparations"], 1
            )
            self.assertEqual(
                descriptor["materializer"]["populated_route_children_per_pass"], 2
            )
            self.assertEqual(sorted(descriptor["routes"]), ["C0-F0", "C0-F1"])
            self.assertEqual([row["tu_seq"] for row in tu_rows], list(range(4)))
            self.assertEqual([row["worker"] for row in tu_rows], [0, 1, 0, 1])
            self.assertEqual([row["rel_seq"] for row in tu_rows], [0, 0, 1, 1])
            self.assertTrue(all(row["exact"] for row in tu_rows))
            self.assertEqual(summary["totals"]["tus"], 4)
            self.assertEqual(
                summary["totals"]["c_to_f_bytes"],
                sum(
                    phase["bytes"]
                    for row in tu_rows
                    for phase in row["phases"]
                    if phase["direction"] == "c_to_f"
                ),
            )

    def test_supervisor_materializes_only_populated_routes(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            scenario_path = write_scenario(root, workers=6)
            codec = root / "codec50-sink"
            codec.write_bytes(b"")
            output = root / "ledger.jsonl"
            with mock.patch.object(
                builder, "checked_run", side_effect=fake_codec_run
            ) as checked:
                builder.build_ledger(
                    scenario_path,
                    codec,
                    output,
                    root / "work",
                    [],
                )
            self.assertEqual(checked.call_count, 2)
            descriptor = json.loads(output.read_text().splitlines()[0])
            self.assertEqual(
                sorted(descriptor["routes"]),
                ["C0-F0", "C0-F1", "C0-F2", "C0-F3"],
            )

    def test_frame_slices_map_to_causal_dialogue_and_tile_each_direction(self) -> None:
        c_data = b"".join(
            (
                frame(1, b"root"),
                frame(2, b"block"),
                frame(7, b"regions"),
                frame(31, b"fallback"),
                frame(0xFE, b"close"),
            )
        )
        f_data = b"".join(
            (
                frame(3, b"need"),
                frame(30, b"request"),
                frame(0xFD, b"ack"),
            )
        )
        c_frames = builder.parse_frames(c_data, 0, len(c_data), "c")
        f_frames = builder.parse_frames(f_data, 0, len(f_data), "f")
        phases = builder.phase_rows(c_frames, f_frames)
        self.assertEqual(
            [row["name"] for row in phases],
            [
                "p29-root",
                "p29-need",
                "p29-fill",
                "p29-fallback-request",
                "p29-fallback-reply",
                "p29-close",
                "p29-ack",
            ],
        )
        self.assertEqual(
            sum(row["bytes"] for row in phases if row["direction"] == "c_to_f"),
            len(c_data),
        )
        self.assertEqual(
            sum(row["bytes"] for row in phases if row["direction"] == "f_to_c"),
            len(f_data),
        )

    def test_frame_parser_rejects_unknown_or_partial_frames(self) -> None:
        with self.assertRaisesRegex(ValueError, "unknown type"):
            builder.parse_frames(frame(99, b"x"), 0, 6, "bad")
        with self.assertRaisesRegex(ValueError, "truncated"):
            builder.parse_frames(frame(1, b"abc")[:-1], 0, 7, "short")

    def test_transaction_graph_forks_lines_from_sent_and_need_from_delivery(self) -> None:
        phases = [
            {"name": "p29-root", "direction": "c_to_f", "bytes": 10},
            {"name": "p29-need", "direction": "f_to_c", "bytes": 2},
            {"name": "p29-lines", "direction": "c_to_f", "bytes": 20},
            {"name": "p29-fill", "direction": "c_to_f", "bytes": 3},
            {"name": "p29-close", "direction": "c_to_f", "bytes": 4},
            {"name": "p29-ack", "direction": "f_to_c", "bytes": 5},
        ]
        graph = builder.transaction_graph(phases)
        by_name = {row["name"]: row for row in graph["phases"]}
        self.assertEqual(by_name["p29-lines"]["depends_on"], ["p29-root:sent"])
        self.assertEqual(by_name["p29-need"]["depends_on"], ["p29-root:delivered"])
        self.assertEqual(by_name["p29-fill"]["depends_on"], ["p29-need:delivered"])
        self.assertEqual(
            set(by_name["p29-close"]["depends_on"]),
            {
                "p29-lines:delivered",
                "p29-fill:delivered",
                "p29-need:delivered",
            },
        )
        self.assertEqual(graph["input_ready_after"][-1], "p29-close:delivered")
        self.assertEqual(graph["commit_after"], ["p29-ack:delivered"])

    def test_transaction_graph_waits_for_an_empty_need_response(self) -> None:
        graph = builder.transaction_graph(
            [
                {"name": "p29-root", "direction": "c_to_f", "bytes": 10},
                {"name": "p29-need", "direction": "f_to_c", "bytes": 2},
                {"name": "p29-close", "direction": "c_to_f", "bytes": 4},
            ]
        )
        by_name = {row["name"]: row for row in graph["phases"]}
        self.assertEqual(
            by_name["p29-close"]["depends_on"], ["p29-need:delivered"]
        )


if __name__ == "__main__":
    unittest.main()
