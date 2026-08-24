#!/usr/bin/env python3
"""Local, no-network S4 round-6 discriminator gate.

This intentionally does not launch Docker or call an SSH host.  It exercises
the generated publication transaction with crafted archives and checks the
source-level private-staging invariants that a remote farm run would amplify.
"""
import hashlib
import json
import os
import pathlib
import subprocess
import sys
import tarfile
import tempfile

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


def run_publish(tmp, tar_path, manifest, root_name="root"):
    root = f"~/role-artifacts/store/p50/{root_name}"
    script = farm._publish_script("p50", manifest, root, "$HOME/bundle.tar", None)
    env = dict(os.environ, HOME=str(tmp))
    (tmp / "bundle.tar").write_bytes(tar_path.read_bytes())
    result = subprocess.run(["bash", "-c", script], env=env,
                            text=True, capture_output=True)
    return result


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
        base["tar"]["sha256"] = hashlib.sha256(good.read_bytes()).hexdigest()
        base["tar"]["size"] = good.stat().st_size

        ok = run_publish(tmp, good, base, "good")
        check(ok.returncode == 0 and "PUBLISH-OK" in ok.stdout,
              f"good archive did not publish: rc={ok.returncode} out={ok.stdout!r} err={ok.stderr!r}")

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

        src = (HERE / "farm.py").read_text()
        check("--tmpfs /work:rw,exec" in src and "/artifact-source:ro" in src,
              "S/F/C do not mount a private work tmpfs beside the read-only source")
        check("/proc/self/fd/" not in src and "ROLE_BINARY_FD" not in src,
              "live-bind FD execution remains in the production source")
        stage = farm._artifact_stage_prefix("p50", "stage-token")
        check("'" not in stage,
              "artifact staging prefix contains a single quote and can break Docker's embedded bash -c")
        for needle in ("/artifact-source/obj/client/icecc",
                       "/artifact-source/obj/client/icecc-create-env",
                       "/artifact-source/obj/daemon/iceccd",
                       "stat -c %s", "ARTIFACT-STAGED-OK-stage-token"):
            check(needle in stage, f"private closure stage missing {needle!r}")

    print("s4-round6-local: PASS (staging invariants + pre-extraction extra/size mutants)")


if __name__ == "__main__":
    main()
