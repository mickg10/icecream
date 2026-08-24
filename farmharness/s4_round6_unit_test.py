#!/usr/bin/env python3
"""Local, no-network S4 round-6 discriminator gate.

This intentionally does not launch Docker or call an SSH host.  It exercises
the generated publication transaction with crafted archives and checks the
source-level private-staging invariants that a remote farm run would amplify.
"""
import ast
import hashlib
import json
import os
import pathlib
import re
import shutil
import signal
import subprocess
import sys
import tarfile
import tempfile
import time

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import farm  # noqa: E402


def check(cond, msg):
    if not cond:
        raise AssertionError(msg)


def make_tar(root, names):
    path = root / "bundle.tar"
    with tarfile.open(path, "w") as tf:
        for name, data, mode in names:
            src = root / ("src-" + name.replace("/", "_"))
            src.write_bytes(data)
            src.chmod(mode)
            tf.add(src, arcname=name, recursive=False)
    return path


def run_publish(tmp, tar_path, manifest, root_name="root", env_extra=None, script=None):
    root = f"~/role-artifacts/store/p50/{root_name}"
    if script is None:
        script = farm._publish_script("p50", manifest, root, "$HOME/bundle.tar", None)
    env = dict(os.environ, HOME=str(tmp))
    if env_extra:
        env.update(env_extra)
    (tmp / "bundle.tar").write_bytes(tar_path.read_bytes())
    result = subprocess.run(["bash", "-c", script], env=env,
                            text=True, capture_output=True)
    return result


