#!/usr/bin/env python3
"""Execute and retain the S8 profile/regime/topology campaign.

The depth planner and multi-TU producer remain the authorities for planning and
simulation.  This module only supplies campaign orchestration: it creates one
immutable, second-granularity campaign directory, records the exact argv for
each cell, runs the predictive commands, and leaves authenticated commands for
the live and comparison stages.  A failed cell is never presented as a pass;
resume creates a new attempt directory and keeps every earlier attempt.
"""

from __future__ import annotations

import argparse
import contextlib
import fcntl
import hashlib
import json
import os
import re
import shlex
import stat
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Callable

try:  # package invocation
    from .s8_schema import CORPORA, PROFILES, REGIMES, SPLITS
except ImportError:  # direct invocation
    from s8_schema import CORPORA, PROFILES, REGIMES, SPLITS


SCHEMA = "icecream-s8-campaign-driver-v3"
CELL_SCHEMA = "icecream-s8-campaign-cell-v3"
SUMMARY_SCHEMA = "icecream-s8-campaign-summary-v3"
DEPTHS = ("100", "200", "full")
TOPOLOGIES = ("C1F1/100000", "C1F20/40")
TOPOLOGY_ARGS = {"C1F1/100000": "C1F1", "C1F20/40": "C1F20"}
CAMPAIGN_STAMP = "%Y%m%dT%H%M%SZ"
STAMP_RE = re.compile(r"^\d{8}T\d{6}Z$")
IMAGE_ID_RE = re.compile(r"^sha256:[0-9a-f]{64}$")
PINNED_IMAGE = "icecream/farm-node:ubuntu22-gcc11-boost174"
OOM_SCORE_ADJ = -1000
# One host-wide inode is shared by every campaign and every protected
# supervisor.  It is deliberately independent of a campaign/temp namespace.
LIVE_LOCK_PATH = Path("/tmp/icecream-s8-live-run.lock")


class CampaignError(ValueError):
    """A campaign declaration, execution, or retained artifact is invalid."""


def canonical(value: object) -> bytes:
    return (json.dumps(value, sort_keys=True, separators=(",", ":"),
                       ensure_ascii=True, allow_nan=False) + "\n").encode("ascii")


def _stamp() -> str:
    return datetime.now(timezone.utc).strftime(CAMPAIGN_STAMP)


def _safe(value: str) -> str:
    return "".join(ch if ch.isalnum() or ch in "_.-" else "-" for ch in value)


def _cell_id(cell: dict[str, str]) -> str:
    return "/".join((cell["corpus"], cell["profile"], cell["regime"], cell["topology"]))


def _slug(cell: dict[str, str]) -> str:
    return _safe("-".join((cell["corpus"], cell["profile"], cell["regime"], cell["topology"])))


def _validate_cell(cell: dict[str, str]) -> None:
    if (set(cell) != {"corpus", "profile", "regime", "topology"} or
            cell["corpus"] not in CORPORA or cell["profile"] not in PROFILES or
            cell["regime"] not in REGIMES or cell["topology"] not in TOPOLOGIES):
        raise CampaignError("cell:undeclared")


def cells(corpus: str) -> list[dict[str, str]]:
    if corpus not in CORPORA:
        raise CampaignError("corpus:undeclared")
    return [{"corpus": corpus, "profile": profile, "regime": regime,
             "topology": topology}
            for profile in PROFILES for regime in REGIMES for topology in TOPOLOGIES]


def _format_path(spec: str | Path, cell: dict[str, str]) -> Path:
    values = {**cell, "topology_arg": TOPOLOGY_ARGS[cell["topology"]],
              "cell": _cell_id(cell)}
    try:
        return Path(str(spec).format(**values)).expanduser().absolute()
    except (KeyError, ValueError) as exc:
        raise CampaignError(f"path_template:invalid:{spec}") from exc


def _write_new(path: Path, raw: bytes) -> None:
    if path.exists() or path.is_symlink():
        raise CampaignError(f"output:already_exists:{path}")
    path.parent.mkdir(parents=True, exist_ok=True)
    try:
        with path.open("xb") as stream:
            stream.write(raw)
            stream.flush()
            os.fsync(stream.fileno())
    except OSError as exc:
        raise CampaignError(f"output:write_failed:{path}") from exc


def _replace_json(path: Path, value: object) -> None:
    """Atomically update mutable campaign state without touching old attempts."""
    temporary = path.with_name(f".{path.name}.{os.getpid()}.tmp")
    try:
        with temporary.open("xb") as stream:
            stream.write(canonical(value))
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    except OSError as exc:
        try:
            temporary.unlink()
        except OSError:
            pass
        raise CampaignError(f"state:write_failed:{path}") from exc


def _sha(path: Path) -> dict[str, object]:
    try:
        info = path.lstat()
        if stat.S_ISLNK(info.st_mode) or not stat.S_ISREG(info.st_mode) or info.st_nlink != 1:
            raise CampaignError(f"artifact:not_private_regular_file:{path}")
        raw = path.read_bytes()
    except OSError as exc:
        raise CampaignError(f"artifact:unavailable:{path}") from exc
    return {"path": str(path), "bytes": len(raw), "sha256": hashlib.sha256(raw).hexdigest()}


def _artifact_tree(root: Path) -> list[dict[str, object]]:
    result = []
    if not root.is_dir() or root.is_symlink():
        raise CampaignError(f"artifact:result_directory_invalid:{root}")
    for path in sorted(root.rglob("*")):
        if path.is_file() and not path.is_symlink():
            item = _sha(path)
            item["path"] = str(path.relative_to(root))
            result.append(item)
    return result


def _git_identity(repo: Path) -> dict[str, object]:
    try:
        head = subprocess.check_output(["git", "-C", str(repo), "rev-parse", "HEAD"],
                                       text=True, stderr=subprocess.DEVNULL).strip()
        tree = subprocess.check_output(["git", "-C", str(repo), "rev-parse", "HEAD^{tree}"],
                                       text=True, stderr=subprocess.DEVNULL).strip()
        tracked_diff = subprocess.check_output(
            ["git", "-C", str(repo), "diff", "--binary", "--no-ext-diff", "HEAD", "--"],
            stderr=subprocess.DEVNULL)
    except (OSError, subprocess.SubprocessError) as exc:
        raise CampaignError("git:identity_unavailable") from exc
    return {
        "root": str(repo), "head": head, "tree": tree,
        "tracked_clean": not tracked_diff,
        "tracked_diff_sha256": hashlib.sha256(tracked_diff).hexdigest(),
    }


def _private_file(path: Path, label: str) -> dict[str, object]:
    """Return a stable descriptor for an authority file."""
    try:
        info = path.lstat()
    except OSError as exc:
        raise CampaignError(f"{label}:unavailable") from exc
    if (path.is_symlink() or not stat.S_ISREG(info.st_mode) or
            info.st_nlink != 1):
        raise CampaignError(f"{label}:not_private_regular_file")
    return _sha(path)


