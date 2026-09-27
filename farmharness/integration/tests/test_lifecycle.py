from __future__ import annotations

import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path

import pytest

from farmharness.integration.tests import farm_fixture

from farmharness.integration import farmtest
from farmharness.integration.farm_spec import load_farm_spec
from farmharness.integration.images import (
    CommandFactory,
    RecordingTransport,
    _image_identity,
)
from farmharness.integration.lifecycle import (
    CANARY_SCRIPT,
    HOST_PREFLIGHT_SCRIPT,
    LifecycleError,
    MIN_FREE_BYTES,
    PreflightRefusal,
    ROLE_BINARY_PATHS,
    bring_up,
    bundle_root,
    corpus_layout,
    down_from_state,
    _run_canaries,
    _rotate_s30_canary_traces,
    _expected_image_labels,
    _diagnostic_client_output_patterns,
    _host_facts,
)
from farmharness.integration.remote import (
    CommandResult,
    PlannedCommand,
    RemoteError,
    decode_ssh_payload,
)
from farmharness.integration.scenario_spec import ScenarioSpecError, load_scenario_spec


INTEGRATION = Path(__file__).resolve().parents[1]
NATIVE_ID = "sha256:" + "1" * 64


class Clock:
    def __init__(self) -> None:
        self.value = 0.0

    def __call__(self) -> float:
        return self.value

    def sleep(self, seconds: float) -> None:
        self.value += seconds


def _inspect_document(farm, label: str) -> dict[str, object]:
    authority = farm.data["authority"]["images"][label]
    return {
        "Architecture": "amd64",
        "Config": {
            "Env": ["PATH=/usr/bin"],
            "Labels": _expected_image_labels(authority),
        },
        "Created": "2026-09-04T00:00:00Z",
        "Id": NATIVE_ID,
        "Os": "linux",
        "RootFS": {"Layers": ["sha256:" + "2" * 64], "Type": "layers"},
        "Size": 500_000_000,
    }


def test_mutant_image_labels_bind_base_source_and_mutant_recipe() -> None:
    farm = load_farm_spec(farm_fixture.example_farm_path())
    authority = farm.data["authority"]["images"]["p50s4-h3-tail-mutant"]

    assert _expected_image_labels(authority) == {
        "icefarm.source.archive_sha256": authority["base_archive_sha256"],
        "icefarm.source.commit": authority["base_commit"],
        "icefarm.mutant.patch_sha256": authority["patch_sha256"],
        "icefarm.mutant.recipe_sha256": authority["recipe_sha256"],
    }


def _farm_scenario_plan(tmp_path: Path, *, up_s: int = 5):
    farm = load_farm_spec(farm_fixture.example_farm_path())
    farm.data["hub"]["results_root"] = str(tmp_path)
    scenario = load_scenario_spec(INTEGRATION / "scenarios" / "S00-smoke.json", farm)
    label = scenario.data["images"]["new"]
    document = _inspect_document(farm, label)
    identity = _image_identity(CommandResult(0, json.dumps(document), ""), "test image")
    farm.data["authority"]["images"][label]["closure_sha256"] = identity.closure_sha256
    farm.data["runtime_image"]["closure_sha256"] = identity.closure_sha256
    for environment in farm.data["client_environments"].values():
        environment["closure_sha256"] = identity.closure_sha256
    scenario.data["timeouts"]["up_s"] = up_s
    plan = farmtest.build_plan(farm, scenario, run_id="unit-run")
    return farm, scenario, plan


def _run_host_preflight_script(
    tmp_path: Path,
    *,
    mount: dict[str, str],
    mount_document: object | None = None,
    findmnt_output: str | None = None,
    zfs_listing: str = "",
    zpool_status: str = "",
    rotations: dict[str, list[str]] | None = None,
):
    fake_bin = tmp_path / "fake-bin"
    fake_bin.mkdir()
    config_path = tmp_path / "commands.json"
    config_path.write_text(
        json.dumps({
            "mount": mount,
            "mount_document": mount_document,
            "findmnt_output": findmnt_output,
            "zfs_listing": zfs_listing,
            "zpool_status": zpool_status,
            "rotations": rotations or {},
        }),
        encoding="utf-8",
    )
    command = fake_bin / "probe-command"
    command.write_text(
        f"#!{sys.executable}\n"
        "import json, os, pathlib, sys\n"
        "cfg = json.load(open(os.environ['ICEFARM_PREFLIGHT_FIXTURE']))\n"
        "name = pathlib.Path(sys.argv[0]).name\n"
        "if name == 'findmnt':\n"
        " if cfg['findmnt_output'] is not None:\n"
        "  sys.stdout.write(cfg['findmnt_output'])\n"
        " else:\n"
        "  document = cfg['mount_document']\n"
        "  if document is None: document = {'filesystems': [cfg['mount']]}\n"
        "  print(json.dumps(document))\n"
        "elif name == 'zfs':\n"
        " print(cfg['zfs_listing'], end='')\n"
        "elif name == 'zpool':\n"
        " print(cfg['zpool_status'], end='')\n"
        "elif name == 'lsblk':\n"
        " print('\\n'.join(cfg['rotations'].get(sys.argv[-1], [])))\n"
        "else:\n"
        " raise SystemExit(64)\n",
        encoding="utf-8",
    )
    command.chmod(0o755)
    for name in ("findmnt", "zfs", "zpool", "lsblk"):
        (fake_bin / name).symlink_to(command)
    scratch = tmp_path / "mounted" / "scratch"
    scratch.mkdir(parents=True)
    environment = os.environ.copy()
    environment.update(
        {
            "ICEFARM_PREFLIGHT_FIXTURE": str(config_path),
            "PATH": str(fake_bin) + os.pathsep + environment.get("PATH", "/usr/bin:/bin"),
        }
    )
    return subprocess.run(
        [
            sys.executable,
            "-c",
            HOST_PREFLIGHT_SCRIPT,
            str(scratch),
            "unit-run",
            "[]",
            "0",
        ],
        capture_output=True,
        text=True,
        env=environment,
        check=False,
    )


def _zpool_status(rows: str, *, state: str = "ONLINE") -> str:
    return (
        "  pool: tank\n"
        f" state: {state}\n"
        "config:\n\n"
        "        NAME        STATE     READ WRITE CKSUM\n"
        f"        tank        {state}       0     0     0\n"
        f"{rows}"
        "\nerrors: No known data errors\n"
    )


