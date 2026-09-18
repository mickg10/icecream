import json
from pathlib import Path


INTEGRATION = Path(__file__).parents[1]
SCENARIOS = INTEGRATION / "scenarios"
HISTORICAL_PRODUCT = "p50s4-57a1e336"
FINAL_PRODUCT = "p50s4-fff328e2"
FINAL_COMMIT = "fff328e25a5983197e41905246a880853511e791"
FINAL_SOURCE_ARCHIVE = "2e7a7b9062d19d266363d3428001860291a169d8d0fdcb1325cc8d4c57ab76bf"
FINAL_CLOSURE = "e5dda2b8a834047b7e6c53d8ba5b09684dd02529b76e8cd1e172f94e9fbf2637"
FINAL_MUTANTS = {
    "p50s90-f-revision-2-f9648cc1",
    "p50s90-f-hidden-skew-f9648cc1",
}
FINAL_ROLE_HASHES = {
    "scheduler": "3f376f1c7b5086db58a3cf5e25826f4a1ef538d6a8045dd6ff5fd6986c4e35e6",
    "client": "f661d93315b027faa4cdfa34961450a6393b4c715ddf687641f7263532384bf9",
    "daemon": "ca06985cefc260006f3eb673d467718eb02c42901d2b65b49edc391d126b0a3f",
}
RETIRED_PRODUCTS = {"p50s4-89917385", "p50s4-b42d65e8"}
REQUALIFIED_PREFIXES = ("S50-", "S60-", "S70-", "S80-", "S90-", "S95-")
BUILDER_HOSTS = {"tt-quietbox4", "tt-quietbox5"}


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


def test_final_product_binding_matches_verified_source_and_runtime() -> None:
    farm = json.loads((INTEGRATION / "farm.example.json").read_text(encoding="utf-8"))
    binding = farm["authority"]["images"][FINAL_PRODUCT]
    assert binding["commit"] == FINAL_COMMIT
    assert binding["archive_sha256"] == FINAL_SOURCE_ARCHIVE
    assert binding["closure_sha256"] == FINAL_CLOSURE
    assert binding["id"] == "sha256:2882264273abef6ebb3e70299ef04e88744d49fb4efcec9fffe402c71dceb65e"


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


def test_expanded_builder_placement_is_explicit_and_deletion_sensitive() -> None:
    """Keep fast-builder placement tied to installed image/topology authority."""

    s60 = sorted(SCENARIOS.glob("S60-*.json"))
    assert len(s60) == 14
    for path in s60:
        instances = json.loads(path.read_text(encoding="utf-8"))["instances"]
        workers = {item["name"]: item for item in instances if item["role"] == "F"}
        assert {name: item["host"] for name, item in workers.items()} == {
            "F1": "tt-quietbox5",
            "F2": "tt-quietbox4",
        }, path.name
        assert {item["role"] for item in instances if item["host"] in BUILDER_HOSTS} == {"F"}

    for path in sorted(SCENARIOS.glob("S70-*.json")):
        instances = json.loads(path.read_text(encoding="utf-8"))["instances"]
        workers = {item["name"]: item for item in instances if item["role"] == "F"}
        assert workers["F1"]["host"] == "tt-quietbox5", path.name
        if len(workers) == 2 and path.name in {
            "S70-b4-worker-bounces.json",
            "S70-b5-interner-failure.json",
        }:
            assert workers["F2"]["host"] == "tt-quietbox4", path.name
        elif len(workers) == 2:
            assert workers["F2"]["host"] == "research6", path.name

    s80_expected = {
        "S80-legacy.json": "tt-quietbox5",
        "S80-p29v1.json": "tt-quietbox5",
        "S80-zstd-route.json": "tt-quietbox5",
        "S80-zstd-tu.json": "tt-quietbox5",
        # q3 is the sole netem-authorized host; preserve the shaped arm.
        "S80-p29v1-shaped-100m.json": "tt-quietbox3",
    }
    for name, expected_host in s80_expected.items():
        scenario = json.loads((SCENARIOS / name).read_text(encoding="utf-8"))
        worker = next(item for item in scenario["instances"] if item["name"] == "F1")
        assert worker["host"] == expected_host, name
        if name.endswith("shaped-100m.json"):
            assert scenario["network"]["shaping"] == [
                {"instance": "F1", "rate": "100mbit", "delay_ms": 2}
            ]

    skew = json.loads((SCENARIOS / "S90-revision-skew.json").read_text(encoding="utf-8"))
    skew_workers = {item["name"]: item for item in skew["instances"] if item["role"] == "F"}
    assert {name: item["host"] for name, item in skew_workers.items()} == {
        "F1": "tt-quietbox5",
        # The r2 mutant is not installed on q4/q5; retain its authorized q3 host.
        "F2": "tt-quietbox3",
    }
    refusal = json.loads(
        (SCENARIOS / "S90-revision-refusal-retry.json").read_text(encoding="utf-8")
    )
    refusal_worker = next(item for item in refusal["instances"] if item["role"] == "F")
    assert refusal_worker["host"] == "tt-quietbox2"

    s95 = json.loads((SCENARIOS / "S95-cache-disk-full.json").read_text(encoding="utf-8"))
    s95_workers = {item["name"]: item for item in s95["instances"] if item["role"] == "F"}
    assert {name: item["host"] for name, item in s95_workers.items()} == {
        "F1": "tt-quietbox5",
        "F2": "tt-quietbox4",
    }

    # H5 deliberately remains on its stale 57a1 image until that exact image
    # is installed and independently sealed on the new builders.
    h5 = json.loads((SCENARIOS / "H5-worker-kill.json").read_text(encoding="utf-8"))
    h5_workers = {item["name"]: item for item in h5["instances"] if item["role"] == "F"}
    assert {name: item["host"] for name, item in h5_workers.items()} == {
        "F1": "tt-quietbox3",
        "F2": "research6",
    }
