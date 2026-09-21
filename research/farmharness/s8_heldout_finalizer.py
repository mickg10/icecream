#!/usr/bin/env python3
"""Finalize retained held-out S7 runs through the existing S8 pipeline.

The host side only authenticates retained runner metadata and writes a plan.
Runtime leaves are intentionally root-owned by the original runner; the plan
therefore emits one pinned-image ``docker run --user 0`` command per cell.
Those commands copy only the named capture files into a fresh output root,
run the existing replay/normalizer/package/metric/driver tools, and stop on
any object, identity, or calibration failure.  No evidence is fabricated.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shlex
import stat
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any

try:
    from .s8_schema import CORPORA, PROFILES, REGIMES
except ImportError:  # pragma: no cover - direct harness invocation.
    from s8_schema import CORPORA, PROFILES, REGIMES


SCHEMA = "icecream-s8-heldout-finalizer-plan-v1"
CALIBRATION_SCHEMA = "icecream-s8-calibration-model-manifest-v1"
PINNED_IMAGE = "icecream/farm-node:ubuntu22-gcc11-boost174"
EXPECTED_RUN_COUNT = 16
HEX64 = re.compile(r"^[0-9a-f]{64}$")
LEAF_RE = re.compile(r"^[A-Za-z0-9_.-]+$")
RUN_RE = re.compile(
    r"^s8-heldout-(?P<corpus>duckdb|llvm-1238)-"
    r"(?P<profile>grz-residual|p29|zstd-route|zstd-tu)-(?P<regime>cold|warm)$"
)
PROFILE_SLUGS = {
    "GRZ_RESIDUAL": "grz-residual", "P29": "p29",
    "ZSTD_ROUTE": "zstd-route", "ZSTD_TU": "zstd-tu",
}
CORPUS_SLUGS = {"DuckDB": "duckdb", "LLVM-1238": "llvm-1238"}
PROFILE_BY_SLUG = {value: key for key, value in PROFILE_SLUGS.items()}
CORPUS_BY_SLUG = {value: key for key, value in CORPUS_SLUGS.items()}
REQUIRED_RUNTIME_BASENAMES = {
    "s7-prewarm-preprocessed.ii", "s7-measured-preprocessed.ii",
    "s7-prewarm-c-action-trace.jsonl", "s7-prewarm-f-action-trace.jsonl",
    "s7-measured-c-action-trace.jsonl", "s7-measured-f-action-trace.jsonl",
    "client-compile-measured.log", "out/local-measured.o", "out/remote-measured.o",
}


class FinalizerError(ValueError):
    """Retained metadata is absent, ambiguous, or inconsistent."""


@dataclass(frozen=True)
class RunRecord:
    cell: str
    corpus: str
    profile: str
    regime: str
    run_dir: Path
    timestamp_dir: Path
    runtime_root: Path
    runtime_leaf: str
    image: str
    source_repo: Path
    workload_repo: Path
    workload_commit: str
    build_root: Path
    source_root: Path
    source_relative: str
    source_file: Path
    compile_db: Path
    source_commit: str
    run_stdout_sha256: str
    inspect_sha256: str
    docker_binds: tuple[str, ...]
    capture_names: tuple[str, ...]
    source_fact: dict[str, object]
    compile_db_fact: dict[str, object]


def _private_file(path: Path, label: str) -> bytes:
    try:
        info = path.lstat()
    except OSError as exc:
        raise FinalizerError(f"{label}:unavailable:{path}") from exc
    if stat.S_ISLNK(info.st_mode) or not stat.S_ISREG(info.st_mode) or info.st_nlink != 1:
        raise FinalizerError(f"{label}:not_private_regular_file:{path}")
    try:
        return path.read_bytes()
    except OSError as exc:
        raise FinalizerError(f"{label}:unreadable:{path}") from exc


def _json(path: Path, label: str) -> tuple[Any, str]:
    raw = _private_file(path, label)
    digest = hashlib.sha256(raw).hexdigest()
    try:
        return json.loads(raw.decode("utf-8")), digest
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise FinalizerError(f"{label}:invalid_json:{path}") from exc


def _env(config: dict[str, Any]) -> dict[str, str]:
    values = config.get("Env")
    if not isinstance(values, list) or any(not isinstance(value, str) for value in values):
        raise FinalizerError("docker_config:environment_invalid")
    result: dict[str, str] = {}
    for value in values:
        if "=" in value:
            key, item = value.split("=", 1)
            result[key] = item
    return result


def _bind_parts(value: str) -> tuple[str, str, str]:
    parts = value.split(":", 2)
    if len(parts) == 2:
        return parts[0], parts[1], ""
    if len(parts) == 3:
        return parts[0], parts[1], parts[2]
    raise FinalizerError(f"docker_bind:invalid:{value}")


def _bind_map(inspect: dict[str, Any]) -> dict[str, tuple[str, str]]:
    host = inspect.get("HostConfig")
    binds = host.get("Binds") if isinstance(host, dict) else None
    if not isinstance(binds, list) or any(not isinstance(item, str) for item in binds):
        raise FinalizerError("docker_config:binds_invalid")
    result: dict[str, tuple[str, str]] = {}
    for item in binds:
        source, destination, mode = _bind_parts(item)
        if destination in result:
            raise FinalizerError(f"docker_bind:duplicate_destination:{destination}")
        result[destination] = (source, mode)
    return result


def _git_head(repository: Path) -> str:
    try:
        value = subprocess.run(
            ["git", "-C", str(repository), "rev-parse", "HEAD"],
            check=True, capture_output=True, text=True, timeout=15,
        ).stdout.strip()
    except (OSError, subprocess.SubprocessError) as exc:
        raise FinalizerError(f"source_repository:commit_unavailable:{repository}") from exc
    if not re.fullmatch(r"[0-9a-fA-F]{40}", value):
        raise FinalizerError("source_repository:commit_invalid")
    return value.lower()


def _git_clean(repository: Path, label: str) -> None:
    try:
        dirty = subprocess.run(
            ["git", "-C", str(repository), "status", "--porcelain", "--untracked-files=no"],
            check=True, capture_output=True, text=True, timeout=15,
        ).stdout
    except (OSError, subprocess.SubprocessError) as exc:
        raise FinalizerError(f"{label}:status_unavailable:{repository}") from exc
    if dirty:
        raise FinalizerError(f"{label}:tracked_edits_present:{repository}")


def _file_fact(path: Path, label: str) -> dict[str, object]:
    raw = _private_file(path, label)
    return {"path": str(path.resolve()), "bytes": len(raw),
            "sha256": hashlib.sha256(raw).hexdigest()}


def _overlap(left: Path, right: Path) -> bool:
    left, right = left.resolve(), right.resolve()
    return left == right or left in right.parents or right in left.parents


def _calibration_facts(path: Path) -> tuple[str, str, int]:
    value, manifest_sha = _json(path, "calibration_manifest")
    if not isinstance(value, dict) or value.get("schema") != CALIBRATION_SCHEMA:
        raise FinalizerError("calibration_manifest:schema_invalid")
    bundle = value.get("bundle")
    if not isinstance(bundle, dict) or set(bundle) != {"path", "sha256", "bytes"}:
        raise FinalizerError("calibration_manifest:bundle_descriptor_invalid")
    relative = bundle["path"]
    if (not isinstance(relative, str) or not relative or Path(relative).is_absolute() or
            any(part in ("", ".", "..") for part in Path(relative).parts)):
        raise FinalizerError("calibration_manifest:bundle_path_invalid")
    bundle_path = path.parent / relative
    bundle_raw = _private_file(bundle_path, "calibration_bundle")
    if (bundle.get("sha256") != hashlib.sha256(bundle_raw).hexdigest() or
            bundle.get("bytes") != len(bundle_raw)):
        raise FinalizerError("calibration_manifest:bundle_binding_mismatch")
    return manifest_sha, str(bundle_path.resolve()), len(bundle_raw)


def _stdout_leaf(stdout: str, regime: str) -> str:
    expected = {
        "S7_PREWARM_INPUT", "S7_MEASURED_INPUT", "S7_PREWARM_C_ACTION_TRACE",
        "S7_PREWARM_F_ACTION_TRACE", "S7_MEASURED_C_ACTION_TRACE",
        "S7_MEASURED_F_ACTION_TRACE",
    }
    found: dict[str, str] = {}
    for line in stdout.splitlines():
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        if key in expected:
            found[key] = value
    required = {
        "S7_MEASURED_INPUT", "S7_MEASURED_C_ACTION_TRACE", "S7_MEASURED_F_ACTION_TRACE",
    }
    if regime == "warm":
        required |= {"S7_PREWARM_INPUT", "S7_PREWARM_C_ACTION_TRACE",
                     "S7_PREWARM_F_ACTION_TRACE"}
    if not required.issubset(found):
        raise FinalizerError(f"run_stdout:runtime_markers_invalid:{sorted(required - set(found))}")
    leaves: set[str] = set()
    for value in found.values():
        path = Path(value)
        if len(path.parts) < 3 or path.parts[0:2] != ("/", "x"):
            raise FinalizerError(f"run_stdout:runtime_path_invalid:{value}")
        leaf = path.parts[2]
        if not LEAF_RE.fullmatch(leaf) or path.parent != Path("/x") / leaf:
            raise FinalizerError(f"run_stdout:runtime_leaf_invalid:{value}")
        leaves.add(leaf)
    if len(leaves) != 1:
        raise FinalizerError("run_stdout:runtime_leaf_ambiguous")
    return next(iter(leaves))


def _cell_from_dir(path: Path) -> tuple[str, str, str, str]:
    match = RUN_RE.fullmatch(path.name)
    if match is None:
        raise FinalizerError(f"run_directory:unsupported_name:{path.name}")
    corpus = CORPUS_BY_SLUG[match.group("corpus")]
    profile = PROFILE_BY_SLUG[match.group("profile")]
    regime = match.group("regime")
    return corpus, profile, regime, f"{corpus}/{profile}/{regime}"


def _one_timestamp(run_dir: Path) -> Path:
    candidates = []
    try:
        candidates = [item for item in run_dir.iterdir() if item.is_dir() and not item.is_symlink()]
    except OSError as exc:
        raise FinalizerError(f"run_directory:unreadable:{run_dir}") from exc
    candidates = [item for item in candidates
                  if (item / "run.stdout").is_file() and (item / "docker-inspect.json").is_file()]
    if len(candidates) != 1:
        raise FinalizerError(f"run_directory:expected_one_timestamp_leaf:{run_dir}")
    return candidates[0]


def discover_runs(runs_root: Path, source_repo: Path, build_root: Path,
                  image: str = PINNED_IMAGE, require_all: bool = True,
                  workload_repositories: dict[str, Path] | None = None,
                  workload_commits: dict[str, str] | None = None) -> list[RunRecord]:
    """Authenticate run metadata without opening root-owned runtime evidence."""
    try:
        info = runs_root.lstat()
    except OSError as exc:
        raise FinalizerError(f"runs_root:unavailable:{runs_root}") from exc
    if stat.S_ISLNK(info.st_mode) or not stat.S_ISDIR(info.st_mode):
        raise FinalizerError("runs_root:not_private_directory")
    source_repo, build_root = source_repo.resolve(), build_root.resolve()
    if source_repo.is_symlink() or not source_repo.is_dir():
        raise FinalizerError("source_repository:not_private_directory")
    if build_root.is_symlink() or not build_root.is_dir():
        raise FinalizerError("build_root:not_private_directory")
    source_commit = _git_head(source_repo)
    _git_clean(source_repo, "source_repository")
    if workload_repositories is None or workload_commits is None:
        raise FinalizerError("workload_identity:explicit_mapping_required")
    expected_workloads = {"DuckDB", "LLVM-1238"}
    if set(workload_repositories) != expected_workloads or set(workload_commits) != expected_workloads:
        raise FinalizerError("workload_identity:explicit_mapping_required")
    workload_repos = {name: path.resolve() for name, path in workload_repositories.items()}
    for name, repository in workload_repos.items():
        if repository.is_symlink() or not repository.is_dir():
            raise FinalizerError(f"workload_repository:not_private_directory:{name}")
        _git_clean(repository, f"workload_repository:{name}")
        commit = workload_commits[name].lower()
        if not re.fullmatch(r"[0-9a-f]{40}", commit) or _git_head(repository) != commit:
            raise FinalizerError(f"workload_repository:commit_mismatch:{name}")
    records: list[RunRecord] = []
    seen: set[str] = set()
    entries = sorted(item for item in runs_root.iterdir() if item.is_dir() and not item.is_symlink())
    for run_dir in entries:
        if not run_dir.name.startswith("s8-heldout-"):
            continue
        # The inventory root may also contain this tool's prior scratch plans;
        # only exact retained-run names participate in the 16-cell matrix.
        if RUN_RE.fullmatch(run_dir.name) is None:
            continue
        corpus, profile, regime, cell = _cell_from_dir(run_dir)
        if cell in seen:
            raise FinalizerError(f"runs_root:duplicate_cell:{cell}")
        seen.add(cell)
        timestamp_dir = _one_timestamp(run_dir)
        stdout_raw = _private_file(timestamp_dir / "run.stdout", "run_stdout")
        exit_code = _private_file(timestamp_dir / "exit-code.txt", "exit_code").decode("ascii", "strict").strip()
        if exit_code != "0":
            raise FinalizerError(f"run_status:not_successful:{cell}")
        inspect_value, inspect_sha = _json(timestamp_dir / "docker-inspect.json", "docker_inspect")
        if not isinstance(inspect_value, list) or len(inspect_value) != 1 or not isinstance(inspect_value[0], dict):
            raise FinalizerError(f"docker_inspect:expected_one_record:{cell}")
        inspect = inspect_value[0]
        state = inspect.get("State")
        config = inspect.get("Config")
        if (not isinstance(state, dict) or state.get("ExitCode") != 0 or
                state.get("OOMKilled") is not False or state.get("Status") != "exited"):
            raise FinalizerError(f"run_status:not_successful:{cell}")
        if not isinstance(config, dict) or config.get("Image") != image:
            raise FinalizerError(f"docker_image:mismatch:{cell}")
        env = _env(config)
        expected_warm = "1" if regime == "warm" else "0"
        if env.get("ICECC_P50_C1F1_WARM") != expected_warm:
            raise FinalizerError(f"run_identity:regime_mismatch:{cell}")
        if env.get("ICECC_P50_PROFILE") not in {profile, "GRZ" if profile == "GRZ_RESIDUAL" else profile}:
            raise FinalizerError(f"run_identity:profile_mismatch:{cell}")
        if not env.get("ICECC_P50_C1F1_SOURCE_ROOT") or not env.get("ICECC_P50_C1F1_SOURCE_RELATIVE"):
            raise FinalizerError(f"run_identity:source_missing:{cell}")
        compile_source = env.get("ICECC_P50_C1F1_COMPILE_SOURCE")
        compile_db = env.get("ICECC_P50_C1F1_COMPILE_DB")
        if not compile_source or not compile_db:
            raise FinalizerError(f"run_identity:compile_identity_missing:{cell}")
        workload_repo = workload_repos[corpus]
        source_file = Path(compile_source).resolve()
        try:
            source_file.relative_to(workload_repo)
        except ValueError as exc:
            raise FinalizerError(f"workload_identity:source_outside_mapping:{cell}") from exc
        binds = _bind_map(inspect)
        if binds.get("/src", (None,))[0] != str(source_repo):
            raise FinalizerError(f"docker_bind:source_repo_mismatch:{cell}")
        if binds.get("/binroot", (None,))[0] != str(build_root):
            raise FinalizerError(f"docker_bind:build_root_mismatch:{cell}")
        runtime_host, runtime_mode = binds.get("/x", (None, None))
        expected_runtime = str(timestamp_dir / "runtime")
        if (runtime_host != expected_runtime or runtime_mode is None or
                "ro" in runtime_mode.split(",")):
            raise FinalizerError(f"docker_bind:runtime_mismatch:{cell}")
        stdout = stdout_raw.decode("utf-8", "strict")
        expected_marker = f"PASS: all-P50 C1F1 {profile} compile is remote and byte-identical"
        if expected_marker not in stdout:
            raise FinalizerError(f"run_stdout:pass_marker_missing:{cell}")
        leaf = _stdout_leaf(stdout, regime)
        capture_names = [
            "s7-measured-preprocessed.ii", "s7-measured-c-action-trace.jsonl",
            "s7-measured-f-action-trace.jsonl", "client-compile-measured.log",
            "out/local-measured.o", "out/remote-measured.o",
        ]
        if regime == "warm":
            capture_names.extend(["s7-prewarm-preprocessed.ii", "s7-prewarm-c-action-trace.jsonl",
                                  "s7-prewarm-f-action-trace.jsonl"])
        source_fact = _file_fact(source_file, f"workload_source:{cell}")
        compile_db_path = Path(compile_db).resolve()
        compile_db_fact = _file_fact(compile_db_path, f"compile_database:{cell}")
        records.append(RunRecord(
            cell=cell, corpus=corpus, profile=profile, regime=regime,
            run_dir=run_dir, timestamp_dir=timestamp_dir,
            runtime_root=timestamp_dir / "runtime", runtime_leaf=leaf,
            image=image, source_repo=source_repo, workload_repo=workload_repo,
            workload_commit=workload_commits[corpus].lower(), build_root=build_root,
            source_root=Path(env["ICECC_P50_C1F1_SOURCE_ROOT"]),
            source_relative=env["ICECC_P50_C1F1_SOURCE_RELATIVE"],
            source_file=Path(compile_source), compile_db=Path(compile_db),
            source_commit=source_commit,
            run_stdout_sha256=hashlib.sha256(stdout_raw).hexdigest(),
            inspect_sha256=inspect_sha, docker_binds=tuple(inspect["HostConfig"]["Binds"]),
            capture_names=tuple(capture_names), source_fact=source_fact, compile_db_fact=compile_db_fact,
        ))
    expected = {f"{corpus}/{profile}/{regime}" for corpus in CORPORA
                for profile in PROFILES for regime in REGIMES
                if corpus in {"DuckDB", "LLVM-1238"}}
    if require_all and seen != expected:
        missing, extra = sorted(expected - seen), sorted(seen - expected)
        raise FinalizerError(f"runs_root:heldout_matrix_mismatch:missing={missing}:extra={extra}")
    if not records:
        raise FinalizerError("runs_root:no_heldout_runs")
    if require_all and len(records) != EXPECTED_RUN_COUNT:
        raise FinalizerError(f"runs_root:expected_{EXPECTED_RUN_COUNT}_runs:{len(records)}")
    return sorted(records, key=lambda item: item.cell)


def _q(value: object) -> str:
    return shlex.quote(str(value))


def _safe_name(cell: str) -> str:
    return cell.replace("/", "--")


def _binary_facts(build_root: Path, simulator_path: Path,
                  simulator_sha256: str) -> dict[str, dict[str, object]]:
    paths = {
        "client": build_root / "client/icecc",
        "daemon": build_root / "daemon/iceccd",
        "scheduler": build_root / "scheduler/icecc-scheduler",
        "cache-service": build_root / "cache/icecc-cache-service",
        "simulator": simulator_path,
    }
    facts = {name: _file_fact(path, f"binary:{name}") for name, path in paths.items()}
    if facts["simulator"]["sha256"] != simulator_sha256.lower():
        raise FinalizerError("simulator:sha256_mismatch")
    return facts


def _shell_verify(path: Path, fact: dict[str, object]) -> str:
    return (f"test -f {_q(path)} && test ! -L {_q(path)} && "
            f"test \"$(wc -c < {_q(path)})\" -eq {_q(fact['bytes'])} && "
            f"test \"$(sha256sum -- {_q(path)} | awk '{{print $1}}')\" = {_q(fact['sha256'])}")


def _root_pipeline_script(record: RunRecord,
                          binary_facts: dict[str, dict[str, object]]) -> str:
    """Copy protected captures and run only replay in the pinned image."""
    cell_root = Path("/out")
    experiment = cell_root / "experiment"
    runtime = experiment / "runtime" / "run"
    replay = experiment / "exact-replay"
    leaf = Path("/run") / record.runtime_leaf
    lines = ["set -eu",
             "marker=/out/replay-complete.json",
             "if test -f \"$marker\"; then exit 0; fi",
             "if test -e /out/experiment/exact-replay; then",
             "  echo 'finalizer: replay output exists without completion marker' >&2; exit 1",
             "fi",
             "if test -e /out/experiment/runtime; then",
             "  echo 'finalizer: capture output exists without completion marker' >&2; exit 1",
             "fi",
             f"mkdir -p {_q(runtime)}",
             f"mkdir -p {_q(runtime / 'out')}"]
    names = [
        "s7-measured-preprocessed.ii", "s7-measured-c-action-trace.jsonl",
        "s7-measured-f-action-trace.jsonl", "client-compile-measured.log",
        "out/local-measured.o", "out/remote-measured.o",
    ]
    if record.regime == "warm":
        names.extend(["s7-prewarm-preprocessed.ii", "s7-prewarm-c-action-trace.jsonl",
                      "s7-prewarm-f-action-trace.jsonl"])
    for name in names:
        source = leaf / name
        destination = runtime / name
        # Validate the protected source before copying, then validate the
        # derived destination.  No chmod/chown is ever applied to /run.
        lines.append(f"test -f {_q(source)} && test ! -L {_q(source)} && test \"$(stat -c %h -- {_q(source)})\" -eq 1")
        lines.append(f"src_bytes=$(wc -c < {_q(source)}); src_sha=$(sha256sum -- {_q(source)} | awk '{{print $1}}')")
        lines.append(f"install -m 0644 -- {_q(source)} {_q(destination)}")
        lines.append(f"dst_bytes=$(wc -c < {_q(destination)}); dst_sha=$(sha256sum -- {_q(destination)} | awk '{{print $1}}')")
        lines.append("test \"$src_bytes\" -eq \"$dst_bytes\" && test \"$src_sha\" = \"$dst_sha\"")
    lines.append(f"cmp -s {_q(runtime / 'out/local-measured.o')} {_q(runtime / 'out/remote-measured.o')}")
    for name, path in (("client", Path("/binroot/client/icecc")),
                       ("daemon", Path("/binroot/daemon/iceccd")),
                       ("scheduler", Path("/binroot/scheduler/icecc-scheduler")),
                       ("cache-service", Path("/binroot/cache/icecc-cache-service")),
                       ("simulator", Path("/src/cache/sim/.p50sim.bin"))):
        lines.append(_shell_verify(path, binary_facts[name]))
    # The inline verifier recomputes all source/destination descriptors and
    # emits canonical JSON only after every copy has passed equality checks.
    lines.extend(["python3 - <<'PY'", "import hashlib, json, os, stat",
                  f"cell = {record.cell!r}", f"leaf = {record.runtime_leaf!r}",
                  f"run_sha = {record.run_stdout_sha256!r}", f"inspect_sha = {record.inspect_sha256!r}",
                  f"names = {list(record.capture_names)!r}", f"binaries = {binary_facts!r}",
                  "base = '/run/' + leaf; out = '/out/experiment/runtime/run'",
                  "def fact(path):",
                  "    s = os.lstat(path)",
                  "    if stat.S_ISLNK(s.st_mode) or not stat.S_ISREG(s.st_mode) or s.st_nlink != 1: raise SystemExit('capture manifest: non-private file')",
                  "    raw = open(path, 'rb').read()",
                  "    return {'path': path, 'bytes': len(raw), 'sha256': hashlib.sha256(raw).hexdigest()}",
                  "captures = {}",
                  "for name in names:",
                  "    src, dst = base + '/' + name, out + '/' + name",
                  "    sf, df = fact(src), fact(dst)",
                  "    if sf['bytes'] != df['bytes'] or sf['sha256'] != df['sha256']: raise SystemExit('capture manifest: copy mismatch')",
                  "    captures[name] = {'source': sf, 'destination': df}",
                  "metadata = {}",
                  "for label, path, expected in (('run.stdout', '/metadata/run.stdout', run_sha), ('docker-inspect.json', '/metadata/docker-inspect.json', inspect_sha)):",
                  "    observed = fact(path)",
                  "    if observed['sha256'] != expected: raise SystemExit('capture manifest: metadata mutation:' + label)",
                  "    metadata[label] = observed",
                  "value = {'schema': 'icecream-s8-heldout-capture-manifest-v1', 'cell': cell, 'runtime_leaf': leaf,",
                  "         'run_stdout_sha256': run_sha, 'docker_inspect_sha256': inspect_sha,",
                  "         'captures': captures, 'binaries': binaries}",
                  "open('/out/capture-manifest.json', 'w').write(json.dumps(value, sort_keys=True, separators=(',', ':')) + '\\n')",
                  "PY"])
    replay_args = ["python3", "/src/research/farmharness/s7_warm_replay.py", "--cell", record.cell,
                   "--out", str(replay), "--sim", "/src/cache/sim/p50sim"]
    if record.regime == "cold":
        replay_args += ["--input", str(runtime / "s7-measured-preprocessed.ii"),
                        "--c-trace", str(runtime / "s7-measured-c-action-trace.jsonl"),
                        "--f-trace", str(runtime / "s7-measured-f-action-trace.jsonl")]
    else:
        replay_args += ["--prewarm-input", str(runtime / "s7-prewarm-preprocessed.ii"),
                        "--measured-input", str(runtime / "s7-measured-preprocessed.ii"),
                        "--prewarm-c-trace", str(runtime / "s7-prewarm-c-action-trace.jsonl"),
                        "--prewarm-f-trace", str(runtime / "s7-prewarm-f-action-trace.jsonl"),
                        "--measured-c-trace", str(runtime / "s7-measured-c-action-trace.jsonl"),
                        "--measured-f-trace", str(runtime / "s7-measured-f-action-trace.jsonl")]
    lines.append(" ".join(_q(arg) for arg in replay_args))
    lines.append("printf '%s\\n' '{\"schema\":\"icecream-s8-heldout-finalizer-replay-v1\",\"status\":\"PASS\"}' > /out/replay-complete.json")
    return "\n".join(lines)


def _mount_destination(mount: str) -> str:
    return _bind_parts(mount)[1]


def docker_command(record: RunRecord, output_cell_host: Path, simulator_path: Path,
                   simulator_sha256: str,
                   binary_facts: dict[str, dict[str, object]]) -> str:
    """Return the root-only capture/replay command, with all evidence read-only."""
    mounts = [
        f"{record.runtime_root}:/run:ro", f"{record.timestamp_dir}:/metadata:ro",
        f"{output_cell_host}:/out:rw",
        f"{record.source_repo}:/src:ro", f"{record.build_root}:/binroot:ro",
        f"{simulator_path}:/src/cache/sim/.p50sim.bin:ro",
        f"{record.workload_repo}:{record.workload_repo}:ro",
    ]
    for bind in record.docker_binds:
        source, destination, mode = _bind_parts(bind)
        if destination in {"/x", "/src", "/binroot", str(record.workload_repo)}:
            continue
        mounts.append(f"{source}:{destination}:{mode or 'ro'}")
    # The argument is intentionally part of the plan, even though the wrapper
    # reads the sibling binary.  This makes a wrong integrated build fail at
    # planning time rather than silently replaying another executable.
    if not HEX64.fullmatch(simulator_sha256):
        raise FinalizerError("simulator:sha256_invalid")
    args = ["docker", "run", "--rm", "--user", "0", "--network", "host"]
    for mount in dict.fromkeys(mounts):
        args += ["-v", mount]
    args += [record.image, "/bin/sh", "-lc", _root_pipeline_script(record, binary_facts)]
    return " ".join(_q(arg) for arg in args)


def _host_pipeline_script(record: RunRecord, output_cell_host: Path,
                          calibration: Path, simulator_path: Path,
                          simulator_sha256: str,
                          binary_facts: dict[str, dict[str, object]]) -> str:
    """Run normalization/package/metric/driver on the host, where git exists."""
    root = output_cell_host.resolve()
    experiment, runtime = root / "experiment", root / "experiment" / "runtime" / "run"
    replay, normalized = experiment / "exact-replay", root / "normalized"
    package, metric, driver = root / "package", root / "metric", root / "driver"
    py = record.source_repo / "research" / "farmharness"
    binaries = [
        f"client={record.build_root / 'client/icecc'}",
        f"daemon={record.build_root / 'daemon/iceccd'}",
        f"scheduler={record.build_root / 'scheduler/icecc-scheduler'}",
        f"cache-service={record.build_root / 'cache/icecc-cache-service'}",
        f"simulator={simulator_path}",
    ]
    args = ["python3", str(py / "s7_grz_results.py"), "--experiment", str(experiment),
            "--runtime", str(runtime), "--replay", str(replay), "--corpus", record.corpus,
            "--profile", record.profile, "--regime", record.regime,
            "--source-repository", str(record.workload_repo), "--source-commit", record.workload_commit,
            "--source-file", str(record.source_file), "--build-root", str(record.build_root),
            "--local-object", str(runtime / "out/local-measured.o"),
            "--remote-object", str(runtime / "out/remote-measured.o")]
    for binary in binaries:
        args += ["--binary", binary]
    args += ["--out", str(normalized)]
    if record.regime == "warm":
        args += ["--prewarm-input", str(runtime / "s7-prewarm-preprocessed.ii"),
                 "--prewarm-c-trace", str(runtime / "s7-prewarm-c-action-trace.jsonl"),
                 "--prewarm-f-trace", str(runtime / "s7-prewarm-f-action-trace.jsonl")]
    lines = ["set -eu",
             "if test -f " + _q(root / "finalizer-complete.json") + "; then exit 0; fi",
             "if test ! -f " + _q(root / "replay-complete.json") + "; then",
             "  echo 'finalizer: root replay phase is incomplete' >&2; exit 1",
             "fi",
             "if test -e " + _q(normalized) + " || test -e " + _q(package) + " || test -e " + _q(metric) + " || test -e " + _q(driver) + "; then",
             "  echo 'finalizer: partial host output exists without completion marker' >&2; exit 1",
             "fi",
             "export PYTHONPATH=" + _q(py)]
    manifest_path = root / "capture-manifest.json"
    lines.extend(["# Authenticate root's canonical capture hand-off before any host stage.",
                  "python3 - <<'PY'", "import hashlib, json, os, stat",
                  f"manifest_path = {str(manifest_path)!r}", f"cell = {record.cell!r}",
                  f"leaf = {record.runtime_leaf!r}", f"run_sha = {record.run_stdout_sha256!r}",
                  f"inspect_sha = {record.inspect_sha256!r}", f"names = {list(record.capture_names)!r}",
                  f"expected_binaries = {binary_facts!r}",
                  "raw = open(manifest_path, 'rb').read(); value = json.loads(raw)",
                  "if raw != (json.dumps(value, sort_keys=True, separators=(',', ':')) + '\\n').encode(): raise SystemExit('capture manifest: noncanonical')",
                  "if value.get('schema') != 'icecream-s8-heldout-capture-manifest-v1' or value.get('cell') != cell or value.get('runtime_leaf') != leaf or value.get('run_stdout_sha256') != run_sha or value.get('docker_inspect_sha256') != inspect_sha: raise SystemExit('capture manifest: identity mismatch')",
                  "if value.get('binaries') != expected_binaries or set(value.get('captures', {})) != set(names): raise SystemExit('capture manifest: descriptor set mismatch')",
                  "def actual(path):",
                  "    info = os.lstat(path)",
                  "    if stat.S_ISLNK(info.st_mode) or not stat.S_ISREG(info.st_mode) or info.st_nlink != 1: raise SystemExit('capture manifest: destination is not private')",
                  "    raw = open(path, 'rb').read(); return {'path': path, 'bytes': len(raw), 'sha256': hashlib.sha256(raw).hexdigest()}",
                  "for name in names:",
                  "    item = value['captures'][name]; source, destination = item.get('source'), item.get('destination')",
                  "    expected_path = " + repr(str(runtime)) + " + '/' + name",
                  "    expected_source = '/run/' + leaf + '/' + name",
                  "    if not isinstance(source, dict) or not isinstance(destination, dict) or source.get('path') != expected_source: raise SystemExit('capture manifest: source identity')",
                  "    if destination != actual(expected_path): raise SystemExit('capture manifest: destination mutation')",
                  "    if source.get('bytes') != destination.get('bytes') or source.get('sha256') != destination.get('sha256'): raise SystemExit('capture manifest: source mismatch')",
                  "PY",
                  ])
    lines.extend([
             "# Re-authenticate every product executable immediately before normalization.",
             *[_shell_verify(path, binary_facts[name]) for name, path in (
                 ("client", record.build_root / "client/icecc"),
                 ("daemon", record.build_root / "daemon/iceccd"),
                 ("scheduler", record.build_root / "scheduler/icecc-scheduler"),
                 ("cache-service", record.build_root / "cache/icecc-cache-service"),
                 ("simulator", simulator_path))],
             " ".join(_q(arg) for arg in args),
             " ".join([_q("python3"), _q(py / "s8_retained_s7_package.py"),
                       _q("--s7-package"), _q(normalized), _q("--replay"), _q(replay),
                       _q("--source-repository"), _q(record.workload_repo), _q("--out"), _q(package)]),
             " ".join([_q("python3"), _q(py / "s8_live_metric_producer.py"),
                       _q("--live-package"), _q(package), _q("--out"), _q(metric)]),
             " ".join([_q("python3"), _q(py / "s8_first_triple_driver.py"),
                       _q("--manifest"), _q(package / "predictive-manifest.json"),
                       _q("--live-package"), _q(package), _q("--live-manifest"),
                       _q(metric / "live-curve-manifest.json"), _q("--experiments"), _q(driver),
                       _q("--corpus"), _q(record.corpus), _q("--profile"), _q(record.profile),
                       _q("--regime"), _q(record.regime), _q("--calibration-manifest"), _q(calibration)]),
             "printf '%s\\n' '{\"schema\":\"icecream-s8-heldout-finalizer-cell-v1\",\"status\":\"PASS\"}' > " + _q(root / "finalizer-complete.json")])
    return "\n".join(lines)


def build_plan(records: list[RunRecord], output_root: Path, calibration: Path,
               image: str = PINNED_IMAGE, simulator_path: Path | None = None,
               simulator_sha256: str | None = None, cell: str | None = None) -> dict[str, Any]:
    output_root = output_root.resolve()
    if simulator_path is None or simulator_sha256 is None:
        raise FinalizerError("simulator:explicit_binary_and_sha256_required")
    simulator_path = simulator_path.resolve()
    try:
        simulator_path.relative_to(records[0].build_root.resolve())
    except (IndexError, ValueError) as exc:
        raise FinalizerError("simulator:outside_build_root") from exc
    simulator_raw = _private_file(simulator_path, "simulator")
    if hashlib.sha256(simulator_raw).hexdigest() != simulator_sha256.lower():
        raise FinalizerError("simulator:sha256_mismatch")
    calibration_sha, calibration_bundle, calibration_bundle_bytes = _calibration_facts(calibration.resolve())
    protected = {calibration.resolve().parent, simulator_path}
    for record in records:
        protected.update({record.run_dir, record.timestamp_dir, record.runtime_root,
                          record.source_repo, record.workload_repo, record.build_root})
    if any(_overlap(output_root, path) for path in protected):
        raise FinalizerError("output_root:overlaps_protected_input")
    binary_facts = _binary_facts(records[0].build_root.resolve(), simulator_path,
                                 simulator_sha256)
    if cell is not None:
        records = [record for record in records if record.cell == cell]
        if not records:
            raise FinalizerError(f"cell:not_found:{cell}")
    output_root.mkdir(parents=True, exist_ok=True)
    cells: list[dict[str, Any]] = []
    commands: list[str] = []
    host_commands: list[str] = []
    for record in records:
        cell_root = output_root / _safe_name(record.cell)
        # Own the per-cell mountpoint before Docker/root enters it.  The
        # replay directory itself remains absent until s7_warm_replay creates
        # it, preserving that tool's fresh-output contract.
        cell_root.mkdir(parents=True, exist_ok=True)
        command = docker_command(record, cell_root, simulator_path, simulator_sha256, binary_facts)
        commands.append(command)
        host_commands.append(_host_pipeline_script(record, cell_root, calibration.resolve(),
                                                   simulator_path, simulator_sha256, binary_facts))
        cells.append({
            "cell": record.cell, "corpus": record.corpus, "profile": record.profile,
            "regime": record.regime, "run_directory": str(record.run_dir.resolve()),
            "timestamp_directory": str(record.timestamp_dir.resolve()),
            "runtime_leaf": record.runtime_leaf, "runtime_root": str(record.runtime_root.resolve()),
            "source_repository": str(record.source_repo), "workload_repository": str(record.workload_repo),
            "build_root": str(record.build_root), "source_commit": record.source_commit,
            "workload_commit": record.workload_commit,
            "source_root": str(record.source_root), "source_relative": record.source_relative,
            "source_file": str(record.source_file), "compile_database": str(record.compile_db),
            "run_stdout_sha256": record.run_stdout_sha256, "docker_inspect_sha256": record.inspect_sha256,
            "simulator": {"path": str(simulator_path), "sha256": simulator_sha256},
            "binaries": binary_facts,
            "capture_names": list(record.capture_names),
            "source_descriptor": record.source_fact,
            "compile_database_descriptor": record.compile_db_fact,
            "output_root": str(cell_root), "status": "PENDING_ROOT_CONTAINER",
        })
    plan = {"schema": SCHEMA, "status": "PASS", "image": image,
            "calibration_manifest": {"path": str(calibration.resolve()), "sha256": calibration_sha,
                                     "bundle": calibration_bundle, "bundle_bytes": calibration_bundle_bytes},
            "expected_cells": len(records), "cells": cells,
            "commands": commands, "host_commands": host_commands, "execution": {
                "mode": "root-replay-then-host-finalize", "idempotent_marker": "finalizer-complete.json",
                "object_check": "cmp_before_normalization", "ownership": "retained_evidence_unchanged",
            }}
    raw = (json.dumps(plan, sort_keys=True, separators=(",", ":")) + "\n").encode()
    plan_path = output_root / "plan.json"
    if plan_path.exists():
        if _private_file(plan_path, "existing_plan") != raw:
            raise FinalizerError("output_root:existing_plan_mismatch")
    else:
        plan_path.write_bytes(raw)
    commands_path = output_root / "run-root.sh"
    script = "#!/bin/sh\nset -eu\n" + "\n".join(commands) + "\n"
    if commands_path.exists():
        if _private_file(commands_path, "existing_commands") != script.encode():
            raise FinalizerError("output_root:existing_commands_mismatch")
    else:
        commands_path.write_bytes(script.encode())
        commands_path.chmod(0o755)
    host_path = output_root / "run-host.sh"
    host_script = "#!/bin/sh\nset -eu\n" + "\n".join(host_commands) + "\n"
    if host_path.exists():
        if _private_file(host_path, "existing_host_commands") != host_script.encode():
            raise FinalizerError("output_root:existing_host_commands_mismatch")
    else:
        host_path.write_bytes(host_script.encode())
        host_path.chmod(0o755)
    return plan


def main(argv: list[str] | None = None) -> int:
    def mapping(value: str, label: str) -> tuple[str, str]:
        if "=" not in value:
            raise FinalizerError(f"{label}:expected_NAME=VALUE")
        name, item = value.split("=", 1)
        if not name or not item:
            raise FinalizerError(f"{label}:expected_NAME=VALUE")
        return name, item

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runs-root", type=Path, required=True)
    parser.add_argument("--source-repository", type=Path, required=True)
    parser.add_argument("--build-root", type=Path, required=True)
    parser.add_argument("--calibration-manifest", type=Path, required=True)
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--workload-repository", action="append", default=[], metavar="CORPUS=PATH",
                        help="explicit DuckDB/LLVM workload git repository mapping")
    parser.add_argument("--workload-commit", action="append", default=[], metavar="CORPUS=SHA")
    parser.add_argument("--simulator-binary", type=Path, required=True,
                        help="exact product simulator binary from --build-root")
    parser.add_argument("--simulator-sha256", required=True,
                        help="sha256 of the exact simulator binary; no inference is performed")
    parser.add_argument("--cell", help="process one declared cell (useful for a bounded canary)")
    parser.add_argument("--image", default=PINNED_IMAGE)
    parser.add_argument("--dry-run", action="store_true",
                        help="validate metadata and emit root-in-image commands without executing them")
    args = parser.parse_args(argv)
    try:
        if args.image != PINNED_IMAGE:
            raise FinalizerError("docker_image:not_pinned")
        repositories = {name: Path(value).absolute()
                        for name, value in (mapping(item, "workload_repository")
                                             for item in args.workload_repository)}
        commits = dict(mapping(item, "workload_commit") for item in args.workload_commit)
        records = discover_runs(args.runs_root.absolute(), args.source_repository.absolute(),
                                args.build_root.absolute(), args.image,
                                workload_repositories=repositories, workload_commits=commits)
        plan = build_plan(records, args.output_root.absolute(), args.calibration_manifest.absolute(), args.image,
                          args.simulator_binary.absolute(), args.simulator_sha256.lower(), args.cell)
        print(json.dumps({"status": "PASS", "plan": str(args.output_root.absolute() / "plan.json"),
                          "cells": len(plan["cells"]), "dry_run": args.dry_run}, sort_keys=True))
    except (FinalizerError, OSError, ValueError) as exc:
        print(f"s8_heldout_finalizer: {exc}", file=sys.stderr)
        return 77
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
