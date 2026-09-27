"""Manifest workload drivers and the deterministic local-SHA oracle."""

from __future__ import annotations

import json
import base64
import hashlib
import os
import re
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from typing import Any

try:
    from .events import EventError, EventProducer, JobReader
    from .farm_spec import FarmSpec
    from .images import CommandFactory, RecordingTransport
    from .layout import compiler_identity_digest
    from .lifecycle import LifecycleError, activate_corpus_turn, bundle_root
    from .remote import CommandResult, PlannedCommand, RemoteError, docker_argv
    from .scenario_spec import PROFILES, ScenarioSpec
    from .schema_validation import canonical_bytes
except ImportError:  # Direct execution from this directory.
    from events import EventError, EventProducer, JobReader
    from farm_spec import FarmSpec
    from images import CommandFactory, RecordingTransport
    from layout import compiler_identity_digest
    from lifecycle import LifecycleError, activate_corpus_turn, bundle_root
    from remote import CommandResult, PlannedCommand, RemoteError, docker_argv
    from scenario_spec import PROFILES, ScenarioSpec
    from schema_validation import canonical_bytes


WORKLOAD_SCHEMA = "icefarm-workload-v1"
SUMMARY_RE = re.compile(
    r"^ICEFARM_WORKLOAD jobs=([0-9]+) failures=([0-9]+) samples=([0-9]+)$",
    re.MULTILINE,
)
D18_R2_ADOPTION_RE = re.compile(
    r"P51 cache-link descriptor adopted by sidecar \(generation ([1-9][0-9]*)/([1-9][0-9]*)\)"
)
IMAGE_GENERATION_RE = re.compile(
    r"^p(43|44|50)(?:s[0-9]+)?(?:-|$)", re.IGNORECASE
)
D18_PROCESS_OBSERVER = r'''
for statfile in /proc/[0-9]*/stat
do
    pid=${statfile#/proc/}
    pid=${pid%/stat}
    IFS= read -r comm <"/proc/$pid/comm" 2>/dev/null || continue
    test "$comm" = cc1plus || continue
    IFS= read -r statline <"$statfile" 2>/dev/null || continue
    rest=${statline##*) }
    set -- $rest
    test "$#" -ge 20 || continue
    printf '%s %s\n' "$pid" "${20}"
done'''

P51_GATE_READ_MARKER = (
    'if test -f "$1"; then cat "$1"; else printf "__WAIT__\\n"; fi'
)
P51_IPTABLES_BUNDLE_MANIFEST_SHA256 = (
    "d1d7980ebc0b342caea791cc4fc86be4a6dcfc44f89e6884f2e4fbc69fd5568a"
)
P51_IPTABLES_BUNDLE_FILES = (
    "iptables_1.8.9-2_amd64.deb",
    "libip6tc2_1.8.9-2_amd64.deb",
    "libmnl0_1.0.4-3_amd64.deb",
    "libnetfilter-conntrack3_1.0.9-3_amd64.deb",
    "libnfnetlink0_1.0.2-2_amd64.deb",
    "libnftnl11_1.2.4-2_amd64.deb",
    "libxtables12_1.8.9-2_amd64.deb",
    "netbase_6.4_all.deb",
)
P51_NEGOTIATED_RE = re.compile(
    r"P51_RECEIPT_GATE_NEGOTIATED profile=([1-3]) window=([1-9][0-9]*) "
    r"epoch=([1-9][0-9]*) generation=([1-9][0-9]*)"
)
P51_PROFILE_IDS = {"P29V1": 1, "ZSTD_TU": 2, "ZSTD_ROUTE": 3}


def _p51_negotiation_matches(
    match: re.Match[str] | None, *, profile: str, window: int
) -> bool:
    return (
        match is not None
        and profile in P51_PROFILE_IDS
        and int(match.group(1)) == P51_PROFILE_IDS[profile]
        and int(match.group(2)) == window
    )


# Values from farm/scenario documents are passed as argv after this fixed
# program.  They are never interpolated into shell source.
MANIFEST_DRIVER = (
    (Path(__file__).parent / "workers" / "manifest_driver.sh").read_text(
        encoding="utf-8"
    )
)


class WorkloadError(RuntimeError):
    """The workload could not produce a complete evidence surface."""


def _atomic_json(path: Path, value: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + f".tmp-{os.getpid()}")
    try:
        temporary.write_bytes(canonical_bytes(value))
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def _docker_transport(farm: FarmSpec, host_name: str) -> str:
    return (
        "docker-context"
        if farm.hosts[host_name].get("docker_context")
        else "ssh-docker"
    )


def _assert_up(farm: FarmSpec, scenario: ScenarioSpec, plan: dict[str, Any]) -> None:
    path = bundle_root(farm, plan["run_id"]) / "lifecycle.json"
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise WorkloadError(f"cannot load UP lifecycle receipt: {exc}") from exc
    if (
        not isinstance(value, dict)
        or value.get("status") != "UP"
        or value.get("run_id") != plan["run_id"]
        or value.get("scenario_digest") != scenario.digest
        or value.get("topology_digest") != plan["topology_digest"]
    ):
        raise WorkloadError("lifecycle receipt does not authenticate this UP run")