def test_host_preflight_preserves_direct_block_device_path(tmp_path: Path) -> None:
    result = _run_host_preflight_script(
        tmp_path,
        mount={
            "source": "/dev/nvme0n1p2",
            "fstype": "ext4",
            "target": str(tmp_path / "mounted"),
        },
        rotations={"/dev/nvme0n1p2": ["0"]},
    )

    assert result.returncode == 0, result.stderr
    facts = json.loads(result.stdout)
    assert facts["device"] == "/dev/nvme0n1p2"
    assert facts["filesystem"] == "ext4"
    assert facts["backing_devices"] == ["/dev/nvme0n1p2"]
    assert facts["rotational"] is False


@pytest.mark.parametrize(
    ("mount_document", "findmnt_output", "message"),
    [
        ({}, None, "ambiguous"),
        ({"filesystems": []}, None, "ambiguous"),
        ({"filesystems": [{"source": "tank/scratch"}]}, None, "incomplete"),
        (None, "not-json\n", "malformed"),
    ],
)
def test_host_preflight_refuses_malformed_or_missing_mount_identity(
    tmp_path: Path,
    mount_document: object | None,
    findmnt_output: str | None,
    message: str,
) -> None:
    result = _run_host_preflight_script(
        tmp_path,
        mount={},
        mount_document=mount_document,
        findmnt_output=findmnt_output,
    )

    assert result.returncode != 0
    assert message in result.stderr


def test_host_preflight_accepts_only_fully_verified_nonrotational_zfs_pool(
    tmp_path: Path,
) -> None:
    mount_target = str(tmp_path / "mounted")
    result = _run_host_preflight_script(
        tmp_path,
        mount={"source": "tank/scratch", "fstype": "zfs", "target": mount_target},
        zfs_listing=f"tank/scratch\t{mount_target}\n",
        zpool_status=_zpool_status(
            "          /dev/sdb1  ONLINE       0     0     0\n"
            "          /dev/sdd1  ONLINE       0     0     0\n"
        ),
        rotations={"/dev/sdb1": ["0"], "/dev/sdd1": ["0"]},
    )

    assert result.returncode == 0, result.stderr
    facts = json.loads(result.stdout)
    assert facts["device"] == "tank/scratch"
    assert facts["filesystem"] == "zfs"
    assert facts["mount_target"] == mount_target
    assert facts["backing_devices"] == ["/dev/sdb1", "/dev/sdd1"]
    assert facts["rotational"] is False


@pytest.mark.parametrize(
    ("mount", "zfs_listing", "zpool_status", "rotations", "message"),
    [
        (
            {"source": "tank/scratch", "fstype": "zfs", "target": "/wrong"},
            "tank/scratch\t/wrong\n",
            _zpool_status("          /dev/sdb1 ONLINE 0 0 0\n"),
            {"/dev/sdb1": ["0"]},
            "not contained",
        ),
        (
            {"source": "tank/scratch", "fstype": "zfs", "target": "MOUNT"},
            "tank/scratch\tDIFFERENT\n",
            _zpool_status("          /dev/sdb1 ONLINE 0 0 0\n"),
            {"/dev/sdb1": ["0"]},
            "does not own",
        ),
        (
            {"source": "tank/scratch", "fstype": "zfs", "target": "MOUNT"},
            "tank/scratch\tMOUNT\n",
            _zpool_status("          /dev/sdb1 ONLINE 0 0 0\n", state="DEGRADED"),
            {"/dev/sdb1": ["0"]},
            "not ONLINE",
        ),
        (
            {"source": "tank/scratch", "fstype": "zfs", "target": "MOUNT"},
            "tank/scratch\tMOUNT\n",
            _zpool_status(
                "          mirror-0 ONLINE 0 0 0\n"
                "            /dev/sdb1 ONLINE 0 0 0\n"
                "            /dev/sdd1 ONLINE 0 0 0\n"
            ),
            {"/dev/sdb1": ["0"], "/dev/sdd1": ["0"]},
            "unsupported",
        ),
        (
            {"source": "tank/scratch", "fstype": "zfs", "target": "MOUNT"},
            "tank/scratch\tMOUNT\n",
            _zpool_status(
                "          /dev/sdb1 ONLINE 0 0 0\n\n"
                "        special\n"
                "          /dev/sdd1 ONLINE 0 0 0\n"
            ),
            {"/dev/sdb1": ["0"], "/dev/sdd1": ["0"]},
            "unsupported vdev layout",
        ),
        (
            {"source": "tank/scratch", "fstype": "zfs", "target": "MOUNT"},
            "tank/scratch\tMOUNT\n",
            _zpool_status(
                "          /dev/sdb1 ONLINE 0 0 0\n\n"
                "          /dev/sdd1 ONLINE 0 0 0\n"
            ),
            {"/dev/sdb1": ["0"], "/dev/sdd1": ["1"]},
            "rotation",
        ),
    ],
)
def test_host_preflight_refuses_ambiguous_or_unsafe_zfs_layouts(
    tmp_path: Path,
    mount: dict[str, str],
    zfs_listing: str,
    zpool_status: str,
    rotations: dict[str, list[str]],
    message: str,
) -> None:
    if mount["target"] == "MOUNT":
        mount = {**mount, "target": str(tmp_path / "mounted")}
        zfs_listing = zfs_listing.replace("\tMOUNT\n", f"\t{mount['target']}\n")
    result = _run_host_preflight_script(
        tmp_path,
        mount=mount,
        zfs_listing=zfs_listing,
        zpool_status=zpool_status,
        rotations=rotations,
    )

    if message == "rotation":
        assert result.returncode == 0, result.stderr
        facts = json.loads(result.stdout)
        assert facts["backing_devices"] == ["/dev/sdb1", "/dev/sdd1"]
        assert facts["rotational"] is True
    else:
        assert result.returncode != 0
        assert message in result.stderr