def _validate_simulator_authority(path: Path, repo: Path) -> dict[str, object]:
    """Authenticate the exact simulator build receipt for this checkout."""
    facts = _private_file(path, "simulator_authority")
    try:
        value = json.loads(path.read_text())
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise CampaignError("simulator_authority:invalid_json") from exc
    if (not isinstance(value, dict) or
            set(value) != {"schema", "source", "binary", "inputs", "configuration"} or
            value.get("schema") != "icecream-p50sim-build-v1"):
        raise CampaignError("simulator_authority:schema_invalid")
    current = _git_identity(repo)
    if current.get("tracked_clean") is not True:
        raise CampaignError("git:tracked_worktree_dirty")
    source = value.get("source")
    if (not isinstance(source, dict) or
            set(source) != {"root", "head", "tree", "tracked_clean"} or
            source.get("root") != str(repo) or source.get("head") != current["head"] or
            source.get("tree") != current["tree"] or source.get("tracked_clean") is not True):
        raise CampaignError("simulator_authority:source_mismatch")
    binary = value.get("binary")
    if (not isinstance(binary, dict) or set(binary) != {"path", "sha256", "bytes"} or
            not isinstance(binary.get("path"), str) or not Path(binary["path"]).is_absolute()):
        raise CampaignError("simulator_authority:binary_descriptor_invalid")
    binary_path = Path(binary["path"])
    observed = _private_file(binary_path, "simulator_authority_binary")
    if (observed["path"] != str(binary_path.resolve()) or
            observed["sha256"] != str(binary.get("sha256", "")).lower() or
            observed["bytes"] != binary.get("bytes")):
        raise CampaignError("simulator_authority:binary_mismatch")
    return {"path": str(path.resolve()), "sha256": facts["sha256"],
            "bytes": facts["bytes"], "binary": observed,
            "source": {"head": current["head"], "tree": current["tree"]}}


def _inspect_container_image(image: str, expected_image_id: str) -> dict[str, str]:
    """Read the pinned image identity without changing the image store."""
    try:
        from . import s8_real_c1f1_live_runner as live_runner
    except ImportError:  # direct invocation
        import s8_real_c1f1_live_runner as live_runner
    try:
        return live_runner.container_image_identity(image, expected_image_id)
    except live_runner.LiveRunnerError as exc:
        raise CampaignError(f"live_preflight:container_image:{exc}") from exc


def _validate_live_prerequisites(*, repo: Path, product_root: Path,
                                 matrix_audit: Path,
                                 compile_db: Path | None,
                                 compile_source_root: Path | None,
                                 compile_output_root: Path | None,
                                 container_image: str | None,
                                 container_image_id: str | None,
                                 container_temp_root: Path | None,
                                 simulator_authority: Path | None,
                                 image_inspector: Callable[[str, str], dict[str, str]] | None,
                                 corpus: str) -> dict[str, object]:
    """Fail before the first predictive command when all-mode is not runnable."""
    if not isinstance(container_image, str) or not container_image.strip():
        raise CampaignError("live_preflight:container_image_reference_required")
    if (not isinstance(container_image_id, str) or
            IMAGE_ID_RE.fullmatch(container_image_id.lower()) is None):
        raise CampaignError("live_preflight:container_image_content_id_required")
    for path, label in ((compile_db, "compile_db"),
                        (compile_source_root, "compile_source_root"),
                        (compile_output_root, "compile_output_root"),
                        (container_temp_root, "container_temp_root")):
        if path is None or not path.is_absolute():
            raise CampaignError(f"live_preflight:{label}_required")
        if path.is_symlink() or not path.exists():
            raise CampaignError(f"live_preflight:{label}_unavailable")
        if label == "compile_db":
            _private_file(path, label)
        elif not path.is_dir():
            raise CampaignError(f"live_preflight:{label}_directory_required")
    _private_file(matrix_audit, "matrix_audit")
    if simulator_authority is None:
        raise CampaignError("live_preflight:simulator_authority_required")
    if product_root.is_symlink() or not product_root.is_dir():
        raise CampaignError("live_preflight:product_root_unavailable")
    for relative in ("scheduler/icecc-scheduler", "daemon/iceccd", "client/icecc",
                     "cache/icecc-cache-service"):
        executable = product_root / relative
        if not executable.is_file() or executable.is_symlink() or not os.access(executable, os.X_OK):
            raise CampaignError(f"live_preflight:product_binary_unavailable:{relative}")
    # The product itself is checked again by the pinned runner immediately
    # before launch.  Checking its Git state here closes the work-before-fail
    # gap for all-mode campaigns.
    repo_identity = _git_identity(repo)
    product_identity = _git_identity(product_root)
    if repo_identity.get("tracked_clean") is not True:
        raise CampaignError("live_preflight:repo_tracked_worktree_dirty")
    if product_identity.get("tracked_clean") is not True:
        raise CampaignError("live_preflight:product_tracked_worktree_dirty")
    simulator = _validate_simulator_authority(simulator_authority, repo)
    image = (image_inspector or _inspect_container_image)(container_image,
                                                          container_image_id.lower())
    if (not isinstance(image, dict) or image.get("reference") != container_image or
            image.get("image_id") != container_image_id.lower()):
        raise CampaignError("live_preflight:container_image_identity_mismatch")
    return {"container_image": image,
            "simulator_authority": simulator,
            "container_temp_root": str(container_temp_root),
            "matrix_audit": str(matrix_audit)}


def _process_ancestors(pid: int | None = None) -> set[int]:
    result: set[int] = set()
    current = os.getpid() if pid is None else pid
    while current > 1 and current not in result:
        result.add(current)
        try:
            status = Path(f"/proc/{current}/status").read_text()
            parent_line = next(line for line in status.splitlines()
                               if line.startswith("PPid:"))
            current = int(parent_line.split()[1])
        except (OSError, StopIteration, ValueError, IndexError):
            break
    return result


def _competing_processes(*, proc_root: Path = Path("/proc"),
                         ancestor_pid: int | None = None) -> list[str]:
    """Find unrelated S8/build/Docker processes before a measurement."""
    excluded = _process_ancestors(ancestor_pid)
    # dockerd/containerd and Docker plumbing are normal host infrastructure,
    # not competing measurements. A docker CLI command is interesting only
    # when its arguments identify an actual S8/build/tool workload.
    infrastructure = {"dockerd", "containerd", "docker-proxy", "docker-init",
                      "containerd-shim", "containerd-shim-runc-v2"}
    workload_names = {"s8_campaign_driver.py", "s8_real_c1f1_live_runner.py",
                      "p50compile", "p50compilee2e-run.sh", "icecc",
                      "iceccd", "icecc-scheduler", "cmake", "ninja", "make",
                      "gcc", "g++", "clang", "clang++", "cc", "c++", "ld",
                      "ar", "ccache", "build", "build.py", "compile", "compile.py"}
    found: list[str] = []
    try:
        entries = list(proc_root.iterdir())
    except OSError:
        return ["/proc:unavailable"]
    for entry in entries:
        if not entry.name.isdigit() or int(entry.name) in excluded:
            continue
        try:
            raw = (entry / "cmdline").read_bytes()
        except OSError:
            continue
        command = raw.replace(b"\0", b" ").decode("utf-8", "replace").strip()
        if not command:
            continue
        words = command.split()
        executable = Path(words[0]).name.lower() if words else ""
        names = {Path(word).name.lower() for word in words}
        # Daemons are ignored by executable name. Docker's client remains
        # eligible only if its command carries an actual workload name.
        if executable in infrastructure:
            continue
        if names & workload_names:
            found.append(f"{entry.name}:{command}")
    found.sort()
    return found