def _strict_p50_required(scenario: ScenarioSpec, plan: dict[str, Any]) -> bool:
    """Require P50 only when every phase of this workload requires it.

    The client wrapper is fixed for a complete turn, so a scenario that starts
    with P50 disabled, or disables it during the turn, must retain the remote
    legacy path.  Remote-only execution is enforced independently by the
    workload and verdict layers.
    """

    # Only active scheduler loss permits the one authenticated fresh legacy
    # retry; all ordinary all-new P50 cells remain strict.
    if scenario.data.get("id") == "S70-b4-scheduler-active-loss":
        return False
    if (
        scenario.data["shape"] != "S'C'F'"
        or scenario.data["controls"]
        or scenario.data.get("id") == "S30-mutant-f-refusal"
        or not all(
            item.get("version") == 50
            for item in plan.get("topology", {}).get("instances", [])
        )
    ):
        return False

    schedulers = [
        item
        for item in scenario.data["instances"]
        if item.get("role") == "S"
    ]
    if len(schedulers) != 1:
        raise WorkloadError("strict P50 requires exactly one scheduler")
    scheduler = schedulers[0]
    scheduler_name = scheduler.get("name")
    named_instances = {
        item.get("name"): item
        for item in scenario.data["instances"]
        if isinstance(item, dict)
    }
    environment = scheduler.get("env", {})
    if not isinstance(environment, dict):
        raise WorkloadError("strict P50 scheduler environment is invalid")
    profile = environment.get("ICECC_P50_PROFILE", "P29V1")
    if profile == "OFF":
        return False
    if profile not in PROFILES:
        raise WorkloadError(f"strict P50 scheduler profile is invalid: {profile!r}")

    timeline = scenario.data.get("timeline", [])
    if not isinstance(timeline, list):
        raise WorkloadError("strict P50 timeline is invalid")
    for event in timeline:
        if not isinstance(event, dict):
            raise WorkloadError("strict P50 timeline event is invalid")
        action = event.get("action")
        target = named_instances.get(event.get("instance"))
        if action in {"upgrade", "downgrade"}:
            alias = event.get("image")
            label = scenario.data.get("images", {}).get(alias)
            generation = (
                IMAGE_GENERATION_RE.match(label.rsplit(":", 1)[-1])
                if isinstance(label, str)
                else None
            )
            if generation is None:
                raise WorkloadError("strict P50 transition image is invalid")
            if int(generation.group(1)) != 50:
                return False
            continue
        if action != "env_set":
            continue
        update = event.get("env")
        if not isinstance(update, dict):
            raise WorkloadError("strict P50 env_set is invalid")
        if isinstance(target, dict) and target.get("role") == "C":
            if update.get("ICECC_P50_MODE") == "off":
                return False
            continue
        if event.get("instance") != scheduler_name:
            continue
        event_profile = update.get("ICECC_P50_PROFILE")
        if event_profile == "OFF":
            return False
        if event_profile not in PROFILES:
            raise WorkloadError(
                f"strict P50 scheduler env_set profile is invalid: {event_profile!r}"
            )
    return True


def _active_loss_serial_through(scenario: ScenarioSpec) -> int:
    """Return the ordered job prefix serialized before active scheduler loss."""

    if scenario.data.get("id") != "S70-b4-scheduler-active-loss":
        return 0
    events = [
        event
        for event in scenario.data.get("timeline", [])
        if isinstance(event, dict)
        and event.get("action") == "scheduler-loss-active"
    ]
    if len(events) != 1:
        raise WorkloadError("active scheduler loss needs one serial admission event")
    trigger = events[0].get("trigger")
    match = re.fullmatch(r"job ([1-9][0-9]*)", trigger or "")
    if match is None:
        raise WorkloadError("active scheduler loss needs a positive serial job boundary")
    return int(match.group(1))


def _s60_admit_through(scenario: ScenarioSpec, corpus: dict[str, Any]) -> int:
    """Bound each S60 client's parallel prefix, preserving a post-event suffix."""
    if not scenario.data.get("id", "").startswith("S60-"):
        return 0
    events = scenario.data.get("timeline", [])
    if len(events) != 1 or events[0].get("action") not in {"upgrade", "downgrade"}:
        raise WorkloadError("S60 admission needs one upgrade/downgrade event")
    match = re.fullmatch(r"job ([1-9][0-9]*)", events[0].get("trigger", ""))
    if match is None:
        raise WorkloadError("S60 admission needs a positive job boundary")
    boundary = int(match.group(1))
    total = corpus["tus"] * corpus.get("repeat", 1) * scenario.data["workload"]["repeat"]
    if boundary >= total:
        raise WorkloadError("S60 admission boundary must preserve a client suffix")
    return boundary


def _driver_command(
    farm: FarmSpec,
    scenario: ScenarioSpec,
    plan: dict[str, Any],
    client: dict[str, Any],
    turn: str,
    factory: CommandFactory,
    *,
    resume: bool = False,
    checkpoint_sha256: str | None = None,
    exec_uid: str = "65534:65534",
) -> PlannedCommand:
    workload = scenario.data["workload"]
    corpus = farm.data["corpora"][workload["corpus"]]
    layout = "single" if "manifest" in corpus else "paired"
    corpus_repeat = corpus.get("repeat", 1)
    strict_p50 = int(_strict_p50_required(scenario, plan))
    active_loss_serial_through = _active_loss_serial_through(scenario)
    s60_admit_through = _s60_admit_through(scenario, corpus)
    timeline = scenario.data.get("timeline", [])
    preferred_worker = (
        timeline[0].get("instance")
        if scenario.data.get("expect", {}).get("engagement")
        == "s95-cache-disk-full"
        and isinstance(timeline, list)
        and len(timeline) == 1
        and isinstance(timeline[0], dict)
        and timeline[0].get("action") == "disk_fill"
        else None
    )
    disk_fill_trigger = 0
    if preferred_worker is not None:
        match = re.fullmatch(r"job ([1-9][0-9]*)", timeline[0].get("trigger", ""))
        if match is None:
            raise WorkloadError("S95 disk fill needs a positive job trigger")
        disk_fill_trigger = int(match.group(1))
    container = f"icefarm-{plan['run_id']}-{client['name']}"
    d18_roles = workload.get("d18_roles")
    d18_role = (
        next(
            (role for role, name in d18_roles["clients"].items() if name == client["name"]),
            None,
        )
        if isinstance(d18_roles, dict)
        else None
    )
    compiler_args = list(client["compiler_recipe"]["arguments"])
    fault = scenario.data.get("fault", {})
    timeout_s = scenario.data["timeouts"]["turn_s"] + 300
    argv = docker_argv(
        farm,
        client["host"],
        (
            "exec",
            "--user",
            exec_uid,
            *(
                (
                    "--env", f"ICEFARM_DISK_FILL_WORKER={preferred_worker}",
                    "--env", f"ICEFARM_DISK_FILL_TRIGGER={disk_fill_trigger}",
                )
                if isinstance(preferred_worker, str) and preferred_worker
                else ()
            ),
            *(
                ("--env", f"ICEFARM_S60_ADMIT_THROUGH={s60_admit_through}")
                if s60_admit_through else ()
            ),
            *(("--env", "ICEFARM_D18_BARRIER=1") if d18_role is not None else ()),
            *(
                (
                    "--env",
                    f"ICEFARM_EVENT_SERIAL_THROUGH={active_loss_serial_through}",
                    "--env",
                    "ICECC_REMOTE_REQUIRED=1",
                )
                if active_loss_serial_through
                else ()
            ),
            container,
            "/bin/bash",
            "-c",
            MANIFEST_DRIVER,
            "icefarm-manifest-driver",
            f"/results/workload/{turn}",
            "/corpus",
            f"/oracle/{turn}",
            client["name"],
            str(corpus["tus"]),
            str(corpus_repeat),
            str(workload["repeat"]),
            str(workload["jobs"]),
            str(scenario.data["timeouts"]["turn_s"]),
            layout,
            str(strict_p50),
            client["compiler_recipe"]["executable"],
            client["compiler_recipe"]["binary_sha256"],
            compiler_identity_digest(client),
            str(len(compiler_args)),
            *compiler_args,
            turn,
            fault.get("kind", ""),
            fault.get("client", ""),
            str(fault.get("job", 0)),
            *(
                (f"/results/workload/{turn}/checkpoint.json", "1")
                + ((checkpoint_sha256,) if checkpoint_sha256 is not None else ())
                if resume
                else ()
            ),
        ),
    )
    return factory.make(
        phase="run.workload",
        host=client["host"],
        instance=client["name"],
        transport=_docker_transport(farm, client["host"]),
        timeout_s=timeout_s,
        argv=argv,
    )


