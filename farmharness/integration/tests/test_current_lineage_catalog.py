import json
from pathlib import Path


INTEGRATION = Path(__file__).parents[1]
SCENARIOS = INTEGRATION / "scenarios"
HISTORICAL_PRODUCT = "p50s4-57a1e336"
FINAL_PRODUCT = "p50s4-diag-5675fc1d"
FINAL_COMMIT = "5675fc1d58a7254638e77ce0cd55f0cb6a0f2f3f"
FINAL_SOURCE_ARCHIVE = "d8928ad08c73772a9f4fb19071932d32021528dd88c973c80ddb26b068abf14c"
FINAL_CLOSURE = "4aa20b6a99523b14aa04d2ad2f12123ee2c52074eb3a50c569def91793bae3dd"
FINAL_MUTANTS = {
    "p50s90-f-revision-2-f9648cc1",
    "p50s90-f-hidden-skew-f9648cc1",
}
FINAL_ROLE_HASHES = {
    "scheduler": "e6714d8daff6cde398c68c9df9611d3672daeb2df55d9f51e57af1dfb2e3bc38",
    "client": "7ff9ef9bd79709cd3f10c0f024afe471f52874628e63f49cd8282f2c2e3c29d3",
    "daemon": "bf6ff4477da9f97e58b05c503099bb054dbfd70a4aa8769d47d2115d4cdfa966",
}
RETIRED_PRODUCTS = {"p50s4-89917385", "p50s4-b42d65e8"}
REQUALIFIED_PREFIXES = ("S50-", "S60-", "S70-", "S80-", "S90-", "S95-")
BUILDER_HOSTS = {"tt-quietbox5"}


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
    assert binding["id"] == "sha256:1df19cb5d11950075c70b379665908c779236e0c7d0671aec3277d7b0df16bac"


def test_direct_builder_catalog_is_f_only_and_deletion_sensitive() -> None:
    farm = json.loads((INTEGRATION / "farm.example.json").read_text(encoding="utf-8"))
    hosts = {host["name"]: host for host in farm["hosts"]}
    builders = {name: hosts[name] for name in ("tt-quietbox5",)}

    assert all(host["roles_allowed"] == ["F"] for host in builders.values())
    assert {host["docker_context"] for host in builders.values()} == {"q5"}
    assert {host["lan_ip"] for host in builders.values()} == {
        "10.0.27.150",
    }
    assert "research7" not in hosts
    assert "tt-quietbox4" not in hosts
    assert "tt-quietbox4" not in farm["authority"]["hosts"]
    assert {
        name: farm["authority"]["hosts"][name]
        for name in builders
    } == {
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
            "F2": "research6",
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
            assert workers["F2"]["host"] == "research6", path.name
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
        "F1": "research6",
        "F2": "tt-quietbox5",
    }

    # H5 qualifies current-product recovery on its authorized original hosts.
    h5 = json.loads((SCENARIOS / "H5-worker-kill.json").read_text(encoding="utf-8"))
    assert h5["images"] == {"new": FINAL_PRODUCT}
    h5_workers = {item["name"]: item for item in h5["instances"] if item["role"] == "F"}
    assert {name: item["host"] for name, item in h5_workers.items()} == {
        "F1": "tt-quietbox3",
        "F2": "research6",
    }
