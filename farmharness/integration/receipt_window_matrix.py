"""Materialize portable per-link P51 receipt-window scenario matrices."""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import math
from pathlib import Path
from typing import Any

try:
    from .farm_spec import load_farm_spec
    from .scenario_spec import ScenarioSpecError, load_scenario_spec
except ImportError:  # Direct execution from this directory.
    from farm_spec import load_farm_spec
    from scenario_spec import ScenarioSpecError, load_scenario_spec


ROOT = Path(__file__).resolve().parent
TEMPLATE_PATH = ROOT / "p51-receipt-window-matrix.template.json"
PROFILE_NAMES = ("P29V1", "ZSTD_TU", "ZSTD_ROUTE")
TOPOLOGY_ROWS = (
    {"id": "C1F2", "clients": 1, "workers": 2},
    {"id": "C1F3", "clients": 1, "workers": 3},
    {"id": "C1F4", "clients": 1, "workers": 4},
    {"id": "C2F1", "clients": 2, "workers": 1},
    {"id": "C3F1", "clients": 3, "workers": 1},
    {"id": "C4F1", "clients": 4, "workers": 1},
)


class MatrixError(ValueError):
    pass


def _load_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise MatrixError(f"cannot read JSON {path}: {exc}") from exc
    if not isinstance(value, dict):
        raise MatrixError(f"{path}: expected an object")
    return value