def _d18_docker_call(
    farm: FarmSpec,
    plan: dict[str, Any],
    instance: dict[str, Any],
    factory: CommandFactory,
    transport: RecordingTransport,
    suffix: str,
    argv: tuple[str, ...],
    *,
    user: str = "0",
) -> CommandResult:
    command = factory.make(
        phase=f"run.d18.{suffix}",
        host=instance["host"],
        instance=instance["name"],
        transport=_docker_transport(farm, instance["host"]),
        timeout_s=20,
        argv=docker_argv(
            farm,
            instance["host"],
            (
                "exec", "--user", user,
                f"icefarm-{plan['run_id']}-{instance['name']}", *argv,
            ),
        ),
    )
    result = transport.invoke(command)
    if result.returncode != 0:
        raise WorkloadError(
            f"D18 {suffix} failed for {instance['name']}: "
            f"{result.stderr.strip() or result.stdout.strip() or result.returncode}"
        )
    return result


def _p51_gate_call(
    farm: FarmSpec,
    plan: dict[str, Any],
    client: dict[str, Any],
    factory: CommandFactory,
    transport: RecordingTransport,
    suffix: str,
    argv: tuple[str, ...],
    *,
    user: str = "0",
    timeout_s: int = 20,
) -> CommandResult:
    command = factory.make(
        phase=f"run.p51-receipt-window.{suffix}",
        host=client["host"],
        instance=client["name"],
        transport=_docker_transport(farm, client["host"]),
        timeout_s=timeout_s,
        argv=docker_argv(
            farm,
            client["host"],
            (
                "exec", "--user", user,
                f"icefarm-{plan['run_id']}-{client['name']}", *argv,
            ),
        ),
    )
    result = transport.invoke(command)
    if result.returncode != 0:
        raise WorkloadError(
            f"P51 receipt gate {suffix} failed for {client['name']}: "
            f"{result.stderr.strip() or result.stdout.strip() or result.returncode}"
        )
    return result


def _stage_p51_iptables_bundle(
    farm: FarmSpec,
    plan: dict[str, Any],
    client: dict[str, Any],
    factory: CommandFactory,
    transport: RecordingTransport,
) -> None:
    bundle_dir = os.environ.get("ICEFARM_P51_IPTABLES_BUNDLE")
    if not bundle_dir:
        raise WorkloadError(
            "P51 receipt gate requires ICEFARM_P51_IPTABLES_BUNDLE with the "
            "pinned Debian bookworm iptables package closure"
        )
    root = Path(bundle_dir)
    manifest = (root / "SHA256SUMS").read_bytes()
    if hashlib.sha256(manifest).hexdigest() != P51_IPTABLES_BUNDLE_MANIFEST_SHA256:
        raise WorkloadError("P51 iptables package bundle manifest hash differs from pin")
    expected: dict[str, str] = {}
    for line in manifest.decode("ascii").splitlines():
        digest, name = line.split(maxsplit=1)
        expected[name.strip().lstrip("* ")] = digest
    if set(expected) != set(P51_IPTABLES_BUNDLE_FILES):
        raise WorkloadError("P51 iptables package bundle has an unexpected file set")

    remote_dir = "/results/p51-receipt-gate/debs"
    _p51_gate_call(
        farm, plan, client, factory, transport, "stage-iptables-bundle-dir",
        ("/bin/sh", "-c", 'set -eu; mkdir -p "$1"', "stage-debs", remote_dir),
    )
    manifest_payload = base64.b64encode(manifest).decode("ascii")
    stage_items = [("SHA256SUMS", manifest_payload)]
    for name in P51_IPTABLES_BUNDLE_FILES:
        payload = (root / name).read_bytes()
        if hashlib.sha256(payload).hexdigest() != expected[name]:
            raise WorkloadError(f"P51 iptables package hash differs from pin: {name}")
        stage_items.append((name, base64.b64encode(payload).decode("ascii")))

    # Keep each argv payload below Linux's per-argument limit after the
    # transport's JSON/base64 wrapping; package bytes remain on the run-private
    # C output mount and never enter the immutable product image.
    chunk_size = 42000
    for name, encoded in stage_items:
        remote_path = f"{remote_dir}/{name}"
        _p51_gate_call(
            farm, plan, client, factory, transport, "stage-iptables-bundle-file",
            ("/bin/sh", "-c", ': > "$1"', "stage-deb", remote_path),
        )
        for offset in range(0, len(encoded), chunk_size):
            chunk = encoded[offset : offset + chunk_size]
            _p51_gate_call(
                farm, plan, client, factory, transport, "stage-iptables-bundle-chunk",
                (
                    "/bin/sh", "-c",
                    'printf %s "$1" | base64 -d >> "$2"',
                    "stage-deb-chunk", chunk, remote_path,
                ),
            )
        remote_hash = _p51_gate_call(
            farm, plan, client, factory, transport, "verify-iptables-bundle-file",
            (
                "/bin/sh", "-c", 'sha256sum "$1" | awk \'{print $1}\'',
                "verify-deb", remote_path,
            ),
        )
        expected_digest = (
            P51_IPTABLES_BUNDLE_MANIFEST_SHA256
            if name == "SHA256SUMS"
            else expected[name]
        )
        if remote_hash.stdout != f"{expected_digest}\n":
            raise WorkloadError(f"staged P51 iptables package hash mismatch: {name}")


def _p51_wait_marker(
    farm: FarmSpec,
    plan: dict[str, Any],
    client: dict[str, Any],
    factory: CommandFactory,
    transport: RecordingTransport,
    marker: str,
    *,
    timeout_s: int,
) -> str | None:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        result = _p51_gate_call(
            farm, plan, client, factory, transport, f"poll-{Path(marker).name}",
            ("/bin/sh", "-c", P51_GATE_READ_MARKER, "read-marker", marker),
        )
        if result.stdout != "__WAIT__\n":
            return result.stdout
        time.sleep(0.2)
    return None


