"""The successor H3 recipe must not inject faults during readiness."""

import os
import json
import hashlib
import copy
import pytest
from types import SimpleNamespace
from pathlib import Path
import subprocess

from farmharness.integration.mutant import (
    derive_scheduler_mutant,
    scheduler_mutant_requires_arming,
    h3_workload_dispatches, MUTANT_ARM_CONTRACT, MutantError,
)
from farmharness.integration.lifecycle import H3_ARM_SCRIPT
from farmharness.integration import farmtest
from farmharness.integration.tests.test_mutant import _farm
from farmharness.integration.scenario_spec import load_scenario_spec
from farmharness.integration.tests.test_lifecycle import _farm_scenario_plan, ScriptedLifecycle
from farmharness.integration.images import RecordingTransport
from farmharness.integration.remote import CommandResult
from farmharness.integration.lifecycle import bring_up, LifecycleError


ROOT = Path(__file__).resolve().parents[1]
PATCH = ROOT / "mutants/scheduler-tail-armed.patch"


def test_new_recipe_plan_selects_arming_and_old_plan_does_not():
    farm = _farm()
    scenario = load_scenario_spec(ROOT / "scenarios/H3-mutant-scheduler.json", farm)
    old = farmtest.build_plan(farm, scenario, run_id="h3-old")
    assert "h3_arm_contract" not in old
    label = scenario.data["images"]["mutant"]
    image = farm.data["authority"]["images"][label]
    recipe = derive_scheduler_mutant(
        image["base_image"], farm.data["authority"]["images"][image["base_image"]],
        PATCH, label=label,
    )
    farm.data["authority"]["images"][label] = {**image, **recipe}
    new = farmtest.build_plan(farm, scenario, run_id="h3-new")
    assert new["h3_arm_contract"] == MUTANT_ARM_CONTRACT
    start = next(command for command in new["commands"] if command["phase"] == "up.start-s")
    assert "ICECC_P50_H3_ARM_FILE=/results/h3-armed.json" in start["argv"]


@pytest.mark.parametrize("canary_fails", [False, True])
def test_lifecycle_arms_only_after_successful_readiness(tmp_path, canary_fails):
    farm, scenario, plan = _farm_scenario_plan(tmp_path)
    plan["h3_arm_contract"] = MUTANT_ARM_CONTRACT
    scripted = ScriptedLifecycle(farm)
    marker = {"schema": MUTANT_ARM_CONTRACT, "run_id": plan["run_id"], "scheduler": "S1"}
    def invoke(command):
        if command.phase == "readiness.canary" and canary_fails:
            raise LifecycleError("canary failed")
        if command.phase == "readiness.h3-arm":
            return CommandResult(0, json.dumps(marker), "")
        return scripted.invoke(command)
    transport = RecordingTransport(SimpleNamespace(invoke=invoke))
    if canary_fails:
        with pytest.raises(LifecycleError, match="canary failed"):
            bring_up(farm, scenario, plan, recorder=transport, probe_bytes=0, sync_corpora=False)
        assert not any(command.phase == "readiness.h3-arm" for command in transport.commands)
    else:
        receipt = bring_up(farm, scenario, plan, recorder=transport, probe_bytes=0, sync_corpora=False)
        assert receipt["h3_arm"] == marker
        phases = [command.phase for command in transport.commands]
        assert phases.index("readiness.canary") < phases.index("readiness.environments") < phases.index("readiness.h3-arm")


def test_armed_recipe_is_distinct_and_preserves_historical_recipe():
    base = {"commit": "a" * 40, "archive_sha256": "b" * 64}
    old = derive_scheduler_mutant("base", base)
    new = derive_scheduler_mutant("base", base, PATCH)
    assert old["recipe_sha256"] != new["recipe_sha256"]
    assert old["base_archive_sha256"] == new["base_archive_sha256"]
    assert not scheduler_mutant_requires_arming(old)
    assert scheduler_mutant_requires_arming(new)
    assert not scheduler_mutant_requires_arming({**new, "kind": "daemon-mutant"})
    assert not scheduler_mutant_requires_arming({**new, "patch_sha256": "0" * 64})


def test_successor_patch_applies_to_exact_historical_base(tmp_path):
    repository = ROOT.parents[1]
    for name in ("scheduler/scheduler.cpp", "services/comm.cpp"):
        original = subprocess.run(
            ["git", "show", f"57a1e33624d324e52c1d17481fcc43046eaa3139:{name}"],
            cwd=repository, check=True, capture_output=True,
        ).stdout
        target = tmp_path / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(original)
    subprocess.run(
        ["git", "apply", "--check", str(PATCH)],
        cwd=tmp_path, check=True,
    )
    subprocess.run(
        ["git", "apply", str(PATCH)],
        cwd=tmp_path, check=True,
    )
    for name in ("scheduler/scheduler.cpp", "services/comm.cpp"):
        assert "ICECC_P50_H3_ARM_FILE" in (tmp_path / name).read_text()


