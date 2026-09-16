import json
from pathlib import Path


INTEGRATION = Path(__file__).parents[1]
SCENARIOS = INTEGRATION / "scenarios"
HISTORICAL_PRODUCT = "p50s4-57a1e336"
FINAL_PRODUCT = "p50s4-74684c92"
FINAL_MUTANTS = {
    "p50s90-f-revision-2-f9648cc1",
    "p50s90-f-hidden-skew-f9648cc1",
}
FINAL_ROLE_HASHES = {
    "scheduler": "0fadf72b4923a318acc64099c1c247bda12804891b7ae6861ebdcbf30bc25cd1",
    "client": "30a3d1b8ceefe0b8919ac4eecef69b4999aba311d19ae6568b7b1d4d64b542e2",
    "daemon": "2c022c43e5988692e94d9ae35b0f959b65f9a11db9f7257302ff9f209ce63276",
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