def _run_p51_receipt_window(
    farm: FarmSpec,
    scenario: ScenarioSpec,
    plan: dict[str, Any],
    client: dict[str, Any],
    factory: CommandFactory,
    transport: RecordingTransport,
    turn: str,
) -> tuple[dict[str, Any], dict[str, Any]]:
    """Run a real manifest batch through the C-side remote receipt gate."""
    workload = scenario.data["workload"]
    gate_spec = workload["receipt_gate"]
    worker = next(item for item in plan["topology"]["instances"] if item["role"] == "F")
    endpoint_port = plan["ports"]["instances"][worker["name"]]
    expected = int(gate_spec["expected_commits"])
    expected_profile = next(
        item.get("env", {}).get("ICECC_P50_PROFILE")
        for item in scenario.data["instances"] if item["role"] == "S"
    )
    gate_dir = "/results/p51-receipt-gate"
    gate_binary = plan["p51_receipt_gate"]["container_path"]
    expected_gate_sha = plan["p51_receipt_gate"]["binary_sha256"]
    container = f"icefarm-{plan['run_id']}-{client['name']}"
    helper_probe = _p51_gate_call(
        farm, plan, client, factory, transport, "verify-staged-helper",
        (
            "/bin/sh", "-c",
            'set -eu; path=$1; expected=$2; test -x "$path"; '
            'actual=$(sha256sum "$path"); set -- $actual; '
            'test "$1" = "$expected"; printf "%s\\n" "$1"',
            "verify-helper", gate_binary, expected_gate_sha,
        ),
    )
    if helper_probe.stdout != expected_gate_sha + "\n":
        raise WorkloadError("staged P51 receipt-gate helper hash differs from the pinned binary")
    uid_result = _p51_gate_call(
        farm, plan, client, factory, transport, "sidecar-uid",
        ("/usr/bin/id", "-u", "nobody"),
    )
    try:
        sidecar_uid = int(uid_result.stdout.strip())
    except ValueError as exc:
        raise WorkloadError("receipt gate could not identify the C sidecar UID") from exc
    if sidecar_uid <= 0:
        raise WorkloadError("receipt gate requires a distinct non-root C sidecar UID")

    # Keep the redirect tool test-only and run-private: the immutable client
    # image intentionally does not ship iptables. Stage a checksum-pinned
    # Debian bookworm package closure from the host so the farm workload does
    # not depend on outbound package-mirror access.
    _stage_p51_iptables_bundle(farm, plan, client, factory, transport)
    _p51_gate_call(
        farm, plan, client, factory, transport, "install-test-iptables",
        (
            "/bin/sh", "-c",
            "set -eu; cd /results/p51-receipt-gate/debs; "
            "echo d1d7980ebc0b342caea791cc4fc86be4a6dcfc44f89e6884f2e4fbc69fd5568a  SHA256SUMS | sha256sum -c -; "
            "sha256sum -c SHA256SUMS; dpkg -i ./*.deb; "
            "test \"$(dpkg-query -W -f='${Version}' iptables)\" = 1.8.9-2; "
            "iptables --version",
            "install-test-iptables",
        ),
        timeout_s=180,
    )

    worker_addr = worker["address"]
    gate_shell = (
        'set +e; "$@" 2> /results/p51-receipt-gate/helper.stderr; rc=$?; '
        'cat /results/p51-receipt-gate/helper.stderr >&2; '
        'printf "%s\\n" "$rc" > /results/p51-receipt-gate/exit; exit 0'
    )
    gate_argv = (
        "exec", "--user", "0", "--env", "ICECC_TEST_POSITIVE_DAEMON=1",
        container, "/bin/sh", "-c", gate_shell, "receipt-gate",
        gate_binary, "--p51-commit-receipt-gate-remote", worker_addr,
        str(endpoint_port), str(sidecar_uid), str(expected), "1", gate_dir,
    )
    gate_command = factory.make(
        phase="run.p51-receipt-window.start-gate",
        host=client["host"], instance=client["name"],
        transport=_docker_transport(farm, client["host"]),
        timeout_s=workload["receipt_gate"].get("command_timeout_s", 260),
        argv=docker_argv(farm, client["host"], gate_argv),
    )
    # Keep normal compile-channel connections outside the UID-scoped receipt
    # redirect.  The C sidecar remains nobody (65534); daemon (UID 1) is a
    # distinct existing account with access to the run-private writable
    # /results mount.
    command = _driver_command(
        farm, scenario, plan, client, turn, factory, exec_uid="1:1"
    )
    _p51_gate_call(
        farm, plan, client, factory, transport, "prepare-control-dir",
        ("/bin/mkdir", "-p", gate_dir),
    )
    workload_future = None
    completed = False
    with ThreadPoolExecutor(max_workers=2) as executor:
        gate_future = executor.submit(transport.invoke, gate_command)
        try:
            ready = _p51_wait_marker(
                farm, plan, client, factory, transport, f"{gate_dir}/ready", timeout_s=20
            )
            if ready is None:
                exit_probe = _p51_gate_call(
                    farm, plan, client, factory, transport, "probe-gate-exit",
                    ("/bin/sh", "-c", P51_GATE_READ_MARKER,
                     "read-marker", f"{gate_dir}/exit"),
                )
                detail = "helper did not publish ready or exit"
                if exit_probe.stdout != "__WAIT__\n":
                    detail = f"helper exit={exit_probe.stdout.strip()}"
                    if gate_future.done():
                        gate_result = gate_future.result()
                        output = (gate_result.stderr or gate_result.stdout).strip()
                        if output:
                            detail += f": {output[-1200:]}"
                raise WorkloadError(f"remote P51 receipt gate did not become ready ({detail})")
            workload_future = executor.submit(transport.invoke, command)
            if gate_spec["expect_observed"]:
                held = _p51_wait_marker(
                    farm, plan, client, factory, transport,
                    f"{gate_dir}/held-1", timeout_s=40,
                )
                if held is None:
                    detail = "remote gate did not observe the expected COMMIT window"
                    if gate_future.done():
                        gate_result = gate_future.result()
                        output = (gate_result.stderr or gate_result.stdout).strip()
                        if output:
                            detail += f": {output[-1600:]}"
                    raise WorkloadError(detail)
                match = re.fullmatch(
                    r"count=([0-9]+) first_ordinal=([0-9]+) last_ordinal=([0-9]+) "
                    r"profile=([1-3]) window=([1-9][0-9]*) "
                    r"relationship=([0-9a-f]{32}) reservation=([0-9a-f]{32}) "
                    r"epoch=([1-9][0-9]*) generation=([1-9][0-9]*)\n",
                    held,
                )
                if match is None:
                    raise WorkloadError("remote gate held marker has invalid exact-window evidence")
                count, first, last = (int(match.group(i)) for i in (1, 2, 3))
                profile_id, selected_window = int(match.group(4)), int(match.group(5))
                profile_id_expected = P51_PROFILE_IDS[expected_profile]
                if (
                    count != expected or last - first + 1 != expected or first != 1
                    or selected_window != gate_spec["negotiated_window"]
                    or profile_id != profile_id_expected
                ):
                    raise WorkloadError(
                        f"remote gate observed unexpected negotiated window: {held.strip()}"
                    )
                if match.group(6) == "0" * 32 or match.group(7) == "0" * 32:
                    raise WorkloadError("remote gate negotiated zero relationship/reservation identity")
                _p51_gate_call(
                    farm, plan, client, factory, transport, "release-window",
                    ("/usr/bin/touch", f"{gate_dir}/release-1"),
                )
                released = _p51_wait_marker(
                    farm, plan, client, factory, transport,
                    f"{gate_dir}/released-1", timeout_s=35,
                )
                if released is None:
                    raise WorkloadError("remote gate did not acknowledge release of the held window")
                workload_result = workload_future.result()
                summary = _parse_summary(workload_result, client["name"])
                expected_jobs = (
                    farm.data["corpora"][workload["corpus"]]["tus"]
                    * farm.data["corpora"][workload["corpus"]].get("repeat", 1)
                    * workload["repeat"]
                )
                if summary["jobs"] != expected_jobs or summary["failures"] != 0:
                    raise WorkloadError("receipt-window manifest did not produce all exact local-SHA outputs")
                _p51_gate_call(
                    farm, plan, client, factory, transport, "finish-gate",
                    ("/usr/bin/touch", f"{gate_dir}/finish"),
                )
                gate_result = gate_future.result()
                exit_text = _p51_wait_marker(
                    farm, plan, client, factory, transport, f"{gate_dir}/exit", timeout_s=5
                )
                if exit_text != "0\n" or P51_NEGOTIATED_RE.search(gate_result.stderr) is None:
                    raise WorkloadError("remote gate did not exit cleanly after one exact R2 window")
                evidence = {
                    "count": count,
                    "first_ordinal": first,
                    "last_ordinal": last,
                    "profile": expected_profile,
                    "negotiated_window": selected_window,
                    "relationship_id": match.group(6),
                    "reservation_id": match.group(7),
                    "epoch": int(match.group(8)),
                    "physical_link_generation": int(match.group(9)),
                    "worker": worker["name"],
                    "worker_address": worker_addr,
                    "worker_port": endpoint_port,
                }
            else:
                # Under negotiated W1, the first exact receipt may be held but
                # the 30-receipt assertion must time out without a W30 marker.
                fail_summary = None
                deadline = time.monotonic() + 35
                while time.monotonic() < deadline:
                    polled = _p51_gate_call(
                        farm, plan, client, factory, transport, "poll-short-window",
                        ("/bin/sh", "-c", P51_GATE_READ_MARKER,
                         "read-marker", f"{gate_dir}/failed"),
                    )
                    if polled.stdout != "__WAIT__\n":
                        fail_summary = polled.stdout
                        break
                    if workload_future.done():
                        # The job may complete locally, but only the explicit
                        # gate failure record is accepted as this control.
                        workload_future.result()
                    time.sleep(0.2)
                if fail_summary is None or "first receipt window failed" not in fail_summary:
                    raise WorkloadError("W1 negative control did not reach the expected bounded gate timeout")
                gate_result = gate_future.result()
                exit_text = _p51_wait_marker(
                    farm, plan, client, factory, transport, f"{gate_dir}/exit", timeout_s=5
                )
                negotiated = P51_NEGOTIATED_RE.search(gate_result.stderr)
                failure = re.search(
                    r"P51_RECEIPT_GATE_FAIL port=[0-9]+ expected=30 first=1 "
                    r"woke=0 failed=0 commits=([0-9]+) peak=([0-9]+) "
                    r"ordinal_count=([0-9]+) ordinal_min=([0-9]+) ordinal_max=([0-9]+)",
                    gate_result.stderr,
                )
                if (
                    exit_text != "1\n" or negotiated is None or failure is None
                    or not _p51_negotiation_matches(
                        negotiated, profile=expected_profile, window=1
                    )
                    or tuple(int(failure.group(i)) for i in (1, 2, 3, 4, 5)) != (1, 1, 1, 1, 1)
                    or _p51_wait_marker(
                        farm, plan, client, factory, transport,
                        f"{gate_dir}/held-1", timeout_s=1,
                    ) is not None
                ):
                    raise WorkloadError("W1 negative control did not prove one held receipt and no W30 window")
                if workload_future is None:
                    raise WorkloadError("W1 negative control did not launch the manifest driver")
                workload_result = workload_future.result()
                summary = _parse_summary(workload_result, client["name"])
                evidence = {
                    "count": 1,
                    "expected_commits": expected,
                    "profile": expected_profile,
                    "negotiated_window": 1,
                    "single_held_ordinal": int(failure.group(4)),
                    "negative_control": "W1-no-W30-window",
                    "worker": worker["name"],
                    "worker_address": worker_addr,
                    "worker_port": endpoint_port,
                }
            completed = True
        finally:
            # Unblock helper waits and preserve a bounded owned process tree on
            # every failure path; these are run-private /results markers.
            for marker in ("abort", "release-1", "finish"):
                try:
                    _p51_gate_call(
                        farm, plan, client, factory, transport,
                        f"cleanup-{marker}", ("/usr/bin/touch", f"{gate_dir}/{marker}"),
                    )
                except BaseException:
                    pass
            if not completed:
                try:
                    transport.invoke(factory.make(
                        phase="run.p51-receipt-window.abort-client-container",
                        host=client["host"], instance=client["name"],
                        transport=_docker_transport(farm, client["host"]),
                        timeout_s=10,
                        argv=docker_argv(
                            farm, client["host"],
                            ("kill", f"icefarm-{plan['run_id']}-{client['name']}"),
                        ),
                    ))
                except BaseException:
                    pass
            if gate_future.done():
                try:
                    gate_future.result()
                except BaseException:
                    pass
            if workload_future is not None and workload_future.done():
                try:
                    workload_future.result()
                except BaseException:
                    pass
    return summary, evidence


