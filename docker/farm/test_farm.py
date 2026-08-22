#!/usr/bin/env python3

import argparse
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock


HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location("icecream_farm", HERE / "farm.py")
assert SPEC is not None and SPEC.loader is not None
farm = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(farm)


def subprocess_result(returncode, stdout=b"", stderr=b""):
    return subprocess.CompletedProcess([], returncode, stdout, stderr)


class ManifestTests(unittest.TestCase):
    def setUp(self):
        self.manifest = farm.load_manifest(farm.DEFAULT_MANIFEST)

    def test_exact_four_environment_classes_and_44_cells(self):
        self.assertEqual(
            set(self.manifest["environment_classes"]),
            {"debian-gcc", "fedora-clang-libcxx", "linuxbrew", "conan-gcc"},
        )
        self.assertEqual(
            sum(item["corpus_cells"] for item in self.manifest["environment_classes"].values()),
            44,
        )
        for item in self.manifest["environment_classes"].values():
            self.assertRegex(item["expected_id"], r"^sha256:[0-9a-f]{64}$")
            self.assertRegex(item["expected_fingerprint"], r"^sha256:[0-9a-f]{64}$")
            self.assertIn("@sha256:", item["image"])

    def test_local_manifest_has_no_wan_hosts_or_overlay_addresses(self):
        self.assertEqual(self.manifest["wan_hosts"], [])
        self.assertEqual(self.manifest["network"]["mode"], "local-lan")
        for host in self.manifest["hosts"].values():
            self.assertTrue(host["address"].startswith("10.0.27."))
            self.assertEqual(
                host["transport"].get("host_override", host["address"]),
                host["address"],
            )

    def test_unready_hosts_are_explicitly_launch_disabled(self):
        expected = {"research6", "research7", "quietbox3"}
        disabled = {
            name
            for name, host in self.manifest["hosts"].items()
            if not host["launch_enabled"]
        }
        self.assertEqual(disabled, expected)
        for name in disabled:
            self.assertTrue(self.manifest["hosts"][name]["launch_block"])
        for name in {"nas642", "quietbox2"}:
            self.assertTrue(self.manifest["hosts"][name]["launch_enabled"])
            self.assertIsNone(self.manifest["hosts"][name]["launch_block"])

    def test_overlay_address_is_rejected(self):
        candidate = copy.deepcopy(self.manifest)
        candidate["hosts"]["research6"]["address"] = "100.73.1.2"
        candidate["hosts"]["research6"]["transport"]["host_override"] = "100.73.1.2"
        with self.assertRaisesRegex(farm.FarmError, "outside|forbidden"):
            farm.validate_manifest(candidate)

    def test_nonempty_wan_default_is_rejected(self):
        candidate = copy.deepcopy(self.manifest)
        candidate["wan_hosts"] = ["research4"]
        with self.assertRaisesRegex(farm.FarmError, "wan_hosts"):
            farm.validate_manifest(candidate)

    def test_conservative_and_max32_capacity_are_separate(self):
        profile = self.manifest["profiles"]["conservative_nas_submitter"]
        self.assertEqual(profile["advertised_worker_slots"], 80)
        self.assertEqual(profile["worker_slots"]["research6"], 16)
        self.assertEqual(profile["worker_slots"]["research7"], 16)
        shared = self.manifest["resource_groups"]["nas-r6-r7-shared"]
        self.assertEqual(shared["aggregate_worker_cap"], 64)
        self.assertEqual(shared["owner_estimated_physical_cpus"], 64)
        maximum = self.manifest["profiles"]["max32_nas_submitter"]
        self.assertEqual(maximum["advertised_worker_slots"], 128)
        self.assertTrue(all(value == 32 for value in maximum["worker_slots"].values()))

    def test_p43_p50_are_selection_seams_without_fabricated_artifacts(self):
        binary_sets = self.manifest["component_binary_sets"]
        self.assertTrue(binary_sets["accepted-current"]["available"])
        for name in ("p43", "p50"):
            self.assertFalse(binary_sets[name]["available"])
            self.assertIsNone(binary_sets[name]["runtime_path_key"])
            self.assertIsNone(binary_sets[name]["runtime_image_class"])

    def test_mount_override_is_narrow(self):
        with tempfile.TemporaryDirectory() as directory:
            overlay = Path(directory) / "mounts.json"
            overlay.write_text(
                json.dumps({"hosts": {"nas642": {"source_root": "/srv/source"}}}),
                encoding="utf-8",
            )
            loaded = farm.load_manifest(farm.DEFAULT_MANIFEST, overlay)
            self.assertEqual(loaded["hosts"]["nas642"]["paths"]["source_root"], "/srv/source")
            overlay.write_text(
                json.dumps({"hosts": {"nas642": {"address": "100.1.2.3"}}}),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(farm.FarmError, "unknown mount keys"):
                farm.load_manifest(farm.DEFAULT_MANIFEST, overlay)

    def test_runtime_override_requires_exact_content_fingerprint(self):
        with self.assertRaisesRegex(farm.FarmError, "requires --node-image-fingerprint"):
            farm.load_manifest(farm.DEFAULT_MANIFEST, node_image_ref="example/runtime:test")
        loaded = farm.load_manifest(
            farm.DEFAULT_MANIFEST,
            node_image_ref="example/runtime:test",
            node_image_fingerprint="sha256:" + "a" * 64,
        )
        self.assertEqual(loaded["runtime_image"]["ref"], "example/runtime:test")
        self.assertIsNone(loaded["runtime_image"]["expected_id"])
        self.assertEqual(
            loaded["runtime_image"]["expected_fingerprint"], "sha256:" + "a" * 64
        )

    def test_image_fingerprint_ignores_engine_specific_identity(self):
        details = {
            "Id": "sha256:" + "1" * 64,
            "Architecture": "amd64",
            "Os": "linux",
            "Comment": "buildkit.dockerfile.v0",
            "Created": "2026-08-21T00:00:00Z",
            "Config": {"Env": ["A=B"]},
            "RootFS": {"Type": "layers", "Layers": ["sha256:" + "2" * 64]},
        }
        changed_engine = copy.deepcopy(details)
        changed_engine["Id"] = "sha256:" + "3" * 64
        changed_engine["RepoDigests"] = ["example@sha256:" + "4" * 64]
        changed_engine["Created"] = "2030-01-01T00:00:00Z"
        self.assertEqual(
            farm.image_fingerprint(details), farm.image_fingerprint(changed_engine)
        )

    def test_scheduler_port_override_keeps_control_pair(self):
        loaded = farm.load_manifest(farm.DEFAULT_MANIFEST, scheduler_port=18765)
        self.assertEqual(loaded["network"]["scheduler_port"], 18765)
        self.assertEqual(loaded["network"]["scheduler_control_port"], 18766)


class PlanTests(unittest.TestCase):
    def setUp(self):
        self.manifest = farm.load_manifest(farm.DEFAULT_MANIFEST)

    def plan(self, scenario="c1f4", profile="conservative_nas_submitter", label="gate-a"):
        return farm.build_plan(
            self.manifest, profile, scenario, "debian-gcc", label
        )

    def test_deterministic_run_identity(self):
        first = self.plan()
        second = self.plan()
        changed = self.plan(label="gate-b")
        self.assertEqual(first["run_id"], second["run_id"])
        self.assertNotEqual(first["run_id"], changed["run_id"])
        self.assertRegex(
            first["run_id"], r"^binary-accepted-current-c1f4-[0-9a-f]{12}$"
        )
        self.assertNotIn("p50", first["run_id"])
        self.assertTrue(
            all("p50" not in record["container"] for record in [
                first["scheduler"], *first["submitters"], *first["workers"]
            ])
        )
        for host_name in farm.plan_hosts(first):
            compose = farm.render_compose(self.manifest, first, host_name)
            self.assertNotIn("p50", compose["name"])
            for service in compose["services"].values():
                self.assertNotIn(
                    "p50", service["labels"]["org.icecream.farm.run_id"]
                )
        self.assertEqual(
            {role: item["name"] for role, item in first["selected_binary_sets"].items()},
            {
                "scheduler": "accepted-current",
                "submitter": "accepted-current",
                "worker": "accepted-current",
            },
        )
        two_submitters = farm.build_plan(
            self.manifest,
            "conservative_nas_submitter",
            "c1f4",
            "debian-gcc",
            "gate-a",
            ["nas642", "nas642"],
        )
        self.assertNotEqual(first["run_id"], two_submitters["run_id"])

    def test_run_identity_claims_protocol_only_for_exact_available_binary_sets(self):
        candidate = copy.deepcopy(self.manifest)
        for name in ("p43", "p50"):
            candidate["component_binary_sets"][name].update(
                {
                    "available": True,
                    "runtime_path_key": "runtime_prefix",
                    "runtime_image_class": "test-only-exact-artifact",
                }
            )
        p50 = farm.build_plan(
            candidate,
            "conservative_nas_submitter",
            "c1f1",
            "debian-gcc",
            "truthful-p50",
            component_version_overrides={
                "scheduler": "p50", "submitter": "p50", "worker": "p50"
            },
        )
        self.assertRegex(p50["run_id"], r"^p50-c1f1-[0-9a-f]{12}$")
        mixed = farm.build_plan(
            candidate,
            "conservative_nas_submitter",
            "c1f1",
            "debian-gcc",
            "truthful-mixed",
            component_version_overrides={
                "scheduler": "p43", "submitter": "p50", "worker": "p50"
            },
        )
        self.assertRegex(mixed["run_id"], r"^mixed-s-p43-c-p50-f-p50-c1f1-")
        self.assertEqual(mixed["selected_binary_sets"]["scheduler"]["protocol"], 43)
        self.assertEqual(mixed["selected_binary_sets"]["worker"]["protocol"], 50)

    def test_c1f_counts_are_physical_daemon_counts_not_slot_counts(self):
        self.assertEqual(len(self.plan("c1f1")["workers"]), 1)
        self.assertEqual(len(self.plan("c1f2")["workers"]), 2)
        plan4 = self.plan("c1f4")
        self.assertEqual(len(plan4["workers"]), 4)
        self.assertEqual(
            plan4["accounting"]["f_daemons_by_host"],
            {"quietbox2": 1, "research6": 1, "quietbox3": 1, "research7": 1},
        )
        self.assertEqual(plan4["accounting"]["selected_compile_slots"], 80)
        self.assertNotEqual(
            plan4["accounting"]["f_daemon_containers"],
            plan4["accounting"]["selected_compile_slots"],
        )
        self.assertEqual(plan4["accounting"]["route_identity_count"], 4)
        self.assertEqual(plan4["accounting"]["cache_identity_owner_count"], 4)
        self.assertEqual(plan4["accounting"]["independent_resource_groups"], 3)

    def test_conservative_profile_is_four_daemons_advertising_80_slots(self):
        plan = self.plan("c1f4")
        self.assertEqual(len(plan["workers"]), 4)
        self.assertEqual(
            plan["accounting"]["slots_by_f_host"],
            {"quietbox2": 24, "research6": 16, "quietbox3": 24, "research7": 16},
        )
        self.assertEqual(
            plan["accounting"]["slots_by_resource_group"]["nas-r6-r7-shared"],
            32,
        )

    def test_max32_profile_is_four_daemons_advertising_128_slots(self):
        plan = self.plan("c1f4", "max32_nas_submitter")
        self.assertEqual(len(plan["workers"]), 4)
        self.assertEqual(plan["accounting"]["selected_compile_slots"], 128)
        self.assertEqual({record["slots"] for record in plan["workers"]}, {32})

    def test_unavailable_protocol_binary_selection_fails_before_a_plan(self):
        for role in ("scheduler", "submitter", "worker"):
            with self.subTest(role=role):
                with self.assertRaisesRegex(farm.FarmError, "declared but unavailable"):
                    farm.build_plan(
                        self.manifest,
                        "conservative_nas_submitter",
                        "c1f1",
                        "debian-gcc",
                        "versions",
                        component_version_overrides={role: "p50"},
                    )

    def test_q2_submitter_profile_excludes_q2_workers(self):
        plan = self.plan("c1f3", "conservative_q2_submitter")
        self.assertEqual(plan["submitters"][0]["host"], "quietbox2")
        self.assertNotIn("quietbox2", plan["accounting"]["slots_by_f_host"])
        self.assertEqual(plan["accounting"]["advertised_profile_worker_slots"], 56)
        self.assertEqual(plan["accounting"]["f_daemon_containers"], 3)
        self.assertEqual(plan["client_capacity"][0]["class"], "faster-lan-comparison")

    def test_research6_recovery_profile_uses_research6_only(self):
        plan = self.plan("c1f1", "r6_recovery_nas_submitter")
        self.assertEqual(plan["submitters"][0]["host"], "nas642")
        self.assertEqual(plan["accounting"]["f_daemons_by_host"], {"research6": 1})
        self.assertEqual(plan["accounting"]["slots_by_f_host"], {"research6": 16})

    def test_submitter_override_cannot_hide_q2_double_duty(self):
        with self.assertRaisesRegex(farm.FarmError, "retains worker slots"):
            farm.build_plan(
                self.manifest,
                "conservative_nas_submitter",
                "c1f1",
                "debian-gcc",
                "override",
                ["quietbox2"],
            )

    def test_multiple_submitters_are_explicit(self):
        plan = farm.build_plan(
            self.manifest,
            "conservative_nas_submitter",
            "c1f1",
            "debian-gcc",
            "two-c",
            ["nas642", "nas642"],
        )
        self.assertEqual(len(plan["submitters"]), 2)
        self.assertEqual(
            {item["registration_port"] for item in plan["submitters"]}, {0}
        )
        self.assertTrue(
            all(not item["accepts_remote_jobs"] for item in plan["submitters"])
        )

    def test_compose_is_per_host_and_host_networked(self):
        plan = self.plan("c1f2")
        nas = farm.render_compose(self.manifest, plan, "nas642")
        q2 = farm.render_compose(self.manifest, plan, "quietbox2")
        r6 = farm.render_compose(self.manifest, plan, "research6")
        self.assertEqual(set(nas["services"]), {"scheduler", "c00"})
        self.assertEqual(set(q2["services"]), {"f00"})
        self.assertEqual(set(r6["services"]), {"f01"})
        self.assertEqual(nas["services"]["c00"]["image"], "ice-ii/debian-gcc:v2-p50")
        for compose in (nas, q2, r6):
            json.loads(json.dumps(compose))
            for service in compose["services"].values():
                self.assertEqual(service["network_mode"], "host")
                self.assertEqual(
                    service["labels"]["org.icecream.farm.run_id"], plan["run_id"]
                )
        c_mounts = {volume["target"] for volume in nas["services"]["c00"]["volumes"]}
        f_mounts = {volume["target"] for volume in q2["services"]["f00"]["volumes"]}
        self.assertIn("/workspace", c_mounts)
        self.assertIn("/corpus", c_mounts)
        self.assertNotIn("/workspace", f_mounts)
        scheduler_command = nas["services"]["scheduler"]["command"]
        submitter_command = nas["services"]["c00"]["command"]
        self.assertEqual(nas["services"]["scheduler"]["entrypoint"], ["/bin/sh", "-ec"])
        self.assertIn("chown 65534:65534 /farm/results", scheduler_command[0])
        self.assertIn("exec /opt/icecream/sbin/icecc-scheduler \"$@\"", scheduler_command[0])
        self.assertIn(
            "chown 65534:65534 /farm/build /farm/results /var/lib/icecc",
            nas["services"]["c00"]["command"][0],
        )
        self.assertIn(
            "chown 65534:65534 /farm/results /var/lib/icecc",
            q2["services"]["f00"]["command"][0],
        )
        worker_command = q2["services"]["f00"]["command"]
        self.assertEqual(worker_command[worker_command.index("-m") + 1], "24")
        self.assertEqual(len(q2["services"]), 1)
        scheduler_health = nas["services"]["scheduler"]["healthcheck"]["test"][-1]
        worker_health = q2["services"]["f00"]["healthcheck"]["test"][-1]
        self.assertIn(" 8766", scheduler_health)
        self.assertIn(" 8766", worker_health)
        self.assertNotIn(" 8765", scheduler_health)
        self.assertNotIn(" 8765", worker_health)
        for health in (scheduler_health, worker_health):
            self.assertIn("listcs", health)
            self.assertIn("quit", health)
            self.assertIn("200 done", health)
            self.assertNotIn("nc -z", health)
        self.assertEqual(scheduler_command[scheduler_command.index("-u") + 1], "nobody")
        self.assertIn("--no-remote", submitter_command)
        self.assertNotIn("-p", submitter_command)
        self.assertNotIn("14000", submitter_command)
        self.assertEqual(
            farm.required_listener_ports(self.manifest, plan, "nas642"),
            [8765, 8766],
        )

        q2_submitter = self.plan("c1f1", "conservative_q2_submitter")
        self.assertEqual(
            farm.required_listener_ports(self.manifest, q2_submitter, "quietbox2"),
            [],
        )

    def test_cluster_registration_requires_exact_planned_endpoint(self):
        plan = self.plan("c1f1")
        retained_snapshot = (
            " c-nas642-00 (10.0.27.127:0) [x86_64] "
            "speed=0.00 jobs=0/0 load=408\n"
            " f-quietbox2 (10.0.27.212:12000) [x86_64] "
            "speed=0.00 jobs=0/24 load=1000\n"
            "200 done\n"
        )
        self.assertEqual(
            farm.expected_registrations(self.manifest, plan),
            [
                "c-nas642-00 (10.0.27.127:0)",
                "f-quietbox2 (10.0.27.212:12000)",
            ],
        )
        self.assertEqual(
            farm.missing_registrations(self.manifest, plan, retained_snapshot), []
        )
        misleading_submitter_port = (
            " c-nas642-00 (10.0.27.127:14000) [x86_64] "
            "speed=0.00 jobs=0/0 load=408\n"
            " f-quietbox2 (10.0.27.212:12000) [x86_64] "
            "speed=0.00 jobs=0/24 load=1000\n"
            "200 done\n"
        )
        self.assertEqual(
            farm.missing_registrations(
                self.manifest, plan, misleading_submitter_port
            ),
            ["c-nas642-00 (10.0.27.127:0) jobs=*/0"],
        )

        wrong_slot_snapshot = retained_snapshot.replace("jobs=0/24", "jobs=0/1")
        self.assertEqual(
            farm.missing_registrations(self.manifest, plan, wrong_slot_snapshot),
            ["f-quietbox2 (10.0.27.212:12000) jobs=*/24"],
        )

    def test_planned_container_collision_is_a_preflight_error(self):
        runner = mock.MagicMock()
        runner.run.side_effect = [
            subprocess_result(0, stdout=b""),
            subprocess_result(0, stdout=b"icefarm-run-f00\n"),
        ]
        records = [
            {"container": "icefarm-run-c00"},
            {"container": "icefarm-run-f00"},
        ]
        report, errors = farm.planned_container_status(runner, records)
        self.assertFalse(report["icefarm-run-c00"]["exists"])
        self.assertTrue(report["icefarm-run-f00"]["exists"])
        self.assertEqual(
            errors, ["planned container already exists: icefarm-run-f00"]
        )

    def test_submitter_source_mount_must_match_acceptance_fixture(self):
        expected = hashlib.sha256(
            (farm.HERE / "acceptance" / "tiny.cpp").read_bytes()
        ).hexdigest()
        runner = mock.MagicMock()
        runner.run.return_value = subprocess_result(
            0, stdout=f"{expected}  /source/tiny.cpp\n".encode()
        )
        status, error = farm.acceptance_source_status(runner, "/source")
        self.assertTrue(status["ok"])
        self.assertIsNone(error)
        runner.run.return_value = subprocess_result(
            0, stdout=("0" * 64 + "  /source/tiny.cpp\n").encode()
        )
        status, error = farm.acceptance_source_status(runner, "/source")
        self.assertFalse(status["ok"])
        self.assertRegex(error or "", "differs")

    def test_launch_disabled_host_stops_before_transport(self):
        plan = self.plan("c1f2")
        with mock.patch.object(farm, "basic_inventory") as inventory:
            report = farm.inspect_host(
                self.manifest, plan, "research6", {"worker"}
            )
        inventory.assert_not_called()
        self.assertFalse(report["ok"])
        self.assertRegex(report["errors"][0], "launch disabled")

    def test_q3_transport_is_lan_inventoried_but_mount_staging_is_blocked(self):
        host = self.manifest["hosts"]["quietbox3"]
        runner = farm.HostRunner("quietbox3", host)
        self.assertTrue(runner.available()[0])
        self.assertIn("HostName=10.0.27.101", runner.prefix())
        self.assertIn("HostKeyAlias=tt-quietbox3", runner.prefix())
        self.assertFalse(host["launch_enabled"])
        self.assertRegex(host["launch_block"], "mount roots")

    def test_quietbox_live_fqdn_identity_matches_exact_manifest(self):
        cases = {
            "quietbox2": "tt-quietbox2",
            "quietbox3": "tt-quietbox3.taildebf6.ts.net",
        }
        for host_name, live_hostname in cases.items():
            host = self.manifest["hosts"][host_name]
            interface = host["interface"]
            interface_row = (
                f"2: {interface} inet {host['address']}/24 brd 10.0.27.255 scope global"
            ).encode()
            facts = {
                "ok": True,
                "hostname": live_hostname,
                "docker_permission": "yes",
            }
            with self.subTest(host=host_name):
                with mock.patch.object(farm, "basic_inventory", return_value=facts):
                    with mock.patch.object(
                        farm.HostRunner,
                        "run",
                        return_value=subprocess_result(0, stdout=interface_row),
                    ):
                        self.assertEqual(
                            farm.verify_transport_identity(self.manifest, host_name), facts
                        )

                wrong = dict(facts, hostname=f"{live_hostname}.wrong")
                with mock.patch.object(farm, "basic_inventory", return_value=wrong):
                    with self.assertRaisesRegex(farm.FarmError, "expected hostname"):
                        farm.verify_transport_identity(self.manifest, host_name)

    def test_ssh_uses_lan_override_with_existing_alias_host_key(self):
        host = self.manifest["hosts"]["research6"]
        prefix = farm.HostRunner("research6", host).prefix()
        self.assertIn("HostName=10.0.27.56", prefix)
        self.assertIn("HostKeyAlias=research6", prefix)
        self.assertNotIn("100.", " ".join(prefix))

    def test_remote_command_has_a_host_side_timeout(self):
        runner = farm.HostRunner("research6", self.manifest["hosts"]["research6"])
        completed = subprocess_result(0)
        with mock.patch.object(farm.subprocess, "run", return_value=completed) as invoke:
            runner.run(["docker", "info"], timeout=9)
        command = invoke.call_args.args[0]
        self.assertIn(
            "timeout --signal=TERM --kill-after=5s 9s docker info", command[-1]
        )
        self.assertEqual(invoke.call_args.kwargs["timeout"], 24)

    def test_scheduler_snapshot_reads_complete_control_reply(self):
        plan = self.plan("c1f1")
        connection = mock.MagicMock()
        connection.__enter__.return_value = connection
        connection.recv.side_effect = [
            b"200-ICECC 1.4.90: 0s uptime\n200 Use 'help'",
            b" for help and 'quit' to quit.\n",
            b" c-nas642-00 (10.0.27.127:0) []\n",
            b" f-quietbox2-00 (10.0.27.212:12000) []\n200 done\n",
        ]
        with mock.patch.object(farm.socket, "create_connection", return_value=connection) as connect:
            snapshot = farm.scheduler_snapshot(self.manifest, plan)
        connect.assert_called_once_with(("10.0.27.127", 8766), timeout=3)
        connection.settimeout.assert_called_once_with(3)
        self.assertEqual(connection.sendall.call_args_list, [mock.call(b"listcs\n"), mock.call(b"quit\n")])
        self.assertIn("c-nas642-00", snapshot)
        self.assertIn("f-quietbox2-00", snapshot)

    def test_scheduler_route_requires_lan_source_and_interface(self):
        self.assertIsNone(
            farm.scheduler_route_error(
                "10.0.27.127 via 10.0.27.1 dev ens34 src 10.0.27.56 uid 1000",
                "ens34",
                "10.0.27.56",
                scheduler_is_local=False,
            )
        )
        self.assertRegex(
            farm.scheduler_route_error(
                "10.0.27.127 dev tailscale0 src 100.88.1.2",
                "ens34",
                "10.0.27.56",
                scheduler_is_local=False,
            ),
            "overlay",
        )
        self.assertIsNone(
            farm.scheduler_route_error(
                "local 10.0.27.127 dev lo src 10.0.27.127 uid 1000",
                "ens160",
                "10.0.27.127",
                scheduler_is_local=True,
            )
        )

    def test_every_c_f_pair_requires_both_explicit_lan_routes(self):
        plan = self.plan("c1f4")
        address_to_name = {
            host["address"]: name for name, host in self.manifest["hosts"].items()
        }

        def route(self_runner, argv, **_kwargs):
            target_address = argv[-1]
            self.assertIn(target_address, address_to_name)
            host = self.manifest["hosts"][self_runner.name]
            output = (
                f"{target_address} via 10.0.27.1 dev {host['interface']} "
                f"src {host['address']} uid 1000\n"
            ).encode()
            return subprocess_result(0, stdout=output)

        with mock.patch.object(farm.HostRunner, "run", autospec=True, side_effect=route):
            snapshot = farm.peer_route_snapshot(self.manifest, plan)
        self.assertEqual(len(snapshot["routes"]), 8)
        self.assertEqual(
            {(row["direction"], row["source_host"], row["target_host"])
             for row in snapshot["routes"]},
            {
                (direction, source, target)
                for worker in plan["workers"]
                for direction, source, target in (
                    ("c-to-f", "nas642", worker["host"]),
                    ("f-to-c", worker["host"], "nas642"),
                )
            },
        )

    def test_peer_route_snapshot_refuses_wrong_interface_before_acceptance(self):
        plan = self.plan("c1f1")

        def route(self_runner, argv, **_kwargs):
            target_address = argv[-1]
            host = self.manifest["hosts"][self_runner.name]
            interface = "tailscale0" if self_runner.name == "nas642" else host["interface"]
            source = "100.88.1.2" if self_runner.name == "nas642" else host["address"]
            return subprocess_result(
                0,
                stdout=(
                    f"{target_address} dev {interface} src {source} uid 1000\n"
                ).encode(),
            )

        with mock.patch.object(farm.HostRunner, "run", autospec=True, side_effect=route):
            with self.assertRaisesRegex(farm.FarmError, "c-to-f.*LAN route failed"):
                farm.peer_route_snapshot(self.manifest, plan)

    def test_preflight_cannot_pass_when_a_c_f_route_fails(self):
        plan = self.plan("c1f1")
        host_report = {
            "ok": True,
            "errors": [],
            "facts": {"online_cpus": "32", "load": "0 0 0"},
            "images": [],
            "runtime_binaries": {},
        }
        with mock.patch.object(farm, "inspect_host", return_value=host_report):
            with mock.patch.object(
                farm,
                "peer_route_snapshot",
                side_effect=farm.FarmError("c-to-f LAN route failed"),
            ):
                report = farm.preflight(self.manifest, plan)
        self.assertFalse(report["ok"])
        self.assertEqual(report["peer_routes"]["status"], "failed")
        self.assertIn("c-to-f LAN route failed", report["errors"])

    def test_directional_network_ledger_keeps_capacity_provenance(self):
        plan = self.plan("c1f1")
        before = {
            "captured_at": "before",
            "monotonic_ns": 1_000_000_000,
            "hosts": {
                "nas642": {"rx_bytes": 100, "tx_bytes": 200},
                "quietbox2": {"rx_bytes": 300, "tx_bytes": 400},
            },
        }
        after = {
            "captured_at": "after",
            "monotonic_ns": 3_000_000_000,
            "hosts": {
                "nas642": {"rx_bytes": 130, "tx_bytes": 250},
                "quietbox2": {"rx_bytes": 370, "tx_bytes": 490},
            },
        }
        route_snapshot = {
            "checked_at": "route-check",
            "network_mode": "local-lan",
            "routes": [
                {"direction": "c-to-f", "ok": True},
                {"direction": "f-to-c", "ok": True},
            ],
        }
        ledger = farm.directional_network_ledger(
            self.manifest, plan, before, after, route_snapshot, route_snapshot
        )
        self.assertEqual(ledger["elapsed_ns"], 2_000_000_000)
        self.assertEqual(ledger["routes"][0]["c_to_f"]["c_interface_tx"], 50)
        self.assertEqual(ledger["routes"][0]["f_to_c"]["f_interface_tx"], 90)
        self.assertEqual(
            ledger["client_capacity"][0]["class"],
            "naturally-bandwidth-limited",
        )
        self.assertIn("not per-flow", ledger["attribution"])
        self.assertEqual(len(ledger["route_checks"]["before"]["routes"]), 2)
        broken_routes = copy.deepcopy(route_snapshot)
        broken_routes["routes"][0]["ok"] = False
        with self.assertRaisesRegex(farm.FarmError, "refuses"):
            farm.directional_network_ledger(
                self.manifest, plan, before, after, broken_routes, route_snapshot
            )

    def test_absent_container_requires_reachable_docker_engine(self):
        runner = farm.HostRunner("nas642", self.manifest["hosts"]["nas642"])
        failed = subprocess_result(1, stderr=b"cannot connect")
        with mock.patch.object(farm, "docker_command", side_effect=[failed, failed]):
            with self.assertRaisesRegex(farm.FarmError, "cannot verify"):
                farm.exact_remove_container(runner, "run-a", "container-a")
        absent = subprocess_result(0, stdout=b"")
        with mock.patch.object(farm, "docker_command", side_effect=[failed, absent]):
            self.assertEqual(
                farm.exact_remove_container(runner, "run-a", "container-a")["result"],
                "absent",
            )

    def test_run_state_is_idempotently_ready_or_absent(self):
        plan = self.plan("c1f1")
        run = {"manifest": self.manifest, "plan": plan}
        running = subprocess_result(
            0, stdout=f"{plan['run_id']}|running\n".encode()
        )
        snapshot = (
            " c-nas642-00 (10.0.27.127:0) jobs=0/0\n"
            " f-quietbox2 (10.0.27.212:12000) jobs=0/24\n200 done\n"
        )
        with mock.patch.object(farm, "docker_command", return_value=running):
            with mock.patch.object(farm, "scheduler_snapshot", return_value=snapshot):
                self.assertEqual(farm.inspect_run_state(run)["state"], "ready")

        absent_inspect = subprocess_result(1, stderr=b"No such container")
        absent_probe = subprocess_result(0, stdout=b"")
        with mock.patch.object(
            farm, "docker_command", side_effect=[
                absent_inspect, absent_probe,
                absent_inspect, absent_probe,
                absent_inspect, absent_probe,
            ]
        ):
            self.assertEqual(farm.inspect_run_state(run)["state"], "absent")

    def test_reconcile_absent_run_records_down_without_removing_anything(self):
        plan = self.plan("c1f1")
        run = {"manifest": self.manifest, "plan": plan, "status": "ready"}
        absent = {
            "checked_at": "now",
            "run_id": plan["run_id"],
            "state": "absent",
            "containers": [],
            "scheduler_snapshot": None,
            "missing_registrations": [],
        }
        with tempfile.TemporaryDirectory() as directory:
            run_dir = Path(directory) / plan["run_id"]
            run_dir.mkdir()
            args = argparse.Namespace(
                run_dir=str(run_dir), desired="down", no_collect=False
            )
            with mock.patch.object(farm, "load_run", return_value=run):
                with mock.patch.object(farm, "inspect_run_state", return_value=absent):
                    with mock.patch.object(farm, "teardown_run") as teardown:
                        with mock.patch.object(farm, "exact_remove_container") as remove:
                            self.assertEqual(farm.command_reconcile(args), 0)
            teardown.assert_not_called()
            remove.assert_not_called()
            persisted = json.loads((run_dir / "run.json").read_text(encoding="utf-8"))
            self.assertEqual(persisted["status"], "down")
            self.assertEqual(persisted["reconciliation"]["observed"], "absent")
            self.assertFalse(
                persisted["reconciliation"]["unrelated_resources_touched"]
            )


class ArtifactTests(unittest.TestCase):
    def test_node_image_is_not_a_fifth_build_environment(self):
        dockerfile = (HERE / "Dockerfile.node").read_text(encoding="utf-8")
        self.assertIn('image-class="daemon-runtime"', dockerfile)
        self.assertIn("gcc-11", dockerfile)
        self.assertIn("BOOST_LIB_VERSION", dockerfile)
        self.assertNotIn("PROFILE_NAME", dockerfile)

    def test_acceptance_requires_remote_assignment_and_exact_output(self):
        script = (HERE / "acceptance.sh").read_text(encoding="utf-8")
        self.assertIn("Have to use host .* - Job ID: [0-9]+", script)
        self.assertIn("icecream-farm-ok 42", script)
        self.assertIn("object_sha256", script)
        self.assertIn("executable_sha256", script)
        self.assertIn("selected_worker_endpoint", script)
        self.assertIn("-std=c++23", script)

    def test_named_topology_entrypoints_cover_both_clients_and_slot_profiles(self):
        expected = {
            "run-nas-c1f1": ("c1f1", "conservative_nas_submitter"),
            "run-nas-c1f2": ("c1f2", "conservative_nas_submitter"),
            "run-nas-c1f4": ("c1f4", "conservative_nas_submitter"),
            "run-q2-c1f1": ("c1f1", "conservative_q2_submitter"),
            "run-q2-c1f2": ("c1f2", "conservative_q2_submitter"),
            "run-q2-c1f3": ("c1f3", "conservative_q2_submitter"),
            "run-nas-c1f4-max32": ("c1f4", "max32_nas_submitter"),
            "run-q2-c1f3-max32": ("c1f3", "max32_q2_submitter"),
        }
        for name, (scenario, profile) in expected.items():
            with self.subTest(name=name):
                script = (HERE / name).read_text(encoding="utf-8")
                self.assertIn(f"--scenario {scenario}", script)
                self.assertIn(f"--profile {profile}", script)
        self.assertFalse((HERE / "run-c1f20").exists())

    def test_inventory_records_current_blocks(self):
        inventory = json.loads(
            (HERE / "inventory" / "2026-08-21-local-lan.json").read_text(encoding="utf-8")
        )
        self.assertEqual(inventory["wan_exclusions"]["default_manifest_wan_hosts"], [])
        self.assertEqual(inventory["environment_classes"]["verified_cells"].split()[0], "44")
        self.assertFalse(inventory["hosts"]["research7"]["docker_permission"])
        self.assertIn("authentication rejected", inventory["hosts"]["quietbox3"]["ssh"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
