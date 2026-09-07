import json
from pathlib import Path


INTEGRATION = Path(__file__).parents[1]
SCENARIOS = INTEGRATION / "scenarios"
FINAL_PRODUCT = "p50s4-57a1e336"
RETIRED_PRODUCTS = {"p50s4-89917385", "p50s4-b42d65e8"}
PENDING_FRESH_MUTANTS = {
    "H3-mutant-scheduler.json",
    "S90-revision-skew.json",
}


def test_ordinary_catalog_uses_the_final_product_lineage() -> None:
    for path in sorted(SCENARIOS.glob("*.json")):
        if path.name in PENDING_FRESH_MUTANTS:
            continue
        scenario = json.loads(path.read_text(encoding="utf-8"))
        labels = set(scenario["images"].values())
        assert labels.isdisjoint(RETIRED_PRODUCTS), path.name

        ordinary = {
            label
            for label in labels
            if label.startswith("p50s4-")
            and "mutant" not in label
        }
        assert ordinary <= {FINAL_PRODUCT}, (path.name, ordinary)