def _stale_p50_containers() -> list[str]:
    """Return stopped or running private S8 container names."""
    try:
        completed = subprocess.run(
            ["docker", "container", "ls", "--all", "--filter", "name=^p50-s8-",
             "--format", "{{.Names}}"], check=False, capture_output=True,
            text=True, timeout=10)
    except (OSError, subprocess.SubprocessError):
        return ["docker:probe_failed"]
    if completed.returncode != 0:
        return ["docker:probe_failed"]
    return [line.strip() for line in completed.stdout.splitlines()
            if line.strip().startswith("p50-s8-")]


def _proc_cpu_stat() -> tuple[int, ...]:
    try:
        line = next(line for line in Path("/proc/stat").read_text().splitlines()
                    if line.startswith("cpu "))
        fields = [int(value) for value in line.split()[1:]
                  if value.isdigit()]
    except (OSError, StopIteration, ValueError):
        return ()
    return tuple(fields[:8])


def _host_is_idle(*, process_scanner: Callable[[], list[str]] = _competing_processes,
                  container_scanner: Callable[[], list[str]] = _stale_p50_containers,
                  stat_reader: Callable[[], tuple[int, ...]] = _proc_cpu_stat,
                  sleep_fn: Callable[[float], None] = time.sleep,
                  sample_seconds: float = 0.1) -> bool:
    """Apply process, stale-container, and short CPU idle gates."""
    if process_scanner() or container_scanner():
        return False
    first = stat_reader()
    if len(first) < 5:
        return False
    sleep_fn(sample_seconds)
    second = stat_reader()
    if len(second) < 5:
        return False
    total = sum(b - a for a, b in zip(first, second))
    if total <= 0:
        return False
    idle = (second[3] - first[3])
    iowait = (second[4] - first[4])
    return idle / total >= 0.80 and iowait / total <= 0.05


