#!/usr/bin/env python3
"""One local Docker build/QA entrypoint, driven by the operator's farm.json."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]
LEGACY_COMMIT = "cd74801e0fa4e83e3ae254ca1d7fe98642f36b89"
BASES = {"ubuntu24.04": "ubuntu:24.04", "ubuntu22.04": "ubuntu:22.04"}
FIELDS = {"version", "profile", "image_repository", "jobs", "memory_gb",
          "base_image", "http_proxy", "image_bundle", "offline"}
GATE_TARGETS = {
    "p51-arm-expiry": ("p50daemonpositive-p51-arm-expiry-check", 240),
    "p51-restart-w30": ("p50daemonpositive-p51-restart-w30-check", 4200),
    "p51-scheduler-restart-w30": ("p51schedulerrestart-w30-check", 1800),
    "p51-scheduler-f-restart-w30": ("p51schedulerrestart-w30-check", 1800),
    "p51-restart-chain-w30": ("p50daemonpositive-p51-restart-chain-w30-check", 1200),
}
GATE_OFFLINE_ENV = (
    "UV_OFFLINE=1",
    "UV_PYTHON_DOWNLOADS=never",
    "UV_PYTHON_INSTALL_DIR=/opt/uv-python",
    "UV_CACHE_DIR=/work/uv-cache",
    "UV_PROJECT_ENVIRONMENT=/work/python-env",
    "VIRTUAL_ENV=/work/python-env",
)


class BootstrapError(RuntimeError):
    pass


def load_spec(path: Path, repository: str | None = None) -> dict:
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict) or set(data) - FIELDS:
        raise BootstrapError("farm.json: unsupported fields; use the small developer farm spec")
    if type(data.get("version")) is not int or data["version"] != 1:
        raise BootstrapError("farm.json: version must be 1")
    result = {"profile": "ubuntu24.04", "image_repository": "", "jobs": 2,
              "memory_gb": 8, "offline": False, **data}
    if not isinstance(result["profile"], str) or result["profile"] not in BASES:
        raise BootstrapError(f"unsupported prepared profile: {result['profile']}")
    for name, limit in (("jobs", 8), ("memory_gb", 256)):
        if type(result[name]) is not int or not 1 <= result[name] <= limit:
            raise BootstrapError(f"{name} must be an integer between 1 and {limit}")
    if type(result["offline"]) is not bool:
        raise BootstrapError("offline must be true or false")
    if repository is not None:
        result["image_repository"] = repository
    repo = result["image_repository"]
    if not isinstance(repo, str) or (repo and (
        not re.fullmatch(r"[a-z0-9][a-z0-9._:/-]*", repo)
        or "://" in repo or repo.endswith("/")
        or ":" in repo.rsplit("/", 1)[-1]
    )):
        raise BootstrapError("image_repository must be a repository path without a tag or URL scheme")
    result.setdefault("base_image", BASES[result["profile"]])
    if not isinstance(result["base_image"], str) or not re.fullmatch(
        r"[A-Za-z0-9][A-Za-z0-9._/@:-]*", result["base_image"]
    ):
        raise BootstrapError("base_image must be a Docker image reference")
    proxy = result.get("http_proxy")
    if proxy is not None and (not isinstance(proxy, str) or
                             not re.fullmatch(r"https?://[^\s]+", proxy)):
        raise BootstrapError("http_proxy must be an http(s) URL")
    if "image_bundle" in result and not isinstance(result["image_bundle"], str):
        raise BootstrapError("image_bundle must be a file path")
    if result.get("image_bundle"):
        bundle = Path(result["image_bundle"])
        if not bundle.is_absolute():
            bundle = path.resolve().parent / bundle
        if not bundle.is_file():
            raise BootstrapError(f"image_bundle is missing: {bundle}")
        result["image_bundle"] = str(bundle.resolve())
    return result


def scratch_root() -> Path:
    value = os.environ.get("ICEFARM_TMPDIR", "")
    if not value or not Path(value).is_absolute():
        raise BootstrapError("ICEFARM_TMPDIR is required and must be an existing absolute directory")
    path = Path(value).resolve()
    if path == Path("/") or not path.is_dir() or "," in str(path):
        raise BootstrapError("ICEFARM_TMPDIR must be an existing non-root directory without commas")
    try:
        with tempfile.TemporaryDirectory(prefix=".icecream-probe-", dir=path):
            pass
    except OSError as exc:
        raise BootstrapError(f"ICEFARM_TMPDIR is not writable: {exc}") from exc
    return path


def snapshot(source: Path, destination: Path) -> str:
    """Copy tracked and non-ignored worktree files, including uncommitted edits."""
    listed = subprocess.run(
        ["git", "-C", str(source), "ls-files", "--cached", "--others",
         "--exclude-standard", "-z"], check=True, capture_output=True,
    ).stdout
    digest = hashlib.sha256()
    destination.mkdir()
    for name in sorted(set(os.fsdecode(item) for item in listed.split(b"\0") if item)):
        original = source / name
        if not original.exists() and not original.is_symlink():
            continue  # A tracked deletion is part of the current edited source.
        target = destination / name
        target.parent.mkdir(parents=True, exist_ok=True)
        digest.update(name.encode() + b"\0")
        if original.is_symlink():
            link = os.readlink(original)
            resolved = original.resolve()
            copied_target = Path(os.path.normpath(str(Path(name).parent / link)))
            if (copied_target.is_absolute() or ".." in copied_target.parts
                    or not resolved.is_relative_to(source.resolve())):
                raise BootstrapError(f"source symlink points outside checkout: {name}")
            target.symlink_to(link)
            digest.update(b"symlink\0" + os.fsencode(link))
        else:
            shutil.copy2(original, target)
            digest.update(str(original.stat().st_mode & 0o777).encode() + b"\0")
            digest.update(target.read_bytes())
    return digest.hexdigest()


class Run:
    def __init__(self, root: Path):
        self.root = root
        self.logs = root / "logs"
        self.logs.mkdir()
        self.steps: list[dict] = []
        self.sdk_reference = ""

    def command(self, name: str, argv: list[str], timeout: int = 7200,
                capture: bool = False) -> str:
        log = self.logs / f"{len(self.steps):02d}-{name}.log"
        print(f"{name}: {log}", flush=True)
        started = time.monotonic()
        status = -1
        try:
            with log.open("w") as output:
                process = subprocess.run(argv, stdout=output, stderr=subprocess.STDOUT,
                                         timeout=timeout, check=False)
                status = process.returncode
            if status:
                raise BootstrapError(f"{name} failed (exit {status}); see {log}")
            return log.read_text(errors="replace") if capture else ""
        finally:
            self.steps.append({"step": name, "exit_code": status, "log": str(log),
                               "elapsed_s": round(time.monotonic() - started, 3)})

    def inspect(self, reference: str) -> dict | None:
        result = subprocess.run(["docker", "image", "inspect", reference],
                                capture_output=True, text=True, timeout=30)
        return json.loads(result.stdout)[0] if result.returncode == 0 else None


def sdk_image(run: Run, spec: dict) -> str:
    metadata = ["pyproject.toml", "uv.lock", ".python-version"]
    missing = [name for name in metadata if not (ROOT / name).is_file()]
    if missing:
        raise BootstrapError("SDK Python metadata is missing: " + ", ".join(missing))
    recipe_input = b"".join((ROOT / name).read_bytes() for name in
                             ["dev/Dockerfile", "dev/run-qa.sh", *metadata])
    recipe = hashlib.sha256(recipe_input + spec["base_image"].encode()
                            + spec["profile"].encode()).hexdigest()
    repository = spec["image_repository"]
    reference = (f"{repository}:sdk-{spec['profile']}" if repository else
                 f"icecream-dev:sdk-{spec['profile']}-{recipe[:12]}")
    if spec.get("image_bundle"):
        run.command("load-images", ["docker", "image", "load", "-i", spec["image_bundle"]])
    # A selected repository is actually consulted, not masked by another local tag.
    if repository and not spec["offline"]:
        run.command("pull-sdk", ["docker", "image", "pull", reference], timeout=300)
    observed = run.inspect(reference)
    if observed is None and not repository and not spec["offline"]:
        context = run.root / "sdk-context"
        context.mkdir()
        for name in ("pyproject.toml", "uv.lock", ".python-version"):
            shutil.copy2(ROOT / name, context / name)
        shutil.copy2(ROOT / "dev/Dockerfile", context / "Dockerfile")
        shutil.copy2(ROOT / "dev/run-qa.sh", context / "run-qa.sh")
        argv = ["docker", "build", "--progress=plain", "--build-arg",
                f"BASE_IMAGE={spec['base_image']}", "--build-arg",
                f"DEV_PROFILE={spec['profile']}", "--build-arg",
                f"RECIPE_REVISION={recipe}", "--tag", reference,
                "--file", str(context / "Dockerfile")]
        env = os.environ.copy()
        if spec.get("http_proxy"):
            # Values stay in the child environment, not persisted command logs.
            env.update(HTTP_PROXY=spec["http_proxy"], HTTPS_PROXY=spec["http_proxy"])
            argv += ["--build-arg", "HTTP_PROXY", "--build-arg", "HTTPS_PROXY"]
        argv.append(str(context))
        previous = {key: os.environ.get(key) for key in ("HTTP_PROXY", "HTTPS_PROXY")}
        try:
            for key in previous:
                if key in env:
                    os.environ[key] = env[key]
            run.command("build-sdk", argv)
        finally:
            for key, value in previous.items():
                if value is None:
                    os.environ.pop(key, None)
                else:
                    os.environ[key] = value
        observed = run.inspect(reference)
    if observed is None:
        raise BootstrapError(f"prepared SDK is unavailable: {reference}; no fallback was attempted")
    labels = observed.get("Config", {}).get("Labels") or {}
    if labels.get("org.icecream.dev.recipe") != recipe:
        raise BootstrapError(f"SDK recipe mismatch: {reference}; prepare it with this checkout's dev recipe")
    if labels.get("org.icecream.dev.profile") != spec["profile"]:
        raise BootstrapError(f"SDK profile mismatch: {reference}")
    run.sdk_reference = reference
    return observed["Id"]


def build_source(run: Run, image: str, source: Path, spec: dict, name: str, mode: str) -> Path:
    work = run.root / name
    work.mkdir()
    temporary = work / "tmp"
    temporary.mkdir()
    temporary.chmod(0o1777)
    container = "icecream-dev-" + uuid.uuid4().hex[:16]
    argv = ["docker", "run", "--name", container, "--rm", "--pull=never",
            # Native descriptor-identity checks require kcmp, which Docker's
            # default runtime profile enables with this capability. The
            # container retains its own PID namespace; no host PID access.
            *(["--cap-add", "SYS_PTRACE"] if mode == "qa" else []),
            "--label", f"icecream.dev.run={run.root.name}", "--network=none",
            "--cpus", str(spec["jobs"]), "--memory", f"{spec['memory_gb']}g",
            "--env", f"ICEFARM_OUTPUT_UID={os.getuid()}",
            "--env", f"ICEFARM_OUTPUT_GID={os.getgid()}",
            "--mount", f"type=bind,src={source},dst=/source,readonly",
            "--mount", f"type=bind,src={work},dst=/work",
            # Older native probes use literal /tmp, ignoring TMPDIR.
            "--mount", f"type=bind,src={temporary},dst=/tmp",
            "--entrypoint", "/usr/local/bin/run-qa.sh", image, mode, str(spec["jobs"])]
    try:
        run.command(name, argv, timeout=14400)
    finally:
        # Only the unique container created by this invocation can be removed.
        subprocess.run(["docker", "rm", "-f", container], capture_output=True, timeout=30)
    return work


def gate_spec(name: str) -> tuple[str, int]:
    try:
        return GATE_TARGETS[name]
    except KeyError as exc:
        choices = ", ".join(sorted(GATE_TARGETS))
        raise BootstrapError(f"unsupported opt-in gate {name!r}; choose one of: {choices}") from exc


def _cleanup_gate_resource(kind: str, name: str, run_id: str) -> str | None:
    if kind == "container":
        template = "{{json .Config.Labels}}"
        label_key = "icecream.dev.gate.id"
        remove = ["docker", "container", "rm", "-f", name]
    elif kind == "network":
        template = "{{json .Labels}}"
        label_key = "icecream.dev.gate.id"
        remove = ["docker", "network", "rm", name]
    else:
        raise AssertionError(kind)
    try:
        inspected = subprocess.run(
            ["docker", kind, "inspect", "--format", template, name],
            capture_output=True, text=True, timeout=30, check=False,
        )
    except (OSError, subprocess.SubprocessError) as exc:
        return f"could not inspect owned {kind} {name}: {exc}"
    if inspected.returncode != 0:
        if "no such" in inspected.stderr.lower() or "not found" in inspected.stderr.lower():
            return None
        return f"could not inspect owned {kind} {name}: {inspected.stderr.strip()}"
    try:
        labels = json.loads(inspected.stdout)
    except json.JSONDecodeError:
        return f"could not parse labels for owned {kind} {name}"
    if not isinstance(labels, dict) or labels.get(label_key) != run_id:
        return f"refusing to remove {kind} {name}: ownership label does not match"
    try:
        removed = subprocess.run(remove, capture_output=True, text=True,
                                 timeout=30, check=False)
    except (OSError, subprocess.SubprocessError) as exc:
        return f"could not remove owned {kind} {name}: {exc}"
    if removed.returncode != 0:
        return f"could not remove owned {kind} {name}: {removed.stderr.strip()}"
    return None


def run_gate(run: Run, image: str, source: Path, work: Path,
             spec: dict, name: str) -> dict:
    target, timeout_s = gate_spec(name)
    run_id = uuid.uuid4().hex
    suffix = run_id[:12]
    network = f"icecream-gate-{suffix}"
    container = f"icecream-gate-{suffix}"
    ownership_label = f"icecream.dev.run={run.root.name}"
    gate_label = f"icecream.dev.gate.id={run_id}"
    temp = work / "tmp"
    if not temp.is_dir():
        raise BootstrapError("gate build scratch is missing; refusing an implicit /tmp fallback")

    failure: Exception | None = None
    cleanup_errors: list[str] = []
    try:
        run.command("gate-network-create", [
            "docker", "network", "create", "--driver", "bridge", "--internal",
            "--label", ownership_label, "--label", gate_label, network,
        ], timeout=30)
        argv = [
            "docker", "run", "--name", container, "--pull=never",
            "--label", ownership_label, "--label", gate_label,
            "--network", network, "--cap-add", "NET_ADMIN",
            "--cpus", str(spec["jobs"]), "--memory", f"{spec['memory_gb']}g",
            "--env", f"ICEFARM_OUTPUT_UID={os.getuid()}",
            "--env", f"ICEFARM_OUTPUT_GID={os.getgid()}",
            "--env", f"ICECREAM_GATE_RUN_ID={run_id}",
            *[item for value in GATE_OFFLINE_ENV for item in ("--env", value)],
            "--mount", f"type=bind,src={source},dst=/source,readonly",
            "--mount", f"type=bind,src={work},dst=/work",
            "--mount", f"type=bind,src={temp},dst=/tmp",
            "--workdir", "/work", "--entrypoint", "/bin/bash", image,
            "/source/dev/run-gate.sh", name,
        ]
        # The outer deadline leaves time for Docker to return and our finally
        # block to remove only the uniquely labeled container/network.
        run.command(f"gate-{name}", argv, timeout=timeout_s + 60)
    except Exception as exc:
        failure = exc
    finally:
        for kind, resource in (("container", container), ("network", network)):
            problem = _cleanup_gate_resource(kind, resource, run_id)
            if problem is not None:
                cleanup_errors.append(problem)

    if failure is not None:
        if cleanup_errors:
            raise BootstrapError(f"{failure}; cleanup errors: {'; '.join(cleanup_errors)}") from failure
        raise failure
    if cleanup_errors:
        raise BootstrapError("opt-in gate cleanup failed: " + "; ".join(cleanup_errors))
    return {"name": name, "target": target, "timeout_s": timeout_s,
            "container": container, "network": network}


def product_image(run: Run, sdk: str, work: Path, name: str, identity: str) -> str:
    reference = f"icecream-dev:{name}-{identity[:16]}"
    # Dockerfile FROM needs a named image, not the sha256 image ID returned by
    # inspect. Keep the exact selected image via an ID-derived local tag.
    if sdk.startswith("sha256:"):
        local_sdk = f"icecream-dev:sdk-runtime-{sdk.split(':', 1)[1]}"
        run.command(f"tag-sdk-{name}", ["docker", "image", "tag", sdk, local_sdk])
        sdk = local_sdk
    run.command(f"image-{name}", ["docker", "build", "--pull=false", "--network=none", "--build-arg",
        f"SDK_IMAGE={sdk}", "--label", f"icecream.source.identity={identity}",
        "-f", str(ROOT / "dev/Dockerfile.runtime"), "-t", reference, str(work / "install")])
    return reference


def legacy_source(run: Run) -> Path:
    archive = run.root / "legacy-source.tar"
    with archive.open("wb") as stream:
        subprocess.run(["git", "-C", str(ROOT), "archive", LEGACY_COMMIT],
                       stdout=stream, check=True)
    destination = run.root / "legacy-source"
    destination.mkdir()
    # The archive comes only from the fixed commit in this repository.
    with tarfile.open(archive) as bundle:
        for item in bundle.getmembers():
            path = Path(item.name)
            if path.is_absolute() or ".." in path.parts or item.issym() or item.islnk():
                raise BootstrapError(f"unsupported legacy archive entry: {item.name}")
        bundle.extractall(destination)
    return destination


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("bootstrap", "qa", "gate"))
    parser.add_argument("--gate", choices=tuple(sorted(GATE_TARGETS)))
    parser.add_argument("--farm", type=Path, default=ROOT / "farm.json")
    parser.add_argument("--image-repo", default=os.environ.get("IMAGE_REPO"))
    args = parser.parse_args(argv)
    if args.action == "gate" and args.gate is None:
        parser.error("gate action requires --gate")
    if args.action != "gate" and args.gate is not None:
        parser.error("--gate is valid only with the gate action")
    run = None
    report = {"status": "FAIL", "action": args.action, "gate": args.gate}
    try:
        spec = load_spec(args.farm, args.image_repo)
        scratch = scratch_root()
        if shutil.disk_usage(scratch).free < 8 * 1024**3:
            raise BootstrapError("scratch requires at least 8 GiB free for a build/QA run")
        run = Run(Path(tempfile.mkdtemp(prefix="icecream-qa-", dir=scratch)))
        print(f"Run directory: {run.root}", flush=True)
        info = json.loads(run.command("docker-info", ["docker", "info", "--format", "{{json .}}"],
                                      capture=True, timeout=30))
        if spec["jobs"] > info["NCPU"] or spec["memory_gb"] * 1024**3 > info["MemTotal"]:
            raise BootstrapError("requested jobs/memory exceed Docker's available resources")
        storage = Path(info["DockerRootDir"])
        if not storage.is_dir():
            raise BootstrapError("use a local Docker daemon: DockerRootDir is not accessible on this host")
        if shutil.disk_usage(storage).free < 8 * 1024**3:
            raise BootstrapError("DockerRootDir requires at least 8 GiB free")
        source = run.root / "source-snapshot"
        identity = snapshot(ROOT, source)
        sdk = sdk_image(run, spec)
        report.update(source_sha256=identity, sdk_image_id=sdk, profile=spec["profile"],
                      sdk_reference=run.sdk_reference,
                      image_repository=spec["image_repository"], docker_root=str(storage),
                      docker_host=info.get("Name"), architecture=info.get("Architecture"))
        build_mode = "bootstrap" if args.action == "gate" else args.action
        current = build_source(run, sdk, source, spec, "current", build_mode)
        if args.action == "gate":
            assert args.gate is not None
            report["gate_result"] = run_gate(run, sdk, source, current,
                                             spec, args.gate)
        elif args.action == "qa":
            current_image = product_image(run, sdk, current, "current", identity)
            report["current_image"] = current_image
            old_source = legacy_source(run)
            old = build_source(run, sdk, old_source, spec, "legacy", "bootstrap")
            old_image = product_image(run, sdk, old, "p43", LEGACY_COMMIT)
            report.update(legacy_image=old_image, legacy_source_commit=LEGACY_COMMIT)
            run.command("mixed", [sys.executable, str(ROOT / "dev/mixed.py"),
                "--current-image", current_image, "--legacy-image", old_image,
                "--output", str(run.root / "mixed"), "--jobs", str(spec["jobs"]),
                "--memory-gb", str(spec["memory_gb"])], timeout=1800)
        report["status"] = "PASS"
        return 0
    except (BootstrapError, OSError, ValueError, subprocess.SubprocessError) as exc:
        report["error"] = str(exc)
        print(f"bootstrap/QA failed: {exc}", file=sys.stderr)
        return 1
    finally:
        if run is not None:
            report["steps"] = run.steps
            (run.root / "result.json").write_text(json.dumps(report, indent=2) + "\n")
            print(f"Result: {run.root / 'result.json'}", flush=True)


if __name__ == "__main__":
    raise SystemExit(main())
