import json
from pathlib import Path


INTEGRATION = Path(__file__).parents[1]
SCENARIOS = INTEGRATION / "scenarios"
HISTORICAL_PRODUCT = "p50s4-57a1e336"
FINAL_PRODUCT = "p50s4-59868aa4"
FINAL_MUTANTS = {
    "p50s90-f-revision-2-f9648cc1",
    "p50s90-f-hidden-skew-f9648cc1",
}
FINAL_ROLE_HASHES = {
    "scheduler": "5297fd92b1ac80101b90ed0d8f3eb2b4fabe7323af55ac6a8b7fafd8088629f9",
    "client": "d0371be430e29431eba1a88cdcd00785de2522c11f771e3545a7d96e37625910",
    "daemon": "5644b04ac7f79b710e38e4965b066ebd628862f985398f881f070afd2e3f1235",
}
RETIRED_PRODUCTS = {"p50s4-89917385", "p50s4-b42d65e8"}
REQUALIFIED_PREFIXES = ("S50-", "S60-", "S70-", "S80-", "S90-", "S95-")


def test_requalified_catalog_uses_the_final_product_lineage() -> None:
    for path in sorted(SCENARIOS.glob("*.json")):
        scenario = json.loads(path.read_text(encoding="utf-8"))
        labels = set(scenario["images"].values())
        assert labels.isdisjoint(RETIRED_PRODUCTS), path.name
        if path.name.startswith(REQUALIFIED_PREFIXES):
            ordinary = {
                label for label in labels if label.startswith("p50s4-")
            }
            assert ordinary <= {FINAL_PRODUCT}, (path.name, ordinary)
            assert HISTORICAL_PRODUCT not in labels, path.name


def test_mutant_scenarios_use_the_final_product_lineage() -> None:
    expected = {
        "H3-mutant-scheduler.json": {
            "mutant": "p50s4-h3-tail-57a1e336",
            "new": HISTORICAL_PRODUCT,
            "old": "p43-1.4.0",
        },
        "S90-revision-skew.json": {
            "r1": FINAL_PRODUCT,
            "r2": "p50s90-f-revision-2-f9648cc1",
        },
        "S90-revision-refusal-retry.json": {
            "mutant": "p50s90-f-hidden-skew-f9648cc1",
            "new": FINAL_PRODUCT,
        },
    }
    for name, images in expected.items():
        scenario = json.loads((SCENARIOS / name).read_text(encoding="utf-8"))
        assert scenario["images"] == images


def test_final_product_role_store_uses_measured_current_binaries() -> None:
    farm = json.loads((INTEGRATION / "farm.example.json").read_text(encoding="utf-8"))
    observed = {
        role: binding["sha256"]
        for role, binding in farm["authority"]["role_stores"]["50"].items()
    }
    assert observed == FINAL_ROLE_HASHES


def test_direct_builder_catalog_is_f_only_and_deletion_sensitive() -> None:
    farm = json.loads((INTEGRATION / "farm.example.json").read_text(encoding="utf-8"))
    hosts = {host["name"]: host for host in farm["hosts"]}
    builders = {name: hosts[name] for name in ("tt-quietbox4", "tt-quietbox5")}

    assert all(host["roles_allowed"] == ["F"] for host in builders.values())
    assert {host["docker_context"] for host in builders.values()} == {"q4", "q5"}
    assert {host["lan_ip"] for host in builders.values()} == {
        "10.0.27.125",
        "10.0.27.150",
    }
    assert "research7" not in hosts
    assert {
        name: farm["authority"]["hosts"][name]
        for name in builders
    } == {
        "tt-quietbox4": {"class": "worker", "address": "10.0.27.125"},
        "tt-quietbox5": {"class": "worker", "address": "10.0.27.150"},
    }
