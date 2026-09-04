from __future__ import annotations

import copy
import json
from pathlib import Path

import pytest

from farmharness import newgen_farm_env
from farmharness.integration.farm_spec import load_farm_spec


INTEGRATION = Path(__file__).resolve().parents[1]


def _inputs():
    farm = load_farm_spec(INTEGRATION / "farm.example.json")
    instances = [
        {
            "name": "S1",
            "role": "S",
            "host": "tt-quietbox3",
            "image": "p50s2-624702e9",
            "env": {"ICECC_P50_PROFILE": "P29V1"},
        },
        {
            "name": "C1",
            "role": "C",
            "host": "research6",
            "image": "p50s2-624702e9",
            "env": {"ICECC_P50_MODE": "on"},
        },
        {
            "name": "F1",
            "role": "F",
            "host": "tt-quietbox3",
            "image": "p50s2-624702e9",
            "slots": 24,
        },
        {
            "name": "F2",
            "role": "F",
            "host": "research6",
            "image": "p50s2-624702e9",
            "slots": 12,
        },
    ]
    environment = {
        "ICEFARM_INSTANCES": json.dumps(instances, separators=(",", ":")),
        "ICEFARM_TOPOLOGY": "C1F2",
        "ICEFARM_PROFILE": "P29V1",
        "ICEFARM_REGIME": "cold",
        "ICEFARM_LINK_RATES": "1000000000,100000000",
        "ICEFARM_CORPUS": "firefox-1000",
        "ICEFARM_DRY_RUN": "1",
    }
    return farm, environment


def _resolve(farm, environment):
    return newgen_farm_env.resolve(
        environment,
        farm.data["authority"],
        host_policies=farm.hosts,
        corpus_authorities=farm.data["corpora"],
    )


def test_v2_resolves_normative_s40_placement_and_is_stable() -> None:
    farm, environment = _inputs()
    first = _resolve(farm, environment)
    second = _resolve(farm, environment)
    assert first == second
    assert first["schema"] == "icecream-newgen-farm-topology-v2"
    assert len(first["topology_digest"]) == 64
    assert [item["name"] for item in first["instances"]] == ["S1", "C1", "F1", "F2"]
    assert all(item["image"]["commit"] == "624702e98f60f453f9be5d406b6da45730eae1ee" for item in first["instances"])
    assert {item["state"] for item in first["relationships"]} == {"s50-c50-f50"}
    assert all(item["cache_expected"] for item in first["relationships"])


def test_v2_digest_binds_image_source_and_corpus_authority() -> None:
    farm, environment = _inputs()
    baseline = _resolve(farm, environment)["topology_digest"]

    changed_image = copy.deepcopy(farm.data["authority"])
    changed_image["images"]["p50s2-624702e9"]["archive_sha256"] = "f" * 64
    image_result = newgen_farm_env.resolve(
        environment,
        changed_image,
        host_policies=farm.hosts,
        corpus_authorities=farm.data["corpora"],
    )
    assert image_result["topology_digest"] != baseline

    changed_corpora = copy.deepcopy(farm.data["corpora"])
    changed_corpora["firefox-1000"]["pair_index_sha256"] = "e" * 64
    corpus_result = newgen_farm_env.resolve(
        environment,
        farm.data["authority"],
        host_policies=farm.hosts,
        corpus_authorities=changed_corpora,
    )
    assert corpus_result["topology_digest"] != baseline


def test_v2_canonicalizes_instance_json_order_and_formatting() -> None:
    farm, environment = _inputs()
    baseline = _resolve(farm, environment)["topology_digest"]
    instances = json.loads(environment["ICEFARM_INSTANCES"])
    environment["ICEFARM_INSTANCES"] = json.dumps(list(reversed(instances)), indent=2)
    assert _resolve(farm, environment)["topology_digest"] == baseline


def test_unused_host_role_policy_does_not_change_digest() -> None:
    farm, environment = _inputs()
    baseline = _resolve(farm, environment)["topology_digest"]
    farm.hosts["tt-quietbox2"]["roles_allowed"].append("S")
    assert _resolve(farm, environment)["topology_digest"] == baseline


@pytest.mark.parametrize(
    ("mutation", "error"),
    [
        (lambda farm, env: farm.data["authority"]["images"]["p50s2-624702e9"].pop("commit"), "immutable commit"),
        (lambda farm, env: farm.data["authority"]["role_stores"]["50"]["daemon"].pop("sha256"), "no sha256"),
        (lambda farm, env: farm.hosts["research6"].update(roles_allowed=["C"]), "does not allow role F"),
        (lambda farm, env: env.update(ICEFARM_PROFILE="GRZ_RESIDUAL"), "ICEFARM_PROFILE"),
        (lambda farm, env: env.update(ICEFARM_TOPOLOGY="C1F1"), "needs 1 F roles, got 2"),
        (lambda farm, env: env.update(ICEFARM_DRY_RUN="maybe"), "ICEFARM_DRY_RUN"),
        (
            lambda farm, env: env.update(
                ICEFARM_INSTANCES=env["ICEFARM_INSTANCES"].replace(
                    '"ICECC_P50_PROFILE":"P29V1"',
                    '"ICECC_P50_PROFILE":"ZSTD_TU"',
                )
            ),
            "conflicts with ICEFARM_PROFILE",
        ),
    ],
)
def test_v2_rules_refuse(mutation, error: str) -> None:
    farm, environment = _inputs()
    mutation(farm, environment)
    with pytest.raises(newgen_farm_env.ResolutionError, match=error):
        _resolve(farm, environment)


def test_v2_refuses_mixed_v1_and_v2_inputs() -> None:
    farm, environment = _inputs()
    environment["ICEFARM_IMAGE"] = "p50s2-624702e9"
    with pytest.raises(newgen_farm_env.ResolutionError, match="cannot be mixed"):
        _resolve(farm, environment)


def test_v2_requires_host_policy_boundary() -> None:
    farm, environment = _inputs()
    with pytest.raises(newgen_farm_env.ResolutionError, match="host policies"):
        newgen_farm_env.resolve(
            environment,
            farm.data["authority"],
            corpus_authorities=farm.data["corpora"],
        )


def test_v2_refuses_surrogate_environment_before_digesting() -> None:
    farm, environment = _inputs()
    instances = json.loads(environment["ICEFARM_INSTANCES"])
    instances[1]["env"]["VALUE"] = "bad\ud800value"
    environment["ICEFARM_INSTANCES"] = json.dumps(instances)
    with pytest.raises(newgen_farm_env.ResolutionError, match="safe string"):
        _resolve(farm, environment)


def test_v1_historical_digest_replays_exactly() -> None:
    authority_path = INTEGRATION / "tests" / "fixtures" / "newgen-v1-authority.json"
    authority = json.loads(authority_path.read_text())
    environment = {
        "ICEFARM_AUTHORITY": "authority.json",
        "ICEFARM_S": "50@q3",
        "ICEFARM_C": "50@q3",
        "ICEFARM_F": "50@q2",
        "ICEFARM_TOPOLOGY": "C1F1/100000",
        "ICEFARM_PROFILE": "P29V1",
        "ICEFARM_IMAGE": "icecream/farm-node:ubuntu22-gcc11-boost174@sha256:abc",
        "ICEFARM_REGIME": "touched",
    }
    result = newgen_farm_env.resolve(environment, authority)
    assert result["schema"] == "icecream-newgen-farm-topology-v1"
    assert result["topology_digest"] == "a5a5d7282f777d92b8981a953886c735132a5dd7246ef496cfd203c161fd43e8"
