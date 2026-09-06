from __future__ import annotations

import copy
from pathlib import Path

import pytest

from farmharness.integration.s50_fairness import S50FairnessError, score_s50_fairness


INTEGRATION = Path(__file__).resolve().parents[1]


def _scenario(name: str) -> dict:
    import json

    return json.loads(
        (INTEGRATION / "scenarios" / f"{name}.json").read_text(encoding="utf-8")
    )


def _bundle(wall_ms: int) -> dict:
    return {
        "observations": {
            "client_turns": {
                "A": {
                    "C1": {"exact_objects": 100, "jobs": 100, "wall_ms": wall_ms},
                    "C2": {"exact_objects": 100, "jobs": 100, "wall_ms": wall_ms},
                }
            },
            "turns": {"A": {"exact_objects": 200, "jobs": 200, "wall_ms": wall_ms}},
        },
        "rows": [
            {"client_instance": client, "exact": True}
            for client in ("C1", "C2")
            for _ in range(100)
        ],
    }


def _cells() -> tuple[dict, list[dict]]:
    control = {
        "bundle_data": _bundle(1000),
        "run_id": "control-run",
        "scenario": "S50-all-legacy-control",
        "scenario_data": _scenario("S50-all-legacy-control"),
        "status": "PASS",
    }
    mixed = []
    for index, name in enumerate(
        ("S50-mixed-pool-1of9", "S50-mixed-pool-5of9", "S50-mixed-pool-8of9"),
        start=1,
    ):
        mixed.append(
            {
                "bundle_data": _bundle(1000 + index * 10),
                "run_id": f"mixed-run-{index}",
                "scenario": name,
                "scenario_data": _scenario(name),
                "status": "PASS",
            }
        )
    return control, mixed


def test_s50_fairness_scores_exact_matched_cells() -> None:
    control, mixed = _cells()
    result = score_s50_fairness(control, mixed)
    assert result["status"] == "PASS"
    assert [item["scenario"] for item in result["mixed"]] == [
        "S50-mixed-pool-1of9",
        "S50-mixed-pool-5of9",
        "S50-mixed-pool-8of9",
    ]


def test_s50_fairness_refuses_ratio_or_count_tampering() -> None:
    control, mixed = _cells()
    bad_ratio = copy.deepcopy(mixed)
    bad_ratio[0]["bundle_data"]["observations"]["client_turns"]["A"]["C1"][
        "wall_ms"
    ] = 1051
    with pytest.raises(S50FairnessError, match="ratio"):
        score_s50_fairness(control, bad_ratio)

    bad_count = copy.deepcopy(mixed)
    bad_count[0]["bundle_data"]["rows"].pop()
    with pytest.raises(S50FairnessError, match="per-client count"):
        score_s50_fairness(control, bad_count)

    bad_turn = copy.deepcopy(mixed)
    client_turns = bad_turn[0]["bundle_data"]["observations"]["client_turns"]
    client_turns["B"] = client_turns.pop("A")
    with pytest.raises(S50FairnessError, match="turn sets differ"):
        score_s50_fairness(control, bad_turn)
