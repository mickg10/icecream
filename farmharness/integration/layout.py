"""Pure host-path layout for immutable inputs and per-cell writable state."""

from __future__ import annotations

import hashlib
from pathlib import PurePosixPath
from typing import Any

try:
    from .farm_spec import FarmSpec
    from .schema_validation import canonical_bytes
except ImportError:  # Direct execution from this directory.
    from farm_spec import FarmSpec
    from schema_validation import canonical_bytes


def instance_root(farm: FarmSpec, host_name: str, run_id: str, instance: str) -> PurePosixPath:
    return (
        PurePosixPath(farm.hosts[host_name]["scratch_root"])
        / "icefarm"
        / run_id
        / instance
    )


def runtime_root(farm: FarmSpec, instance: dict[str, Any]) -> PurePosixPath:
    closure = instance["image"].get("closure_sha256")
    if closure is None and instance["image"].get("kind") in ("scheduler-mutant", "daemon-mutant"):
        closure = instance["image"].get("recipe_sha256")
    if not isinstance(closure, str) or len(closure) != 64:
        raise ValueError(
            f"product image {instance['image']['label']} has no portable closure"
        )
    return (
        PurePosixPath(farm.hosts[instance["host"]]["scratch_root"])
        / "icefarm"
        / "runtimes"
        / closure
        / "root"
    )


def compiler_identity_digest(instance: dict[str, Any]) -> str:
    recipe = instance.get("compiler_recipe")
    if not isinstance(recipe, dict):
        raise ValueError(f"client {instance['name']} has no compiler recipe")
    return hashlib.sha256(
        canonical_bytes(
            {
                "client_environment": instance["container_image"],
                "compiler_recipe": recipe,
            }
        )
    ).hexdigest()


def oracle_root(
    farm: FarmSpec, instance: dict[str, Any], corpus_name: str
) -> PurePosixPath:
    return (
        PurePosixPath(farm.hosts[instance["host"]]["scratch_root"])
        / "icefarm"
        / "oracle"
        / corpus_name
        / compiler_identity_digest(instance)
    )


def toolchain_root(farm: FarmSpec, instance: dict[str, Any]) -> PurePosixPath | None:
    recipe = instance.get("compiler_recipe")
    toolchain = recipe.get("toolchain") if isinstance(recipe, dict) else None
    if not isinstance(toolchain, dict):
        return None
    return (
        PurePosixPath(farm.hosts[instance["host"]]["scratch_root"])
        / "icefarm"
        / "toolchains"
        / toolchain["archive_sha256"]
        / "root"
    )