def generate_matrix(
    *, farm_path: Path, base_path: Path, helper_path: Path, output_dir: Path,
    template_path: Path = TEMPLATE_PATH,
) -> list[Path]:
    """Write the exact 6-topology × 2-window × 3-profile scenario matrix."""
    template = _load_json(template_path)
    if template.get("schema") != "icefarm-p51-receipt-matrix-v1":
        raise MatrixError("unsupported matrix template schema")
    if template.get("profiles") != list(PROFILE_NAMES) or template.get("windows") != [1, 30]:
        raise MatrixError("template must preserve all three profiles and W1/W30")
    if template.get("topologies") != list(TOPOLOGY_ROWS):
        raise MatrixError("template topology list differs from required six topologies")
    if template.get("restart_extension") != "pending-not-run":
        raise MatrixError("restart extension must remain explicitly pending")

    farm = load_farm_spec(farm_path)
    base = _load_json(base_path)
    topology_authority = farm.data["authority"]["topologies"]
    for row in TOPOLOGY_ROWS:
        authority = topology_authority.get(row["id"])
        if not isinstance(authority, dict) or authority.get("f_relationships") != row["workers"]:
            raise MatrixError(
                f"farm authority must explicitly authorize {row['id']} with "
                f"{row['workers']} F roles"
            )
    for row in TOPOLOGY_ROWS:
        authority = topology_authority[row["id"]]
        required_slots_per_f = row["clients"] * 30 + int(row["clients"] == 1)
        if int(authority.get("slots_per_f", 0)) < required_slots_per_f:
            raise MatrixError(
                f"farm authority {row['id']} needs slots_per_f >= "
                f"{required_slots_per_f} for simultaneous W30 gates and scheduler credit"
            )
    helper_path = helper_path.resolve(strict=True)
    helper_stat = helper_path.stat()
    if not helper_path.is_file() or not helper_stat.st_mode & 0o111:
        raise MatrixError("receipt helper must be an executable regular file")
    helper_sha = hashlib.sha256(helper_path.read_bytes()).hexdigest()
    output_dir = output_dir.resolve()
    if output_dir.exists() and (not output_dir.is_dir() or any(output_dir.iterdir())):
        raise MatrixError("output directory must be new or empty")

    instances = base.get("instances")
    if not isinstance(instances, list):
        raise MatrixError("base scenario has no instance list")
    client_templates = [
        item for item in instances
        if item.get("role") == "C"
        and isinstance(item.get("env"), dict)
        and item["env"].get("ICECC_P50_MODE") == "on"
        and item["env"].get("ICECC_P51_MODE") == "on"
    ]
    if len(client_templates) != 1:
        raise MatrixError(
            "base scenario must contain exactly one P50/R2 client template "
            "(ICECC_P50_MODE=on and ICECC_P51_MODE=on)"
        )
    scheduler_templates = [
        item for item in instances
        if item.get("role") == "S"
        and isinstance(item.get("env"), dict)
        and item["env"].get("ICECC_P51_MODE") == "on"
    ]
    worker_templates = [
        item for item in instances
        if item.get("role") == "F"
        and isinstance(item.get("env"), dict)
        and item["env"].get("ICECC_P51_MODE") == "on"
    ]
    if len(scheduler_templates) != 1:
        raise MatrixError(
            "base scenario must contain exactly one P51-enabled scheduler template"
        )
    if len(worker_templates) != 1:
        raise MatrixError(
            "base scenario must contain exactly one P51-enabled worker template"
        )
    role_templates = {
        "S": scheduler_templates[0],
        "C": client_templates[0],
        "F": worker_templates[0],
    }
    corpus_name = base.get("workload", {}).get("corpus")
    corpus = farm.data["corpora"].get(corpus_name)
    if not isinstance(corpus, dict) or corpus.get("kind") != "tu-manifest":
        raise MatrixError("base workload corpus must be an authorized TU manifest")
    base_jobs = int(corpus["tus"]) * int(corpus.get("repeat", 1))
    if base_jobs < 1:
        raise MatrixError("base corpus must contain at least one manifest job")

    # Validate template selection before creating the destination so failures
    # leave no misleading output directory behind.
    output_dir.mkdir(parents=True, exist_ok=True)
    written: list[Path] = []
    for topology in TOPOLOGY_ROWS:
        for window in template["windows"]:
            links_per_client = topology["workers"] if topology["clients"] == 1 else 1
            repeat = max(1, math.ceil(links_per_client * window / base_jobs))
            manifest_jobs = base_jobs * repeat
            workload_jobs = links_per_client * window
            client_names = [f"C{index}" for index in range(1, topology["clients"] + 1)]
            worker_names = [f"F{index}" for index in range(1, topology["workers"] + 1)]
            worker_link_count = {
                name: sum(1 for client in client_names for worker in worker_names if worker == name)
                for name in worker_names
            }
            for profile in template["profiles"]:
                scenario = copy.deepcopy(base)
                scenario["id"] = f"P51-receipt-{topology['id']}-{profile}-W{window}"
                scenario["instances"] = []
                scheduler = copy.deepcopy(role_templates["S"])
                scheduler["name"] = "S1"
                scheduler.setdefault("env", {})["ICECC_P51_MODE"] = "on"
                scheduler["env"]["ICECC_P50_PROFILE"] = profile
                scenario["instances"].append(scheduler)
                for index, name in enumerate(worker_names, start=1):
                    worker = copy.deepcopy(role_templates["F"])
                    worker["name"] = name
                    worker["slots"] = worker_link_count[name] * window
                    # One-C/many-F fills exactly the submitter's dispatch
                    # credit with the gate cohort. Keep one additional
                    # advertised slot so the scheduler's farm_slots-1
                    # clamp does not reduce that cohort's credit.
                    if topology["clients"] == 1 and index == 1:
                        worker["slots"] += 1
                    worker.setdefault("env", {})["ICECC_P51_MODE"] = "on"
                    scenario["instances"].append(worker)
                for name in client_names:
                    client = copy.deepcopy(role_templates["C"])
                    client["name"] = name
                    client.setdefault("env", {}).update(
                        {"ICECC_P50_MODE": "on", "ICECC_P51_MODE": "on"}
                    )
                    scenario["instances"].append(client)

                links: list[dict[str, Any]] = []
                for client_name in client_names:
                    assigned_workers = worker_names if topology["clients"] == 1 else [worker_names[0]]
                    quotient, remainder = divmod(manifest_jobs, len(assigned_workers))
                    first = 1
                    for link_index, worker_name in enumerate(assigned_workers):
                        length = quotient + (1 if link_index < remainder else 0)
                        links.append({
                            "client": client_name,
                            "worker": worker_name,
                            "first_job": first,
                            "last_job": first + length - 1,
                        })
                        first += length

                scenario["workload"].update({
                    "driver": "p51-receipt-window",
                    "clients": client_names,
                    "turns": ["A"],
                    "jobs": workload_jobs,
                    "repeat": repeat,
                    "receipt_gate": {
                        "binary": str(helper_path),
                        "binary_sha256": helper_sha,
                        "expected_commits": window,
                        "negotiated_window": window,
                        "expect_observed": True,
                        # The remote helper gets the declared turn budget less
                        # the runner's 30s shutdown/collection margin.
                        "command_timeout_s": int(scenario["timeouts"]["turn_s"]),
                        "links": links,
                    },
                })
                scenario["workload"].pop("d18_roles", None)
                destination = output_dir / f"{scenario['id']}.json"
                if destination.exists():
                    raise MatrixError(f"refusing to overwrite {destination}")
                destination.write_text(
                    json.dumps(scenario, sort_keys=True, indent=2) + "\n",
                    encoding="utf-8",
                )
                try:
                    load_scenario_spec(destination, farm)
                except ScenarioSpecError as exc:
                    destination.unlink()
                    raise MatrixError(f"generated scenario {scenario['id']} is invalid: {exc}") from exc
                written.append(destination)
    return written


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--farm", required=True, type=Path)
    parser.add_argument("--base", required=True, type=Path)
    parser.add_argument("--helper", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--template", type=Path, default=TEMPLATE_PATH)
    args = parser.parse_args()
    try:
        written = generate_matrix(
            farm_path=args.farm, base_path=args.base, helper_path=args.helper,
            output_dir=args.output_dir, template_path=args.template,
        )
    except MatrixError as exc:
        parser.error(str(exc))
    print(f"P51_RECEIPT_MATRIX_SCENARIOS={len(written)}")
    for path in written:
        print(path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