def _d18_processes(
    farm: FarmSpec,
    plan: dict[str, Any],
    worker: dict[str, Any],
    factory: CommandFactory,
    transport: RecordingTransport,
) -> dict[int, int]:
    result = _d18_docker_call(
        farm,
        plan,
        worker,
        factory,
        transport,
        "observe-cc1plus",
        (
            "/bin/bash", "-c", D18_PROCESS_OBSERVER,
            "d18-observer",
        ),
    )
    observed: dict[int, int] = {}
    for line in result.stdout.splitlines():
        fields = line.split()
        if len(fields) != 2 or not all(field.isdecimal() for field in fields):
            continue
        pid, starttime = map(int, fields)
        if pid > 0 and starttime > 0:
            observed[pid] = starttime
    return observed


def _d18_concurrent_witness(
    first: dict[str, dict[int, int]],
    second: dict[str, dict[int, int]],
    expected_by_worker: dict[str, int],
) -> dict[str, list[dict[str, int]]] | None:
    """Require exact process counts and the same live identities in two sweeps."""
    if set(first) != set(expected_by_worker) or set(second) != set(expected_by_worker):
        return None
    if any(
        len(first[worker]) != count
        or len(second[worker]) != count
        or first[worker] != second[worker]
        for worker, count in expected_by_worker.items()
    ):
        return None
    return {
        worker: [
            {"pid": pid, "starttime_ticks": starttime}
            for pid, starttime in sorted(identities.items())
        ]
        for worker, identities in first.items()
    }