@contextlib.contextmanager
def _live_run_lock(_temp_root: Path | None = None):
    """Own the live measurement slot for exactly one cell."""
    lock_path = LIVE_LOCK_PATH
    lock_path.parent.mkdir(parents=True, exist_ok=True)
    with lock_path.open("a+") as stream:
        try:
            fcntl.flock(stream.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as exc:
            raise CampaignError("live_preflight:single_live_run_owned") from exc
        try:
            yield
        finally:
            fcntl.flock(stream.fileno(), fcntl.LOCK_UN)


def _verify_oom_protection() -> None:
    """Require the executor itself to be protected before a live run."""
    path = Path("/proc/self/oom_score_adj")
    try:
        observed = int(path.read_text().strip())
    except (OSError, ValueError) as exc:
        raise CampaignError("live_preflight:oom_protection_unavailable") from exc
    if observed != OOM_SCORE_ADJ:
        try:
            path.write_text(str(OOM_SCORE_ADJ))
            observed = int(path.read_text().strip())
        except (OSError, ValueError) as exc:
            raise CampaignError("live_preflight:oom_protection_unavailable") from exc
    if observed != OOM_SCORE_ADJ:
        raise CampaignError("live_preflight:oom_protection_unavailable")


def _command_record(argv: list[str], cwd: Path, *, stage: str, executable: bool = True,
                    reason: str | None = None) -> dict[str, object]:
    value: dict[str, object] = {
        "stage": stage, "argv": [str(item) for item in argv],
        "shell": shlex.join(str(item) for item in argv), "cwd": str(cwd),
        "executable": executable,
    }
    if reason is not None:
        value["reason"] = reason
    value["sha256"] = hashlib.sha256(canonical(value)).hexdigest()
    return value


def _source_commands(cell_dir: Path, cell: dict[str, str], *, depth: str,
                     source_manifest: Path, source_root: Path, matrix_audit: Path,
                     engine_manifest: Path, product_root: Path, python: str,
                     campaign_stamp: str, compile_db: Path | None,
                     compile_source_root: Path | None, compile_output_root: Path | None,
                     repo: Path, container_image: str | None = None,
                     container_image_id: str | None = None,
                     container_temp_root: Path | None = None) -> tuple[list[dict[str, object]], dict[str, object],
                                          dict[str, object], list[dict[str, object]],
                                          list[Path]]:
    attempt = cell_dir / "attempt-001"
    result_dir = attempt / f"s8-{_slug(cell)}-{campaign_stamp}"
    plan = attempt / ("depth-plan-full-1.json" if depth == "full" else "depth-plan.json")
    depth_argv = [python, str((repo / "farmharness/s8_depth_runner.py").absolute()),
                  "--source-manifest", str(source_manifest), "--source-root", str(source_root),
                  "--matrix-audit", str(matrix_audit), "--result-dir", str(result_dir),
                  "--corpus", cell["corpus"], "--profile", cell["profile"],
                  "--regime", cell["regime"], "--depth", depth,
                  "--topology", TOPOLOGY_ARGS[cell["topology"]], "--out", str(plan)]
    producer_argv = [python, str((repo / "farmharness/s8_multitu_predictive_producer.py").absolute()),
                     "--plan", str(plan), "--engine-manifest", str(engine_manifest),
                     "--product-build-root", str(product_root)]
    plan_commands: list[dict[str, object]] = []
    result_dirs = [result_dir]
    # The producer owns the warm lifecycle too: warm full is an authenticated
    # prewarm followed by scored full-1/full-2 in one paired product process.
    paired_full = depth == "full"
    if depth == "full":
        repeat_plan = attempt / "depth-plan-full-2.json"
        repeat_result = attempt / f"s8-{_slug(cell)}-{campaign_stamp}-full-2"
        repeat_argv = [python, str((repo / "farmharness/s8_depth_runner.py").absolute()),
                       "--source-manifest", str(source_manifest), "--source-root", str(source_root),
                       "--matrix-audit", str(matrix_audit), "--result-dir", str(repeat_result),
                       "--corpus", cell["corpus"], "--profile", cell["profile"],
                       "--regime", cell["regime"], "--depth", "repeat-full",
                       "--repeat-of", str(plan), "--topology", TOPOLOGY_ARGS[cell["topology"]],
                       "--out", str(repeat_plan)]
        if paired_full:
            producer_argv.extend(["--repeat-plan", str(repeat_plan)])
            result_dirs.append(repeat_result)
        plan_commands.append(_command_record(repeat_argv, repo, stage="predictive_plan_full_2"))
    if depth != "full" or not paired_full:
        producer_argv.extend(("--output-dir", str(result_dir)))
    plan_commands.insert(0, _command_record(depth_argv, repo, stage="predictive_plan" if depth != "full" else "predictive_plan_full_1"))
    producer_command = _command_record(producer_argv, repo, stage="predictive_producer")

    # These are deliberately staged, not guessed.  A live run requires a
    # compile database and a retained source checkout that are not predictive
    # inputs.  Keeping their exact argv makes the later handoff auditable.
    prep_dir = attempt / "live-prep"
    compile_db_arg = str(compile_db) if compile_db else "<compile-db-required>"
    compile_src_arg = str(compile_source_root) if compile_source_root else "<compile-source-root-required>"
    compile_out_arg = str(compile_output_root) if compile_output_root else str(attempt / "compile-output")
    prep_argv = [python, str((repo / "farmharness/s8_live_batch_prep.py").absolute(),),
                 "--predictive-plan", str(plan), "--compile-db", compile_db_arg,
                 "--compile-source-root", compile_src_arg,
                 "--compile-output-root", compile_out_arg, "--output", str(prep_dir)]
    live_out = attempt / "live-output"
    live_target = (live_out / "icecream" / cell["topology"].replace("/", "-") /
                   campaign_stamp / cell["profile"])
    live_argv = [python, str((repo / "farmharness/s8_real_c1f1_live_runner.py").absolute()),
                 "--batch-manifest", str(prep_dir / "batch-manifest.jsonl"),
                 "--predictive-plan", str(plan), "--topology", str(prep_dir / "topology.json"),
                 "--suite", cell["topology"], "--profile", cell["profile"],
                 "--product-root", str(product_root), "--corpus", cell["corpus"],
                 "--regime", cell["regime"], "--depth", depth, "--passes", "2" if depth == "full" else "1",
                 "--output", str(live_out), "--timestamp", campaign_stamp,
                 "--execute"]
    if container_image is not None:
        live_argv.extend(("--container-image", container_image))
    if container_image_id is not None:
        live_argv.extend(("--container-image-id", container_image_id))
    if container_temp_root is not None:
        live_argv.extend(("--container-temp-root", str(container_temp_root)))
    if depth == "full":
        live_argv.extend(("--repeat-predictive-plan", str(repeat_plan)))
    comparison_specs = [("comparison", result_dir / "predictive_curve_manifest.json",
                         live_target / "live_curve_manifest.json", attempt / "records.jsonl")]
    if depth == "full":
        comparison_specs = [
            ("comparison_full_1", result_dir / "predictive_curve_manifest.json",
             live_target / "live_curve_manifest_full-1.json", attempt / "records-full-1.jsonl"),
            ("comparison_full_2", repeat_result / "predictive_curve_manifest.json",
             live_target / "live_curve_manifest_full-2.json", attempt / "records-full-2.jsonl"),
        ]
    missing = []
    if compile_db is None:
        missing.append("compile-db")
    if compile_source_root is None:
        missing.append("compile-source-root")
    live_executable = not missing and container_image_id is not None and container_temp_root is not None
    reason = "live inputs not configured: " + ", ".join(missing) if missing else "live execution staged by request"
    comparison_commands = []
    for stage, predictive_manifest, live_manifest, records in comparison_specs:
        comparison_argv = [
            python, str((repo / "farmharness/s8_predictive_live_normalizer.py").absolute()),
            "--predictive-manifest", str(predictive_manifest),
            "--live-manifest", str(live_manifest), "--out", str(records),
        ]
        comparison_commands.append(_command_record(
            comparison_argv, repo, stage=stage, executable=live_executable,
            reason=None if live_executable else "requires authenticated live curve"))
    return (plan_commands, producer_command,
            {"prepare": _command_record(prep_argv, repo, stage="live_prepare",
                                         executable=live_executable,
                                         reason=None if live_executable else reason),
             "run": _command_record(live_argv, repo, stage="live_run",
                                     executable=live_executable,
                                     reason=None if live_executable else reason)},
            comparison_commands, result_dirs)


def _run_command(command: dict[str, object], cwd: Path, stdout: Path, stderr: Path) -> int:
    argv = [str(item) for item in command["argv"]]  # type: ignore[index]
    stdout.parent.mkdir(parents=True, exist_ok=True)
    with stdout.open("ab") as out, stderr.open("ab") as err:
        try:
            completed = subprocess.run(argv, cwd=str(cwd), stdout=out, stderr=err,
                                       check=False, timeout=6 * 60 * 60)
        except subprocess.TimeoutExpired:
            err.write(b"campaign driver: command timed out after 21600 seconds\n")
            return 124
        except OSError as exc:
            err.write((f"campaign driver: unable to execute: {exc}\n").encode())
            return 127
    return int(completed.returncode)


def _load_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text())
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise CampaignError(f"state:invalid_json:{path}") from exc
    if not isinstance(value, dict):
        raise CampaignError(f"state:object_required:{path}")
    return value


def _summary(campaign: Path, cell_records: list[dict[str, Any]], config: dict[str, object]) -> dict[str, object]:
    by_cell = {str(row.get("cell_id", row.get("cell"))): row for row in cell_records}
    complete_records: list[dict[str, Any]] = []
    for cell in cells(str(config["corpus"])):
        complete_records.append(by_cell.get(_cell_id(cell),
                                            {"cell": cell, "cell_id": _cell_id(cell),
                                             "status": "PENDING"}))
    statuses = {status: sum(1 for row in complete_records if row.get("status") == status)
                for status in ("PENDING", "RUNNING", "PASS", "FAIL", "INTERRUPTED", "STAGED")}
    if statuses["FAIL"] or statuses["INTERRUPTED"]:
        status = "PARTIAL_FAILURE"
    elif statuses["PENDING"] or statuses["RUNNING"]:
        status = "IN_PROGRESS"
    elif statuses["STAGED"]:
        status = "PARTIAL_STAGED"
    else:
        status = "PASS"
    return {"schema": SUMMARY_SCHEMA, "status": status, "campaign_root": str(campaign),
            "expected_cells": len(complete_records), "counts": statuses,
            "cells": [{"cell": row["cell"], "status": row["status"],
                       "attempt": row.get("attempt"), "result": row.get("result"),
                       "error": row.get("error")} for row in complete_records],
            "config_sha256": hashlib.sha256(canonical(config)).hexdigest()}


def _new_campaign_root(output_root: Path, stamp: str | None = None) -> Path:
    output_root = output_root.absolute()
    if output_root.exists() and (output_root.is_symlink() or not output_root.is_dir()):
        raise CampaignError("campaign_root:not_directory")
    output_root.mkdir(parents=True, exist_ok=True)
    stamp = stamp or _stamp()
    if STAMP_RE.fullmatch(stamp) is None:
        raise CampaignError("timestamp:must_be_second_granularity")
    for suffix in range(1000):
        name = f"s8-campaign-{stamp}" + (f"-{suffix:02d}" if suffix else "")
        candidate = output_root / name
        try:
            candidate.mkdir()
            (candidate / "cells").mkdir()
            return candidate
        except FileExistsError:
            continue
    raise CampaignError("campaign_root:timestamp_collisions_exhausted")