def test_host_facts_refuses_rotational_zfs_leaf(tmp_path: Path) -> None:
    farm, _scenario, _plan = _farm_scenario_plan(tmp_path)
    document = {
        "backing_devices": ["/dev/sdb1", "/dev/sdd1"],
        "device": "tank/scratch",
        "filesystem": "zfs",
        "free_bytes": 100_000_000_000,
        "mount_target": "/tanksmall/scratch",
        "protected": {"bigfarm": 0},
        "rotational": True,
        "scratch_root": farm.hosts["tt-quietbox3"]["scratch_root"],
        "write_bps": None,
    }

    class FactsRecorder:
        def invoke(self, command):
            assert command.phase == "preflight.host"
            return CommandResult(0, json.dumps(document), "")

    with pytest.raises(PreflightRefusal, match="scratch backing device is rotational"):
        _host_facts(
            farm,
            "tt-quietbox3",
            "unit-run",
            FactsRecorder(),
            CommandFactory(),
            timeout_s=1,
            probe_bytes=0,
        )


def test_host_facts_accepts_verified_zfs_identity(tmp_path: Path) -> None:
    farm, _scenario, _plan = _farm_scenario_plan(tmp_path)
    document = {
        "backing_devices": ["/dev/sdb1", "/dev/sdd1"],
        "device": "tank/scratch",
        "filesystem": "zfs",
        "free_bytes": 100_000_000_000,
        "mount_target": "/tanksmall/scratch",
        "protected": {"bigfarm": 0},
        "rotational": False,
        "scratch_root": farm.hosts["tt-quietbox3"]["scratch_root"],
        "write_bps": 900_000_000,
    }

    class FactsRecorder:
        def invoke(self, command):
            assert command.phase == "preflight.host"
            return CommandResult(0, json.dumps(document), "")

    facts = _host_facts(
        farm,
        "tt-quietbox3",
        "unit-run",
        FactsRecorder(),
        CommandFactory(),
        timeout_s=1,
        probe_bytes=1,
    )
    assert facts["filesystem"] == "zfs"
    assert facts["device"] == "tank/scratch"
    assert facts["backing_devices"] == ["/dev/sdb1", "/dev/sdd1"]