def test_both_mutant_sites_require_explicit_arm_file(tmp_path):
    patch = PATCH.read_text()
    gate = '(getenv("ICECC_P50_H3_MUTANT") != nullptr && getenv("ICECC_P50_H3_ARM_FILE") != nullptr && access(getenv("ICECC_P50_H3_ARM_FILE"), F_OK) == 0)'
    assert patch.count(gate) == 2
    source = tmp_path / "gate.cpp"
    binary = tmp_path / "gate"
    source.write_text('#include <cstdlib>\n#include <unistd.h>\nint main() { return ' + gate + ' ? 42 : 0; }\n')
    subprocess.run(["c++", str(source), "-o", str(binary)], check=True)
    env = {k: v for k, v in os.environ.items() if not k.startswith("ICECC_P50_H3_")}
    def observed():
        return subprocess.run([str(binary)], env=env, check=False).returncode
    assert observed() == 0
    env["ICECC_P50_H3_MUTANT"] = "1"
    assert observed() == 0
    marker = tmp_path / "armed"
    env["ICECC_P50_H3_ARM_FILE"] = str(marker)
    assert observed() == 0
    marker.touch()
    assert observed() == 42
    del env["ICECC_P50_H3_MUTANT"]
    assert observed() == 0


def test_arming_retains_log_boundary_and_refuses_rearming(tmp_path):
    log = tmp_path / "scheduler.log"
    log.write_text("ready\ncanary complete\n")
    marker = tmp_path / "h3-armed.json"
    script = H3_ARM_SCRIPT.replace("/var/log/icecream/scheduler.log", str(log))
    command = ["python3", "-c", script, str(marker), "run", "S1", "contract"]
    result = subprocess.run(command, check=True, capture_output=True, text=True)
    receipt = json.loads(result.stdout)
    assert json.loads(marker.read_text()) == receipt
    assert receipt["scheduler_log_bytes"] == len(log.read_bytes())
    assert receipt["scheduler_log_lines"] == 2
    original = marker.read_bytes()
    assert subprocess.run(command, capture_output=True).returncode != 0
    assert marker.read_bytes() == original


def test_arming_refuses_premature_mutant_emission(tmp_path):
    marker = tmp_path / "h3-armed.json"
    (tmp_path / "h3-mutant.jsonl").write_text("unexpected\n")
    result = subprocess.run(
        ["python3", "-c", H3_ARM_SCRIPT, str(marker), "run", "S1", "contract"],
        capture_output=True,
    )
    assert result.returncode != 0
    assert not marker.exists()


@pytest.mark.parametrize("tamper", [None, "prefix", "receipt", "missing-canary", "late", "failed", "extra"])
def test_h3_boundary_authenticates_every_readiness_job(tamper):
    prefix = b"ready\nnew\ndispatch\nend\n"
    marker = {"schema": MUTANT_ARM_CONTRACT, "run_id": "run", "scheduler": "S1",
              "scheduler_log_bytes": len(prefix), "scheduler_log_lines": 4,
              "scheduler_log_sha256": hashlib.sha256(prefix).hexdigest()}
    receipt = copy.deepcopy(marker)
    ready = {"line": 2, "terminal_line": 4, "terminal": "completion", "status": 0,
             "scheduler_job": 1, "client": "C1", "worker": "F1"}
    work = {**ready, "line": 5, "terminal_line": 8, "scheduler_job": 2}
    jobs, claims = [ready, work], [dict(ready)]
    if tamper == "prefix":
        prefix = prefix.replace(b"ready", b"READY")
    elif tamper == "receipt":
        receipt["run_id"] = "wrong"
    elif tamper == "missing-canary":
        claims = []
    elif tamper == "late":
        ready["terminal_line"] = 6
    elif tamper == "failed":
        ready["status"] = 1
    elif tamper == "extra":
        jobs.append({**ready, "scheduler_job": 3})
    def evaluate():
        return h3_workload_dispatches(marker, receipt, prefix + b"work\n", jobs,
                                     claims, run_id="run", scheduler="S1")
    if tamper is None:
        assert evaluate() == [work]
    else:
        with pytest.raises(MutantError):
            evaluate()