def _attempt(cell_dir: Path) -> int:
    numbers = []
    for path in cell_dir.glob("attempt-*"):
        try:
            numbers.append(int(path.name.split("-", 1)[1]))
        except (ValueError, IndexError):
            continue
    return max(numbers, default=0) + 1


def _cell_state(path: Path) -> dict[str, Any]:
    if not path.exists():
        return {"status": "PENDING", "history": []}
    value = _load_json(path)
    if value.get("schema") != CELL_SCHEMA:
        raise CampaignError(f"cell_state:schema_invalid:{path}")
    return value


def _authenticate_comparison(
        path: Path, *, expected_cell: tuple[str, str, str] | None = None,
        experiment_identity: dict[str, str] | None = None) -> dict[str, object]:
    """Authenticate and summarize one immutable per-experiment comparison.

    The campaign result is the operator-facing per-experiment artifact.  A
    path/hash/point-count tuple was insufficient because it did not bind the
    comparison to the campaign cell or expose the final predicted/observed
    transfer and elapsed values.  When ``expected_cell`` is supplied, reuse
    the canonical matrix-auditor checks so malformed or mutated point errors
    cannot be promoted to a PASS.
    """
    descriptor = _private_file(path, "comparison_output")
    try:
        rows = [json.loads(line) for line in path.read_text().splitlines()
                if line.strip()]
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise CampaignError("comparison_output:invalid_jsonl") from exc
    if len(rows) != 3 or not all(isinstance(row, dict) for row in rows):
        raise CampaignError("comparison_output:record_count_invalid")
    if [row.get("record_type") for row in rows] != ["predictive_sim", "live", "comparison"]:
        raise CampaignError("comparison_output:record_types_invalid")
    comparison = rows[-1]
    if comparison.get("schema") != "icecream-s8-predictive-live-record-v1":
        raise CampaignError("comparison_output:schema_invalid")
    if not isinstance(comparison.get("point_errors"), list) or not isinstance(
            comparison.get("loss_curve"), list):
        raise CampaignError("comparison_output:authenticated_fields_missing")
    result: dict[str, object] = {
        "path": str(path), "bytes": descriptor["bytes"],
        "sha256": descriptor["sha256"], "record_type": "comparison",
        "points": len(comparison["loss_curve"]),
    }
    if expected_cell is None:
        return result

    try:
        from . import s8_matrix_auditor as auditor
    except ImportError:  # direct invocation
        import s8_matrix_auditor as auditor
    try:
        identities = [auditor._record_identity(row, expected_cell,
                                                f"comparison.record.{index}")
                      for index, row in enumerate(rows)]
        if identities[0] != identities[2]:
            raise CampaignError("comparison_output:comparison_identity_mismatch")
        shared = ("corpus", "profile", "regime", "split", "input_digest")
        if any(identities[0][key] != identities[1][key] for key in shared):
            raise CampaignError("comparison_output:source_identity_mismatch")
        units = [auditor._record_units(row, f"comparison.record.{index}")
                 for index, row in enumerate(rows)]
        if units[0] != units[1] or units[0] != units[2]:
            raise CampaignError("comparison_output:units_mismatch")
        curves = auditor._curves(rows, identities, str(path))
    except CampaignError:
        raise
    except (auditor.AuditError, KeyError, TypeError, OverflowError) as exc:
        raise CampaignError(f"comparison_output:invalid_measurements:{exc}") from exc

    # If a producer has begun emitting explicit metadata, it must agree with
    # the campaign declaration.  Older normalizer records omit these fields;
    # the campaign identity below remains explicit and immutable in that
    # compatibility case.
    if experiment_identity is not None:
        for row in rows:
            for key in ("topology", "depth_class", "pass_id"):
                if key in row and row[key] != experiment_identity[key]:
                    raise CampaignError(f"comparison_output:{key}_mismatch")
        predicted_curve = curves["predicted"][0]
        observed_curve = curves["observed"][0]
        predicted_final = predicted_curve[-1]["cumulative"]
        observed_final = observed_curve[-1]["cumulative"]
        final_errors = curves["errors"][-1]["errors"]
        final_loss = curves["losses"][-1]
        def metric_snapshot(value: object) -> dict[str, object]:
            if not isinstance(value, dict):
                raise CampaignError("comparison_output:cumulative_metrics_invalid")
            try:
                return {
                    "transfer_bytes": value["channel_bytes"],
                    "elapsed_ns": value["elapsed_ns"],
                    "C_TO_F_bytes": value["C_TO_F_bytes"],
                    "F_TO_C_bytes": value["F_TO_C_bytes"],
                    "throughput_bytes_per_s": value["throughput_bytes_per_s"],
                }
            except KeyError as exc:
                raise CampaignError(
                    "comparison_output:cumulative_metrics_invalid") from exc
        if not isinstance(predicted_final, dict) or not isinstance(observed_final, dict):
            raise CampaignError("comparison_output:cumulative_metrics_invalid")
        result.update({
            "identity": dict(experiment_identity),
            "predicted": metric_snapshot(predicted_final),
            "observed": metric_snapshot(observed_final),
            "errors": {
                "transfer_bytes": final_errors["cumulative.channel_bytes"],
                "elapsed_ns": final_errors["cumulative.elapsed_ns"],
                "metrics": final_errors,
            },
            "aggregate": {
                "loss_curve_points": len(curves["losses"]),
                "final": final_loss,
                "curve_in_records": True,
            },
        })
    return result