class ScriptedLifecycle:
    def __init__(
        self,
        farm,
        *,
        scheduler_interrupt: bool = False,
        scheduler_timeout: bool = False,
        stale: bool = False,
        free_bytes: int = 100_000_000_000,
        bad_role_hash: bool = False,
        client_cache_timeout: bool = False,
        fail_output_capture: bool = False,
    ) -> None:
        self.farm = farm
        self.scheduler_interrupt = scheduler_interrupt
        self.scheduler_timeout = scheduler_timeout
        self.free_bytes = free_bytes
        self.bad_role_hash = bad_role_hash
        self.client_cache_timeout = client_cache_timeout
        self.fail_output_capture = fail_output_capture
        runtime_closure = farm.data["runtime_image"]["closure_sha256"]
        matching_labels = [
            label
            for label, authority in farm.data["authority"]["images"].items()
            if authority.get("closure_sha256") == runtime_closure
        ]
        assert len(matching_labels) == 1
        self.product_label = matching_labels[0]
        self.protected_count = 1
        self.remote_archives: set[str] = set()
        self.containers: dict[str, dict[str, str]] = {}
        self.receipt_networks: dict[str, dict[str, object]] = {}
        if stale:
            self.containers["aaaaaaaaaaaa"] = {
                "created": "2026-09-01T00:00:00Z",
                "host": "tt-quietbox3",
                "name": "icefarm-old-run-S1",
                "run_id": "old-run",
            }

    def _containers_on(self, host: str) -> list[str]:
        return [key for key, value in self.containers.items() if value["host"] == host]

    def invoke(self, command: PlannedCommand) -> CommandResult:
        if command.phase == "diagnostics.sync-output" and self.fail_output_capture:
            raise RemoteError("injected output capture failure")
        if command.phase == "preflight.stale-list":
            return CommandResult(
                0, "\n".join(self._containers_on(command.host)) + "\n", ""
            )
        if command.phase == "preflight.stale-inspect":
            item = self.containers[command.argv[-1]]
            return CommandResult(
                0,
                json.dumps(
                    {
                        "Config": {"Labels": {"icefarm.run": item["run_id"]}},
                        "Created": item["created"],
                        "Name": "/" + item["name"],
                    }
                ),
                "",
            )
        if command.phase == "preflight.host":
            return CommandResult(
                0,
                json.dumps(
                    {
                        "backing_devices": ["/dev/nvme0n1p2"],
                        "device": "/dev/nvme0n1p2",
                        "filesystem": "ext4",
                        "free_bytes": self.free_bytes,
                        "mount_target": "/scratch",
                        "protected": {"bigfarm": self.protected_count},
                        "rotational": False,
                        "scratch_root": self.farm.hosts[command.host]["scratch_root"],
                        "write_bps": 900_000_000,
                    }
                ),
                "",
            )
        if command.phase == "preflight.image":
            label = command.argv[-1].rsplit(":", 1)[-1]
            return CommandResult(0, json.dumps(_inspect_document(self.farm, label)), "")
        if command.phase == "preflight.container-image":
            return CommandResult(
                0,
                json.dumps(_inspect_document(self.farm, self.product_label)),
                "",
            )
        if command.phase == "preflight.runtime-materialize":
            return CommandResult(
                0,
                f"materialized /icefarm-runtimes/{command.argv[-1]}\n",
                "",
            )
        if command.phase == "preflight.role-hashes":
            binary_names = {"S": "scheduler", "C": "client", "F": "daemon"}
            rows = []
            for role, path in ROLE_BINARY_PATHS.items():
                if path not in command.argv:
                    continue
                digest = self.farm.data["authority"]["role_stores"]["50"][
                    binary_names[role]
                ]["sha256"]
                if self.bad_role_hash and not rows:
                    digest = "f" * 64
                rows.append(f"{digest}  {path}")
            return CommandResult(0, "\n".join(rows) + "\n", "")
        if command.phase == "preflight.compiler":
            return CommandResult(
                0,
                f"compiler-ok {command.argv[-3]} {command.argv[-2]}\n",
                "",
            )
        if command.phase == "preflight.corpus-archive-verify":
            remote = decode_ssh_payload(command.argv)
            digest = remote[-2]
            if digest in self.remote_archives:
                return CommandResult(
                    0,
                    json.dumps(
                        {
                            "bytes": int(remote[-1]),
                            "ready": True,
                            "reason": "ready",
                            "sha256": digest,
                        }
                    ),
                    "",
                )
            return CommandResult(0, '{"ready": false, "reason": "absent"}\n', "")
        if command.phase == "preflight.corpus-archive-sync":
            filename = command.argv[-1].rsplit("/", 1)[-1]
            self.remote_archives.add(filename.removesuffix(".tar.zst"))
            return CommandResult(0, "", "")
        if command.phase == "run.corpus-clear":
            return CommandResult(0, f"cleared {command.argv[-1]}\n", "")
        if command.phase == "run.corpus-materialize":
            return CommandResult(0, "materialized /icefarm-corpus-cache/active\n", "")
        if command.phase == "run.corpus-activate":
            return CommandResult(0, f"activated {command.argv[-5]}\n", "")
        if command.phase == "preflight.stale-reap":
            self.containers.pop(command.argv[-1])
            return CommandResult(0, "", "")
        if command.phase == "up.receipt-network-create":
            labels = {}
            for index, argument in enumerate(command.argv[:-1]):
                if argument == "--label":
                    key, value = command.argv[index + 1].split("=", 1)
                    labels[key] = value
            name = command.argv[-1]
            run_id = labels["icefarm.run"]
            intent = json.loads(
                (bundle_root(self.farm, run_id) / "lifecycle.json").read_text(
                    encoding="utf-8"
                )
            )["receipt_client_networks"]
            assert intent["status"] == "CREATING"
            assert intent["bindings"][0]["bridge"]["network_id"] is None
            identifier = hashlib.sha256(name.encode()).hexdigest()
            self.receipt_networks[identifier] = {
                "name": name,
                "labels": labels,
                "instance": command.instance,
            }
            return CommandResult(0, identifier + "\n", "")
        if command.phase.startswith("up.start-"):
            if command.phase == "up.start-s" and self.receipt_networks:
                run_id = next(
                    command.argv[index + 1].split("=", 1)[1]
                    for index, argument in enumerate(command.argv[:-1])
                    if argument == "--label"
                    and command.argv[index + 1].startswith("icefarm.run=")
                )
                starting = json.loads(
                    (bundle_root(self.farm, run_id) / "lifecycle.json").read_text(
                        encoding="utf-8"
                    )
                )
                receipt_networks = starting.get("receipt_client_networks")
                if receipt_networks is not None:
                    assert receipt_networks["status"] == "CREATED"
                    assert all(
                        row["bridge"]["network_id"] in self.receipt_networks
                        for row in receipt_networks["bindings"]
                    )
            name = command.argv[command.argv.index("--name") + 1]
            run_label = command.argv[command.argv.index("--label") + 1].split("=", 1)[1]
            identifier = f"{len(self.containers) + 1:012x}"
            self.containers[identifier] = {
                "created": "2026-09-04T00:00:00Z",
                "host": command.host,
                "name": name,
                "run_id": run_label,
                "network": command.argv[command.argv.index("--network") + 1],
            }
            return CommandResult(0, identifier + "\n", "")
        if command.phase == "readiness.receipt-network-inspect":
            network_id = command.argv[-1]
            value = self.receipt_networks[network_id]
            return CommandResult(
                0,
                json.dumps({
                    "Id": network_id,
                    "Name": value["name"],
                    "Driver": "bridge",
                    "Labels": value["labels"],
                }),
                "",
            )
        if command.phase == "readiness.receipt-client-network":
            container = next(
                value for value in self.containers.values()
                if value["name"] == command.argv[-1]
            )
            network_id = next(
                key for key, value in self.receipt_networks.items()
                if value["name"] == container["network"]
            )
            network = self.receipt_networks[network_id]
            return CommandResult(
                0,
                json.dumps({
                    "Name": "/" + str(container["name"]),
                    "Config": {"Labels": {
                        "icefarm.run": container["run_id"],
                        "icefarm.instance": command.instance,
                    }},
                    "State": {"Running": True},
                    "HostConfig": {"NetworkMode": container["network"]},
                    "NetworkSettings": {"Networks": {
                        container["network"]: {
                            "NetworkID": network_id,
                            "IPAddress": "172.28.0.2",
                        }
                    }},
                }),
                "",
            )
        if command.phase == "readiness.listcs":
            if self.scheduler_interrupt:
                raise KeyboardInterrupt
            if self.scheduler_timeout:
                raise RemoteError("scheduler unavailable")
            return CommandResult(0, " F1 x86_64 jobs=0/24\n", "")
        if command.phase == "readiness.container":
            return CommandResult(
                0,
                json.dumps(
                    {"Error": "", "ExitCode": 0, "Running": True, "Status": "running"}
                ),
                "",
            )
        if command.phase == "readiness.client-cache":
            ready = not self.client_cache_timeout
            return CommandResult(
                0,
                json.dumps(
                    {
                        "lifecycle": 3 if ready else 2,
                        "line": "cache sidecar adapter state=2 lifecycle=3"
                        if ready
                        else None,
                        "ready": ready,
                        "reason": "ready" if ready else "not-ready",
                        "state": 2 if ready else 3,
                    }
                ),
                "",
            )
        if command.phase == "readiness.canary":
            return CommandResult(0, "abcd  local\nabcd  remote\n", "")
        if command.phase == "readiness.environments":
            return CommandResult(
                0,
                json.dumps(
                    {
                        "matched": {"F1": "RELOGIN F1 [gcc] cache=on"},
                        "ready": True,
                    }
                ),
                "",
            )
        if command.phase == "down.remove-container":
            self.containers.pop(command.argv[-1])
            return CommandResult(0, "", "")
        if command.phase == "down.receipt-network-list":
            binding_name = command.instance
            return CommandResult(
                0,
                "\n".join(
                    identifier for identifier, value in self.receipt_networks.items()
                    if value["instance"] == binding_name
                ),
                "",
            )
        if command.phase == "down.receipt-network-inspect":
            network_id = command.argv[-1]
            value = self.receipt_networks[network_id]
            return CommandResult(0, json.dumps({
                "Id": network_id, "Name": value["name"], "Driver": "bridge",
                "Labels": value["labels"],
            }), "")
        if command.phase == "down.receipt-network-remove":
            self.receipt_networks.pop(command.argv[-1], None)
            return CommandResult(0, "", "")
        return CommandResult(0, "", "")


