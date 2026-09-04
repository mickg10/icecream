#!/usr/bin/env python3
"""Spec-driven controlled-farm runner (I1: validation, plan, fake up)."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path, PurePosixPath
from typing import Any

try:
    from farmharness import newgen_farm_env
    from .farm_spec import FarmSpec, FarmSpecError, load_farm_spec
    from .remote import FakeRecorder, PlannedCommand, docker_argv, execute, ssh_argv
    from .scenario_spec import ScenarioSpec, ScenarioSpecError, load_scenario_spec
    from .schema_validation import canonical_bytes
except ImportError:  # Executed as ./farmtest.py.
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
    import newgen_farm_env

    from farm_spec import FarmSpec, FarmSpecError, load_farm_spec
    from remote import FakeRecorder, PlannedCommand, docker_argv, execute, ssh_argv
    from scenario_spec import ScenarioSpec, ScenarioSpecError, load_scenario_spec
    from schema_validation import canonical_bytes


PLAN_SCHEMA = "icefarm-plan-v1"
RUN_ID_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,79}$")


class PlanError(ValueError):
    """The validated specs cannot resolve to one safe command plan."""


def _image_reference(farm: FarmSpec, label: str) -> str:
    if "/" in label:
        return label
    registry = farm.data["registry"]
    return f"{registry['host']}:{registry['port']}/icefarm/icecream:{label}"


def _resolver_environment(farm: FarmSpec, scenario: ScenarioSpec) -> dict[str, str]:
    data = scenario.data
    by_role = {
        role: [item for item in data["instances"] if item["role"] == role]
        for role in ("S", "C", "F")
    }
    profiles = sorted(
        {
            item.get("env", {}).get("ICECC_P50_PROFILE")
            for item in data["instances"]
            if item.get("env", {}).get("ICECC_P50_PROFILE") is not None
        }
    )
    if len(profiles) > 1:
        raise PlanError(f"scenario declares conflicting P50 profiles: {profiles!r}")
    any_p50 = any(
        data["images"][item["image"]].rsplit(":", 1)[-1].lower().startswith("p50")
        for item in data["instances"]
    )
    instances = [
        {
            "name": item["name"],
            "role": item["role"],
            "host": item["host"],
            "image": data["images"][item["image"]],
            **({"env": dict(sorted(item.get("env", {}).items()))} if item.get("env") else {}),
            **({"slots": item["slots"]} if item["role"] == "F" else {}),
        }
        for item in data["instances"]
    ]
    icefarm_env: dict[str, str] = {
        "ICEFARM_AUTHORITY_SHA256": hashlib.sha256(
            canonical_bytes(farm.data["authority"])
        ).hexdigest(),
        "ICEFARM_CORPUS": data["workload"]["corpus"],
        "ICEFARM_DRY_RUN": "1",
        "ICEFARM_INSTANCES": json.dumps(
            instances, sort_keys=True, separators=(",", ":"), ensure_ascii=False
        ),
        "ICEFARM_LINK_RATES": "1000000000,100000000",
        "ICEFARM_REGIME": "cold",
        "ICEFARM_TOPOLOGY": f"C{len(by_role['C'])}F{len(by_role['F'])}",
    }
    if any_p50:
        icefarm_env["ICEFARM_PROFILE"] = profiles[0] if profiles else "P29V1"
    corpus = farm.data["corpora"][data["workload"]["corpus"]]
    if "manifest" in corpus:
        icefarm_env["ICEFARM_MANIFEST"] = corpus["manifest"]
    elif "turn_a_manifest" in corpus:
        icefarm_env["ICEFARM_MANIFEST"] = corpus["turn_a_manifest"]
    elif "turn_a" in corpus:
        icefarm_env["ICEFARM_MANIFEST"] = str(
            PurePosixPath(corpus["hub_path"]) / corpus["turn_a"]
        )
    return icefarm_env


def resolve_topology(farm: FarmSpec, scenario: ScenarioSpec) -> dict[str, Any]:
    """Resolve through the normative v2 boundary; never fork digest logic."""

    try:
        return newgen_farm_env.resolve(
            _resolver_environment(farm, scenario),
            farm.data["authority"],
            host_policies=farm.hosts,
            corpus_authorities=farm.data["corpora"],
        )
    except newgen_farm_env.ResolutionError as exc:
        raise PlanError(str(exc)) from exc


def _container_name(run_id: str, instance: str) -> str:
    return f"icefarm-{run_id}-{instance}"


def _env_args(environment: dict[str, str]) -> list[str]:
    result: list[str] = []
    for key, value in sorted(environment.items()):
        result.extend(("--env", f"{key}={value}"))
    return result


def _planned_commands(
    farm: FarmSpec, scenario: ScenarioSpec, topology: dict[str, Any], run_id: str
) -> list[PlannedCommand]:
    commands: list[PlannedCommand] = []
    timeout = scenario.data["timeouts"]["up_s"]
    sequence = 0

    scheduler = next(item for item in topology["instances"] if item["role"] == "S")
    scheduler_port = farm.data["port_range"][0]
    scheduler_addr = f"{scheduler['address']}:{scheduler_port}"
    netname = f"{farm.data['netname_prefix']}-{run_id}"

    for instance in topology["instances"]:
        host = farm.hosts[instance["host"]]
        root = PurePosixPath(host["scratch_root"]) / "icefarm" / run_id / instance["name"]
        directories = [str(root / leaf) for leaf in ("cache", "tmp", "log", "results")]
        commands.append(
            PlannedCommand(
                sequence=sequence,
                phase="up.prepare",
                host=instance["host"],
                instance=instance["name"],
                transport="ssh",
                timeout_s=timeout,
                argv=ssh_argv(farm, instance["host"], ("install", "-d", "-m", "0777", "--", *directories)),
            )
        )
        sequence += 1

    role_order = {"S": 0, "F": 1, "C": 2}
    for instance in sorted(topology["instances"], key=lambda item: (role_order[item["role"]], item["name"])):
        host = farm.hosts[instance["host"]]
        root = PurePosixPath(host["scratch_root"]) / "icefarm" / run_id / instance["name"]
        args = [
            "run",
            "--detach",
            "--name",
            _container_name(run_id, instance["name"]),
            "--label",
            f"icefarm.run={run_id}",
            "--label",
            f"icefarm.instance={instance['name']}",
            "--network",
            "host",
        ]
        environment = {
            **instance["env"],
            "ICECC_NETNAME": netname,
            "ICECC_TEST_SOCKET": f"/tmp/{netname}-{instance['name']}.sock",
        }
        if len(environment["ICECC_TEST_SOCKET"].encode("ascii")) > 107:
            raise PlanError(
                f"instance {instance['name']!r} ICECC_TEST_SOCKET exceeds the 107-byte Unix limit"
            )
        if instance["role"] == "F":
            args.extend(("--user", "0", "--cap-add", "SYS_CHROOT"))
        for source, target in (
            (root / "cache", "/var/cache/icecream"),
            (root / "tmp", "/tmp/icefarm"),
            (root / "log", "/var/log/icecream"),
            (root / "results", "/results"),
        ):
            args.extend(("--mount", f"type=bind,src={source},dst={target}"))
        args.extend(_env_args(environment))
        entrypoint = {
            "S": "/opt/icecream/entry-scheduler.sh",
            "F": "/opt/icecream/entry-daemon.sh",
            "C": "/opt/icecream/entry-client.sh",
        }[instance["role"]]
        args.extend(
            (
                "--entrypoint",
                entrypoint,
                _image_reference(farm, instance["image"]["label"]),
            )
        )
        if instance["role"] == "S":
            args.extend(("--port", str(scheduler_port), "--netname", netname))
        elif instance["role"] == "F":
            args.extend(
                (
                    "--scheduler",
                    scheduler_addr,
                    "--slots",
                    str(instance["slots"]),
                    "--name",
                    instance["name"],
                )
            )
        else:
            args.extend(("--scheduler", scheduler_addr, "--idle"))
        transport = "docker-context" if host.get("docker_context") else "ssh-docker"
        commands.append(
            PlannedCommand(
                sequence=sequence,
                phase=f"up.start-{instance['role'].lower()}",
                host=instance["host"],
                instance=instance["name"],
                transport=transport,
                timeout_s=timeout,
                argv=docker_argv(farm, instance["host"], args),
            )
        )
        sequence += 1
    return commands


def build_plan(
    farm: FarmSpec, scenario: ScenarioSpec, *, run_id: str | None = None
) -> dict[str, Any]:
    topology = resolve_topology(farm, scenario)
    selected_run_id = run_id or f"plan-{topology['topology_digest'][:12]}"
    if RUN_ID_RE.fullmatch(selected_run_id) is None or selected_run_id in (".", ".."):
        raise PlanError("run id must be 1-80 safe, non-dot filename/label characters")
    commands = _planned_commands(farm, scenario, topology, selected_run_id)
    return {
        "commands": [command.as_dict() for command in commands],
        "farm": str(farm.path),
        "farm_digest": farm.digest,
        "icefarm_env": _resolver_environment(farm, scenario),
        "run_id": selected_run_id,
        "scenario": str(scenario.path),
        "schema": PLAN_SCHEMA,
        "scenario_digest": scenario.digest,
        "topology": topology,
        "topology_digest": topology["topology_digest"],
    }


def render_plan(plan: dict[str, Any]) -> str:
    return json.dumps(plan, indent=2, sort_keys=True, ensure_ascii=False) + "\n"


def _commands_from_plan(plan: dict[str, Any]) -> list[PlannedCommand]:
    return [
        PlannedCommand(
            sequence=item["sequence"],
            phase=item["phase"],
            host=item["host"],
            instance=item["instance"],
            transport=item["transport"],
            timeout_s=item["timeout_s"],
            argv=tuple(item["argv"]),
        )
        for item in plan["commands"]
    ]


def fake_up(plan: dict[str, Any]) -> list[dict[str, Any]]:
    recorder = FakeRecorder()
    execute(_commands_from_plan(plan), recorder)
    return [command.as_dict() for command in recorder.commands]


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    for command in ("plan", "up"):
        child = subparsers.add_parser(command)
        child.add_argument("--farm", required=True)
        child.add_argument("--scenario", required=True)
        child.add_argument("--run-id")
        if command == "up":
            child.add_argument("--fake-recorder", action="store_true")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        farm = load_farm_spec(args.farm)
        scenario = load_scenario_spec(args.scenario, farm)
        plan = build_plan(farm, scenario, run_id=args.run_id)
        if args.command == "up":
            if not args.fake_recorder:
                print("farmtest up is unavailable until lifecycle I3; no command executed", file=sys.stderr)
                return 2
            recorded = fake_up(plan)
            if recorded != plan["commands"]:
                print("farmtest internal error: fake recorder diverged from plan", file=sys.stderr)
                return 2
        print(render_plan(plan), end="")
        return 0
    except (FarmSpecError, ScenarioSpecError, PlanError) as exc:
        print(f"farmtest refused: {exc}", file=sys.stderr)
        return 3


if __name__ == "__main__":
    raise SystemExit(main())