def run_post_pin_race(tmp, source_tar, replacement_tar, manifest, root_name, mode,
                      script=None):
    """Run one publication while replacing/deleting the source after pinning."""
    root = f"~/role-artifacts/store/p50/{root_name}"
    if script is None:
        script = farm._publish_script("p50", manifest, root, "$HOME/bundle.tar", None)
    source = tmp / "bundle.tar"
    source.write_bytes(source_tar.read_bytes())
    ready = tmp / f"{root_name}.ready"
    cont = tmp / f"{root_name}.continue"
    env = dict(os.environ, HOME=str(tmp),
               FARM_PUBLISH_PIN_READY_FILE=str(ready),
               FARM_PUBLISH_PIN_CONTINUE_FILE=str(cont))
    proc = subprocess.Popen(["bash", "-c", script], env=env,
                            text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    deadline = time.time() + 10
    while not ready.exists() and proc.poll() is None and time.time() < deadline:
        time.sleep(0.01)
    check(ready.exists(), f"publication did not expose its post-pin race point: {proc.poll()}")
    # Readiness is a touch-only seam, never an authority channel. Replace it
    # with an attacker-controlled error-like file (and a symlink) after the
    # shell has touched it; publication must continue to use its fixed FD.
    attacker_error = tmp / f"{root_name}.error"
    attacker_error.write_bytes(replacement_tar.read_bytes())
    ready.unlink()
    ready.symlink_to(attacker_error)
    if mode == "replace":
        swap = tmp / f"{root_name}.swap.tar"
        shutil.copyfile(replacement_tar, swap)
        os.replace(swap, source)
    elif mode == "delete":
        source.unlink()
    else:
        raise AssertionError(f"unknown race mode {mode!r}")
    cont.touch()
    stdout, stderr = proc.communicate(timeout=20)
    return subprocess.CompletedProcess(proc.args, proc.returncode, stdout, stderr)


def find_keeper(publisher_pid):
    """Find the publisher's direct child that owns the fixed memfd FD."""
    children = pathlib.Path(f"/proc/{publisher_pid}/task/{publisher_pid}/children")
    try:
        child_pids = [int(value) for value in children.read_text().split()]
    except (OSError, ValueError):
        return None
    for child_pid in child_pids:
        if pathlib.Path(f"/proc/{child_pid}/fd/198").exists():
            return child_pid
    return None


def run_sigkill_lifecycle(tmp, source_tar, root_name, script):
    """SIGKILL the publication shell after pinning; return keeper liveness."""
    root = tmp / f"role-artifacts/store/p50/{root_name}"
    source = tmp / "bundle.tar"
    source.write_bytes(source_tar.read_bytes())
    ready = tmp / f"{root_name}.ready"
    cont = tmp / f"{root_name}.continue"
    env = dict(os.environ, HOME=str(tmp),
               FARM_PUBLISH_PIN_READY_FILE=str(ready),
               FARM_PUBLISH_PIN_CONTINUE_FILE=str(cont))
    proc = subprocess.Popen(["bash", "-c", script], env=env,
                            text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    helper_pid = None
    try:
        deadline = time.time() + 10
        while not ready.exists() and proc.poll() is None and time.time() < deadline:
            time.sleep(0.01)
        check(ready.exists(), f"SIGKILL gate did not reach post-pin point: {proc.poll()}")
        deadline = time.time() + 10
        while proc.poll() is None and time.time() < deadline:
            helper_pid = find_keeper(proc.pid)
            if helper_pid is not None:
                break
            time.sleep(0.01)
        check(helper_pid is not None,
              f"SIGKILL gate did not expose fixed-FD keeper: {proc.poll()}")
        check(pathlib.Path(f"/proc/{helper_pid}").exists(),
              "SIGKILL gate keeper was gone before publisher termination")
        os.kill(proc.pid, signal.SIGKILL)
        proc.wait(timeout=10)
        deadline = time.time() + 5
        while pathlib.Path(f"/proc/{helper_pid}").exists() and time.time() < deadline:
            time.sleep(0.02)
        keeper_alive = pathlib.Path(f"/proc/{helper_pid}").exists()
        return root, helper_pid, keeper_alive
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait(timeout=10)
        # A deliberately neutralized mutant is expected to leak until this
        # cleanup. Never leave a test-created keeper or memfd behind.
        if helper_pid is not None and pathlib.Path(f"/proc/{helper_pid}").exists():
            os.kill(helper_pid, signal.SIGKILL)
            deadline = time.time() + 5
            while pathlib.Path(f"/proc/{helper_pid}").exists() and time.time() < deadline:
                time.sleep(0.02)


def run_old_fd_handoff_exploit(tmp, approved_source, malicious_tar, root_name, script):
    """Exercise a deliberately restored mutable PID/FD handoff mutant."""
    root = tmp / f"role-artifacts/store/p50/{root_name}"
    source = tmp / "bundle.tar"
    source.write_bytes(approved_source.read_bytes())
    ready = tmp / f"{root_name}.ready"
    cont = tmp / f"{root_name}.continue"
    fd_metadata = tmp / f"{root_name}.fd"
    fd_metadata.write_text("1\n")
    mutable_fd_object = tmp / "mutable-fd-object"
    env = dict(os.environ, HOME=str(tmp),
               FARM_PUBLISH_PIN_READY_FILE=str(ready),
               FARM_PUBLISH_PIN_CONTINUE_FILE=str(cont),
               FARM_PUBLISH_PIN_FD_FILE=str(fd_metadata))
    proc = subprocess.Popen(["bash", "-c", script], env=env,
                            text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        deadline = time.time() + 10
        while not ready.exists() and proc.poll() is None and time.time() < deadline:
            time.sleep(0.01)
        check(ready.exists(), f"old FD-handoff mutant did not reach post-pin point: {proc.poll()}")
        check(mutable_fd_object.exists(),
              "old FD-handoff mutant did not expose its mutable regular FD object")
        # Rewrite the already-open regular FD object after pinning. A genuine
        # old pathname/FD handoff consumes these attacker bytes.
        mutable_fd_object.write_bytes(malicious_tar.read_bytes())
        fd_metadata.write_text("2\n")
        cont.touch()
        stdout, stderr = proc.communicate(timeout=20)
        return subprocess.CompletedProcess(proc.args, proc.returncode, stdout, stderr), root
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait(timeout=10)


def function_call_count(source, function_name, callee_name):
    tree = ast.parse(source)
    function = next(node for node in ast.walk(tree)
                    if isinstance(node, ast.FunctionDef) and node.name == function_name)
    return sum(1 for node in ast.walk(function)
               if isinstance(node, ast.Call)
               and isinstance(node.func, ast.Name)
               and node.func.id == callee_name)


def main():
    with tempfile.TemporaryDirectory(prefix="s4-r6-") as td:
        tmp = pathlib.Path(td)
        payload = b"approved-payload\n"
        digest = hashlib.sha256(payload).hexdigest()
        base = {
            "set": "p50",
            "binaries": [{"path": "obj/client/icecc", "sha256": digest,
                          "mode": "755", "size": len(payload)}],
            "tar": {"path": "bundle.tar", "sha256": "", "size": 0},
        }
        good = make_tar(tmp, [("obj/client/icecc", payload, 0o755)])
        approved_source = tmp / "approved-source.tar"
        approved_source.write_bytes(good.read_bytes())
        base["tar"]["sha256"] = hashlib.sha256(good.read_bytes()).hexdigest()
        base["tar"]["size"] = good.stat().st_size

        ok = run_publish(tmp, good, base, "good")
        check(ok.returncode == 0 and "PUBLISH-OK" in ok.stdout,
              f"good archive did not publish: rc={ok.returncode} out={ok.stdout!r} err={ok.stderr!r}")

        # Round-7 discriminator: after pinning, replacement/rewrite of the
        # mutable source pathname must not alter the bytes validated or
        # extracted.  The replacement archive has the same manifest shape but
        # a different payload and hash; the pinned original must still win.
        replacement_dir = tmp / "replacement"
        replacement_dir.mkdir()
        replacement_payload = b"replacement-must-not-win\n"
        replacement = make_tar(replacement_dir,
                               [("obj/client/icecc", replacement_payload, 0o755)])
        raced = run_post_pin_race(tmp, approved_source, replacement, base,
                                  "post-pin-replacement", "replace")
        check(raced.returncode == 0 and "PUBLISH-OK" in raced.stdout,
              f"sealed pin did not survive source replacement: rc={raced.returncode} "
              f"out={raced.stdout!r} err={raced.stderr!r}")
        published = tmp / "role-artifacts/store/p50/post-pin-replacement/obj/client/icecc"
        check(published.read_bytes() == payload,
              "source replacement after pinning influenced extracted bytes")

        # Deletion is a separate mutant: a source pathname disappearing after
        # pinning is equally harmless to the immutable publication.
        deleted = run_post_pin_race(tmp, approved_source, replacement, base,
                                    "post-pin-deletion", "delete")
        check(deleted.returncode == 0 and "PUBLISH-OK" in deleted.stdout,
              f"sealed pin did not survive source deletion: rc={deleted.returncode} "
              f"out={deleted.stdout!r} err={deleted.stderr!r}")
        deleted_published = tmp / "role-artifacts/store/p50/post-pin-deletion/obj/client/icecc"
        check(deleted_published.read_bytes() == payload,
              "source deletion after pinning changed extracted bytes")

        # Lifecycle discriminator: hold the publisher immediately after pin,
        # SIGKILL its shell, and require the PDEATHSIG-armed keeper to vanish
        # without publishing a root or leaving a process/memfd owner behind.
        sigkill_script = farm._publish_script(
            "p50", base, "~/role-artifacts/store/p50/sigkill-lifecycle",
            "$HOME/bundle.tar", None)
        sigkill_root, sigkill_pid, sigkill_alive = run_sigkill_lifecycle(
            tmp, approved_source, "sigkill-lifecycle", sigkill_script)
        check(not sigkill_alive,
              f"SIGKILL of publisher left sealed-memfd keeper {sigkill_pid} alive")
        check(not sigkill_root.exists(),
              "SIGKILLed publication unexpectedly published a root")
        check(not pathlib.Path(f"/proc/{sigkill_pid}").exists(),
              "SIGKILLed publication left a keeper process/memfd owner behind")

        generated = farm._publish_script("p50", base,
                                         "~/role-artifacts/store/p50/pin-source-check",
                                         "$HOME/bundle.tar", None)
        check("memfd_create" in generated and "F_SEAL_SEAL" in generated,
              "publication script does not create a fully sealed Linux memfd")
        check("PIN_PID=$!" in generated and "PIN_FD=198" in generated,
              "shell does not own the helper PID and fixed descriptor")
        check("/proc/$PIN_PID/fd/$PIN_FD" in generated,
              "publication script does not consume the pinned descriptor")
        check("F_GET_SEALS" in generated and "os.fstat(fd).st_size" in generated,
              "publication script does not verify exact seals and pinned size")
        check("PIN_INFO" not in generated and
              "FARM_PUBLISH_PIN_PID_FILE" not in generated and
              "FARM_PUBLISH_PIN_FD_FILE" not in generated,
              "mutable PID/FD metadata remains in the publication authority path")
        check('tar -tf "$SRC_TAR"' not in generated and
              'tar -tvf "$SRC_TAR"' not in generated and
              'sha256sum "$SRC_TAR"' not in generated,
              "a validation/extraction command still reopens the mutable source pathname")

        # The fixed-FD gate must ignore an attacker-supplied legacy metadata
        # path entirely. With an approved source and a manifest for different
        # bytes, this current script fails closed rather than consulting the
        # metadata file to reach an attacker-controlled descriptor.
        fd_metadata = tmp / "current-fd-metadata.attack"
        fd_metadata.write_text("2\n")
        fixed_fd_attack = run_publish(
            tmp, approved_source, base, "fixed-fd-metadata-attack",
            env_extra={"FARM_PUBLISH_PIN_FD_FILE": str(fd_metadata)},
            script=generated)
        fixed_fd_attack_root = tmp / "role-artifacts/store/p50/pin-source-check"
        check(fixed_fd_attack.returncode == 0 and
              (fixed_fd_attack_root / "obj/client/icecc").read_bytes() == payload,
              "attacker-supplied legacy FD metadata influenced fixed-FD publication")

        # Explicit old-mutable-FD-handoff exploit: restore the former pattern
        # (FD selected from mutable metadata, no seals/fstat gate), point it at
        # a regular stdout file, then rewrite that already-open object after
        # pinning. The mutant publishes attacker bytes, proving why the fixed
        # descriptor and exact-seal gate are load-bearing.
        attacker_payload = b"attacker-payload\n"
        attacker_dir = tmp / "attacker-tar"
        attacker_dir.mkdir()
        attacker_tar = make_tar(attacker_dir,
                                [("obj/client/icecc", attacker_payload, 0o755)])
        exploit_manifest = json.loads(json.dumps(base))
        exploit_manifest["binaries"][0]["sha256"] = hashlib.sha256(attacker_payload).hexdigest()
        exploit_manifest["binaries"][0]["size"] = len(attacker_payload)
        exploit_manifest["tar"]["sha256"] = hashlib.sha256(attacker_tar.read_bytes()).hexdigest()
        exploit_manifest["tar"]["size"] = attacker_tar.stat().st_size
        old_fd_mutant = farm._publish_script(
            "p50", exploit_manifest,
            "~/role-artifacts/store/p50/old-fd-handoff-exploit",
            "$HOME/bundle.tar", None)
        old_fd_mutant = old_fd_mutant.replace(
            "PIN_FD=198", "PIN_FD=$(cat \"$FARM_PUBLISH_PIN_FD_FILE\")")
        integrity_start = old_fd_mutant.index("# PINNED-FD-INTEGRITY-BEGIN")
        integrity_end = old_fd_mutant.index("# PINNED-FD-INTEGRITY-END", integrity_start)
        integrity_end = old_fd_mutant.index("\n", integrity_end) + 1
        old_fd_mutant = (old_fd_mutant[:integrity_start] +
                          "# OLD MUTABLE-FD-HANDOFF MUTANT: integrity gate removed\n" +
                          old_fd_mutant[integrity_end:])
        old_fd_mutant = old_fd_mutant.replace(
            ">/dev/null 2>/dev/null <<'PIN_HELPER_PY'",
            ">\"$HOME/mutable-fd-object\" 2>/dev/null <<'PIN_HELPER_PY'")
        exploit_result, exploit_root = run_old_fd_handoff_exploit(
            tmp, approved_source, attacker_tar,
            "old-fd-handoff-exploit", old_fd_mutant)
        exploit_file = exploit_root / "obj/client/icecc"
        check(exploit_result.returncode == 0 and exploit_file.read_bytes() == attacker_payload,
              "old mutable FD-handoff exploit did not demonstrate attacker-byte influence")

        arm_failure = generated.replace(
            "prctl_result = prctl(1, signal.SIGKILL, 0, 0, 0)  # PR_SET_PDEATHSIG",
            "prctl_result = -1  # forced arm failure mutant")
        failed_arm = run_publish(tmp, approved_source, base,
                                 "pdeath-arm-failure-mutant", script=arm_failure)
        check(failed_arm.returncode != 0 and "PUBLISH-TAR-PIN-FAILED" in failed_arm.stdout,
              "a failed PDEATHSIG arm did not fail publication closed")

        # Deterministic deletion mutant: replacing all pinned-object consumers
        # with the mutable pathname must go red at the fixed-FD integrity gate.
        # This is a regression discriminator, not a permitted production
        # fallback.
        unpinned = generated.replace('"$PIN_TAR"', '"$SRC_TAR"')
        mutant = run_publish(tmp, approved_source, base,
                             "unpinned-deletion-mutant", script=unpinned)
        mutant_root = tmp / "role-artifacts/store/p50/unpinned-deletion-mutant"
        check(mutant.returncode != 0 and not mutant_root.exists(),
              "deletion mutant unexpectedly published after removing pinned consumers")
        replacement_mutant = run_publish(tmp, replacement, base,
                                         "unpinned-replacement-mutant", script=unpinned)
        replacement_mutant_root = tmp / "role-artifacts/store/p50/unpinned-replacement-mutant"
        check(replacement_mutant.returncode != 0 and not replacement_mutant_root.exists(),
              "replacement mutant unexpectedly published after removing pinned consumers")

        # Neutralization mutant: if the prctl arm is removed, the exact same
        # SIGKILL gate must observe a surviving keeper. This proves the gate
        # goes red when parent-death handling is deleted, while the helper is
        # explicitly cleaned up by run_sigkill_lifecycle().
        pdeath_mutant = sigkill_script.replace(
            "prctl_result = prctl(1, signal.SIGKILL, 0, 0, 0)  # PR_SET_PDEATHSIG",
            "prctl_result = 0  # neutralized PDEATHSIG mutant")
        check(pdeath_mutant != sigkill_script and "neutralized PDEATHSIG mutant" in pdeath_mutant,
              "parent-death neutralization mutant was not constructed")
        mutant_root, mutant_pid, mutant_alive = run_sigkill_lifecycle(
            tmp, approved_source, "pdeath-neutralized-mutant", pdeath_mutant)
        check(mutant_alive,
              "SIGKILL lifecycle gate did not go red when PDEATHSIG was neutralized")
        check(not mutant_root.exists(),
              "neutralized PDEATHSIG mutant unexpectedly published a root")

        extra = make_tar(tmp, [("obj/client/icecc", payload, 0o755),
                              ("unexpected", b"escape", 0o644)])
        extra_manifest = json.loads(json.dumps(base))
        extra_manifest["tar"]["sha256"] = hashlib.sha256(extra.read_bytes()).hexdigest()
        out = run_publish(tmp, extra, extra_manifest, "extra")
        check("manifest-name-type-size-mismatch" in out.stdout,
              "an extra regular tar member reached extraction instead of header rejection")
        check(not (tmp / "role-artifacts/store/p50/extra/unexpected").exists(),
              "extra archive member escaped into the publication root")

        duplicate = make_tar(tmp, [("./obj/client/icecc", payload, 0o755),
                                   ("obj/client/icecc", payload, 0o755)])
        duplicate_manifest = json.loads(json.dumps(base))
        duplicate_manifest["tar"]["sha256"] = hashlib.sha256(duplicate.read_bytes()).hexdigest()
        out = run_publish(tmp, duplicate, duplicate_manifest, "duplicate")
        check("duplicate-normalized-member-names" in out.stdout,
              "./x and x duplicate normalized paths were not rejected in the header gate")

        wrong_size = make_tar(tmp, [("obj/client/icecc", payload + b"tamper", 0o755)])
        wrong_manifest = json.loads(json.dumps(base))
        wrong_manifest["tar"]["sha256"] = hashlib.sha256(wrong_size.read_bytes()).hexdigest()
        wrong_manifest["binaries"][0]["sha256"] = hashlib.sha256(payload + b"tamper").hexdigest()
        wrong_manifest["binaries"][0]["size"] = len(payload)
        out = run_publish(tmp, wrong_size, wrong_manifest, "wrong-size")
        check("manifest-name-type-size-mismatch" in out.stdout,
              "per-file tar size mismatch was not rejected before extraction")

        tar_size_manifest = json.loads(json.dumps(base))
        tar_size_manifest["tar"]["size"] += 1
        out = run_publish(tmp, good, tar_size_manifest, "wrong-tar-size")
        check(("PUBLISH-TAR-SIZE-MISMATCH" in out.stdout or
               "PUBLISH-TAR-PIN-FAILED" in out.stdout),
              "pinned tar byte-size was not checked against manifest.tar.size")

        src = (HERE / "farm.py").read_text()
        publish_script = farm._publish_script("p50", base,
                                              "~/role-artifacts/store/p50/size-column",
                                              "$HOME/bundle.tar", None)
        check(re.search(r"TAR_TOTAL_SIZE=.*?sum\+=\$3", publish_script, re.DOTALL) is not None,
              "tar aggregate-size cap is not summing GNU tar's size column ($3)")
        check("--tmpfs /work:rw,exec" in src and "/artifact-source:ro" in src,
              "S/F/C do not mount a private work tmpfs beside the read-only source")
        check("/proc/self/fd/" not in src and "ROLE_BINARY_FD" not in src,
              "live-bind FD execution remains in the production source")
        check(function_call_count(src, "up", "_artifact_stage_prefix") == 2,
              "up() no longer inserts private staging at both S and F production callsites")
        check(function_call_count(src, "run_client", "_artifact_stage_prefix") == 1,
              "run_client() no longer inserts private staging at the C production callsite")
        stage = farm._artifact_stage_prefix("p50", "stage-token")
        check("'" not in stage,
              "artifact staging prefix contains a single quote and can break Docker's embedded bash -c")
        for needle in ("/artifact-source/obj/client/icecc",
                       "/artifact-source/obj/client/icecc-create-env",
                       "/artifact-source/obj/daemon/iceccd",
                       "stat -c %s", "ARTIFACT-STAGED-OK-stage-token"):
            check(needle in stage, f"private closure stage missing {needle!r}")

        # Execute the generated shell, not just source-grep it. The exact
        # round-6 predecessor emitted `find -printf %P\\n` without quoting
        # the format, so bash consumed the backslash and every selected
        # launch failed with an inventory named `...n`. A one-file closure
        # is the smallest production-shaped discriminator for that bug.
        original_load_manifest = farm.load_manifest
        farm.load_manifest = lambda _binary_set: base
        try:
            executable_stage = farm._artifact_stage_prefix("p50", "executed-stage")
        finally:
            farm.load_manifest = original_load_manifest
        source_root = tmp / "artifact-source"
        private_root = tmp / "private-work"
        source_file = source_root / "obj/client/icecc"
        source_file.parent.mkdir(parents=True)
        source_file.write_bytes(payload)
        source_file.chmod(0o555)
        private_root.mkdir()
        local_stage = (executable_stage
                       .replace("/artifact-source", str(source_root))
                       .replace("/work", str(private_root)))
        executed = subprocess.run(["bash", "-c", local_stage], text=True,
                                  capture_output=True)
        check(executed.returncode == 0 and "ARTIFACT-STAGED-OK-executed-stage" in executed.stdout,
              f"generated private-stage shell rejected an exact closure: "
              f"rc={executed.returncode} out={executed.stdout!r} err={executed.stderr!r}")
        staged_file = private_root / "obj/client/icecc"
        check(staged_file.read_bytes() == payload and staged_file.stat().st_mode & 0o222 == 0,
              "generated private-stage shell did not preserve exact bytes and harden them")

        # The no-Git fallback itself is a committed gate. Run it from two
        # different absolute extraction paths and require one nonempty,
        # identical authority hash. This catches both the former
        # absolute-path contamination and the `/bin/sh`-incompatible
        # `read -d` loop that silently hashed zero rows.
        labels = []
        for name in ("no-git-A", "no-git-B"):
            copied = tmp / name / "farmharness"
            shutil.copytree(HERE, copied, ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
            env = dict(os.environ,
                       ARTIFACT_TEST_ONLY_FRESH="1",
                       ARTIFACT_TEST_SCRATCH=str(tmp / f"scratch-{name}"))
            result = subprocess.run([str(copied / "artifact_selection_test.sh")], env=env,
                                    text=True, capture_output=True)
            check(result.returncode == 0,
                  f"no-Git gate failed from {name}: out={result.stdout!r} err={result.stderr!r}")
            match = re.search(r"no-git tree-hash ([0-9a-f]{64})", result.stdout)
            check(match is not None, f"no-Git gate emitted no authority hash from {name}")
            labels.append(match.group(1))
        check(labels[0] == labels[1],
              f"no-Git authority hash depends on extraction path: {labels}")
        check(labels[0] != hashlib.sha256(b"").hexdigest(),
              "no-Git authority hash is the empty-input digest")

    print("s4-round6/7-local: PASS (staging + pre-extraction + sealed-source TOCTOU mutants)")


if __name__ == "__main__":
    main()