def _d18_barrier_and_observe(
    farm: FarmSpec,
    scenario: ScenarioSpec,
    plan: dict[str, Any],
    clients: list[dict[str, Any]],
    futures: list[Any],
    factory: CommandFactory,
    transport: RecordingTransport,
    deadline_s: int,
) -> dict[str, dict[str, Any]]:
    roles = scenario.data["workload"]["d18_roles"]
    by_name = {item["name"]: item for item in plan["topology"]["instances"]}
    worker_names = roles["workers"]
    expected_by_worker = {worker_names["R1"]: 2, worker_names["R2"]: 1}
    client_by_role = {role: by_name[name] for role, name in roles["clients"].items()}
    expected_ready = set(roles["clients"].values())
    ready_deadline = time.monotonic() + min(60, deadline_s)
    while True:
        ready = set()
        for client in clients:
            if client["name"] not in expected_ready:
                continue
            result = _d18_docker_call(
                farm, plan, client, factory, transport, "barrier-ready",
                ("/bin/bash", "-c", "test ! -f /results/workload/A/d18/ready || echo D18_READY"),
            )
            if "D18_READY" in result.stdout:
                ready.add(client["name"])
        if ready == expected_ready:
            break
        if any(future.done() for future in futures):
            for future in futures:
                if future.done():
                    future.result()
            raise WorkloadError("D18 client exited before the shared measured-start barrier")
        if time.monotonic() >= ready_deadline:
            raise WorkloadError("D18 clients did not all reach the measured-start barrier")
        time.sleep(0.1)

    # Release all three client-side gates concurrently only after each has
    # completed oracle warmup and published its ready marker.
    with ThreadPoolExecutor(max_workers=3) as release_pool:
        release_futures = [
            release_pool.submit(
                _d18_docker_call,
                farm, plan, client_by_role[role], factory, transport,
                f"barrier-release-{role}",
                ("/usr/bin/touch", "/results/workload/A/d18/go"),
            )
            for role in ("P43", "R1", "R2")
        ]
        for future in release_futures:
            future.result()

    deadline = time.monotonic() + min(60, deadline_s)
    while True:
        first: dict[str, dict[int, int]] = {}
        for worker_name in expected_by_worker:
            worker = by_name[worker_name]
            snapshots = _d18_processes(farm, plan, worker, factory, transport)
            if snapshots:
                first[worker_name] = snapshots
        second: dict[str, dict[int, int]] = {}
        if set(first) == set(expected_by_worker):
            for worker_name in expected_by_worker:
                worker = by_name[worker_name]
                second[worker_name] = _d18_processes(
                    farm, plan, worker, factory, transport
                )
            witness = _d18_concurrent_witness(first, second, expected_by_worker)
            if witness is not None:
                return {"workers": witness}
        if all(future.done() for future in futures):
            for future in futures:
                future.result()
            raise WorkloadError(
                "D18 jobs completed without simultaneous two-process R1-worker "
                "and one-process R2-worker evidence"
            )
        if time.monotonic() >= deadline:
            raise WorkloadError("D18 simultaneous three-role process witness timed out")
        time.sleep(0.05)


def _d18_verify_remote_rows(
    farm: FarmSpec,
    scenario: ScenarioSpec,
    plan: dict[str, Any],
    client: dict[str, Any],
    role: str,
    worker_name: str,
    factory: CommandFactory,
    transport: RecordingTransport,
) -> dict[str, Any]:
    script = r'''python3 - "$1" "$2" "$3" <<'PY'
import json, pathlib, sys
root, worker, expected = pathlib.Path(sys.argv[1]), sys.argv[2], int(sys.argv[3])
rows = []
for path in sorted(root.glob("*/result.tsv")):
    values = path.read_text(encoding="ascii").splitlines()
    if len(values) != 1 or len(values[0].split("\t")) != 14:
        raise SystemExit("malformed measured result row")
    row = values[0].split("\t")
    if row[5] != worker or row[8] != "0" or row[11] != "1" or row[12] != "1":
        raise SystemExit("role job was not exact remote work on its required F")
    rows.append({"job_id": row[4], "worker": row[5], "index": int(row[0])})
if len(rows) != expected:
    raise SystemExit(f"expected {expected} measured rows, observed {len(rows)}")
print(json.dumps({"jobs": rows, "count": len(rows)}, sort_keys=True))
PY'''
    expected = farm.data["corpora"][scenario.data["workload"]["corpus"]]["tus"]
    expected *= farm.data["corpora"][scenario.data["workload"]["corpus"]].get("repeat", 1)
    expected *= scenario.data["workload"]["repeat"]
    result = _d18_docker_call(
        farm, plan, client, factory, transport, f"verify-remote-{role}",
        ("/bin/bash", "-c", script, "d18-verify",
         "/results/workload/A/jobs", worker_name, str(expected)),
    )
    try:
        receipt = json.loads(result.stdout)
    except (TypeError, ValueError, json.JSONDecodeError) as exc:
        raise WorkloadError(f"D18 {role} remote-row receipt is malformed") from exc
    if not isinstance(receipt, dict) or receipt.get("count") != expected:
        raise WorkloadError(f"D18 {role} lacks complete exact remote-job evidence")
    return receipt


