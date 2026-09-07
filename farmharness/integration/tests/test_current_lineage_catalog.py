import json
from pathlib import Path


INTEGRATION = Path(__file__).parents[1]
SCENARIOS = INTEGRATION / "scenarios"
FINAL_PRODUCT = "p50s4-57a1e336"
FINAL_MUTANTS = {"p50s4-h3-tail-57a1e336", "p50s90-f-revision-2-57a1e336"}
RETIRED_PRODUCTS = {"p50s4-89917385", "p50s4-b42d65e8"}


def test_ordinary_catalog_uses_the_final_product_lineage() -> None:
    for path in sorted(SCENARIOS.glob("*.json")):
        scenario = json.loads(path.read_text(encoding="utf-8"))
        labels = set(scenario["images"].values())
        assert labels.isdisjoint(RETIRED_PRODUCTS), path.name

        ordinary = {
            label
            for label in labels
            if label.startswith("p50s4-")
            and label not in FINAL_MUTANTS
        }
        assert ordinary <= {FINAL_PRODUCT}, (path.name, ordinary)


def test_mutant_scenarios_use_the_final_product_lineage() -> None:
    expected = {
        "H3-mutant-scheduler.json": {
            "mutant": "p50s4-h3-tail-57a1e336",
            "new": FINAL_PRODUCT,
            "old": "p43-1.4.0",
        },
        "S90-revision-skew.json": {
            "r1": FINAL_PRODUCT,
            "r2": "p50s90-f-revision-2-57a1e336",
        },
        "S90-revision-refusal-retry.json": {
            "mutant": "p50s90-f-hidden-skew-candidate",
            "new": FINAL_PRODUCT,
        },
    }
    for name, images in expected.items():
        scenario = json.loads((SCENARIOS / name).read_text(encoding="utf-8"))
        assert scenario["images"] == images