def test_successful_up_executes_exact_planned_starts_and_proves_canary(
    tmp_path: Path,
) -> None:
    farm, scenario, plan = _farm_scenario_plan(tmp_path)
    scripted = ScriptedLifecycle(farm)
    transport = RecordingTransport(scripted)
    receipt = bring_up(
        farm,
        scenario,
        plan,
        recorder=transport,
        probe_bytes=0,
        sync_corpora=False,
    )
    assert receipt["status"] == "UP"
    assert set(receipt["canaries"]) == {"C1"}
    assert set(receipt["canaries"]["C1"]) == {"F1"}
    assert receipt["client_cache_readiness"]["C1"]["ready"] is True
    planned_starts = [
        item["argv"]
        for item in plan["commands"]
        if item["phase"].startswith("up.start-")
    ]
    observed_starts = [
        list(command.argv)
        for command in transport.commands
        if command.phase.startswith("up.start-")
    ]
    assert observed_starts == planned_starts
    assert len(scripted.containers) == 3
    phases = [command.phase for command in transport.commands]
    assert phases.index("readiness.client-cache") < phases.index("readiness.canary")
    assert "'<building_local>'" in CANARY_SCRIPT
    assert "readiness canary compiled locally" in CANARY_SCRIPT


def test_p51_receipt_helper_is_staged_between_prepare_and_client_start(
    tmp_path: Path,
) -> None:
    farm, scenario, _ = _farm_scenario_plan(tmp_path)
    farm.data["authority"]["topologies"]["C1F1"]["slots_per_f"] = 31
    scenario.data["id"] = "P51-receipt-stage-test"
    scenario.data["instances"][0].setdefault("env", {})["ICECC_P51_MODE"] = "on"
    scenario.data["instances"][1]["slots"] = 31
    scenario.data["instances"][1].setdefault("env", {})["ICECC_P51_MODE"] = "on"
    scenario.data["instances"][2]["env"].update(
        {"ICECC_P50_MODE": "on", "ICECC_P51_MODE": "on"}
    )
    helper = Path("/bin/true")
    scenario.data["workload"].update(
        {
            "driver": "p51-receipt-window",
            "jobs": 1,
            "receipt_gate": {
                "binary": str(helper),
                "binary_sha256": hashlib.sha256(helper.read_bytes()).hexdigest(),
                "expected_commits": 1,
                "negotiated_window": 1,
                "expect_observed": True,
            },
        }
    )
    plan = farmtest.build_plan(farm, scenario, run_id="p51-receipt-stage")
    transport = RecordingTransport(ScriptedLifecycle(farm))

    receipt = bring_up(
        farm,
        scenario,
        plan,
        recorder=transport,
        probe_bytes=0,
        sync_corpora=False,
    )

    assert receipt["status"] == "UP"
    phases = [command.phase for command in transport.commands]
    assert phases.count("up.stage-p51-receipt-gate") == 1
    assert phases.index("up.prepare") < phases.index("up.stage-p51-receipt-gate")
    assert phases.index("up.stage-p51-receipt-gate") < phases.index("up.start-c")


def test_p51_receipt_window_requires_r2_scheduler_mode(tmp_path: Path) -> None:
    farm, scenario, _plan = _farm_scenario_plan(tmp_path)
    scenario.data["id"] = "P51-receipt-r2-scheduler-validation"
    scenario.data["instances"][0]["env"]["ICECC_P50_PROFILE"] = "P29V1"
    scenario.data["instances"][1].setdefault("env", {})["ICECC_P51_MODE"] = "on"
    scenario.data["instances"][1]["slots"] = 31
    scenario.data["instances"][2]["env"].update(
        {"ICECC_P50_MODE": "on", "ICECC_P51_MODE": "on"}
    )
    helper = Path("/bin/true")
    scenario.data["workload"].update(
        {
            "driver": "p51-receipt-window",
            "jobs": 1,
            "receipt_gate": {
                "binary": str(helper),
                "binary_sha256": hashlib.sha256(helper.read_bytes()).hexdigest(),
                "expected_commits": 1,
                "negotiated_window": 1,
                "expect_observed": True,
            },
        }
    )
    path = tmp_path / "receipt-scenario.json"
    path.write_text(json.dumps(scenario.data), encoding="utf-8")

    with pytest.raises(ScenarioSpecError, match="selected-profile R2 scheduler"):
        load_scenario_spec(path, farm)

    scenario.data["instances"][0]["env"]["ICECC_P51_MODE"] = "on"
    path.write_text(json.dumps(scenario.data), encoding="utf-8")
    loaded = load_scenario_spec(path, farm)
    assert loaded.data["instances"][0]["env"]["ICECC_P51_MODE"] == "on"


def test_readiness_canaries_cover_every_workload_client_worker_pair(
    tmp_path: Path,
) -> None:
    farm, scenario, plan = _farm_scenario_plan(tmp_path)
    first_client = next(
        item for item in plan["topology"]["instances"] if item["name"] == "C1"
    )
    plan["topology"]["instances"].append({**first_client, "name": "C2"})
    scenario.data["workload"]["clients"].append("C2")
    transport = RecordingTransport(ScriptedLifecycle(farm))
    clock = Clock()

    canaries = _run_canaries(
        farm,
        scenario,
        plan,
        transport,
        CommandFactory(),
        deadline=10,
        monotonic=clock,
    )

    assert set(canaries) == {"C1", "C2"}
    assert all(set(by_worker) == {"F1"} for by_worker in canaries.values())
    commands = [
        command for command in transport.commands if command.phase == "readiness.canary"
    ]
    assert len(commands) == 2
    containers = {
        next(arg for arg in command.argv if arg.startswith("icefarm-unit-run-C"))
        for command in commands
    }
    assert containers == {"icefarm-unit-run-C1", "icefarm-unit-run-C2"}


def test_p51_receipt_window_canary_does_not_preopen_the_r2_link(
    tmp_path: Path,
) -> None:
    farm, scenario, plan = _farm_scenario_plan(tmp_path)
    scenario.data["workload"]["driver"] = "p51-receipt-window"
    transport = RecordingTransport(ScriptedLifecycle(farm))

    _run_canaries(
        farm,
        scenario,
        plan,
        transport,
        CommandFactory(),
        deadline=10,
        monotonic=Clock(),
    )

    command = next(
        item for item in transport.commands if item.phase == "readiness.canary"
    )
    assert "--env" in command.argv
    assert "ICECC_P50_MODE=off" in command.argv
    assert "ICECC_P51_MODE=off" in command.argv

    ordinary_farm, ordinary_scenario, ordinary_plan = _farm_scenario_plan(tmp_path)
    ordinary_transport = RecordingTransport(ScriptedLifecycle(ordinary_farm))
    _run_canaries(
        ordinary_farm,
        ordinary_scenario,
        ordinary_plan,
        ordinary_transport,
        CommandFactory(),
        deadline=10,
        monotonic=Clock(),
    )
    ordinary_command = next(
        item for item in ordinary_transport.commands if item.phase == "readiness.canary"
    )
    assert "ICECC_P50_MODE=off" not in ordinary_command.argv
    assert "ICECC_P51_MODE=off" not in ordinary_command.argv