def _d18_verify_r2_link_adoption(
    farm: FarmSpec,
    plan: dict[str, Any],
    worker: dict[str, Any],
    factory: CommandFactory,
    transport: RecordingTransport,
) -> dict[str, Any]:
    result = _d18_docker_call(
        farm,
        plan,
        worker,
        factory,
        transport,
        "verify-r2-link-adoption",
        ("/bin/bash", "-c", "cat /var/log/icecream/iceccd.log"),
    )
    matches = [
        {"generation": int(generation), "attempt": int(attempt)}
        for generation, attempt in D18_R2_ADOPTION_RE.findall(result.stdout)
    ]
    if not matches:
        raise WorkloadError(
            "D18 R2 worker has no cache-link descriptor adoption evidence"
        )
    return {"adoptions": matches, "worker": worker["name"]}


def _parse_summary(result: CommandResult, client: str) -> dict[str, int | str]:
    if result.returncode != 0:
        detail = result.stderr.strip() or "no stderr"
        raise WorkloadError(
            f"client {client} workload command failed rc={result.returncode}: {detail}"
        )
    matches = SUMMARY_RE.findall(result.stdout)
    if len(matches) != 1:
        raise WorkloadError(f"client {client} returned no unique workload summary")
    jobs, failures, samples = (int(value) for value in matches[0])
    if jobs < 1 or samples < 1 or failures > jobs:
        raise WorkloadError(f"client {client} returned an invalid workload summary")
    return {"client": client, "failures": failures, "jobs": jobs, "samples": samples}


