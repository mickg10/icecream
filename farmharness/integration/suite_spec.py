"""Validated, fixed-order suite specifications."""

from __future__ import annotations

import hashlib
from dataclasses import dataclass
from functools import cached_property
from pathlib import Path
from typing import Any

try:
    from .schema_validation import ValidationError, canonical_bytes, load_json, validate
except ImportError:  # Direct execution from this directory.
    from schema_validation import ValidationError, canonical_bytes, load_json, validate


SCHEMA_PATH = Path(__file__).with_name("schemas") / "suite-v1.json"

CONTROL_SCENARIOS = (
    ("H1-role-hash-refusal", "H1"),
    ("H2-client-kill-switch", "H2"),
    ("H3-mutant-scheduler", "H3"),
    ("H4-corrupt-object", "H4"),
    ("H5-worker-kill", "H5"),
)
CONTROL_SCENARIO_IDS = tuple(scenario_id for scenario_id, _control in CONTROL_SCENARIOS)
SMOKE_SCENARIO_IDS = (*CONTROL_SCENARIO_IDS, "S00-smoke")
S50_FAIRNESS_SCENARIO_IDS = (
    "S50-all-legacy-control",
    "S50-mixed-pool-1of9",
    "S50-mixed-pool-5of9",
    "S50-mixed-pool-8of9",
    "S50-all-f-new-mixed-c",
)
S60_SCENARIO_IDS = tuple(f"S60-{index:02d}-{name}" for index, name in enumerate(
    (
        "s-up",
        "f1-up",
        "f2-up",
        "c1-up",
        "c2-up",
        "c2-down",
        "c1-down",
        "f2-down",
        "f1-down",
        "s-down",
        "warm-f2-down",
        "warm-f2-up",
        "warm-s-down",
        "warm-s-up",
    ),
    start=1,
))
S70_SCENARIO_IDS = (
    "S70-b4-worker-bounces",
    "S70-b4-scheduler-restart",
    "S70-b4-client-route-restart",
    "S70-b5-interner-failure",
    "S70-b6-kill-switch",
    "S70-b7-rollback",
    "S70-b7-rollforward",
)


class SuiteSpecError(ValueError):
    """A suite document is malformed or names unsafe scenario identifiers."""


@dataclass(frozen=True)
class SuiteSpec:
    path: Path
    data: dict[str, Any]
    included_suites: tuple[tuple[str, SuiteSpec], ...] = ()

    @cached_property
    def digest(self) -> str:
        if not self.is_composite:
            digest_input: dict[str, Any] = self.data
        else:
            digest_input = {
                "document": self.data,
                "included_suites": [
                    {
                        "digest": child.digest,
                        "name": suite_name,
                    }
                    for suite_name, child in self.included_suites
                ],
                "schema": "icefarm-composite-suite-digest-v1",
            }
        return hashlib.sha256(canonical_bytes(digest_input)).hexdigest()

    @property
    def scenario_dir(self) -> Path:
        return self.path.parent.parent / "scenarios"

    def scenario_path(self, scenario_id: str) -> Path:
        path = self.scenario_dir / f"{scenario_id}.json"
        if not path.is_file() or path.is_symlink():
            raise SuiteSpecError(
                f"suite scenario {scenario_id!r} is absent or unsafe: {path}"
            )
        return path

    def suite_path(self, suite_name: str) -> Path:
        path = self.path.parent / f"{suite_name}.json"
        if not path.is_file() or path.is_symlink():
            raise SuiteSpecError(
                f"included suite {suite_name!r} is absent or unsafe: {path}"
            )
        return path

    @property
    def is_composite(self) -> bool:
        return "suites" in self.data

    def expanded_scenario_ids(self) -> tuple[tuple[str, int], ...]:
        """Return declared cells as ``(scenario id, repetition)`` pairs."""

        repetitions = self.data.get("repetitions", {})
        return tuple(
            (scenario_id, repetition)
            for scenario_id in self.data["scenarios"]
            for repetition in range(1, repetitions.get(scenario_id, 1) + 1)
        )