def test_s30_mutant_rotates_readiness_trace_before_workload(tmp_path: Path) -> None:
    farm, scenario, plan = _farm_scenario_plan(tmp_path)
    scenario.data["id"] = "S30-mutant-f-refusal"
    worker = next(
        item for item in plan["topology"]["instances"] if item["role"] == "F"
    )
    worker["image"]["kind"] = "daemon-mutant"
    transport = RecordingTransport(ScriptedLifecycle(farm))

    receipt = _rotate_s30_canary_traces(
        farm,
        scenario,
        plan,
        transport,
        CommandFactory(),
        timeout_s=30,
    )

    assert receipt == {
        "F1": {"canary_refusals": 1, "output": ""},
    }
    command = transport.commands[-1]
    assert command.phase == "readiness.s30-mutant-trace-boundary"
    assert any("s30-mutant-f-canary.jsonl" in argument for argument in command.argv)
    assert command.argv[-1] == "1"


def test_client_cache_ready_is_required_before_canary_and_timeout_cleans_up(
    tmp_path: Path,
) -> None:
    farm, scenario, plan = _farm_scenario_plan(tmp_path, up_s=2)
    scripted = ScriptedLifecycle(farm, client_cache_timeout=True)
    transport = RecordingTransport(scripted)
    clock = Clock()
    with pytest.raises(LifecycleError, match="client cache READY"):
        bring_up(
            farm,
            scenario,
            plan,
            recorder=transport,
            probe_bytes=0,
            sync_corpora=False,
            monotonic=clock,
            sleeper=clock.sleep,
        )
    assert scripted.containers == {}
    assert any(
        command.phase == "readiness.client-cache" for command in transport.commands
    )
    assert not any(
        command.phase == "readiness.canary" for command in transport.commands
    )
    assert any(command.phase == "diagnostics.inspect" for command in transport.commands)


def test_up_syncs_only_zstd19_archive_and_activates_one_read_only_turn(
    tmp_path: Path,
) -> None:
    farm, scenario, plan = _farm_scenario_plan(tmp_path)
    scripted = ScriptedLifecycle(farm)
    transport = RecordingTransport(scripted)
    receipt = bring_up(
        farm,
        scenario,
        plan,
        recorder=transport,
        probe_bytes=0,
    )
    phases = [command.phase for command in transport.commands]
    assert "preflight.corpus-archive-sync" in phases
    assert "run.corpus-materialize" in phases
    assert "run.corpus-activate" in phases
    assert "preflight.corpus-sync" not in phases
    preflight = json.loads(
        (tmp_path / "results" / "unit-run" / "preflight.json").read_text()
    )
    assert preflight["corpus"]["active_turn"]["turn"] == "A"
    assert preflight["corpus"]["active_turn"]["group"] == "files"
    assert preflight["corpus"]["hosts"]["tt-quietbox3"]["files"]["mode"] == "synced"
    assert receipt["status"] == "UP"


def test_forced_readiness_timeout_collects_and_removes_every_labelled_object(
    tmp_path: Path,
) -> None:
    farm, scenario, plan = _farm_scenario_plan(tmp_path, up_s=2)
    scripted = ScriptedLifecycle(farm, scheduler_timeout=True)
    transport = RecordingTransport(scripted)
    clock = Clock()
    with pytest.raises(LifecycleError, match="readiness timeout"):
        bring_up(
            farm,
            scenario,
            plan,
            recorder=transport,
            probe_bytes=0,
            sync_corpora=False,
            monotonic=clock,
            sleeper=clock.sleep,
        )
    assert scripted.containers == {}
    assert any(command.phase == "diagnostics.inspect" for command in transport.commands)
    assert any(
        command.phase == "down.remove-container" for command in transport.commands
    )
    failure = json.loads(
        (tmp_path / "results" / "unit-run" / "lifecycle.json").read_text()
    )
    assert failure["status"] == "FAILED"


def test_stale_labelled_container_is_refused_without_reap_and_never_started(
    tmp_path: Path,
) -> None:
    farm, scenario, plan = _farm_scenario_plan(tmp_path)
    scripted = ScriptedLifecycle(farm, stale=True)
    transport = RecordingTransport(scripted)
    with pytest.raises(PreflightRefusal, match="--reap-stale"):
        bring_up(
            farm,
            scenario,
            plan,
            recorder=transport,
            probe_bytes=0,
            sync_corpora=False,
        )
    assert set(scripted.containers) == {"aaaaaaaaaaaa"}
    assert not any(
        command.phase.startswith("up.start-") for command in transport.commands
    )
    assert not any(
        command.phase.startswith("diagnostics.") or command.phase.startswith("down.")
        for command in transport.commands
    )


def test_keyboard_interrupt_after_start_collects_and_removes_every_labelled_object(
    tmp_path: Path,
) -> None:
    farm, scenario, plan = _farm_scenario_plan(tmp_path)
    scripted = ScriptedLifecycle(farm, scheduler_interrupt=True)
    transport = RecordingTransport(scripted)
    with pytest.raises(KeyboardInterrupt):
        bring_up(
            farm,
            scenario,
            plan,
            recorder=transport,
            probe_bytes=0,
            sync_corpora=False,
        )
    assert scripted.containers == {}
    assert any(command.phase == "diagnostics.inspect" for command in transport.commands)
    assert any(
        command.phase == "down.remove-container" for command in transport.commands
    )
    failure = json.loads(
        (tmp_path / "results" / "unit-run" / "lifecycle.json").read_text()
    )
    assert failure["error"] == "KeyboardInterrupt"
    assert failure["status"] == "FAILED"