def run_workload(
    farm: FarmSpec,
    scenario: ScenarioSpec,
    plan: dict[str, Any],
    *,
    recorder: RecordingTransport | None = None,
    require_up: bool = True,
    event_job_reader: JobReader | None = None,
    event_path: Path | None = None,
) -> dict[str, Any]:
    """Run each authenticated turn, preserving live daemon state between turns."""

    if require_up:
        _assert_up(farm, scenario, plan)
    transport = recorder or RecordingTransport()
    factory = CommandFactory()
    client_names = set(scenario.data["workload"]["clients"])
    clients = sorted(
        (
            item
            for item in plan["topology"]["instances"]
            if item["role"] == "C" and item["name"] in client_names
        ),
        key=lambda item: item["name"],
    )
    if {item["name"] for item in clients} != client_names:
        raise WorkloadError("resolved topology does not contain every workload client")
    command_offset = len(transport.commands)
    totals = {
        client["name"]: {
            "client": client["name"],
            "failures": 0,
            "jobs": 0,
            "samples": 0,
        }
        for client in clients
    }
    turn_receipts: list[dict[str, Any]] = []
    turn_context: dict[str, Any] = {
        "turn": None,
        "futures": [],
        "commands": [],
        "ready": None,
    }
    turn_overrides: dict[str, list[CommandResult]] = {}
    d18_roles = scenario.data["workload"].get("d18_roles")
    checkpointed_client_events = [
        event
        for event in scenario.data.get("timeline", [])
        if event.get("trigger", "").startswith("job ")
        and event.get("action") in {"upgrade", "downgrade", "env_set"}
        and any(
            item.get("name") == event.get("instance") and item.get("role") == "C"
            for item in scenario.data["instances"]
        )
    ]
    if checkpointed_client_events and len(scenario.data["workload"]["turns"]) != 1:
        raise WorkloadError(
            "checkpointed C transitions require one unambiguous workload turn"
        )
    transition_waiters: dict[str, threading.Event] = (
        {scenario.data["workload"]["turns"][0]: threading.Event()}
        if checkpointed_client_events
        else {}
    )

    def _checkpoint_documents(
        turn: str, workload_clients: tuple[dict[str, Any], ...]
    ) -> dict[str, Any]:
        if turn_context["turn"] != turn:
            raise WorkloadError("checkpoint requested for an inactive workload turn")
        ready = turn_context["ready"]
        if not isinstance(ready, threading.Event) or not ready.wait(
            timeout=max(1.0, scenario.data["timeouts"]["turn_s"])
        ):
            raise WorkloadError("checkpoint requested before workload dispatch was reserved")
        if turn_context["turn"] != turn:
            raise WorkloadError("checkpoint workload turn changed during dispatch")
        for future in turn_context["futures"]:
            future.result()
        documents: dict[str, Any] = {}
        for client in workload_clients:
            container = f"icefarm-{plan['run_id']}-{client['name']}"
            command = factory.make(
                phase="event.checkpoint",
                host=client["host"],
                instance=client["name"],
                transport=_docker_transport(farm, client["host"]),
                timeout_s=scenario.data["timeouts"]["turn_s"],
                argv=docker_argv(
                    farm,
                    client["host"],
                    (
                        "exec",
                        "--user",
                        "65534:65534",
                        container,
                        "cat",
                        f"/results/workload/{turn}/checkpoint.json",
                    ),
                ),
            )
            result = transport.invoke(command)
            if result.returncode != 0:
                raise WorkloadError(f"client {client['name']} checkpoint read failed")
            try:
                documents[client["name"]] = json.loads(result.stdout)
            except (TypeError, ValueError, json.JSONDecodeError) as exc:
                raise WorkloadError(f"client {client['name']} checkpoint is malformed") from exc
        if turn not in transition_waiters:
            raise WorkloadError("checkpoint requested without a reserved transition turn")
        return documents

    def _relaunch_from_checkpoint(
        turn: str, checkpoints: dict[str, Any]
    ) -> dict[str, Any]:
        commands = [
            _driver_command(
                farm,
                scenario,
                plan,
                client,
                turn,
                factory,
                resume=True,
                checkpoint_sha256=checkpoints[client["name"]]["checkpoint_sha256"],
            )
            for client in clients
        ]
        with ThreadPoolExecutor(max_workers=len(commands)) as resume_executor:
            futures = [resume_executor.submit(transport.invoke, command) for command in commands]
            results = [future.result() for future in futures]
        evidence: dict[str, Any] = {}
        for client, result in zip(clients, results, strict=True):
            if result.returncode != 0:
                detail = result.stderr.strip() or "no stderr"
                raise WorkloadError(
                    f"client {client['name']} checkpoint relaunch failed "
                    f"rc={result.returncode}: {detail}"
                )
            summary = _parse_summary(result, client["name"])
            expected_jobs = int(checkpoints[client["name"]]["expected_jobs"])
            evidence[client["name"]] = {
                "client": client["name"],
                "expected_jobs": expected_jobs,
                "failures": int(summary["failures"]),
                "jobs": int(summary["jobs"]),
                "status": "COMPLETE",
            }
        turn_overrides[turn] = results
        if turn not in transition_waiters:
            raise WorkloadError("checkpoint relaunch has no reserved transition turn")
        transition_waiters[turn].set()
        return evidence

    # Validate all actions before dispatching a workload command.  The event
    # worker is kept under this function's ownership and is always joined.
    events = EventProducer(
        farm,
        scenario,
        plan,
        recorder=transport,
        factory=factory,
        job_reader=event_job_reader,
        event_path=event_path,
        deadline_s=scenario.data["timeouts"]["turn_s"]
        * len(scenario.data["workload"]["turns"]),
        quiesce_workload=_checkpoint_documents,
        relaunch_workload=_relaunch_from_checkpoint,
    )
    primary: BaseException | None = None
    try:
        events.start()
        for index, turn in enumerate(scenario.data["workload"]["turns"]):
            events.raise_if_failed()
            activation = None
            if index > 0:
                activation = activate_corpus_turn(
                    farm,
                    scenario,
                    plan,
                    turn,
                    transport,
                    factory,
                    timeout_s=scenario.data["timeouts"]["turn_s"],
                )
            events.signal_turn_start(turn)
            if scenario.data["workload"]["driver"] == "p51-receipt-window":
                summary, window_evidence = _run_p51_receipt_window(
                    farm, scenario, plan, clients[0], factory, transport, turn
                )
                total = totals[clients[0]["name"]]
                for field in ("failures", "jobs", "samples"):
                    total[field] += int(summary[field])
                turn_receipts.append({
                    "clients": [summary],
                    "p51_receipt_window": window_evidence,
                    "turn": turn,
                })
                events.signal_turn_complete(turn)
                events.raise_if_failed()
                continue
            commands = [
                _driver_command(farm, scenario, plan, client, turn, factory)
                for client in clients
            ]
            executor = ThreadPoolExecutor(max_workers=len(commands))
            dispatch_ready = threading.Event()
            futures = []
            d18_evidence: dict[str, Any] | None = None
            try:
                turn_context.update(
                    {
                        "turn": turn,
                        "futures": futures,
                        "commands": commands,
                        "ready": dispatch_ready,
                    }
                )
                for command in commands:
                    futures.append(executor.submit(transport.invoke, command))
                dispatch_ready.set()
                if isinstance(d18_roles, dict):
                    d18_evidence = _d18_barrier_and_observe(
                        farm,
                        scenario,
                        plan,
                        clients,
                        futures,
                        factory,
                        transport,
                        scenario.data["timeouts"]["turn_s"],
                    )
                if _active_loss_serial_through(scenario):
                    events.prepare_active_compiler_boundary(turn)
                results = [future.result() for future in futures]
            finally:
                dispatch_ready.set()
                executor.shutdown(wait=True, cancel_futures=True)
            waiter = transition_waiters.get(turn)
            if waiter is not None:
                deadline = time.monotonic() + max(
                    1.0, scenario.data["timeouts"]["turn_s"]
                )
                while not waiter.wait(timeout=0.05):
                    events.raise_if_failed()
                    if time.monotonic() >= deadline:
                        raise WorkloadError(
                            "checkpointed C transition did not relaunch the workload"
                        )
                events.raise_if_failed()
            results = turn_overrides.pop(turn, results)
            turn_context.update(
                {"turn": None, "futures": [], "commands": [], "ready": None}
            )
            summaries = [
                _parse_summary(result, client["name"])
                for result, client in zip(results, clients, strict=True)
            ]
            for summary in summaries:
                total = totals[str(summary["client"])]
                for field in ("failures", "jobs", "samples"):
                    total[field] += int(summary[field])
            turn_receipt: dict[str, Any] = {
                "activation": activation,
                "clients": summaries,
                "turn": turn,
            }
            if isinstance(d18_roles, dict):
                if d18_evidence is None:
                    raise WorkloadError("D18 workload lacks a live overlap observation")
                role_jobs: dict[str, Any] = {}
                for role, client_name in d18_roles["clients"].items():
                    target_role = "R1" if role in ("P43", "R1") else "R2"
                    target_worker = d18_roles["workers"][target_role]
                    role_jobs[role] = _d18_verify_remote_rows(
                        farm,
                        scenario,
                        plan,
                        next(item for item in clients if item["name"] == client_name),
                        role,
                        target_worker,
                        factory,
                        transport,
                    )
                d18_evidence["measured_remote_jobs"] = role_jobs
                r2_worker = next(
                    item
                    for item in plan["topology"]["instances"]
                    if item["name"] == d18_roles["workers"]["R2"]
                )
                d18_evidence["r2_link_adoption"] = _d18_verify_r2_link_adoption(
                    farm, plan, r2_worker, factory, transport
                )
                turn_receipt["d18_concurrent_processes"] = d18_evidence
            turn_receipts.append(turn_receipt)
            events.signal_turn_complete(turn)
            events.raise_if_failed()
        # A successful workload cannot cancel still-pending timeline events.
        # Wait for every declared trigger/action to become terminal so a
        # missing job/turn trigger is a harness error rather than absent
        # evidence that silently looks like an empty timeline.
        events.wait()
    except (LifecycleError, RemoteError, EventError) as exc:
        try:
            events.raise_if_failed()
        except BaseException as event_exc:
            primary = WorkloadError(f"timeline failure: {event_exc}; workload failure: {exc}")
            primary.__cause__ = event_exc
        else:
            primary = WorkloadError(str(exc))
    except BaseException as exc:
        primary = exc
    finally:
        try:
            events.stop()
        except BaseException as event_exc:
            if primary is None:
                primary = event_exc
            else:
                combined = WorkloadError(
                    f"timeline failure: {event_exc}; workload failure: {primary}"
                )
                combined.__cause__ = event_exc
                primary = combined
    if primary is not None:
        raise primary
    summaries = [totals[client["name"]] for client in clients]
    commands = sorted(
        transport.commands[command_offset:], key=lambda command: command.sequence
    )
    receipt = {
        "clients": summaries,
        "commands": [command.as_dict() for command in commands],
        "farm_digest": farm.digest,
        "run_id": plan["run_id"],
        "scenario_digest": scenario.digest,
        "schema": WORKLOAD_SCHEMA,
        "status": "COMPLETE"
        if all(item["failures"] == 0 for item in summaries)
        else "COMPLETE_WITH_JOB_FAILURES",
        "topology_digest": plan["topology_digest"],
        "turns": turn_receipts,
    }
    _atomic_json(bundle_root(farm, plan["run_id"]) / "workload.json", receipt)
    return receipt
