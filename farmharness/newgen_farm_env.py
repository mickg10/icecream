#!/usr/bin/env python3
"""Pure, fail-closed resolver for the controlled ICEFARM environment.

Version 2 resolves a canonical per-instance list and deliberately uses a new
digest domain.  The original flat v1 interface remains available only for
replaying historical bundles; v1 and v2 topology digests are never
comparable.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import re
import sys
from pathlib import Path
from typing import Any, Iterable, Mapping, NoReturn

try:
    from .integration.schema_validation import ValidationError, canonical_bytes, load_json
except ImportError:  # Executed directly from farmharness/.
    from integration.schema_validation import ValidationError, canonical_bytes, load_json


SCHEMA_V1 = "icecream-newgen-farm-topology-v1"
SCHEMA_V2 = "icecream-newgen-farm-topology-v2"
SCHEMA = SCHEMA_V2
AUTHORITY_SCHEMA = "icecream-newgen-farm-authority-v1"
FARM_SCHEMA = "icefarm-farm-v1"
ROLE_BINARY = {"S": "scheduler", "C": "client", "F": "daemon"}
ROLE_ORDER = {"S": 0, "C": 1, "F": 2}
PROFILES = ("P29V1", "ZSTD_TU", "ZSTD_ROUTE")
V1_PROFILES = ("P29", "P29V1", "ZSTD_TU", "ZSTD_ROUTE", "GRZ_RESIDUAL", "RAW_II")
REGIMES = ("cold", "warm", "touched")
ROLE_RE = re.compile(r"^(43|44|50)@([A-Za-z0-9._-]+)$")
NAME_RE = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]{0,99}")
ENV_RE = re.compile(r"[A-Z_][A-Z0-9_]*")
IMAGE_RE = re.compile(r"^p(43|44|50)(?:s[0-9]+)?(?:-|$)", re.IGNORECASE)
SHA256_RE = re.compile(r"[0-9a-f]{64}")
COMMIT_RE = re.compile(r"[0-9a-f]{40}")
IMAGE_ID_RE = re.compile(r"sha256:[0-9a-f]{64}")
RUNNER_ENV = frozenset(("ICECC_NETNAME", "ICECC_SCHEDULER", "ICECC_TEST_SOCKET", "ICECC_VERSION"))


class ResolutionError(ValueError):
    """The requested topology is absent, unauthenticated, or unsafe."""


def _refuse(message: str) -> NoReturn:
    raise ResolutionError(message)


def _need(env: Mapping[str, str], name: str) -> str:
    value = env.get(name)
    if value is None or value == "":
        _refuse(f"{name} is required")
    return value


def _strict_json(text: str, name: str) -> Any:
    def pairs(values: list[tuple[str, Any]]) -> dict[str, Any]:
        result: dict[str, Any] = {}
        for key, value in values:
            if key in result:
                _refuse(f"{name} contains duplicate key {key!r}")
            result[key] = value
        return result

    def constant(value: str) -> NoReturn:
        _refuse(f"{name} contains non-finite number {value!r}")

    def number(value: str) -> float:
        parsed = float(value)
        if not math.isfinite(parsed):
            constant(value)
        return parsed

    try:
        return json.loads(
            text,
            object_pairs_hook=pairs,
            parse_constant=constant,
            parse_float=number,
        )
    except (json.JSONDecodeError, UnicodeError) as exc:
        raise ResolutionError(f"{name} is not strict JSON: {exc}") from exc


def _safe_name(value: Any, field: str) -> str:
    if not isinstance(value, str) or NAME_RE.fullmatch(value) is None or value in (".", ".."):
        _refuse(f"{field} is not a safe non-dot name")
    return value


def _parse_link_rates(text: str) -> list[int]:
    try:
        rates = [int(value) for value in text.split(",") if value]
    except ValueError as exc:
        raise ResolutionError("ICEFARM_LINK_RATES must be positive integers") from exc
    if not rates or any(rate <= 0 for rate in rates):
        _refuse("ICEFARM_LINK_RATES must be positive integers")
    return rates


def _authority_parts(path: str | Path) -> tuple[dict[str, Any], dict[str, Any] | None, dict[str, Any] | None]:
    try:
        value = load_json(Path(path))
    except ValidationError as exc:
        raise ResolutionError(str(exc)) from exc
    if not isinstance(value, dict):
        _refuse("authority document is not an object")
    if value.get("schema") == FARM_SCHEMA:
        authority = value.get("authority")
        hosts = value.get("hosts")
        corpora = value.get("corpora")
        if not isinstance(authority, dict) or not isinstance(hosts, list) or not isinstance(corpora, dict):
            _refuse("farm document lacks authority, hosts, or corpora")
        policies = {
            host["name"]: host
            for host in hosts
            if isinstance(host, dict) and isinstance(host.get("name"), str)
        }
        return authority, policies, corpora
    return value, None, None


def load_authority(path: str | Path) -> dict[str, Any]:
    """Load only the authority object, accepting an authority or farm file."""

    authority, _policies, _corpora = _authority_parts(path)
    _validate_authority_shape(authority)
    return authority


def _validate_authority_shape(authority: Mapping[str, Any]) -> None:
    if authority.get("schema") != AUTHORITY_SCHEMA:
        _refuse("authority schema is not " + AUTHORITY_SCHEMA)
    for key in ("hosts", "role_stores", "images", "topologies"):
        if not isinstance(authority.get(key), dict):
            _refuse(f"authority lacks {key}")


def _binary_hash(authority: Mapping[str, Any], version: int, role: str) -> str:
    store = authority["role_stores"].get(str(version))
    entry = store.get(ROLE_BINARY[role]) if isinstance(store, dict) else None
    digest = entry.get("sha256") if isinstance(entry, dict) else None
    if not isinstance(digest, str) or SHA256_RE.fullmatch(digest) is None:
        _refuse(
            f"role store {version}/{ROLE_BINARY[role]} has no sha256 "
            "(rule 1: no fallback)"
        )
    return digest


def _authority_host(authority: Mapping[str, Any], host: str) -> Mapping[str, Any]:
    entry = authority["hosts"].get(host)
    if not isinstance(entry, dict) or not isinstance(entry.get("address"), str):
        _refuse(f"host {host!r} is not in the authority (rule 1)")
    return entry


def _version_from_image(label: str) -> int:
    short = label.rsplit(":", 1)[-1]
    match = IMAGE_RE.match(short)
    if match is None:
        _refuse(f"image label {label!r} does not encode p43, p44, or p50")
    return int(match.group(1))


def _pinned_image(authority: Mapping[str, Any], label: str) -> dict[str, Any]:
    entry = authority["images"].get(label)
    if not isinstance(entry, dict):
        _refuse(f"image {label!r} is absent from the authority (rule 1)")
    commit = entry.get("commit")
    archive = entry.get("archive_sha256")
    image_id = entry.get("id")
    if not isinstance(commit, str) or COMMIT_RE.fullmatch(commit) is None:
        _refuse(f"image {label!r} has no immutable commit (rule 1)")
    if not isinstance(archive, str) or SHA256_RE.fullmatch(archive) is None:
        _refuse(f"image {label!r} has no archive_sha256 (rule 1)")
    if image_id is not None and (
        not isinstance(image_id, str) or IMAGE_ID_RE.fullmatch(image_id) is None
    ):
        _refuse(f"image {label!r} has an invalid config id")
    return {
        "archive_sha256": archive,
        "commit": commit,
        "id": image_id,
        "label": label,
    }


def _corpus_binding(corpus: Mapping[str, Any]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key in ("kind", "tus", "repeat"):
        if key in corpus:
            result[key] = corpus[key]
    for key, value in sorted(corpus.items()):
        if key.endswith("_sha256"):
            if not isinstance(value, str) or SHA256_RE.fullmatch(value) is None:
                _refuse(f"corpus authority field {key!r} is not a sha256")
            result[key] = value
    if not any(key.endswith("_sha256") for key in result):
        _refuse("corpus authority has no authenticated manifest hash")
    return result


def _parse_v2_instances(text: str) -> list[dict[str, Any]]:
    value = _strict_json(text, "ICEFARM_INSTANCES")
    if not isinstance(value, list) or not value:
        _refuse("ICEFARM_INSTANCES must be a nonempty JSON array")
    instances: list[dict[str, Any]] = []
    names: set[str] = set()
    counts = {"S": 0, "C": 0, "F": 0}
    allowed_fields = {"name", "role", "host", "image", "env", "slots"}
    for index, raw in enumerate(value):
        field = f"ICEFARM_INSTANCES[{index}]"
        if not isinstance(raw, dict):
            _refuse(f"{field} must be an object")
        extra = sorted(set(raw) - allowed_fields)
        if extra:
            _refuse(f"{field} has unsupported fields {extra!r}")
        missing = sorted({"name", "role", "host", "image"} - set(raw))
        if missing:
            _refuse(f"{field} lacks fields {missing!r}")
        name = _safe_name(raw["name"], f"{field}.name")
        if name in names:
            _refuse(f"{field}.name duplicates {name!r}")
        names.add(name)
        role = raw["role"]
        if role not in ROLE_BINARY:
            _refuse(f"{field}.role must be S, C, or F")
        counts[role] += 1
        host = _safe_name(raw["host"], f"{field}.host")
        image = raw["image"]
        if (
            not isinstance(image, str)
            or not image
            or "\0" in image
            or any(0xD800 <= ord(character) <= 0xDFFF for character in image)
        ):
            _refuse(f"{field}.image must be a nonempty non-NUL string")
        environment = raw.get("env", {})
        if not isinstance(environment, dict):
            _refuse(f"{field}.env must be an object")
        for key, item in environment.items():
            if not isinstance(key, str) or ENV_RE.fullmatch(key) is None:
                _refuse(f"{field}.env has unsafe key {key!r}")
            if (
                not isinstance(item, str)
                or "\0" in item
                or any(0xD800 <= ord(character) <= 0xDFFF for character in item)
            ):
                _refuse(f"{field}.env.{key} must be a safe string")
            if key in RUNNER_ENV:
                _refuse(f"{field}.env.{key} is owned by the runner")
        selected_profile = environment.get("ICECC_P50_PROFILE")
        if selected_profile is not None and role != "S":
            _refuse(f"{field}.env.ICECC_P50_PROFILE is valid only on S")
        client_mode = environment.get("ICECC_P50_MODE")
        if client_mode is not None:
            if role != "C" or client_mode not in ("on", "off"):
                _refuse(f"{field}.env.ICECC_P50_MODE must be on/off on C")
        slots = raw.get("slots")
        if role == "F":
            if not isinstance(slots, int) or isinstance(slots, bool) or slots <= 0:
                _refuse(f"{field}.slots must be a positive integer for F")
        elif "slots" in raw:
            _refuse(f"{field}.slots is valid only for F")
        instances.append(
            {
                "env": dict(sorted(environment.items())),
                "host": host,
                "image": image,
                "name": name,
                "role": role,
                "slots": slots,
            }
        )
    if counts["S"] != 1 or counts["C"] < 1 or counts["F"] < 1:
        _refuse("ICEFARM_INSTANCES requires exactly one S and at least one C and F")
    return instances


def _normalized_environment(
    env: Mapping[str, str], authority: Mapping[str, Any], instances: Iterable[Mapping[str, Any]]
) -> dict[str, str]:
    mapping = {
        key: value
        for key, value in sorted(env.items())
        if key.startswith("ICEFARM_") and key != "ICEFARM_AUTHORITY"
    }
    mapping["ICEFARM_AUTHORITY_SHA256"] = hashlib.sha256(
        canonical_bytes(authority)
    ).hexdigest()
    mapping["ICEFARM_INSTANCES"] = json.dumps(
        list(instances), sort_keys=True, separators=(",", ":"), ensure_ascii=False
    )
    return mapping


def _resolve_v2(
    env: Mapping[str, str],
    authority: Mapping[str, Any],
    host_policies: Mapping[str, Mapping[str, Any]] | None,
    corpus_authorities: Mapping[str, Mapping[str, Any]] | None,
) -> dict[str, Any]:
    if host_policies is None:
        _refuse("v2 resolution requires farm host policies for roles_allowed")
    requested = _parse_v2_instances(_need(env, "ICEFARM_INSTANCES"))
    topology_id = _need(env, "ICEFARM_TOPOLOGY")
    topology = authority["topologies"].get(topology_id)
    if not isinstance(topology, dict):
        _refuse(f"topology {topology_id!r} is not in the authority")
    worker_count = sum(item["role"] == "F" for item in requested)
    if topology.get("f_relationships") != worker_count:
        _refuse(
            f"ICEFARM_TOPOLOGY {topology_id} needs {topology.get('f_relationships')} "
            f"F roles, got {worker_count}"
        )

    regime = env.get("ICEFARM_REGIME", "cold")
    if regime not in REGIMES:
        _refuse(f"ICEFARM_REGIME {regime!r} is not one of {REGIMES}")
    rates = _parse_link_rates(env.get("ICEFARM_LINK_RATES", "1000000000,100000000"))
    dry_run = env.get("ICEFARM_DRY_RUN", "0")
    if dry_run not in ("0", "1"):
        _refuse("ICEFARM_DRY_RUN must be 0 or 1")
    profile = env.get("ICEFARM_PROFILE")
    versions = [_version_from_image(item["image"]) for item in requested]
    if any(version == 50 for version in versions) and profile not in PROFILES:
        _refuse(
            "ICEFARM_PROFILE is required for version-50 roles and must be one of "
            + ",".join(PROFILES)
        )
    instance_profiles = {
        item["env"]["ICECC_P50_PROFILE"]
        for item in requested
        if "ICECC_P50_PROFILE" in item["env"]
    }
    if len(instance_profiles) > 1 or (instance_profiles and instance_profiles != {profile}):
        _refuse("S ICECC_P50_PROFILE conflicts with ICEFARM_PROFILE")

    resolved: list[dict[str, Any]] = []
    for item, version in zip(requested, versions):
        policy = host_policies.get(item["host"])
        if not isinstance(policy, Mapping):
            _refuse(f"host {item['host']!r} has no farm policy (rule 1)")
        roles_allowed = policy.get("roles_allowed")
        if not isinstance(roles_allowed, list) or item["role"] not in roles_allowed:
            _refuse(
                f"host {item['host']!r} does not allow role {item['role']} (rule 3)"
            )
        host = _authority_host(authority, item["host"])
        if "lan_ip" in policy and policy["lan_ip"] != host["address"]:
            _refuse(f"host {item['host']!r} farm address differs from authority (rule 1)")
        image = _pinned_image(authority, item["image"])
        resolved.append(
            {
                "address": host["address"],
                "binary": ROLE_BINARY[item["role"]],
                "env": item["env"],
                "host": item["host"],
                "image": image,
                "name": item["name"],
                "profile": profile if version == 50 else None,
                "role": item["role"],
                "sha256": _binary_hash(authority, version, item["role"]),
                "slots": item["slots"],
                "version": version,
            }
        )
    resolved.sort(key=lambda item: (ROLE_ORDER[item["role"]], item["name"]))

    schedulers = [item for item in resolved if item["role"] == "S"]
    clients = [item for item in resolved if item["role"] == "C"]
    workers = [item for item in resolved if item["role"] == "F"]
    relationships: list[dict[str, Any]] = []
    relationship_id = 0
    for worker in workers:
        for client in clients:
            state = (
                f"s{schedulers[0]['version']}-c{client['version']}-f{worker['version']}"
            )
            relationships.append(
                {
                    "c": client["name"],
                    "cache_expected": state == "s50-c50-f50",
                    "f": worker["name"],
                    "profile": profile if state == "s50-c50-f50" else None,
                    "relationship": relationship_id,
                    "state": state,
                }
            )
            relationship_id += 1

    corpus_name = env.get("ICEFARM_CORPUS")
    corpus_binding = None
    if corpus_name is not None:
        corpus = corpus_authorities.get(corpus_name) if corpus_authorities else None
        if not isinstance(corpus, Mapping):
            _refuse(f"corpus {corpus_name!r} is absent from the farm authority")
        corpus_binding = _corpus_binding(corpus)

    normalized_requested = [
        {
            "env": item["env"],
            "host": item["host"],
            "image": item["image"],
            "name": item["name"],
            "role": item["role"],
            **({"slots": item["slots"]} if item["role"] == "F" else {}),
        }
        for item in sorted(requested, key=lambda item: (ROLE_ORDER[item["role"]], item["name"]))
    ]
    environment = _normalized_environment(env, authority, normalized_requested)
    placement_digest = hashlib.sha256(canonical_bytes(resolved)).hexdigest()
    body: dict[str, Any] = {
        "corpus": corpus_name,
        "corpus_authority": corpus_binding,
        "environment_digest": hashlib.sha256(canonical_bytes(environment)).hexdigest(),
        "instances": resolved,
        "link_rates": rates,
        "placement_digest": placement_digest,
        "profile": profile,
        "regime": regime,
        "relationships": relationships,
        "schema": SCHEMA_V2,
        "topology": topology_id,
        "topology_slots": topology,
    }
    body["topology_digest"] = hashlib.sha256(canonical_bytes(body)).hexdigest()
    return body


def _parse_v1_roles(text: str, name: str) -> list[dict[str, Any]]:
    roles: list[dict[str, Any]] = []
    for item in [value.strip() for value in text.split(",") if value.strip()]:
        match = ROLE_RE.match(item)
        if match is None:
            _refuse(
                f"{name} entry {item!r} is not version@host with version in 43/44/50"
            )
        roles.append({"version": int(match.group(1)), "host": match.group(2)})
    if not roles:
        _refuse(f"{name} is empty")
    return roles


def _resolve_v1(env: Mapping[str, str], authority: Mapping[str, Any]) -> dict[str, Any]:
    """Original resolver semantics retained for historical digest replay."""

    s_roles = _parse_v1_roles(_need(env, "ICEFARM_S"), "ICEFARM_S")
    if len(s_roles) != 1:
        _refuse("ICEFARM_S names exactly one scheduler")
    c_roles = _parse_v1_roles(_need(env, "ICEFARM_C"), "ICEFARM_C")
    f_roles = _parse_v1_roles(_need(env, "ICEFARM_F"), "ICEFARM_F")
    topology_id = _need(env, "ICEFARM_TOPOLOGY")
    topology = authority["topologies"].get(topology_id)
    if not isinstance(topology, dict):
        _refuse(f"topology {topology_id!r} is not in the authority")
    if topology.get("f_relationships") != len(f_roles):
        _refuse(
            f"ICEFARM_TOPOLOGY {topology_id} needs {topology.get('f_relationships')} "
            f"F roles, got {len(f_roles)}"
        )
    image_ref = _need(env, "ICEFARM_IMAGE")
    image = authority["images"].get(image_ref)
    if not isinstance(image, dict) or not str(image.get("id", "")).startswith("sha256:"):
        _refuse("ICEFARM_IMAGE is not a pinned image id in the authority")
    regime = env.get("ICEFARM_REGIME", "cold")
    if regime not in REGIMES:
        _refuse(f"ICEFARM_REGIME {regime!r} is not one of {REGIMES}")
    rates = _parse_link_rates(env.get("ICEFARM_LINK_RATES", "1000000000,100000000"))
    profile = env.get("ICEFARM_PROFILE")
    all_roles = s_roles + c_roles + f_roles
    if any(item["version"] == 50 for item in all_roles) and profile not in V1_PROFILES:
        _refuse(
            "ICEFARM_PROFILE is required for version-50 roles and must be one of "
            + ",".join(V1_PROFILES)
        )

    def host_class(name: str) -> str:
        entry = _authority_host(authority, name)
        if entry.get("class") not in ("submission", "orchestration", "worker"):
            _refuse(f"host {name!r} is not in the authority (rule 1)")
        return entry["class"]

    if host_class(s_roles[0]["host"]) != "submission":
        _refuse("S must run on the submission host (rule 3)")
    for client in c_roles:
        if host_class(client["host"]) != "submission":
            _refuse(
                f"C {client['version']}@{client['host']} must run on the submission host (rule 3)"
            )
    for worker in f_roles:
        if host_class(worker["host"]) != "worker":
            _refuse(
                f"F {worker['version']}@{worker['host']} must run on a worker host, "
                "never submission or orchestration (rule 3)"
            )

    def role(kind: str, item: Mapping[str, Any]) -> dict[str, Any]:
        host = _authority_host(authority, item["host"])
        return {
            "role": kind,
            "version": item["version"],
            "host": item["host"],
            "address": host["address"],
            "binary": ROLE_BINARY[kind],
            "sha256": _binary_hash(authority, item["version"], kind),
            "profile": profile if item["version"] == 50 else None,
        }

    roles = [role("S", s_roles[0])] + [role("C", item) for item in c_roles] + [
        role("F", item) for item in f_roles
    ]
    relationships: list[dict[str, Any]] = []
    for index, worker in enumerate(f_roles):
        for client in c_roles:
            state = f"s{s_roles[0]['version']}-c{client['version']}-f{worker['version']}"
            relationships.append(
                {
                    "relationship": index,
                    "c": f"{client['version']}@{client['host']}",
                    "f": f"{worker['version']}@{worker['host']}",
                    "state": state,
                    "cache_expected": state == "s50-c50-f50",
                    "profile": profile if state == "s50-c50-f50" else None,
                }
            )
    mapping = {key: value for key, value in sorted(env.items()) if key.startswith("ICEFARM_")}
    placement_digest = hashlib.sha256(json.dumps(roles, sort_keys=True).encode()).hexdigest()
    body: dict[str, Any] = {
        "schema": SCHEMA_V1,
        "topology": topology_id,
        "topology_slots": topology,
        "image": {"ref": image_ref, "id": image["id"]},
        "regime": regime,
        "link_rates": rates,
        "corpus": env.get("ICEFARM_CORPUS"),
        "manifest": env.get("ICEFARM_MANIFEST"),
        "roles": roles,
        "relationships": relationships,
        "placement_digest": placement_digest,
        "environment_digest": hashlib.sha256(
            json.dumps(mapping, sort_keys=True).encode()
        ).hexdigest(),
    }
    body["topology_digest"] = hashlib.sha256(
        json.dumps(body, sort_keys=True).encode()
    ).hexdigest()
    return body


def resolve(
    env: Mapping[str, str],
    authority: Mapping[str, Any] | None = None,
    *,
    host_policies: Mapping[str, Mapping[str, Any]] | None = None,
    corpus_authorities: Mapping[str, Mapping[str, Any]] | None = None,
) -> dict[str, Any]:
    """Resolve v2 when ``ICEFARM_INSTANCES`` exists, otherwise replay v1."""

    if authority is None:
        authority, loaded_policies, loaded_corpora = _authority_parts(
            _need(env, "ICEFARM_AUTHORITY")
        )
        if host_policies is None:
            host_policies = loaded_policies
        if corpus_authorities is None:
            corpus_authorities = loaded_corpora
    _validate_authority_shape(authority)
    if "ICEFARM_INSTANCES" in env:
        if any(name in env for name in ("ICEFARM_S", "ICEFARM_C", "ICEFARM_F", "ICEFARM_IMAGE")):
            _refuse("v2 ICEFARM_INSTANCES cannot be mixed with v1 flat role/image inputs")
        return _resolve_v2(env, authority, host_policies, corpus_authorities)
    return _resolve_v1(env, authority)


def explain(
    env: Mapping[str, str],
    authority: Mapping[str, Any] | None = None,
    *,
    host_policies: Mapping[str, Mapping[str, Any]] | None = None,
    corpus_authorities: Mapping[str, Mapping[str, Any]] | None = None,
) -> str:
    try:
        body = resolve(
            env,
            authority,
            host_policies=host_policies,
            corpus_authorities=corpus_authorities,
        )
    except ResolutionError as error:
        return f"REFUSED: {error}"
    instances = body.get("instances", body.get("roles", []))
    lines = [
        f"{item['role']} {item.get('name', '')} {item['version']}@{item['host']} "
        f"({item['address']}) {item['binary']} {item['sha256'][:12]} "
        f"profile={item['profile']}"
        for item in instances
    ]
    lines.extend(
        f"relationship {item['relationship']}: {item['c']} -> {item['f']} "
        f"state={item['state']} cache_expected={item['cache_expected']}"
        for item in body["relationships"]
    )
    lines.append(f"topology_digest {body['topology_digest']}")
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("resolve", "dry-run", "explain"))
    args = parser.parse_args(argv)
    env = dict(os.environ)
    try:
        if args.command == "explain":
            print(explain(env))
            return 0
        body = resolve(env)
    except ResolutionError as error:
        print("ICEFARM resolution refused: " + str(error), file=sys.stderr)
        return 2
    print(json.dumps(body, indent=1, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