def test_explicit_old_stale_reap_removes_only_labelled_icefarm_container(
    tmp_path: Path,
) -> None:
    farm, scenario, plan = _farm_scenario_plan(tmp_path)
    scripted = ScriptedLifecycle(farm, stale=True)
    transport = RecordingTransport(scripted)
    bring_up(
        farm,
        scenario,
        plan,
        recorder=transport,
        probe_bytes=0,
        sync_corpora=False,
        reap_stale_hours=1,
    )
    assert "aaaaaaaaaaaa" not in scripted.containers
    assert any(
        command.phase == "preflight.stale-reap" for command in transport.commands
    )


def test_capacity_includes_one_copy_of_each_required_image_per_host(
    tmp_path: Path,
) -> None:
    farm, scenario, plan = _farm_scenario_plan(tmp_path)
    required = MIN_FREE_BYTES + corpus_layout(farm, "fmt-100").bytes + 1_500_000_000
    scripted = ScriptedLifecycle(farm, free_bytes=required - 1)
    transport = RecordingTransport(scripted)
    with pytest.raises(PreflightRefusal, match="images 1500000000"):
        bring_up(
            farm,
            scenario,
            plan,
            recorder=transport,
            probe_bytes=0,
            sync_corpora=False,
        )
    assert not any(
        command.phase.startswith("up.start-") for command in transport.commands
    )


def _attach_test_toolchain(
    plan: dict[str, object], archive: Path, archive_bytes: int
) -> None:
    client = next(item for item in plan["topology"]["instances"] if item["role"] == "C")
    client["compiler_recipe"]["toolchain"] = {
        "archive": str(archive),
        "archive_bytes": archive_bytes,
        "archive_sha256": hashlib.sha256(archive.read_bytes()).hexdigest()
        if archive.is_file()
        else "a" * 64,
        "compression": {
            "checksum": True,
            "codec": "zstd",
            "level": 19,
            "long": 31,
            "threads": 8,
        },
        "mount": "/opt/test-toolchain",
        "unpacked_bytes": 1,
    }


def test_toolchain_capacity_uses_declared_transport_bytes_before_local_stat(
    tmp_path: Path,
) -> None:
    farm, scenario, plan = _farm_scenario_plan(tmp_path)
    declared = 200_000_000_000
    _attach_test_toolchain(plan, tmp_path / "absent.tar.zst", declared)
    scripted = ScriptedLifecycle(farm)
    transport = RecordingTransport(scripted)
    with pytest.raises(PreflightRefusal, match=f"toolchains {declared + 1}"):
        bring_up(
            farm,
            scenario,
            plan,
            recorder=transport,
            probe_bytes=0,
            sync_corpora=False,
        )
    assert not any(
        command.phase.startswith("up.start-") for command in transport.commands
    )


def test_toolchain_archive_size_mismatch_is_refused_before_persistent_start(
    tmp_path: Path,
) -> None:
    farm, scenario, plan = _farm_scenario_plan(tmp_path)
    archive = tmp_path / "toolchain.tar.zst"
    archive.write_bytes(b"not-a-real-archive")
    _attach_test_toolchain(plan, archive, archive.stat().st_size + 1)
    scripted = ScriptedLifecycle(farm)
    transport = RecordingTransport(scripted)
    with pytest.raises(PreflightRefusal, match="toolchain archive size mismatch"):
        bring_up(
            farm,
            scenario,
            plan,
            recorder=transport,
            probe_bytes=0,
            sync_corpora=False,
        )
    assert not any(
        command.phase.startswith("up.start-") for command in transport.commands
    )


def test_runtime_role_hash_mismatch_is_refused_before_persistent_start(
    tmp_path: Path,
) -> None:
    farm, scenario, plan = _farm_scenario_plan(tmp_path)
    scripted = ScriptedLifecycle(farm, bad_role_hash=True)
    transport = RecordingTransport(scripted)
    with pytest.raises(PreflightRefusal, match="hash mismatch"):
        bring_up(
            farm,
            scenario,
            plan,
            recorder=transport,
            probe_bytes=0,
            sync_corpora=False,
        )
    assert not any(
        command.phase.startswith("up.start-") for command in transport.commands
    )
    refusal = json.loads(
        (tmp_path / "results" / "unit-run" / "preflight-refusal.json").read_text()
    )
    assert refusal["reason_code"] == "role-hash-mismatch"
    assert refusal["jobs_started"] == 0
    assert refusal["persistent_start_attempted"] is False
    mismatched = next(
        instance
        for instance in plan["topology"]["instances"]
        if instance["host"] == refusal["details"]["host"]
        and instance["role"] == refusal["details"]["role"]
    )
    assert refusal["details"] == {
        "expected_sha256": mismatched["sha256"],
        "host": mismatched["host"],
        "observed_sha256": "f" * 64,
        "product": mismatched["image"]["label"],
        "role": mismatched["role"],
    }
    assert not any(
        command["phase"].startswith("up.start-") for command in refusal["commands"]
    )
    lifecycle = json.loads(
        (tmp_path / "results" / "unit-run" / "lifecycle.json").read_text()
    )
    assert lifecycle["farm_digest"] == plan["farm_digest"]
    assert lifecycle["scenario_digest"] == plan["scenario_digest"]
    assert lifecycle["topology_digest"] == plan["topology_digest"]


def test_scheduler_mutant_cannot_fall_back_to_normal_role_store(
    tmp_path: Path,
) -> None:
    farm = load_farm_spec(farm_fixture.example_farm_path())
    farm.data["hub"]["results_root"] = str(tmp_path)
    mutant = farm.data["authority"]["images"]["p50s4-h3-armed-57a1e336"]
    farm.data["runtime_image"]["closure_sha256"] = mutant["closure_sha256"]
    mutant.pop("role_overrides")
    scenario = load_scenario_spec(
        INTEGRATION / "scenarios" / "H3-mutant-scheduler.json", farm
    )
    plan = farmtest.build_plan(farm, scenario, run_id="missing-mutant-role")
    transport = RecordingTransport(ScriptedLifecycle(farm))
    with pytest.raises(PreflightRefusal, match="lacks an observed scheduler role hash"):
        bring_up(
            farm,
            scenario,
            plan,
            recorder=transport,
            probe_bytes=0,
            sync_corpora=False,
        )
    assert not any(
        command.phase.startswith("up.start-") for command in transport.commands
    )


