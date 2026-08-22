#!/usr/bin/env python3

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

    def test_shared_group_capacity_is_not_tripled(self):
        profile = self.manifest["profiles"]["local_80_nas_submitter"]
        self.assertEqual(profile["advertised_worker_slots"], 80)
        self.assertEqual(profile["worker_slots"]["research6"], 16)
        self.assertEqual(profile["worker_slots"]["research7"], 16)
        shared = self.manifest["resource_groups"]["nas-r6-r7-shared"]
        self.assertEqual(shared["aggregate_worker_cap"], 32)
        self.assertEqual(shared["owner_estimated_physical_cpus"], 64)

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

    def plan(self, scenario="c1f20", profile="local_80_nas_submitter", label="gate-a"):
        return farm.build_plan(
            self.manifest, profile, scenario, "debian-gcc", label
        )

    def test_deterministic_run_identity(self):
        first = self.plan()
        second = self.plan()
        changed = self.plan(label="gate-b")
        self.assertEqual(first["run_id"], second["run_id"])
        self.assertNotEqual(first["run_id"], changed["run_id"])
        self.assertRegex(first["run_id"], r"^p50-c1f20-[0-9a-f]{12}$")
        two_submitters = farm.build_plan(
            self.manifest,
            "local_80_nas_submitter",
            "c1f20",
            "debian-gcc",
            "gate-a",
            ["nas642", "nas642"],
        )
        self.assertNotEqual(first["run_id"], two_submitters["run_id"])

    def test_c1f1_c1f2_c1f20_counts(self):
        self.assertEqual(len(self.plan("c1f1")["workers"]), 1)
        self.assertEqual(len(self.plan("c1f2")["workers"]), 2)
        plan20 = self.plan("c1f20")
        self.assertEqual(len(plan20["workers"]), 20)
        self.assertEqual(
            plan20["accounting"]["worker_containers_by_host"],
            {"quietbox2": 5, "research6": 5, "quietbox3": 5, "research7": 5},
        )
        self.assertEqual(plan20["accounting"]["independent_resource_groups"], 3)

    def test_full_profile_keeps_declared_80(self):
        plan = self.plan("c1f80")
        self.assertEqual(len(plan["workers"]), 80)
        self.assertEqual(
            plan["accounting"]["worker_containers_by_host"],
            {"quietbox2": 24, "research6": 16, "quietbox3": 24, "research7": 16},
        )
        self.assertEqual(
            plan["accounting"]["worker_containers_by_resource_group"]["nas-r6-r7-shared"],
            32,
        )

    def test_q2_submitter_profile_excludes_q2_workers(self):
        plan = self.plan("c1f20", "local_56_q2_submitter")
        self.assertEqual(plan["submitters"][0]["host"], "quietbox2")
        self.assertNotIn("quietbox2", plan["accounting"]["worker_containers_by_host"])
        self.assertEqual(plan["accounting"]["advertised_profile_worker_slots"], 56)

    def test_research6_recovery_profile_uses_research6_only(self):
        plan = self.plan("c1f1", "local_16_r6_smoke")
        self.assertEqual(plan["submitters"][0]["host"], "nas642")
        self.assertEqual(plan["accounting"]["worker_containers_by_host"], {"research6": 1})

    def test_submitter_override_cannot_hide_q2_double_duty(self):
        with self.assertRaisesRegex(farm.FarmError, "retains worker slots"):
            farm.build_plan(
                self.manifest,
                "local_80_nas_submitter",
                "c1f1",
                "debian-gcc",
                "override",
                ["quietbox2"],
            )

    def test_multiple_submitters_are_explicit(self):
        plan = farm.build_plan(
            self.manifest,
            "local_80_nas_submitter",
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
        self.assertEqual(nas["services"]["c00"]["image"], "ice-ii/debian-gcc:v2")
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

        q2_submitter = self.plan("c1f1", "local_56_q2_submitter")
        self.assertEqual(
            farm.required_listener_ports(self.manifest, q2_submitter, "quietbox2"),
            [],
        )

    def test_cluster_registration_requires_exact_planned_endpoint(self):
        plan = self.plan("c1f1")
        retained_snapshot = (
            " c-nas642-00 (10.0.27.127:0) [x86_64] "
            "speed=0.00 jobs=0/0 load=408\n"
            " f-quietbox2-00 (10.0.27.212:12000) [x86_64] "
            "speed=0.00 jobs=0/1 load=1000\n"
            "200 done\n"
        )
        self.assertEqual(
            farm.expected_registrations(self.manifest, plan),
            [
                "c-nas642-00 (10.0.27.127:0)",
                "f-quietbox2-00 (10.0.27.212:12000)",
            ],
        )
        self.assertEqual(
            farm.missing_registrations(self.manifest, plan, retained_snapshot), []
        )
        misleading_submitter_port = (
            " c-nas642-00 (10.0.27.127:14000) [x86_64] "
            "speed=0.00 jobs=0/0 load=408\n"
            " f-quietbox2-00 (10.0.27.212:12000) [x86_64] "
            "speed=0.00 jobs=0/1 load=1000\n"
            "200 done\n"
        )
        self.assertEqual(
            farm.missing_registrations(
                self.manifest, plan, misleading_submitter_port
            ),
            ["c-nas642-00 (10.0.27.127:0)"],
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

    def test_uninventoried_q3_transport_is_unavailable(self):
        host = self.manifest["hosts"]["quietbox3"]
        self.assertEqual(farm.HostRunner("quietbox3", host).available()[0], False)

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