def run_campaign(*, output_root: Path, repo: Path, corpus: str, depth: str,
                 source_manifest: str | Path, source_root: str | Path,
                 matrix_audit: Path, engine_manifest_template: str | Path,
                 product_build_root: Path, python: str | None = None,
                 resume: Path | None = None, retry_failed: bool = False,
                 compile_db: str | Path | None = None,
                 compile_source_root: str | Path | None = None,
                 compile_output_root: str | Path | None = None,
                 execute: bool = True,
                 mode: str = "predictive-only",
                 command_runner: Callable[[dict[str, object], Path, Path, Path], int] | None = None,
                 timestamp: str | None = None,
                 container_image: str | None = PINNED_IMAGE,
                 container_image_id: str | None = None,
                 container_temp_root: Path | None = None,
                 simulator_authority: Path | None = None,
                 idle_host_gate: Callable[[], bool] | None = None,
                 oom_protection: Callable[[], None] | None = None,
                 image_inspector: Callable[[str, str], dict[str, str]] | None = None) -> Path:
    """Run a campaign, or resume it without rewriting prior attempts."""
    if depth not in DEPTHS:
        raise CampaignError("depth:undeclared")
    if mode not in ("predictive-only", "all"):
        raise CampaignError("mode:undeclared")
    if corpus not in CORPORA:
        raise CampaignError("corpus:undeclared")
    if mode == "all" and SPLITS[corpus] == "held_out_validation":
        raise CampaignError("live_preflight:all_mode_is_calibration_only")
    repo = repo.absolute()
    python = python or sys.executable
    source_manifest_spec, source_root_spec = str(source_manifest), str(source_root)
    matrix_audit = matrix_audit.absolute()
    product_build_root = product_build_root.absolute()
    compile_db_path = Path(compile_db).absolute() if compile_db else None
    compile_source_path = Path(compile_source_root).absolute() if compile_source_root else None
    compile_output_path = Path(compile_output_root).absolute() if compile_output_root else None
    container_temp_path = Path(container_temp_root).absolute() if container_temp_root else None
    simulator_authority_path = (Path(simulator_authority).absolute()
                                 if simulator_authority else None)
    live_authority: dict[str, object] | None = None
    if mode == "all" and execute:
        live_authority = _validate_live_prerequisites(
            repo=repo, product_root=product_build_root, matrix_audit=matrix_audit,
            compile_db=compile_db_path, compile_source_root=compile_source_path,
            compile_output_root=compile_output_path, container_image=container_image,
            container_image_id=container_image_id, container_temp_root=container_temp_path,
            simulator_authority=simulator_authority_path,
            image_inspector=image_inspector, corpus=corpus)
        (oom_protection or _verify_oom_protection)()
    config: dict[str, object] = {"corpus": corpus, "depth": depth,
        "mode": mode,
        "source_manifest": source_manifest_spec, "source_root": source_root_spec,
        "matrix_audit": str(matrix_audit), "engine_manifest_template": str(engine_manifest_template),
        "product_build_root": str(product_build_root), "python": python,
        "compile_db": str(compile_db_path) if compile_db_path else None,
        "compile_source_root": str(compile_source_path) if compile_source_path else None,
        "compile_output_root": str(compile_output_path) if compile_output_path else None,
        "container_image": container_image if mode == "all" else None,
        "container_image_id": container_image_id.lower() if isinstance(container_image_id, str) else None,
        "container_temp_root": str(container_temp_path) if container_temp_path else None,
        "simulator_authority": str(simulator_authority_path) if simulator_authority_path else None,
        "dimensions": {"profiles": list(PROFILES), "regimes": list(REGIMES),
                       "topologies": list(TOPOLOGIES)}}
    if resume is None:
        requested_stamp = timestamp or _stamp()
        campaign = _new_campaign_root(output_root, requested_stamp)
        identity = _git_identity(repo)
        metadata = {"schema": SCHEMA, "status": "IN_PROGRESS", "created_utc": requested_stamp,
                    "campaign_root": str(campaign), "git": identity, "config": config,
                    "mode": mode if execute else "plan-only", "staged_stages": ["live", "comparison"],
                    "live_authority": live_authority,
                    "config_sha256": hashlib.sha256(canonical(config)).hexdigest(),
                    "matrix": {"corpus": corpus, "profiles": list(PROFILES),
                               "regimes": list(REGIMES), "topologies": list(TOPOLOGIES),
                               "depth": depth, "expected_cells": len(cells(corpus))}}
        _write_new(campaign / "campaign.json", canonical(metadata))
    else:
        campaign = resume.absolute()
        metadata = _load_json(campaign / "campaign.json")
        if metadata.get("schema") != SCHEMA or metadata.get("config") != config:
            raise CampaignError("resume:campaign_configuration_mismatch")
        if metadata.get("git") != _git_identity(repo):
            raise CampaignError("resume:source_identity_mismatch")
        if mode == "all" and metadata.get("live_authority") != live_authority:
            raise CampaignError("resume:live_authority_mismatch")
    runner = command_runner or _run_command
    records: list[dict[str, Any]] = []
    stamp = str(metadata["created_utc"])
    for cell in cells(corpus):
        _validate_cell(cell)
        cell_label = _cell_id(cell)
        cell_dir = campaign / "cells" / _slug(cell)
        cell_dir.mkdir(parents=True, exist_ok=True)
        state_path = cell_dir / "status.json"
        old = _cell_state(state_path)
        if old.get("status") == "PASS":
            records.append(old)
            continue
        if old.get("status") in {"FAIL", "INTERRUPTED"} and not retry_failed:
            records.append(old)
            continue
        if old.get("status") in {"FAIL", "STAGED", "INTERRUPTED"}:
            old.setdefault("history", []).append({
                "attempt": old.get("attempt"), "status": old.get("status"),
                "error": old.get("error"), "result": old.get("result"),
                "ended_utc": old.get("ended_utc"),
            })
        if old.get("status") == "RUNNING":
            old.setdefault("history", []).append({"status": "RUNNING", "ended": "interrupted_on_resume"})
            old["status"] = "INTERRUPTED"
            old["ended_utc"] = datetime.now(timezone.utc).isoformat()
            _replace_json(state_path, old)
            records.append(old)
            continue
        attempt_no = _attempt(cell_dir)
        attempt_dir = cell_dir / f"attempt-{attempt_no:03d}"
        attempt_dir.mkdir()
        source_manifest = _format_path(source_manifest_spec, cell)
        source_root = _format_path(source_root_spec, cell)
        engine_manifest = _format_path(engine_manifest_template, cell)
        plan_cmds, producer_cmd, live_cmds, comparison_cmd, result_dirs = _source_commands(
            cell_dir, cell, depth=depth, source_manifest=source_manifest,
            source_root=source_root, matrix_audit=matrix_audit, engine_manifest=engine_manifest,
            product_root=product_build_root, python=python, campaign_stamp=stamp,
            compile_db=compile_db_path, compile_source_root=compile_source_path,
            compile_output_root=compile_output_path, repo=repo,
            container_image=container_image if mode == "all" else None,
            container_image_id=container_image_id if mode == "all" else None,
            container_temp_root=container_temp_path if mode == "all" else None)
        # _source_commands uses attempt-001 as a stable template. Rebase every
        # generated path to this attempt so retries cannot overwrite outputs.
        def rebase(value: object) -> object:
            if isinstance(value, Path):
                return Path(str(value).replace(str(cell_dir / "attempt-001"), str(attempt_dir)))
            if isinstance(value, str):
                return value.replace(str(cell_dir / "attempt-001"), str(attempt_dir))
            if isinstance(value, list):
                return [rebase(item) for item in value]
            if isinstance(value, dict):
                return {key: rebase(item) for key, item in value.items()}
            return value
        def rehash(value: object) -> object:
            if isinstance(value, list):
                return [rehash(item) for item in value]
            if isinstance(value, dict):
                mapped = {key: rehash(item) for key, item in value.items() if key != "sha256"}
                if {"stage", "argv", "shell", "cwd", "executable"}.issubset(mapped):
                    mapped["sha256"] = hashlib.sha256(canonical(mapped)).hexdigest()
                return mapped
            return value
        plan_cmds, producer_cmd, live_cmds, comparison_cmd, result_dirs = tuple(
            rehash(rebase(item)) for item in (plan_cmds, producer_cmd, live_cmds, comparison_cmd, result_dirs)
        )  # type: ignore[assignment]
        commands = {"predictive_plan": plan_cmds, "predictive_producer": producer_cmd,
                    "live": live_cmds, "comparison": comparison_cmd}
        _write_new(attempt_dir / "commands.json", canonical(commands))
        status: dict[str, Any] = {"schema": CELL_SCHEMA, "cell": cell, "cell_id": cell_label,
                                  "split": SPLITS[corpus], "status": "RUNNING", "attempt": attempt_no,
                                  "started_utc": datetime.now(timezone.utc).isoformat(),
                                  "commands": {"path": str((attempt_dir / "commands.json").relative_to(campaign)),
                                               "sha256": hashlib.sha256((attempt_dir / "commands.json").read_bytes()).hexdigest()},
                                  "history": old.get("history", [])}
        _replace_json(state_path, status)
        if not execute:
            status["status"] = "STAGED"
            status["ended_utc"] = datetime.now(timezone.utc).isoformat()
            status["reason"] = "predictive execution disabled"
            result_path = attempt_dir / "result.json"
            _write_new(result_path, canonical({"schema": "icecream-s8-campaign-result-v2",
                                               "cell": cell, "attempt": attempt_no,
                                               "identity": {
                                                   "method": cell["profile"], "corpus": cell["corpus"],
                                                   "regime": cell["regime"], "split": SPLITS[corpus],
                                                   "topology": cell["topology"], "depth_class": depth,
                                                   "pass_id": "full-1",
                                               },
                                               "status": "STAGED", "result": None,
                                               "error": status["reason"]}))
            status["result_record"] = {"path": str(result_path.relative_to(campaign)),
                                        "bytes": result_path.stat().st_size,
                                        "sha256": hashlib.sha256(result_path.read_bytes()).hexdigest()}
            _replace_json(state_path, status)
            records.append(status)
            continue
        stdout, stderr = attempt_dir / "stdout.log", attempt_dir / "stderr.log"
        result: dict[str, Any] | None = None
        error: str | None = None
        interrupted = False
        try:
            for command in plan_cmds + [producer_cmd]:
                stage = str(command["stage"])
                rc = runner(command, repo, stdout, stderr)
                if rc != 0:
                    error = f"{stage}:returncode:{rc}"
                    break
        except KeyboardInterrupt:
            interrupted = True
            error = "campaign:interrupted"
        if error is None and mode == "all":
            # Live execution is intentionally sequential.  Preparation may
            # produce the authenticated batch/topology handoff, but only the
            # lock-and-gate immediately around live_run owns the measurement.
            live_prepare = live_cmds["prepare"]
            live_run = live_cmds["run"]
            try:
                for command in (live_prepare,):
                    rc = runner(command, repo, stdout, stderr)
                    if rc != 0:
                        error = f"{command['stage']}:returncode:{rc}"
                        break
                if error is None:
                    prep_argv = [str(item) for item in live_prepare["argv"]]  # type: ignore[index]
                    prep_dir = Path(prep_argv[prep_argv.index("--output") + 1])
                    prep_names = {str(item["path"]) for item in _artifact_tree(prep_dir)}
                    if not {"batch-manifest.jsonl", "topology.json"}.issubset(prep_names):
                        raise CampaignError("live_prepare:required_artifact_missing")
                if error is None:
                    assert container_temp_path is not None
                    with _live_run_lock(container_temp_path):
                        gate = idle_host_gate or _host_is_idle
                        try:
                            idle = bool(gate())
                        except Exception as exc:  # a gate must fail closed
                            raise CampaignError("live_run:idle_host_gate_failed") from exc
                        if not idle:
                            raise CampaignError("live_run:host_not_idle")
                        if live_run.get("executable") is not True:
                            raise CampaignError("live_run:command_not_executable")
                        rc = runner(live_run, repo, stdout, stderr)
                    if rc != 0:
                        error = f"{live_run['stage']}:returncode:{rc}"
                if error is None:
                    live_argv = [str(item) for item in live_run["argv"]]  # type: ignore[index]
                    live_out = Path(live_argv[live_argv.index("--output") + 1])
                    target = (live_out / "icecream" / cell["topology"].replace("/", "-") /
                              stamp / cell["profile"])
                    live_names = {str(item["path"]) for item in _artifact_tree(target)}
                    if "live_curve_manifest.json" not in live_names:
                        raise CampaignError("live_run:authenticated_curve_missing")
                    live_record = _sha(target / "live_curve_manifest.json")
                    live_record["path"] = str((target / "live_curve_manifest.json").relative_to(campaign))
                    if result is None:
                        result = {}
                    result["live"] = live_record
                if error is None:
                    comparisons: list[dict[str, object]] = []
                    for comparison_index, command in enumerate(comparison_cmd):
                        if command.get("executable") is not True:
                            raise CampaignError(f"{command['stage']}:command_not_executable")
                        rc = runner(command, repo, stdout, stderr)
                        if rc != 0:
                            error = f"{command['stage']}:returncode:{rc}"
                            break
                        comparison_argv = [str(item) for item in command["argv"]]  # type: ignore[index]
                        comparison_path = Path(comparison_argv[comparison_argv.index("--out") + 1])
                        pass_id = ("full-2" if depth == "full" and comparison_index == 1
                                   else "full-1")
                        depth_class = "repeat-full" if pass_id == "full-2" else depth
                        experiment_identity = {
                            "method": cell["profile"], "corpus": cell["corpus"],
                            "regime": cell["regime"], "split": SPLITS[corpus],
                            "topology": cell["topology"], "depth_class": depth_class,
                            "pass_id": pass_id,
                        }
                        comparison_record = _authenticate_comparison(
                            comparison_path,
                            expected_cell=(cell["corpus"], cell["profile"], cell["regime"]),
                            experiment_identity=experiment_identity)
                        try:
                            comparison_record["path"] = str(
                                comparison_path.resolve().relative_to(campaign.resolve()))
                        except ValueError as exc:
                            raise CampaignError(
                                "comparison_output:path_not_under_campaign") from exc
                        comparison_record["records"] = {
                            "path": comparison_record["path"],
                            "sha256": comparison_record["sha256"],
                            "bytes": comparison_record["bytes"],
                        }
                        comparisons.append(comparison_record)
                    if error is None:
                        assert result is not None
                        result["comparisons"] = comparisons
            except KeyboardInterrupt:
                interrupted = True
                error = "campaign:interrupted"
            except (CampaignError, OSError, ValueError) as exc:
                error = f"live:{exc}"
        if error is None:
            try:
                live_evidence = ({key: value for key, value in result.items()
                                  if key in {"live", "comparisons"}}
                                 if mode == "all" and result is not None else {})
                segments: list[dict[str, Any]] = []
                for result_dir in result_dirs:
                    result_dir = Path(result_dir)
                    artifacts = _artifact_tree(result_dir)
                    names = {str(item["path"]) for item in artifacts}
                    if "predictive_sim.jsonl" not in names or "producer_manifest.json" not in names:
                        raise CampaignError("predictive_result:required_artifact_missing")
                    curve_lines = (result_dir / "predictive_sim.jsonl").read_text().splitlines()
                    if not curve_lines:
                        raise CampaignError("predictive_result:empty_curve")
                    curve = json.loads(curve_lines[-1])
                    segment_index = len(segments)
                    segment_pass = ("full-2" if depth == "full" and segment_index == 1
                                    else "full-1")
                    segment_identity = {
                        "method": cell["profile"], "corpus": cell["corpus"],
                        "regime": cell["regime"], "split": SPLITS[corpus],
                        "topology": cell["topology"],
                        "depth_class": ("repeat-full" if segment_pass == "full-2" else depth),
                        "pass_id": segment_pass,
                    }
                    segments.append({"directory": str(result_dir), "identity": segment_identity,
                                     "artifacts": artifacts,
                                     "points": len(curve_lines),
                                     "final": curve.get("cumulative", {}) if isinstance(curve, dict) else {}})
                result = {"segments": segments,
                          "directories": [item["directory"] for item in segments],
                          "points": sum(int(item["points"]) for item in segments),
                          "final": segments[-1]["final"] if segments else {},
                          "plans": []}
                for plan_command in plan_cmds:
                    plan_argv = [str(item) for item in plan_command["argv"]]  # type: ignore[index]
                    plan_path = Path(plan_argv[plan_argv.index("--out") + 1])
                    plan_record = _sha(plan_path)
                    plan_record["path"] = str(plan_path.relative_to(campaign))
                    result["plans"].append(plan_record)
                if len(segments) == 1:
                    result.update(segments[0])
                result.update(live_evidence)
            except (OSError, ValueError, IndexError, CampaignError) as exc:
                error = f"predictive_result:{exc}"
        if error is None:
            status.update(status="PASS", result=result)
        elif interrupted:
            status.update(status="INTERRUPTED", error=error)
        else:
            status.update(status="FAIL", error=error)
        status["ended_utc"] = datetime.now(timezone.utc).isoformat()
        result_record = {"schema": "icecream-s8-campaign-result-v2", "cell": cell,
                         "identity": {"method": cell["profile"], "corpus": cell["corpus"],
                                      "regime": cell["regime"], "split": SPLITS[corpus],
                                      "topology": cell["topology"], "depth_class": depth,
                                      "pass_id": "full-1"},
                         "attempt": attempt_no, "status": status["status"],
                         "result": result, "error": error}
        result_path = attempt_dir / "result.json"
        _write_new(result_path, canonical(result_record))
        status["result_record"] = {"path": str(result_path.relative_to(campaign)),
                                    "bytes": result_path.stat().st_size,
                                    "sha256": hashlib.sha256(result_path.read_bytes()).hexdigest()}
        if stdout.exists() and stderr.exists():
            status["logs"] = {
                "stdout": {"path": str(stdout.relative_to(campaign)),
                            "bytes": stdout.stat().st_size,
                            "sha256": hashlib.sha256(stdout.read_bytes()).hexdigest()},
                "stderr": {"path": str(stderr.relative_to(campaign)),
                            "bytes": stderr.stat().st_size,
                            "sha256": hashlib.sha256(stderr.read_bytes()).hexdigest()},
            }
        _replace_json(state_path, status)
        records.append(status)
        # Update summary after each cell so a killed process can resume with a
        # truthful partial view.
        _replace_json(campaign / "summary.json", _summary(campaign, records, config))
        if interrupted or (mode == "all" and error is not None):
            # The failed/interrupted attempt is retained as-is.  Remaining
            # cells stay PENDING until an explicitly requested retry/resume.
            break
    # Include untouched prior states when an early skip left records sparse.
    all_records = []
    for cell in cells(corpus):
        all_records.append(_cell_state(campaign / "cells" / _slug(cell) / "status.json"))
    summary = _summary(campaign, all_records, config)
    _replace_json(campaign / "summary.json", summary)
    metadata["status"] = summary["status"]
    summary_raw = (campaign / "summary.json").read_bytes()
    metadata["summary"] = {"path": "summary.json", "bytes": len(summary_raw),
                            "sha256": hashlib.sha256(summary_raw).hexdigest()}
    _replace_json(campaign / "campaign-status.json", metadata)
    return campaign


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--corpus", choices=CORPORA, default="DuckDB")
    parser.add_argument("--depth", choices=DEPTHS, required=True)
    parser.add_argument("--source-manifest", required=True,
                        help="path or template using {corpus},{profile},{regime},{topology}")
    parser.add_argument("--source-root", required=True)
    parser.add_argument("--matrix-audit", type=Path, required=True)
    parser.add_argument("--engine-manifest-template", required=True)
    parser.add_argument("--product-build-root", type=Path, required=True)
    parser.add_argument("--output-root", type=Path, default=Path("experiments"))
    parser.add_argument("--resume", type=Path)
    parser.add_argument("--retry-failed", action="store_true")
    parser.add_argument("--compile-db")
    parser.add_argument("--compile-source-root")
    parser.add_argument("--compile-output-root")
    parser.add_argument("--container-image", default=PINNED_IMAGE)
    parser.add_argument("--container-image-id")
    parser.add_argument("--container-temp-root", type=Path)
    parser.add_argument("--simulator-authority", type=Path)
    parser.add_argument("--plan-only", action="store_true",
                        help="retain the full matrix and staged commands without executing predictive cells")
    parser.add_argument("--mode", choices=("predictive-only", "all"), default="predictive-only",
                        help="run predictive cells, or execute serialized live and comparison stages")
    parser.add_argument("--repo", type=Path, default=Path.cwd())
    parser.add_argument("--python", default=sys.executable)
    args = parser.parse_args(argv)
    try:
        campaign = run_campaign(output_root=args.output_root, repo=args.repo,
                                corpus=args.corpus, depth=args.depth,
                                source_manifest=args.source_manifest, source_root=args.source_root,
                                matrix_audit=args.matrix_audit,
                                engine_manifest_template=args.engine_manifest_template,
                                product_build_root=args.product_build_root, python=args.python,
                                resume=args.resume, retry_failed=args.retry_failed,
                                compile_db=args.compile_db, compile_source_root=args.compile_source_root,
                                compile_output_root=args.compile_output_root, execute=not args.plan_only,
                                mode=args.mode, container_image=args.container_image,
                                container_image_id=args.container_image_id,
                                container_temp_root=args.container_temp_root,
                                simulator_authority=args.simulator_authority)
    except CampaignError as exc:
        print(f"s8_campaign_driver: {exc}", file=sys.stderr)
        return 2
    print(campaign)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