def test_down_removes_containers_before_reporting_protected_count_change(
    tmp_path: Path,
) -> None:
    farm, scenario, plan = _farm_scenario_plan(tmp_path)
    scripted = ScriptedLifecycle(farm)
    bring_up(
        farm,
        scenario,
        plan,
        recorder=RecordingTransport(scripted),
        probe_bytes=0,
        sync_corpora=False,
    )
    scripted.protected_count = 2
    teardown = RecordingTransport(scripted)
    with pytest.raises(LifecycleError, match="protected-count-changed"):
        down_from_state(farm, plan, recorder=teardown)
    assert scripted.containers == {}
    cleanups = [command for command in teardown.commands if command.phase == "down.remove-scratch"]
    assert len(cleanups) == len(plan["topology"]["instances"])
    for command in cleanups:
        assert "/cleanup/system-source" in command.argv[-1]
        assert "system-source-snapshots" not in " ".join(command.argv)


def test_failed_client_output_capture_is_bounded_and_retained_before_cleanup(
    tmp_path: Path,
) -> None:
    farm, scenario, plan = _farm_scenario_plan(tmp_path)
    plan["diagnostic_capture_client_output"] = True
    plan["diagnostic_capture_client_output_kind"] = "d18-workload"
    scripted = ScriptedLifecycle(farm, fail_output_capture=True)
    bring_up(farm, scenario, plan, recorder=RecordingTransport(scripted), probe_bytes=0)

    teardown = RecordingTransport(scripted)
    receipt = down_from_state(farm, plan, recorder=teardown)

    phases = [command.phase for command in teardown.commands]
    assert phases.index("diagnostics.sync-output") < phases.index("down.remove-scratch")
    output_problem = next(
        error for error in receipt["diagnostic_errors"]
        if error.endswith(":sync-output:injected output capture failure")
    )
    client_host = next(
        instance["host"]
        for instance in plan["topology"]["instances"]
        if instance["name"] == "C1"
    )
    assert output_problem.startswith(f"{client_host}:C1:")
    assert receipt["preserved_output_instances"] == ["C1"]
    assert receipt["preserved_outputs"] == [
        {
            "host": client_host,
            "instance": "C1",
            "path": str(
                Path(farm.hosts[client_host]["scratch_root"])
                / "icefarm/unit-run/C1/output"
            ),
        }
    ]
    client_cleanup = next(
        command.argv[-1]
        for command in teardown.commands
        if command.phase == "down.remove-scratch"
        and "/unit-run/C1" in " ".join(command.argv)
    )
    assert "/cleanup/output" not in client_cleanup
    assert any(
        command.phase == "down.remove-scratch"
        and "/cleanup/cache" in command.argv[-1]
        for command in teardown.commands
    )
    assert any(
        command.phase == "down.remove-scratch"
        and "/cleanup/tmp" in command.argv[-1]
        for command in teardown.commands
    )
    assert any(
        command.phase == "down.remove-scratch"
        and "/cleanup/log" in command.argv[-1]
        for command in teardown.commands
    )


def test_diagnostic_output_filter_keeps_receipts_not_compiled_payloads(
    tmp_path: Path,
) -> None:
    source = tmp_path / "remote-output"
    destination = tmp_path / "captured-output"
    files = {
        "workload/A/corpus-manifest.sha256": "manifest\n",
        "workload/A/oracle-samples.tsv": "sample\n",
        "workload/A/oracle-summary.tsv": "summary\n",
        "workload/A/summary.tsv": "jobs\t1\n",
        "workload/A/jobs/000001/result.tsv": "row\n",
        "workload/A/jobs/000001/client-debug.log": "debug\n",
        "workload/A/jobs/000001/client-output.log": "output\n",
        "workload/A/jobs/000001/remote.o": "compiled-object-must-not-copy\n",
        "p51-receipt-gate/helper.stderr": "gate diagnostic\n",
        "p51-receipt-gate/exit": "0\n",
        "p51-receipt-gate/links/C1-F1/helper.stderr": "link diagnostic\n",
        "p51-receipt-gate/links/C1-F1/held-1": "window=30\n",
        "p51-receipt-gate/identity-1": "relationship=abc123 window=30\n",
        "p51-receipt-gate/links/C1-F1/identity-1": "relationship=abc123 window=30\n",
        "p51-receipt-gate/links/C1-F1/debs/iptables.deb": "package-must-not-copy\n",
        "p51-receipt-gate/debs/iptables.deb": "package-must-not-copy\n",
        "p50daemonpositive": "helper-must-not-copy\n",
    }
    for relative, content in files.items():
        path = source / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="ascii")
    destination.mkdir()
    patterns = _diagnostic_client_output_patterns("p51-receipt-window")
    result = subprocess.run(
        [
            "rsync", "--archive", "--no-owner", "--no-group",
            *(argument for pattern in patterns for argument in ("--include", pattern)),
            "--exclude", "*", str(source) + "/", str(destination) + "/",
        ],
        check=False,
        capture_output=True,
        text=True,
    )

    assert result.returncode == 0, result.stderr
    for relative in (
        "workload/A/summary.tsv",
        "workload/A/corpus-manifest.sha256",
        "workload/A/oracle-samples.tsv",
        "workload/A/oracle-summary.tsv",
        "workload/A/jobs/000001/result.tsv",
        "workload/A/jobs/000001/client-debug.log",
        "workload/A/jobs/000001/client-output.log",
        "p51-receipt-gate/helper.stderr",
        "p51-receipt-gate/exit",
        "p51-receipt-gate/links/C1-F1/helper.stderr",
        "p51-receipt-gate/links/C1-F1/held-1",
        "p51-receipt-gate/identity-1",
        "p51-receipt-gate/links/C1-F1/identity-1",
    ):
        assert (destination / relative).is_file(), relative
    assert not (destination / "workload/A/jobs/000001/remote.o").exists()
    assert not (destination / "p51-receipt-gate/debs/iptables.deb").exists()
    assert not (destination / "p51-receipt-gate/links/C1-F1/debs/iptables.deb").exists()
    assert not (destination / "p50daemonpositive").exists()


def test_ports_are_unique_and_every_daemon_gets_an_explicit_port(
    tmp_path: Path,
) -> None:
    _farm, _scenario, plan = _farm_scenario_plan(tmp_path)
    ports = [plan["ports"]["scheduler"], plan["ports"]["scheduler_control"]]
    ports.extend(plan["ports"]["instances"].values())
    assert len(ports) == len(set(ports))
    for command in plan["commands"]:
        if command["phase"] in ("up.start-f", "up.start-c"):
            assert "--port" in command["argv"]
            assert "--pull=never" in command["argv"]