def _load_suite_spec(path: str | Path, ancestors: tuple[Path, ...]) -> SuiteSpec:
    resolved = Path(path).resolve()
    if resolved in ancestors:
        cycle = " -> ".join(item.name for item in (*ancestors, resolved))
        raise SuiteSpecError(f"suite include cycle: {cycle}")
    try:
        value = load_json(resolved)
        validate(value, SCHEMA_PATH)
    except ValidationError as exc:
        raise SuiteSpecError(str(exc)) from exc
    if not isinstance(value, dict):
        raise SuiteSpecError("$: suite root must be an object")
    has_scenarios = "scenarios" in value
    has_suites = "suites" in value
    if has_scenarios == has_suites:
        raise SuiteSpecError("suite must declare exactly one of scenarios or suites")
    suite = SuiteSpec(path=resolved, data=value)
    if suite.is_composite:
        if value.get("kind") != "composite":
            raise SuiteSpecError("a suite with included suites must have kind 'composite'")
        if len({name.casefold() for name in value["suites"]}) != len(value["suites"]):
            raise SuiteSpecError(
                "composite suite names collide case-insensitively"
            )
        forbidden = sorted(set(value) & {"fairness", "performance", "repetitions"})
        if forbidden:
            raise SuiteSpecError(
                f"composite suite cannot declare atomic metadata: {forbidden!r}"
            )
        included_suites = tuple(
            (
                suite_name,
                _load_suite_spec(
                    suite.suite_path(suite_name),
                    (*ancestors, resolved),
                ),
            )
            for suite_name in value["suites"]
        )
        return SuiteSpec(
            path=resolved,
            data=value,
            included_suites=included_suites,
        )
    if value.get("kind") == "composite":
        raise SuiteSpecError("a composite suite must declare included suites")
    for scenario_id in value["scenarios"]:
        suite.scenario_path(scenario_id)
    kind = value.get("kind", "generic")
    repetitions = value.get("repetitions", {})
    unknown_repetitions = sorted(set(repetitions) - set(value["scenarios"]))
    if unknown_repetitions:
        raise SuiteSpecError(
            "suite repetitions name scenarios absent from suite: "
            f"{unknown_repetitions!r}"
        )
    if repetitions and kind in {"controls", "smoke"}:
        raise SuiteSpecError(f"{kind} suite cannot override exact repetitions")
    required_ids: tuple[str, ...] | None = None
    if kind == "controls":
        required_ids = CONTROL_SCENARIO_IDS
    elif kind == "smoke":
        required_ids = SMOKE_SCENARIO_IDS
    elif kind == "s60-transitions":
        required_ids = S60_SCENARIO_IDS
    elif kind == "s70-resilience":
        required_ids = S70_SCENARIO_IDS
    if required_ids is not None and tuple(value["scenarios"]) != required_ids:
        raise SuiteSpecError(
            f"{kind} suite scenarios must be exactly {list(required_ids)!r} in order"
        )
    if kind in {"controls", "smoke"}:
        for scenario_id, control_id in CONTROL_SCENARIOS:
            try:
                scenario = load_json(suite.scenario_path(scenario_id))
            except ValidationError as exc:
                raise SuiteSpecError(str(exc)) from exc
            if not isinstance(scenario, dict):
                raise SuiteSpecError(
                    f"control scenario {scenario_id!r} must be an object"
                )
            if scenario.get("id") != scenario_id:
                raise SuiteSpecError(
                    f"control scenario file {scenario_id!r} declares id {scenario.get('id')!r}"
                )
            if scenario.get("controls") != [control_id]:
                raise SuiteSpecError(
                    f"control scenario {scenario_id!r} must declare exactly [{control_id!r}]"
                )
    if kind == "smoke":
        smoke = load_json(suite.scenario_path("S00-smoke"))
        if not isinstance(smoke, dict) or smoke.get("id") != "S00-smoke":
            raise SuiteSpecError("smoke scenario file must declare id 'S00-smoke'")
        if smoke.get("controls") != []:
            raise SuiteSpecError("S00-smoke must not declare harness controls")
    if kind == "s50-fairness":
        if repetitions:
            raise SuiteSpecError("S50 fairness suite cannot override repetitions")
        if tuple(value["scenarios"]) != S50_FAIRNESS_SCENARIO_IDS:
            raise SuiteSpecError(
                "S50 fairness suite scenarios must be exactly "
                f"{list(S50_FAIRNESS_SCENARIO_IDS)!r} in order"
            )
        fairness = value.get("fairness")
        if not isinstance(fairness, dict):
            raise SuiteSpecError("S50 fairness suite requires fairness metadata")
        if fairness.get("control") != S50_FAIRNESS_SCENARIO_IDS[0]:
            raise SuiteSpecError("S50 fairness control must be the first scenario")
        if tuple(fairness.get("mixed", ())) != S50_FAIRNESS_SCENARIO_IDS[1:4]:
            raise SuiteSpecError("S50 fairness mixed cells must be the three mixed scenarios")
        ratio_limit = fairness.get("ratio_limit")
        if not isinstance(ratio_limit, (int, float)) or isinstance(ratio_limit, bool) or ratio_limit <= 1:
            raise SuiteSpecError("S50 fairness ratio_limit must be greater than one")
    performance = value.get("performance")
    if performance is not None:
        if repetitions:
            raise SuiteSpecError("S80 performance suite has its own exact repetitions")
        arm_scenarios = list(performance["arms"].values())
        if len(set(arm_scenarios)) != len(arm_scenarios):
            raise SuiteSpecError("S80 arms must name four distinct scenarios")
        if set(arm_scenarios) != set(value["scenarios"]):
            raise SuiteSpecError(
                "S80 arm scenarios must exactly match the suite scenario list"
            )
    return suite


def load_suite_spec(path: str | Path) -> SuiteSpec:
    return _load_suite_spec(path, ())
