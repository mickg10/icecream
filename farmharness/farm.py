#!/usr/bin/env python3
"""farm.py -- single-command icecream cluster orchestrator (runs on the hub, drives hosts via SSH).
Phases: up (scheduler + one iceccd per F, --network host, SSD scratch) -> test -> down.
Down runs in a context manager so a crash still tears the cluster down.
This first cut proves cross-host registration; the test phase is layered on next."""
import hashlib, io, json, os, re, secrets, subprocess, sys, tarfile, time, argparse

IMG = "icecream/farm-node:ubuntu22-gcc11-boost174"
NET = "farmnet"
SCHED_PORT = 22000
TREE = "~/icecream-review/c9488d74"          # built P50 tree (same on every host)
SCRATCH = "~/farm-scratch"                    # host SSD scratch (bind target)

# S4 (superseded): the ORIGINAL mutable-per-host layout, one fixed
# "<set>-root" path per host that `distribute` used to repair in place on
# drift. No code path in this file resolves through this dict anymore --
# every role/host now resolves via immutable_root() below, which derives a
# CONTENT-ADDRESSED path (a fresh directory named after the exact tar
# sha256 the committed manifest currently records) that is never repaired
# in place; a rebuild that changes the manifest's tar hash simply resolves
# to a different, new path. Kept only as a documented historical/migration
# reference (see S4_ROLE_ARTIFACTS.md) -- these directories may still
# physically exist on some hosts as harmless orphaned data from before this
# migration; nothing reads them anymore.
LEGACY_MUTABLE_ROOT = {
    "p43": "~/role-artifacts/p43-root",
    "p50": "~/role-artifacts/p50-root",
}
KNOWN_SETS = tuple(sorted(LEGACY_MUTABLE_ROOT))
MANIFEST_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "role-manifests")
_MANIFEST_CACHE = {}

STORE_ROOT = "~/role-artifacts/store"

def immutable_root(binary_set):
    """The CONTENT-ADDRESSED, immutable runtime root for `binary_set`:
    $HOME/role-artifacts/store/<set>/<tar-sha256>, derived from the
    committed manifest's OWN tar.sha256 -- never a fixed, mutable per-set
    path, never a symlink or alias. Two different builds of the same set
    always resolve to two different paths (the manifest's tar hash
    changed), so nothing ever needs to be edited or repaired inside an
    existing published directory: a directory at this exact path either
    already has the exact content its name asserts (verified on every
    access by preflight/publish, never just trusted from the name alone),
    or it does not exist yet."""
    if binary_set not in KNOWN_SETS:
        raise ValueError(f"unknown binary set {binary_set!r} (choices: {KNOWN_SETS})")
    manifest = load_manifest(binary_set)
    return f"{STORE_ROOT}/{binary_set}/{manifest['tar']['sha256']}"

def role_tree(binary_set):
    return TREE if binary_set is None else immutable_root(binary_set)

def load_manifest(binary_set):
    """The committed farmharness/role-manifests/{set}.json is the authority
    preflight/distribute/launch_image all consume -- exact source SHA,
    every binary's path/sha256/mode/size, the image digest, and the
    version-probe strings. S4_ROLE_ARTIFACTS.md is narrative only; this is
    what the code checks against. Any read/parse problem is reported as a
    RuntimeError (not a raw OSError/JSONDecodeError) so main()'s existing
    `except RuntimeError` prints a clean REFUSED line instead of a
    traceback -- an unreadable manifest is exactly the kind of fail-closed
    condition gap #1/#2 exist to catch, not a crash."""
    if binary_set not in _MANIFEST_CACHE:
        path = os.path.join(MANIFEST_DIR, f"{binary_set}.json")
        try:
            with open(path) as f:
                _MANIFEST_CACHE[binary_set] = json.load(f)
        except (OSError, json.JSONDecodeError) as exc:
            raise RuntimeError(f"role-manifest for {binary_set!r} unreadable ({path}): {exc}") from exc
    return _MANIFEST_CACHE[binary_set]

def sha256_remote(host, path):
    r = sh(host, f"sha256sum {path} 2>/dev/null")
    if r.returncode != 0 or not r.stdout.strip():
        return None
    return r.stdout.split()[0]

def mode_remote(host, path):
    r = sh(host, f"stat -c %a {path} 2>/dev/null")
    if r.returncode != 0 or not r.stdout.strip():
        return None
    return r.stdout.strip()

def _write_stripped(mode_str):
    """'755' -> '555', '664' -> '444' -- each octal digit with its write
    bit (2) cleared (digit & 5, since 5 = 0b101 keeps read+execute only).
    This is exactly what publish_immutable_root()'s post-verification
    `chmod -R a-w` produces from the manifest's recorded (pre-hardening)
    mode -- verified live against a real chmod -R a-w on q3. A file whose
    mode is its manifest mode with every write bit cleared is in a
    STRICTLY SAFER state than the manifest asserts, never a more
    permissive one, so accepting it as equally valid in verify_role_files()
    below is not a weakening; requiring an EXACT match instead would mean
    the read-only hardening step permanently and incorrectly reddens its
    own output on every future verification (caught live: it did, before
    this fix)."""
    return "".join(str(int(c) & 5) for c in mode_str)

def image_digest_remote(host, binary_set):
    """Round-5 finding (Deep Reviewer): the previous design inspected the
    MUTABLE IMG tag's own RepoDigests as a proxy for "is the right image
    present" -- entirely decoupled from what docker_run_detached()/
    run_client() actually launch (plan.s_img/f_img/c_img, always
    launch_image(binary_set)'s digest-qualified ref). Re-pointing the tag
    (a decoy, or ordinary housekeeping) has NOTHING to do with what would
    actually run, yet could flip this check's verdict either way. Fixed:
    inspect the EXACT digest-qualified launch reference directly.

    Deliberately does NOT compare any field of the inspect output (not
    `.Id`, not `.RepoDigests`) -- the research7 fix already proved those
    fields mean DIFFERENT things on different local docker store
    architectures (containerd-image-store hosts report `.Id` as the
    MANIFEST digest; research7's classic overlay2 store reports `.Id` as
    the CONFIG digest for the identical content -- see
    research7-docker-store-fix). Comparing either field would silently
    reintroduce that exact cross-host inconsistency. Instead: a
    successful `docker image inspect <exact-ref>` IS the whole proof --
    it means that exact reference resolves to *some* local image object,
    which is precisely what `docker run --pull=never <exact-ref>` also
    needs to succeed, store architecture never entering into it.

    Returns the digest-qualified ref itself on success (so the caller's
    simple string-equality check against `launch_image(binary_set)`
    still works unchanged), None if that exact object is not locally
    resolvable."""
    ref = launch_image(binary_set)
    r = sh(host, f"docker image inspect {ref} >/dev/null 2>&1")
    return ref if r.returncode == 0 else None

def launch_image(binary_set):
    """The image reference for docker run: digest-pinned (repo@sha256:...,
    read live from the manifest -- itself populated by reading the digest
    live off q3, never hardcoded) when a binary_set is selected; the
    mutable tag stays only as the bootstrap alias for default/unselected
    launches (binary_set=None), matching prior behavior exactly."""
    if binary_set is None:
        return IMG
    manifest = load_manifest(binary_set)
    repo = IMG.split(":", 1)[0]
    return f"{repo}@{manifest['build']['image_digest']}"

def verify_role_files(host, root, manifest):
    """Pure verification: return a list of precise problem strings (empty
    list == every manifest-listed file matches on `host` exactly, sha256
    AND the HARDENED mode). Never mutates anything. Shared by preflight()
    (which must never repair -- only refuse), revalidate_before_mutation()/
    revalidate_entire_plan(), and publish_immutable_root() (which uses
    this same check, at three separate points in its locked transaction,
    to decide correctness).

    The expected mode is ALWAYS `_write_stripped(b["mode"])` -- not
    "either the manifest mode or its write-stripped variant". This used to
    accept either, because the read-only chmod hardening ran AFTER the
    atomic rename with its failure silently ignored, so a published root
    could legitimately still be in its pre-hardening (writable) mode. LO/BO
    both flagged this as fail-OPEN: a crashed or silently-failed chmod left
    a writable final root that this check still accepted as current. Fixed
    at the source (publish_immutable_root() now chmods the TEMP tree
    BEFORE the atomic rename and REQUIRES that chmod to succeed, so nothing
    is ever renamed into its final, permanent name without already being
    hardened) -- which means this check can now safely require the
    hardened mode EXACTLY, making a regression in the hardening step
    (or an external tamper that chmods a published root back writable)
    deletion-sensitive again instead of silently tolerated."""
    problems = []
    for b in manifest["binaries"]:
        remote_path = f"{root}/{b['path']}"
        actual_sha = sha256_remote(host, remote_path)
        if actual_sha is None:
            problems.append(f"{remote_path} is absent or unreadable")
            continue
        if actual_sha != b["sha256"]:
            problems.append(f"{remote_path} sha256 mismatch (manifest {b['sha256']}, actual {actual_sha})")
            continue
        actual_mode = mode_remote(host, remote_path)
        expect_mode = _write_stripped(b["mode"])
        if actual_mode != expect_mode:
            problems.append(f"{remote_path} mode mismatch (expected hardened {expect_mode}, actual {actual_mode})")
    return problems

def _publish_script(binary_set, manifest, root, src_tar_path, incoming_cleanup):
    """Build ONE remote bash script implementing the full locked canonical
    publication transaction, under a single HOST-CANONICAL flock held for
    its ENTIRE duration -- not a hub-local lock (which only serializes
    invocations sharing this one checkout's lockfile path; a genuinely
    concurrent publisher using a different checkout, or `distribute` run
    from cron, would not see it). The lock is a real `flock()` on a
    SINGLE CANONICAL file UNDER THE TARGET HOST'S OWN FILESYSTEM
    (`$HOME/role-artifacts/.publish.lock` -- one lock file per HOST, not
    one per binary_set), acquired via `exec 9>...` + `flock -x -w120 9`
    inside this one SSH-delivered script, shared by every checkout,
    every binary_set, and every operator invoking `distribute` on this
    host. Publications for DIFFERENT sets on the same host serialize
    through this same lock -- a single well-known, auditable canonical
    lock path wins over the small amount of extra concurrency a
    per-set lock would allow (publish transactions are fast and rare
    enough that this never matters in practice).

    Steps, all inside the lock (`src_tar_path` is ALREADY a local file on
    this host by this point -- for non-q3 hosts, publish_immutable_root()
    uploads the hub-relayed tar bytes to a unique incoming filename BEFORE
    ever acquiring the lock; only the transaction that CONSUMES that file
    runs locked, same as q3 consuming its own resident tar):

    1. Pin the source tar into a Linux sealed memfd. The helper opens the
       source pathname exactly once, copies those bytes into the memfd, and
       applies F_SEAL_WRITE|F_SEAL_GROW|F_SEAL_SHRINK|F_SEAL_SEAL before
       handing the descriptor back. The hash, size, header validation, and
       extraction below all reopen only the immutable /proc/<pid>/fd/<fd>
       object -- never the mutable source pathname. This closes the same-UID
       source-tar replacement/rewrite/deletion TOCTOU that a plain open fd or
       a same-UID pathname copy would leave open.
    2. If the final name already exists: verify it (inventory + hash +
       hardened mode, step 9's check) and exit -- already-current if
       clean, a hard unrepaired failure if not. Never proceeds past here
       in that case.
    3. PRE-EXTRACTION tar header validation (round-5, Deep Reviewer):
       `tar -tf`/`tar -tvf` only ever LIST the archive's index -- never
       write a byte to disk -- so this runs entirely before extraction.
       Rejects absolute-path members, `..` path-traversal components,
       duplicate member names, any non-regular/non-directory entry type
       (symlink, device, fifo, socket), and enforces member-count
       (<=200) and total-declared-size (<=2GiB) bounds. This closes a
       real gap the OLD design had: step 5's post-extraction verify()
       can only ever see what landed WITHIN $TMP -- a tar smuggling an
       absolute-path or `../`-traversal member writes OUTSIDE $TMP
       during extraction itself, before verify() ever runs, so nothing
       downstream could have caught it. Only a listing-only, pre-
       extraction gate can.
    4. Extract into a FRESH TEMP SIBLING (never into the final name).
    5. Exact inventory check on the temp tree: every non-directory entry
       found must correspond to exactly one manifest path and vice versa
       (no extra members, nothing missing) -- catches a tar smuggling
       something the manifest never listed.
    6. Every tracked path must be a regular, non-symlink file, and its
       sha256 must match the manifest, at the PRE-hardening (original)
       mode.
    7. `chmod -R a-w` the temp tree -- REQUIRED to succeed (previously
       this ran AFTER the rename with failure silently ignored via
       `chmod ...; true`, which LO/BO both flagged as fail-open: a
       crashed/failed chmod left a writable final root that verification
       still accepted, since it tolerated either mode).
    8. Re-verify the temp tree's inventory/hash/mode, now requiring the
       HARDENED (write-stripped) mode exactly -- confirms the chmod
       actually took effect, file by file, before anything is renamed
       into its permanent name.
    9. Atomic, FAIL-LOUD rename (`mv -T`, not `mv -Tn`) of the temp
       sibling onto the final name. This is safe and correct to do
       without the old `-n` no-clobber guard: by construction, we only
       reach this line after already confirming (step 2, still holding
       the SAME lock) that the final name does not exist, and the lock
       has been held continuously since before that check, so no other
       process can have created it in between -- an unexpected rename
       failure here (permissions, disk full, ...) is now a genuine,
       surprising error worth failing loudly on, not a benign
       already-done case. Re-verify once more on the final path
       (inventory/type/hash/hardened-mode) before declaring success.

    Emits exactly one final, machine-parseable status line:
    PUBLISH-ALREADY-CURRENT / PUBLISH-OK / PUBLISH-LOCK-TIMEOUT /
    PUBLISH-TAR-HASH-MISMATCH:<sha> / PUBLISH-TAR-HEADER-INVALID:<reason> /
    PUBLISH-EXTRACT-FAILED / PUBLISH-EXISTS-BUT-FAILS:<problems> /
    PUBLISH-TEMP-VERIFY-FAILED:<problems> /
    PUBLISH-CHMOD-FAILED / PUBLISH-HARDENED-VERIFY-FAILED:<problems> /
    PUBLISH-RENAME-FAILED / PUBLISH-FINAL-VERIFY-FAILED:<problems>."""
    relpaths = [b["path"] for b in manifest["binaries"]]
    if len(relpaths) != len(set(relpaths)):
        raise RuntimeError(f"role manifest {binary_set!r} contains duplicate binary paths")
    if any(not re.fullmatch(r"[A-Za-z0-9_./+-]+", p) or p.startswith("/") or ".." in p.split("/")
           for p in relpaths):
        raise RuntimeError(f"role manifest {binary_set!r} contains an unsafe relative path")
    expected_relative = "\n".join(sorted(relpaths))
    expected_header = "\n".join(
        f"{b['path']}\tF\t{b['size']}" for b in sorted(manifest["binaries"], key=lambda x: x["path"])
    )
    filelist = "\n".join(
        f"{b['path']}\t{b['sha256']}\t{b['mode']}\t{_write_stripped(b['mode'])}"
        for b in manifest["binaries"]
    )
    # `~` is NOT expanded by bash inside double quotes (only an UNQUOTED
    # leading `~` is) -- caught live: assigning ROOT="~/role-artifacts/..."
    # silently created a directory literally named "~" under the SSH
    # session's cwd instead of under $HOME, and every later step in the
    # SAME script stayed internally consistent with that wrong path, so
    # the script still reported PUBLISH-OK. Every quoted path embedded in
    # this script uses $HOME (which DOES expand inside double quotes,
    # being ordinary parameter expansion, not tilde expansion) instead.
    store_root_expanded = STORE_ROOT.replace("~", "$HOME", 1)
    role_artifacts_expanded = store_root_expanded.rsplit("/", 1)[0]  # parent of .../store
    root_expanded = root.replace("~", "$HOME", 1)
    # Single canonical lock, one per HOST -- not one per binary_set --
    # so p43 and p50 publications on the same host also serialize
    # through it (see the docstring above for why that's the right
    # tradeoff).
    lockfile = f"{role_artifacts_expanded}/.publish.lock"
    cleanup_line = f'rm -f "{incoming_cleanup}"' if incoming_cleanup else ":"
    return f'''set -u

# Keep the sealed memfd helper alive until this script exits. Its descriptor
# is the sole source consumed by every validation/extraction command below.
cleanup_pin() {{
    if [ -n "${{PIN_HELPER_PID:-}}" ]; then
        kill "$PIN_HELPER_PID" 2>/dev/null || :
        wait "$PIN_HELPER_PID" 2>/dev/null || :
    fi
    [ -z "${{PIN_INFO:-}}" ] || rm -f -- "$PIN_INFO"
    [ -z "${{PIN_ERR:-}}" ] || rm -f -- "$PIN_ERR"
}}
PIN_HELPER_PID=""
PIN_INFO=""
PIN_ERR=""
trap 'cleanup_pin; {cleanup_line}' EXIT
mkdir -p "{store_root_expanded}/{binary_set}"
exec 9>"{lockfile}"
if ! flock -x -w 120 9; then echo "PUBLISH-LOCK-TIMEOUT"; exit 75; fi

ROOT="{root_expanded}"
TMP="{root_expanded}.tmp-$$"
SRC_TAR="{src_tar_path}"

# Pin the source pathname exactly once. The helper arms Linux
# PR_SET_PDEATHSIG(SIGKILL) before touching the source, so a SIGKILL of this
# shell cannot strand the keeper. libc's memfd_create() plus all four content
# seals and F_SEAL_SEAL provide one same-UID-resistant immutable byte object.
# The bounded copy also prevents an unexpected source rewrite from turning
# publication into an unbounded memory allocation.
PIN_INFO=$(mktemp "$HOME/role-artifacts/.publish-pin.XXXXXX")
PIN_ERR="$PIN_INFO.err"
python3 - "$SRC_TAR" "{manifest['tar']['size']}" >"$PIN_INFO" 2>"$PIN_ERR" <<'PIN_HELPER_PY' &
import ctypes, fcntl, os, signal, sys, time

try:
    # A normal EXIT trap cannot run when the publication shell is SIGKILLed.
    # Arm the kernel's parent-death signal before opening/copying the source,
    # and check the parent PID immediately before and after prctl() to close
    # the fork/arm race: if the shell dies before arming, the post-check sees
    # reparenting; if it dies after arming, the kernel delivers SIGKILL here.
    parent_pid = os.getppid()
    if parent_pid <= 1:
        raise RuntimeError("publication parent is already gone")
    libc = ctypes.CDLL(None, use_errno=True)
    try:
        prctl = libc.prctl
    except AttributeError:
        raise RuntimeError("libc.prctl unavailable")
    prctl.argtypes = [ctypes.c_int, ctypes.c_ulong, ctypes.c_ulong,
                      ctypes.c_ulong, ctypes.c_ulong]
    prctl.restype = ctypes.c_int
    if os.getppid() != parent_pid:
        raise RuntimeError("publication parent changed before PDEATHSIG arm")
    prctl_result = prctl(1, signal.SIGKILL, 0, 0, 0)  # PR_SET_PDEATHSIG
    if prctl_result != 0:
        err = ctypes.get_errno()
        raise OSError(err, os.strerror(err))
    if os.getppid() != parent_pid:
        raise RuntimeError("publication parent changed during PDEATHSIG arm")

    path = sys.argv[1]
    max_size = int(sys.argv[2])
    create = libc.memfd_create
    create.argtypes = [ctypes.c_char_p, ctypes.c_uint]
    create.restype = ctypes.c_int
    fd = create(b"farm-publish-tar", 2)  # MFD_ALLOW_SEALING
    if fd < 0:
        err = ctypes.get_errno()
        raise OSError(err, os.strerror(err))
    total = 0
    with open(path, "rb", buffering=0) as source:
        while True:
            chunk = source.read(1024 * 1024)
            if not chunk:
                break
            total += len(chunk)
            if total > max_size:
                raise RuntimeError("source exceeds manifest tar.size")
            view = memoryview(chunk)
            while view:
                written = os.write(fd, view)
                view = view[written:]
    # F_SEAL_SEAL|F_SEAL_SHRINK|F_SEAL_GROW|F_SEAL_WRITE.
    fcntl.fcntl(fd, 1033, 0x1 | 0x2 | 0x4 | 0x8 | 0x10)
    os.lseek(fd, 0, os.SEEK_SET)
    os.write(1, (str(os.getpid()) + " " + str(fd) + "\\n").encode())
    os.close(1)
    while True:
        time.sleep(3600)
except Exception as exc:
    try:
        os.write(2, ("pin failed: " + str(exc) + "\\n").encode())
    except OSError:
        pass
    os._exit(111)
PIN_HELPER_PY
PIN_HELPER_PID=$!
PIN_WAIT=0
while [ ! -s "$PIN_INFO" ]; do
    if ! kill -0 "$PIN_HELPER_PID" 2>/dev/null; then
        wait "$PIN_HELPER_PID" 2>/dev/null || :
        echo "PUBLISH-TAR-PIN-FAILED"
        exit 5
    fi
    if [ "$PIN_WAIT" -ge 1200 ]; then
        echo "PUBLISH-TAR-PIN-FAILED"
        exit 5
    fi
    PIN_WAIT=$((PIN_WAIT + 1))
    sleep 0.1
done
read -r PIN_PID PIN_FD < "$PIN_INFO"
if [ -z "$PIN_PID" ] || [ -z "$PIN_FD" ] || [ "$PIN_PID" != "$PIN_HELPER_PID" ]; then
    echo "PUBLISH-TAR-PIN-FAILED"
    exit 5
fi
PIN_TAR="/proc/$PIN_PID/fd/$PIN_FD"
if [ ! -r "$PIN_TAR" ]; then
    echo "PUBLISH-TAR-PIN-FAILED"
    exit 5
fi
# Test-only observability for the SIGKILL lifecycle gate. Production callers
# never set this path; it does not participate in the publication decision.
if [ -n "${{FARM_PUBLISH_PIN_PID_FILE:-}}" ]; then
    printf '%s %s\n' "$PIN_PID" "$PIN_FD" > "$FARM_PUBLISH_PIN_PID_FILE"
fi

# Deterministic unit-test seam; inert unless both variables are supplied by a
# local test process. Production callers never set these environment values.
if [ -n "${{FARM_PUBLISH_PIN_READY_FILE:-}}" ]; then
    touch -- "$FARM_PUBLISH_PIN_READY_FILE"
fi
if [ -n "${{FARM_PUBLISH_PIN_CONTINUE_FILE:-}}" ]; then
    PIN_DEADLINE=$((SECONDS + 120))
    while [ ! -e "$FARM_PUBLISH_PIN_CONTINUE_FILE" ]; do
        if [ "$SECONDS" -ge "$PIN_DEADLINE" ]; then
            echo "PUBLISH-TAR-PIN-FAILED"
            exit 5
        fi
        sleep 0.1
    done
fi

# verify BASE MODE_COLUMN -- MODE_COLUMN is 3 (original, pre-hardening) or
# 4 (write-stripped, post-hardening). Checks the EXACT inventory (every
# non-directory entry under BASE corresponds to exactly one manifest path
# and vice versa -- no extras, nothing missing), then that every tracked
# path is a regular, non-symlink file with the expected sha256 and mode.
verify() {{
    base="$1"; mcol="$2"
    FAIL=""
    actual_inv=$(find "$base" -mindepth 1 ! -type d 2>/dev/null | sed "s|^$base/||" | sort)
    if [ "$actual_inv" != "$EXPECTED_INV" ]; then
        FAIL="inventory-mismatch actual=[$actual_inv]"
        return
    fi
    while IFS=$'\\t' read -r path sha mode_orig mode_hard; do
        f="$base/$path"
        if [ "$mcol" = "4" ]; then expect_mode="$mode_hard"; else expect_mode="$mode_orig"; fi
        if [ -L "$f" ]; then FAIL="$FAIL $path:symlink"; continue; fi
        if [ ! -f "$f" ]; then FAIL="$FAIL $path:not-regular-file"; continue; fi
        h=$(sha256sum "$f"); h=${{h%% *}}
        [ "$h" = "$sha" ] || FAIL="$FAIL $path:sha256=$h"
        m=$(stat -c %a "$f")
        [ "$m" = "$expect_mode" ] || FAIL="$FAIL $path:mode=$m(want $expect_mode)"
    done <<'FILELIST'
{filelist}
FILELIST
}}

EXPECTED_INV=$(cat <<'RELPATHS'
{expected_relative}
RELPATHS
)

# Step 1: hash and size the SEALED PINNED tar object on THIS host, every host
# including q3. SRC_TAR is deliberately absent from every tar/hash command:
# replacement, rewrite, or deletion of that mutable pathname after pinning
# cannot influence validation or extraction.
pin_size=$(wc -c < "$PIN_TAR") || {{ echo "PUBLISH-TAR-PIN-FAILED"; exit 5; }}
if [ "$pin_size" != "{manifest['tar']['size']}" ]; then
    echo "PUBLISH-TAR-SIZE-MISMATCH:$pin_size"; exit 5
fi
tarh=$(sha256sum "$PIN_TAR") || {{ echo "PUBLISH-TAR-PIN-FAILED"; exit 5; }}
tarh=${{tarh%% *}}
if [ "$tarh" != "{manifest['tar']['sha256']}" ]; then
    echo "PUBLISH-TAR-HASH-MISMATCH:$tarh"; exit 5
fi

# Step 2: an existing final name is verified (hardened mode), never edited.
if [ -d "$ROOT" ]; then
    verify "$ROOT" 4
    if [ -z "$FAIL" ]; then echo "PUBLISH-ALREADY-CURRENT"; exit 0; fi
    echo "PUBLISH-EXISTS-BUT-FAILS:$FAIL"; exit 3
fi

# Step 3 (round-6): PRE-EXTRACTION tar header validation.  Normalize a
# leading ./ and directory slash, reject traversal/absolute names and
# normalized duplicates, then compare every regular member one-to-one
# against the committed manifest's name, type and exact per-file size.
# The post-extraction inventory remains defense-in-depth.
TAR_NAMES=$(tar -tf "$PIN_TAR") || {{ echo "PUBLISH-TAR-HEADER-INVALID:list-failed"; exit 10; }}
TAR_N=$(printf '%s\n' "$TAR_NAMES" | grep -c .)
if [ "$TAR_N" -eq 0 ] || [ "$TAR_N" -gt 200 ]; then
    echo "PUBLISH-TAR-HEADER-INVALID:member-count=$TAR_N"; exit 10
fi
if printf '%s\n' "$TAR_NAMES" | grep -qE '^/|(^|/)\.\.(/|$)'; then
    echo "PUBLISH-TAR-HEADER-INVALID:path-traversal-or-absolute"; exit 10
fi
TAR_NORMALIZED=$(printf '%s\n' "$TAR_NAMES" | sed -e 's#^\./##' -e 's#/$##')
if [ "$(printf '%s\n' "$TAR_NORMALIZED" | sort -u | wc -l)" != "$TAR_N" ]; then
    echo "PUBLISH-TAR-HEADER-INVALID:duplicate-normalized-member-names"; exit 10
fi
TAR_VERBOSE=$(tar -tvf "$PIN_TAR" --numeric-owner --quoting-style=escape) || {{ echo "PUBLISH-TAR-HEADER-INVALID:verbose-list-failed"; exit 10; }}
TAR_BADTYPES=$(printf '%s\n' "$TAR_VERBOSE" | awk 'substr($1,1,1) !~ /^[-d]$/ {{print substr($1,1,1)}}')
if [ -n "$TAR_BADTYPES" ]; then
    echo "PUBLISH-TAR-HEADER-INVALID:non-regular-entry-type"; exit 10
fi
TAR_TOTAL_SIZE=$(printf '%s\n' "$TAR_VERBOSE" | awk 'substr($1,1,1) == "-" {{sum+=$3}} END{{print sum+0}}')
if [ "$TAR_TOTAL_SIZE" -gt 2147483648 ]; then
    echo "PUBLISH-TAR-HEADER-INVALID:total-size=$TAR_TOTAL_SIZE"; exit 10
fi
# GNU tar's numeric-owner listing is mode owner/group size date time name.
# Manifest names use a safe no-space alphabet; a hostile quoted/whitespace
# name cannot be mistaken for one of them.
TAR_FILE_ROWS=$(printf '%s\n' "$TAR_VERBOSE" | awk 'substr($1,1,1) == "-" {{
    if (NF != 6) {{ print "__INVALID__"; next }}
    print $6 "\\tF\\t" $3
}}')
if printf '%s\n' "$TAR_FILE_ROWS" | grep -q '^__INVALID__'; then
    echo "PUBLISH-TAR-HEADER-INVALID:unparseable-member-name"; exit 10
fi
TAR_FILE_ROWS=$(printf '%s\n' "$TAR_FILE_ROWS" | sed 's#^\./##' | sort)
EXPECTED_HEADER=$(cat <<'MANIFEST_HEADER'
{expected_header}
MANIFEST_HEADER
)
if [ "$TAR_FILE_ROWS" != "$EXPECTED_HEADER" ]; then
    echo "PUBLISH-TAR-HEADER-INVALID:manifest-name-type-size-mismatch"; exit 10
fi
while IFS= read -r d; do
    [ -n "$d" ] || continue
    d=$(printf '%s' "$d" | sed -e 's#^\./##' -e 's#/$##')
    case "$EXPECTED_INV" in
        *"$d/"*) : ;;
        *) echo "PUBLISH-TAR-HEADER-INVALID:extra-directory=$d"; exit 10 ;;
    esac
done < <(printf '%s\n' "$TAR_VERBOSE" | awk 'substr($1,1,1) == "d" {{print $6}}')

# Step 4: extract into a fresh temp sibling. --same-permissions (-p) is
# REQUIRED here: plain `tar -x` as a non-root user applies the extracting
# process's UMASK on top of the archive's stored mode bits instead of
# reproducing them exactly (caught live in a local dry run: the real p43
# tar stores MANIFEST.tsv at mode 664, matching the manifest, but a
# umask-022 extraction silently truncated it to 644, which the new
# strict pre-hardening mode check -- correctly -- then refused; every
# publish target host extracts as the non-root mickg10 SSH user, so this
# would have hit any real host, not just the sandboxed dry run).
rm -rf "$TMP"; mkdir -p "$TMP"
if ! tar --same-permissions -xf "$PIN_TAR" -C "$TMP"; then echo "PUBLISH-EXTRACT-FAILED"; rm -rf "$TMP"; exit 1; fi

# Steps 5-6: exact inventory + hash/type at the PRE-hardening mode.
verify "$TMP" 3
if [ -n "$FAIL" ]; then echo "PUBLISH-TEMP-VERIFY-FAILED:$FAIL"; rm -rf "$TMP"; exit 2; fi

# Step 7: chmod the TEMP tree read-only -- REQUIRED to succeed.
if ! chmod -R a-w "$TMP"; then echo "PUBLISH-CHMOD-FAILED"; rm -rf "$TMP"; exit 6; fi

# Step 8: re-verify at the HARDENED mode before this is ever renamed into
# its permanent, final name.
verify "$TMP" 4
if [ -n "$FAIL" ]; then echo "PUBLISH-HARDENED-VERIFY-FAILED:$FAIL"; rm -rf "$TMP"; exit 8; fi

# Step 9: fail-loud atomic rename (never -n/no-clobber -- $ROOT is
# guaranteed absent here, confirmed under this SAME lock hold in step 2;
# a rename failure now is a genuine, surprising error, not a benign race).
if ! mv -T "$TMP" "$ROOT"; then echo "PUBLISH-RENAME-FAILED"; rm -rf "$TMP" 2>/dev/null; exit 7; fi
verify "$ROOT" 4
if [ -n "$FAIL" ]; then echo "PUBLISH-FINAL-VERIFY-FAILED:$FAIL"; exit 4; fi
echo "PUBLISH-OK"
'''

def publish_immutable_root(host, binary_set):
    """Idempotently ensure immutable_root(binary_set) exists and is
    hash-clean on `host`, under a HOST-CANONICAL lock spanning the ENTIRE
    sealed-pin->stage->verify->harden->bind transaction (see _publish_script()).
    If it already verifies (at the HARDENED mode), this is a pure no-op --
    "already-current". Otherwise it extracts a tar-hash-verified copy into
    a FRESH TEMP SIBLING (never the final name directly), verifies every
    manifest entry (exact inventory, no extras; regular non-symlink files;
    sha256) against that temp copy at the pre-hardening mode, chmods it
    read-only and REQUIRES that to succeed, re-verifies at the hardened
    mode, and only then atomically, fail-loudly renames it into the final
    immutable name, re-verifying once more on the final path.

    An EXISTING final name that fails verification is a hard, unrepaired
    failure, by design: content-addressing means a hash-named directory
    that doesn't match its own name violates the one invariant the whole
    scheme rests on (tampering, or a bug -- either way, worth a human
    looking, not a silent auto-fix). Recovery is `rm -rf` that exact path
    on that host and re-running this function -- an explicit, visible,
    separately-authorized action, never performed automatically here.

    q3 cannot reach research6/research7/q2 directly on this network
    (confirmed: ssh from q3 to research6 fails host-key verification), so
    the hub -- which reaches every host in HOSTS -- relays the tar bytes.
    For non-q3 hosts, the relayed bytes are uploaded to a unique incoming
    filename on the target host BEFORE the lock is ever acquired (a plain
    file write, nothing to protect there); the LOCKED transaction then
    consumes that local file exactly like q3 consumes its own resident
    tar -- so the in-lock script is identical for every host, including
    q3, which is also what makes the tar-hash check (step 1) uniform
    everywhere rather than only on the hub-relay path.

    The source tar is pinned into a sealed memfd before any manifest/hash/
    header/extraction consumer opens it; the mutable source pathname is never
    reopened after that pin. Returns (error, status): error=None and a human-readable status on
    success ("already-current" / "published (root was absent)"); a
    precise reason string as error (status=None) on failure. Never
    touches docker."""
    manifest = load_manifest(binary_set)
    root = immutable_root(binary_set)
    tar_name = manifest["tar"]["path"]
    incoming_cleanup = None

    if host == "q3":
        src_tar_path = f"$HOME/role-artifacts/{tar_name}"
    else:
        pull = subprocess.run(HOSTS["q3"]["ssh"] + [f"cat ~/role-artifacts/{tar_name}"],
                               capture_output=True, timeout=120)
        if pull.returncode != 0 or not pull.stdout:
            return (f"publish[{host}/{binary_set}]: could not read {tar_name} from q3: "
                    f"{pull.stderr.decode(errors='replace')[:200]}"), None
        incoming_name = f"incoming-{binary_set}-{os.getpid()}-{int(time.time() * 1000)}.tar"
        push = subprocess.run(HOSTS[host]["ssh"] + [f"mkdir -p ~/role-artifacts && cat > ~/role-artifacts/{incoming_name}"],
                               input=pull.stdout, capture_output=True, timeout=120)
        if push.returncode != 0:
            # `cat > file` CREATES the destination file before it can fail
            # partway through writing it (e.g. ENOSPC) -- caught live on a
            # near-full disk: a failed push here left a real, partial
            # multi-MB file behind that nothing else would ever clean up,
            # since the locked script's own cleanup trap (which handles
            # the SUCCESSFUL-upload case) never gets a chance to run when
            # the upload itself is what failed. Best-effort: a failed
            # cleanup here must never mask the original, more actionable
            # push error.
            sh(host, f"rm -f ~/role-artifacts/{incoming_name}")
            return (f"publish[{host}/{binary_set}]: could not upload incoming tar to {host}: "
                    f"{push.stderr.decode(errors='replace')[:200]}"), None
        src_tar_path = f"$HOME/role-artifacts/{incoming_name}"
        incoming_cleanup = f"$HOME/role-artifacts/{incoming_name}"

    script = _publish_script(binary_set, manifest, root, src_tar_path, incoming_cleanup)
    r = sh(host, script, timeout=180)
    out, rc = r.stdout, r.returncode

    line = out.strip().splitlines()[-1] if out.strip() else ""
    if line == "PUBLISH-ALREADY-CURRENT":
        return None, "already-current"
    if line == "PUBLISH-OK":
        return None, "published (root was absent)"
    if line == "PUBLISH-LOCK-TIMEOUT":
        return (f"publish[{host}/{binary_set}]: could not acquire the host-canonical publish "
                f"lock within 120s -- another publisher is (or was) mid-critical-section "
                f"on {host} for {binary_set}"), None
    if line == "PUBLISH-TAR-PIN-FAILED":
        return (f"publish[{host}/{binary_set}]: could not create and seal one immutable source-tar "
                f"byte object on {host} (source pathname was never consumed by validation/extraction)", None)
    if line.startswith("PUBLISH-TAR-SIZE-MISMATCH:"):
        return (f"publish[{host}/{binary_set}]: pinned source tar size does not match manifest.tar.size "
                f"(manifest {manifest['tar']['size']}, actual {line.split(':', 1)[1]})"), None
    if line.startswith("PUBLISH-TAR-HASH-MISMATCH:"):
        return (f"publish[{host}/{binary_set}]: source tar on {host} does not match manifest.tar.sha256 "
                f"(manifest {manifest['tar']['sha256']}, actual {line.split(':', 1)[1]})"), None
    if line.startswith("PUBLISH-TAR-HEADER-INVALID:"):
        return (f"publish[{host}/{binary_set}]: source tar on {host} failed PRE-extraction header "
                f"validation ({line.split(':', 1)[1]}) -- refused before a single byte was extracted"), None
    if line.startswith("PUBLISH-EXISTS-BUT-FAILS:"):
        problems = line.split(":", 1)[1]
        return (f"publish[{host}/{binary_set}]: {root} EXISTS but FAILS verification "
                f"({problems}) -- this violates the content-addressing invariant "
                f"(a hash-named directory must always match its own name); NOT auto-repaired "
                f"(immutable roots are never fixed in place). Manual recovery: inspect for "
                f"tampering, then `rm -rf {root}` on {host} and re-run distribute."), None
    if line == "PUBLISH-EXTRACT-FAILED":
        return f"publish[{host}/{binary_set}]: extract failed on {host} (rc={rc}): {out[-300:]}", None
    if line.startswith("PUBLISH-TEMP-VERIFY-FAILED:"):
        return (f"publish[{host}/{binary_set}]: freshly-extracted temp sibling failed verification "
                f"(never renamed into the final immutable name): {line.split(':', 1)[1]}"), None
    if line == "PUBLISH-CHMOD-FAILED":
        return f"publish[{host}/{binary_set}]: chmod -R a-w of the temp sibling FAILED on {host} -- never renamed", None
    if line.startswith("PUBLISH-HARDENED-VERIFY-FAILED:"):
        return (f"publish[{host}/{binary_set}]: temp sibling failed verification AFTER hardening "
                f"(never renamed into the final immutable name): {line.split(':', 1)[1]}"), None
    if line == "PUBLISH-RENAME-FAILED":
        return f"publish[{host}/{binary_set}]: atomic rename into {root} FAILED on {host} (rc={rc})", None
    if line.startswith("PUBLISH-FINAL-VERIFY-FAILED:"):
        return (f"publish[{host}/{binary_set}]: final immutable path {root} failed verification "
                f"after publish: {line.split(':', 1)[1]}"), None
    return f"publish[{host}/{binary_set}]: unrecognized publish script output (rc={rc}): {out[-300:]!r}", None

def preflight(host, binary_set):
    """PURE, GENUINELY read-only resolution check: root presence + every
    manifest file's sha256 AND mode (catches a same-version, wrong-hash
    swap -- e.g. a trivial rebuild of one file -- which a version-string
    check alone would miss) + the pinned image's live RepoDigest. Zero
    `docker run` of any kind (`docker image inspect` is a read-only
    metadata query, not a container launch) -- no version-probe container
    is launched here or anywhere in resolution. Not role-specific (a
    host+set's root/image identity doesn't depend on which role will use
    it). BO's preferred, simpler design: rather than a separate throwaway
    probe container proving only that SOME container with this mount
    COULD run the binary and print the right banner, identity is proven
    by exact executable hash (stronger than a banner) at resolution time,
    and independently reconfirmed from INSIDE the actually-launched
    container after it starts (see verify_launched_container_identity() in
    up()/run_client()) -- so there is no separate "launch-validation"
    phase to be honest or dishonest about here at all. Returns None on
    success, a precise reason string on failure. Never raises, never
    touches anything -- an absent or wrong root is refused here, never
    repaired here (see publish_immutable_root()'s docstring for why that
    split matters)."""
    manifest = load_manifest(binary_set)
    root = immutable_root(binary_set)

    if sh(host, f"test -d {root}").returncode != 0:
        return f"preflight[{host}/{binary_set}]: {root} is absent (run: python3 farm.py distribute --sets {binary_set} --hosts {host})"

    problems = verify_role_files(host, root, manifest)
    if problems:
        return f"preflight[{host}/{binary_set}]: " + "; ".join(problems)

    expect_digest = launch_image(binary_set)
    actual_digest = image_digest_remote(host, binary_set)
    if actual_digest != expect_digest:
        return (f"preflight[{host}/{binary_set}]: launch image object {expect_digest} is not "
                f"locally resolvable on {host} (docker image inspect on that exact ref failed) "
                f"-- run: python3 farm.py distribute --sets {binary_set} --hosts {host}")
    return None

HUB_LOG_DIR = os.path.expanduser("~/.farm-hub-logs")   # deliberately OUTSIDE the git
    # checkout: HUB_DIR (the checkout's own farmharness/ directory) is used
    # ONLY for sourcing hash-pinned harness scripts (see
    # verify_harness_scripts()) -- writing log output there would pollute
    # a tracked source directory with an untracked file on every run.

def log_launch(line):
    """Print (the existing farm.py convention -- everything else in this
    file reports via stdout, captured by callers/tests already) AND append
    to a durable hub-side log file (HUB_LOG_DIR, not the checkout), so
    PREFLIGHT-OK/PREFLIGHT-FAIL markers survive past a single captured-
    stdout run too. Best-effort on the file half: an unwritable log must
    never itself block a launch decision."""
    print(line)
    try:
        os.makedirs(HUB_LOG_DIR, exist_ok=True)
        with open(os.path.join(HUB_LOG_DIR, "farm-launch.log"), "a") as f:
            f.write(line + "\n")
    except OSError:
        pass

def resolve_role(host, binary_set, role):
    """Resolve the /work bind-source and docker image for one role --
    PURE identity-binding resolution (preflight()): zero docker actions of
    any kind. binary_set=None reproduces prior behavior exactly (TREE, the
    mutable IMG tag, no preflight -- nothing existing breaks).

    A selected set is ONLY preflighted here -- never published. Bringing
    role-artifacts onto a host is exclusively the explicit `distribute`
    subcommand's job; the launch path must refuse (not silently fetch) if
    that step wasn't already run, per the spec's fail-closed requirement.
    Raises RuntimeError with the exact reason on any preflight failure,
    logging a PREFLIGHT-FAIL line first; logs PREFLIGHT-OK and returns the
    resolved (tree, image) pair on success. The caller must not perform any
    docker action for this role/host until this call returns normally."""
    if binary_set is None:
        return TREE, IMG
    err = preflight(host, binary_set)
    if err:
        log_launch(f"PREFLIGHT-FAIL host={host} role={role} set={binary_set} reason={err}")
        raise RuntimeError(err)
    log_launch(f"PREFLIGHT-OK host={host} role={role} set={binary_set}")
    return immutable_root(binary_set), launch_image(binary_set)

def revalidate_before_mutation(host, binary_set):
    """Race-gate defense, called immediately before the FIRST
    cluster-mutating action for one role/host inside up()/run_client().
    resolve_launch_plan() already validated every role in this plan
    before up()/run_client() were even called -- this closes the
    remaining TOCTOU window between that resolution and THIS role's
    actual container start, in case anything (a concurrent publish, an
    out-of-band tamper) touched the selected immutable root in between.
    Re-running preflight() here is cheap (a handful of read-only SSH
    calls, zero docker actions of its own) and is the second of the two
    independent defenses against a race on an immutable root -- the first
    being that publish_immutable_root() never writes into an existing
    final name at all. binary_set=None is a no-op, matching every other
    function in this file's convention."""
    if binary_set is None:
        return
    err = preflight(host, binary_set)
    if err:
        log_launch(f"PREFLIGHT-FAIL host={host} set={binary_set} stage=revalidate-before-mutation reason={err}")
        raise RuntimeError(f"REVALIDATION FAILED immediately before launch (immutable root changed "
                            f"since resolution): {err}")

class LaunchPlan:
    """Every role resolved for one up[-test][-down] invocation, in the
    order resolve_launch_plan() resolves them (S, then all F, then C when
    used). Carrying every resolution in one object -- rather than up() and
    run_client() each re-resolving their own roles, as before -- is what
    makes "resolve EVERYTHING before mutating ANYTHING" possible without a
    double preflight run: there is exactly one place any role/host
    combination is ever preflighted for a given invocation, and it runs
    entirely before up()/run_client() perform a single docker/scratch/push
    action. Also carries the binary_set each role resolved against
    (binary_set_s/binary_set_f/binary_set_c), purely so up()/run_client()
    can call revalidate_before_mutation() with the right set immediately
    before each role's actual container start -- the race-gate's
    TOCTOU-closing second check, independent of the immutability
    publish_immutable_root() itself already provides."""
    __slots__ = ("s_tree", "s_img", "binary_set_s", "f_resolved", "binary_set_f",
                 "c_tree", "c_img", "binary_set_c")
    def __init__(self, s_tree, s_img, binary_set_s, f_resolved, binary_set_f,
                 c_tree=None, c_img=None, binary_set_c=None):
        self.s_tree, self.s_img, self.binary_set_s = s_tree, s_img, binary_set_s
        self.f_resolved, self.binary_set_f = f_resolved, binary_set_f
        self.c_tree, self.c_img, self.binary_set_c = c_tree, c_img, binary_set_c

def resolve_launch_plan(worker_hosts, binary_set_s, binary_set_f, client_host=None, binary_set_c=None):
    """Resolve the COMPLETE requested role plan -- S, every F host, and C
    (only when client_host is not None, i.e. the phase will actually use a
    client) -- in ONE genuinely read-only pass (resolve_role() ->
    preflight()), entirely before up()/run_client() perform a single
    docker removal/start, scratch write, or client push. Root presence,
    per-file sha256/mode, and the image digest only -- zero `docker run`
    of any kind, for every role, in order.

    Raises RuntimeError with the exact reason on the first role that
    fails; by construction, at the moment this raises, NOTHING has
    happened for ANY role in this plan -- including roles that resolved
    successfully earlier in this same pass. That is the ordering fix this
    function exists for: previously, up() resolved+launched the
    scheduler, THEN resolved+launched each worker in the same loop, so a
    bad second worker was only discovered after the scheduler and first
    worker were already torn down and started (LO's finding on 02622ab6).

    There used to be a second, launch-classified pass here that ran a
    throwaway version-probe container per role after this one succeeded
    (a still-defensible design LO/BO both initially accepted) -- removed
    per BO's stronger, simpler recommendation: an exact executable hash
    (already checked above, host-side) is a stronger identity proof than
    a probe's printed banner, and the residual value a probe had --
    confirming what a container ACTUALLY sees through its mount, not just
    what SSH sees on the host side -- is now provided by
    verify_launched_container_identity() checking the REAL, in-use
    container from the INSIDE, in up()/run_client(), after it has
    actually started. That removes an entire class of "is resolution
    honestly docker-free" bookkeeping this function used to need, because
    there is no longer any docker action anywhere in it to be honest or
    dishonest about.

    Neither up() nor run_client() resolve or preflight anything
    themselves -- they only ever consume a LaunchPlan that this function
    already validated in full."""
    s_tree, s_img = resolve_role(SCHED_HOST, binary_set_s, "S")
    f_resolved = [resolve_role(h, binary_set_f, "F") for h in worker_hosts]
    c_tree = c_img = None
    if client_host is not None:
        c_tree, c_img = resolve_role(client_host, binary_set_c, "C")
    return LaunchPlan(s_tree, s_img, binary_set_s, f_resolved, binary_set_f,
                       c_tree, c_img, binary_set_c)

def revalidate_entire_plan(worker_hosts, plan, client_host=None):
    """Zero-mutation WHOLE-PLAN barrier: re-preflight EVERY role already
    resolved into `plan` -- S, every F, and C (when used) -- in ONE pass,
    called immediately before the first mutating action of this
    invocation (see main(): the old `down_needed = True` set right after
    resolve_launch_plan() succeeded is replaced by MUTATIONS, a tracker
    set only from inside the actual mutating primitives themselves, so a
    refusal HERE -- like a resolve_launch_plan() refusal -- can never
    trigger down()).

    Why this exists on top of resolve_launch_plan() (which already
    preflighted every role once) and revalidate_before_mutation() (which
    re-preflights ONE role immediately before THAT role's own mutation):
    resolve_launch_plan() proves every role was valid AT THE MOMENT IT,
    INDIVIDUALLY, was resolved -- S first, then each F in turn, then C.
    Between the moment S is resolved and the moment the LAST F (or C) is
    resolved, nothing has re-checked S; and between the moment resolution
    of the WHOLE plan finishes and the moment up() performs its very
    first mutation, nothing has re-checked ANYTHING yet either. A tamper
    landing in either of those windows -- on any role, not just the one
    about to be mutated -- would previously only be caught by
    revalidate_before_mutation() at THAT role's own turn, which means an
    EARLIER role could already have been torn down and relaunched before
    a LATER role's corruption is discovered. This barrier closes that gap
    by re-checking the WHOLE plan, atomically with respect to this
    function's caller (nothing else runs between this returning clean
    and main() proceeding into the first mutation), immediately before
    ANY role is touched -- so a tamper anywhere in the plan is caught
    before ANYTHING in the plan is touched, not merely before its own
    role's turn.

    revalidate_before_mutation() remains as defense-in-depth, unchanged:
    it catches anything that lands AFTER this barrier clears but before
    that specific role's own mutation (up()'s S->F loop and run_client()
    each take real wall-clock time -- docker pull/start, useradd, etc. --
    during which a LATER role in the same invocation is still exposed).
    Neither check replaces the other; both are required, independently,
    for the guarantee LO/BO asked for: zero mutations before a clean
    whole-plan revalidation, AND zero mutation of role N+1 without a
    fresh check of role N+1 specifically.

    Raises RuntimeError with the exact reason on the first role that
    fails. By construction, at the moment this raises, this invocation
    has not mutated anything: MUTATIONS.started is still False, because
    nothing that sets it has run yet (this is always called, in main(),
    strictly before up()/run_client() are ever invoked)."""
    err = preflight(SCHED_HOST, plan.binary_set_s)
    if err:
        log_launch(f"PREFLIGHT-FAIL host={SCHED_HOST} role=S set={plan.binary_set_s} stage=revalidate-entire-plan reason={err}")
        raise RuntimeError(f"BARRIER REVALIDATION FAILED for S on {SCHED_HOST} "
                            f"(immutable root or image changed since resolution): {err}")
    for h in worker_hosts:
        err = preflight(h, plan.binary_set_f)
        if err:
            log_launch(f"PREFLIGHT-FAIL host={h} role=F set={plan.binary_set_f} stage=revalidate-entire-plan reason={err}")
            raise RuntimeError(f"BARRIER REVALIDATION FAILED for F on {h} "
                                f"(immutable root or image changed since resolution): {err}")
    if client_host is not None:
        err = preflight(client_host, plan.binary_set_c)
        if err:
            log_launch(f"PREFLIGHT-FAIL host={client_host} role=C set={plan.binary_set_c} stage=revalidate-entire-plan reason={err}")
            raise RuntimeError(f"BARRIER REVALIDATION FAILED for C on {client_host} "
                                f"(immutable root or image changed since resolution): {err}")
    log_launch(f"BARRIER-OK plan S={plan.binary_set_s} F={plan.binary_set_f} C={plan.binary_set_c} "
               f"workers={worker_hosts} client={client_host}")

class _MutationTracker:
    """Tracks whether ANY real cluster-mutating primitive has actually
    run yet during this process's current invocation of main(). Replaces
    the old `down_needed = True`, which used to be set immediately after
    resolve_launch_plan() succeeded -- i.e. BEFORE revalidate_entire_plan()
    or up() performed a single real action -- so a barrier-revalidation
    failure would still have left down_needed True, and down() would then
    tear down a possibly-unrelated, pre-existing, exact-name running
    cluster that this invocation never touched (LO/BO's finding).

    `mark()` is called from inside the actual mutating primitives
    themselves -- docker_rm(), scratch_prepare(), docker_run_detached(),
    docker_run_foreground_staged() -- never from any call site above
    them (not from up(), not from run_client(), not from main()). That
    is what makes it
    structurally impossible for `started` to become True without a real
    mutating action having actually run at least once: there is no code
    path that sets it in anticipation of a mutation, only ones that set
    it AS one already happened."""
    def __init__(self):
        self.started = False
    def mark(self):
        self.started = True

MUTATIONS = _MutationTracker()

def _race_gate_pause():
    """Inert by default -- ZERO behavior change in every normal
    invocation. A test-only seam, gated entirely by two environment
    variables that nothing in this file ever sets (only an external test
    harness sets them, in the environment of a subprocess it launches
    running `python3 farm.py ...`), letting that harness pause a REAL
    main() between plan resolution and the whole-plan barrier
    (revalidate_entire_plan()) so it can inject genuine concurrent
    activity -- a production distribute() call, or a tamper attempt --
    against the SAME plan this invocation just resolved, using the SAME
    physical host+root it actually selected, then let this invocation
    proceed into revalidate_entire_plan() and prove the barrier catches
    it, with zero mutations recorded and down() never called.

    FARM_RACE_READY_FILE: if set, this function touches that file the
    moment it's called -- signaling "resolve_launch_plan() just returned,
    about to enter revalidate_entire_plan()" to whatever is watching.

    FARM_RACE_CONTINUE_FILE: if set, this function then blocks (polling,
    bounded so a broken test can't hang main() forever) until that file
    appears, before returning control to main() to proceed into
    revalidate_entire_plan().

    Called from main() exactly once per invocation, immediately after
    resolve_launch_plan() succeeds and before revalidate_entire_plan()."""
    ready = os.environ.get("FARM_RACE_READY_FILE")
    cont = os.environ.get("FARM_RACE_CONTINUE_FILE")
    if not ready and not cont:
        return
    if ready:
        with open(ready, "w") as f:
            f.write("ready\n")
    if cont:
        deadline = time.time() + 120
        while not os.path.exists(cont):
            if time.time() > deadline:
                raise RuntimeError(f"_race_gate_pause: continue file {cont} never appeared within 120s")
            time.sleep(0.2)

# per-host SSH argv + LAN IP + role capability
HOSTS = {
    "q3":        {"ssh": ["ssh","-o","HostName=10.0.27.101","-o","HostKeyAlias=tt-quietbox3","-o","BatchMode=yes","mickg10@tt-quietbox3"], "ip": "10.0.27.101"},
    "research6": {"ssh": ["ssh","-o","BatchMode=yes","research6"],                                                                        "ip": "10.0.27.56"},
    "research7": {"ssh": ["ssh","-o","BatchMode=yes","research7"],                                                                        "ip": "10.0.27.58"},
    "q2":        {"ssh": ["ssh","-o","HostName=100.91.242.69","-o","BatchMode=yes","mickg10@tt-quietbox2"],                               "ip": "10.0.27.212"},
}
SCHED_HOST = "q3"

def sh(host, cmd, timeout=120, check=False):
    r = subprocess.run(HOSTS[host]["ssh"] + [cmd], capture_output=True, text=True, timeout=timeout)
    if check and r.returncode != 0:
        raise RuntimeError(f"[{host}] {cmd!r} rc={r.returncode}: {r.stderr.strip()[:300]}")
    return r

def sh_stdin(host, cmd, input_bytes, timeout=120):
    """Like sh(), but pipes `input_bytes` (raw bytes, not text) into the
    remote command's own stdin over the SAME SSH connection -- the
    mechanism run_client() uses to stream the harness bundle directly
    into a foreground `docker run -i`'s stdin (which docker, in turn,
    connects to the container's own stdin). Returns a result object with
    the same .stdout/.stderr/.returncode shape as sh()'s, decoded as
    text (replacing undecodable bytes) for uniform handling by callers
    that only ever expect these commands' own text output, never binary
    passthrough on the way OUT."""
    r = subprocess.run(HOSTS[host]["ssh"] + [cmd], input=input_bytes, capture_output=True, timeout=timeout)
    class _Result:
        pass
    out = _Result()
    out.returncode = r.returncode
    out.stdout = r.stdout.decode(errors="replace")
    out.stderr = r.stderr.decode(errors="replace")
    return out

def docker_rm(host, name):
    MUTATIONS.mark()
    sh(host, f"docker rm -f {name} 2>/dev/null; true")

def docker_run_detached(host, name, tree, img, inner_cmd):
    """Start a detached (docker run -d), named, persistent container.
    Together with docker_rm(), this is the ONLY docker-mutating primitive
    up() uses -- factored out of the inline sh() calls that used to be
    here specifically so a no-network test can monkeypatch exactly these
    two named functions (and nothing else, no command-text pattern
    matching needed) to prove zero containers are touched for a launch
    plan that never made it past resolution. The selected host root is
    mounted read-only at `/artifact-source`; `/work` is a private tmpfs
    populated and verified by the caller before the role starts. This
    closes same-inode host truncation as well as directory-entry swaps --
    an FD to a live bind is not an immutable byte object. Logs/state go to
    the separate `/scratch` mount, which stays writable.

    `inner_cmd` is expected to already carry its staging and attestation prefixes
    (see _attestation_prefix()) when the caller wants one -- this
    function itself is attestation-agnostic, same as it's tree/set-
    agnostic; it just runs whatever single command string it's given as
    the container's `bash -c` argument (PID 1), never touching or
    parsing it.

    `--pull=never` (round-5, Deep Reviewer): `img` is always
    launch_image(binary_set) -- an exact, manifest-pinned, digest-
    qualified reference, already confirmed locally resolvable by
    preflight()/image_digest_remote() before this ever runs. Without
    `--pull=never`, an absent-locally object would make `docker run`
    silently fall back to a network pull (Docker's default `--pull`
    policy is `missing`) instead of refusing outright -- a fail-open
    path this design has no legitimate use for (every image this
    launches is supposed to already be local, verified, and pinned)."""
    MUTATIONS.mark()
    if "ARTIFACT-STAGED-OK-" in inner_cmd:
        work_mount = f"-v {tree}:/artifact-source:ro --tmpfs /work:rw,exec"
    else:
        # Preserve the pre-S4 default (binary_set=None) path, whose legacy
        # TREE is already the runtime tree and has no manifest closure to
        # stage.  Selected sets always take the private branch above.
        work_mount = f"-v {tree}:/work:ro"
    sh(host, f"docker run -d --pull=never --name {name} --network host {work_mount} -v {SCRATCH}:/scratch -u 0:0 {img} "
             f"bash -c '{inner_cmd}'", check=True)

def scratch_prepare(host, mkdir_path, log_path):
    """Reset the SCRATCH-relative logging area on host before a launch.
    The ONLY scratch-mutating primitive up() uses -- factored out for the
    same reason as docker_run_detached() above."""
    MUTATIONS.mark()
    sh(host, f"mkdir -p {mkdir_path} && chmod 1777 {SCRATCH}/farm && rm -f {log_path}", check=True)

def _attestation_token():
    """A short, unguessable, per-container-invocation token, used only to
    make this invocation's ARTIFACT-ATTEST-OK marker un-mistakable for a
    stale marker left in `docker logs` by some earlier container that
    happened to reuse the same name (see _attestation_prefix())."""
    return secrets.token_hex(8)

ARTIFACT_STAGE_ROOT = "/work"

def _artifact_stage_prefix(binary_set, token):
    """Stage and verify the complete selected artifact closure privately.

    `/artifact-source` is the host bind and is only an input.  Every
    manifest-listed file is copied into the container-private `/work` tmpfs,
    then checked there for exact relative path, regular-file type, byte size,
    sha256, and hardened mode.  A source rewrite/truncate during the copy can
    therefore only produce a rejected private copy.  Staging the complete
    closure covers C's icecc, icecc-create-env, iceccd, and every other
    selected artifact-root file opened by farm_client/replay.
    """
    if binary_set is None:
        return ""
    manifest = load_manifest(binary_set)
    paths = [b["path"] for b in manifest["binaries"]]
    if len(paths) != len(set(paths)):
        raise RuntimeError(f"artifact manifest {binary_set!r} contains duplicate paths")
    if any(not re.fullmatch(r"[A-Za-z0-9_./+-]+", p) or p.startswith("/") or ".." in p.split("/")
           for p in paths):
        raise RuntimeError(f"artifact manifest {binary_set!r} contains an unsafe relative path")
    expected = "\n".join(sorted(paths))
    rows = "\n".join(
        f"{b['path']}\t{b['sha256']}\t{b['size']}\t{_write_stripped(b['mode'])}"
        for b in manifest["binaries"]
    )
    parts = [
        "mkdir -p /work",
        "find /work -mindepth 1 -maxdepth 1 -exec rm -rf -- {} +",
    ]
    for p in paths:
        parts.extend([
            f'if [ -L "/artifact-source/{p}" ] || [ ! -f "/artifact-source/{p}" ]; then echo ARTIFACT-STAGE-FAIL; exit 98; fi',
            f'mkdir -p "$(dirname "/work/{p}")"',
            f'if ! cp -- "/artifact-source/{p}" "/work/{p}"; then echo ARTIFACT-STAGE-FAIL; exit 98; fi',
        ])
    parts.extend([
        "STAGE_INV=$(find /work -mindepth 1 ! -type d -printf \"%P\\n\" 2>/dev/null | sort)",
        "EXPECTED_STAGE_INV=$(cat <<ARTIFACT_PATHS\n" + expected + "\nARTIFACT_PATHS\n)",
        'if [ "$STAGE_INV" != "$EXPECTED_STAGE_INV" ]; then echo ARTIFACT-STAGE-FAIL; exit 98; fi',
        "while read -r path sha size mode; do\n"
        '  f="/work/$path"; if [ -L "$f" ] || [ ! -f "$f" ]; then echo ARTIFACT-STAGE-FAIL; exit 98; fi\n'
        '  h=$(sha256sum "$f"); h=${h%% *}; [ "$h" = "$sha" ] || { echo ARTIFACT-STAGE-FAIL; exit 98; }\n'
        '  s=$(stat -c %s "$f"); [ "$s" = "$size" ] || { echo ARTIFACT-STAGE-FAIL; exit 98; }\n'
        '  m=$(stat -c %a "$f"); [ "$m" = "$mode" ] || { echo ARTIFACT-STAGE-FAIL; exit 98; }\n'
        "done <<ARTIFACT_ROWS\n" + rows.replace("\t", " ") +
        "\nARTIFACT_ROWS\n"
        "if ! chmod -R a-w /work; then echo ARTIFACT-STAGE-FAIL; exit 98; fi\n"
        f"echo ARTIFACT-STAGED-OK-{token}",
    ])
    return "; ".join(parts) + "; "

def _attestation_prefix(binary_set, token, role_binary_path=None):
    """Build a bash snippet -- safe to embed inside a single-quoted
    `bash -c '...'` docker argument; it contains no single quotes
    anywhere -- that is meant to run as the FIRST thing inside a role
    container's own launch command, BEFORE the real role binary is ever
    exec'd. For every file manifest[binary_set] tracks, it checks (from
    INSIDE the container, through its own /work bind mount) that the file
    is a regular, non-symlink file and that its sha256 matches the
    manifest EXACTLY. If every check passes, it prints the unique marker
    `ARTIFACT-ATTEST-OK-<token>` and falls through to whatever real
    command the caller appends after this prefix. If ANY check fails, it
    prints `ARTIFACT-ATTEST-FAIL` and calls `exit 97` -- which, because
    this text runs as the container's own PID 1 (`bash -c 'PREFIX;
    REAL_CMD'`), terminates the ENTIRE container immediately: the real
    role binary is never reached, not merely "not yet verified". This is
    the load-bearing difference from the round-3 design it replaces
    (verify_launched_container_identity(), a `docker exec` run AFTER the
    role process had already started): attestation here is baked directly
    into the same shell invocation that starts the role process, so there
    is no window, however small, during which an unattested container is
    already running the real binary.

    `role_binary_path` is retained as a caller-side assertion naming the
    executable whose already-staged private path the caller will exec. The
    staging prefix has copied and verified *all* manifest files before this
    function runs, so this attestation rechecks the private closure rather
    than the live host bind. There is deliberately no FD to a host-owned
    inode here: same-inode truncation after an FD hash would still change
    the bytes observed through that FD. The private tmpfs copy is hardened
    before its marker and role exec.

    Raises RuntimeError immediately (before returning any bash text) if
    role_binary_path is given but isn't one of manifest[binary_set]'s
    own tracked paths -- a caller-side programming error, not something
    that should ever reach a real container.

    binary_set=None returns "" (a true no-op prefix), matching every
    other function in this file's convention -- the caller's real command
    then runs exactly as it did before attestation existed."""
    if binary_set is None:
        return ""
    manifest = load_manifest(binary_set)
    tracked_paths = {b["path"] for b in manifest["binaries"]}
    if role_binary_path is not None and role_binary_path not in tracked_paths:
        raise RuntimeError(f"_attestation_prefix: role_binary_path {role_binary_path!r} is not a "
                            f"tracked file in binary_set {binary_set!r}'s manifest ({sorted(tracked_paths)})")
    parts = []
    for b in manifest["binaries"]:
        remote = f"/work/{b['path']}"
        parts.append(f'if [ -L "{remote}" ] || [ ! -f "{remote}" ]; then echo ARTIFACT-ATTEST-FAIL; exit 97; fi')
        parts.append(f'h=$(sha256sum "{remote}"); h=${{h%% *}}')
        parts.append(f'if [ "$h" != "{b["sha256"]}" ]; then echo ARTIFACT-ATTEST-FAIL; exit 97; fi')
    parts.append(f"echo ARTIFACT-ATTEST-OK-{token}")
    return "; ".join(parts) + "; "

def wait_for_attestation(host, name, token, timeout=30):
    """Poll `docker logs NAME` for this invocation's unique
    ARTIFACT-ATTEST-OK-<token> marker. This is a proof-COLLECTION step,
    not itself part of the gate: the gate is the container's own launch
    command refusing to exec the real role binary (see
    _attestation_prefix()), which has already succeeded or failed by the
    time `docker run`/`docker exec` returns -- this function exists so
    the caller gets a clear, fast, explicit RuntimeError instead of
    silently proceeding to treat a dead-on-arrival container as healthy.
    Fails fast on an explicit ARTIFACT-ATTEST-FAIL line rather than
    waiting out the full timeout. Raises RuntimeError on either an
    explicit failure or a timeout with neither line seen (e.g. the
    container crashed before logging anything at all) -- never silently
    treated as still-pending."""
    marker = f"ARTIFACT-ATTEST-OK-{token}"
    deadline = time.time() + timeout
    r = None
    while time.time() < deadline:
        r = sh(host, f"docker logs {name} 2>&1", timeout=15)
        if "ARTIFACT-STAGE-FAIL" in r.stdout:
            raise RuntimeError(f"in-container artifact staging FAILED for {name} on {host} "
                                f"(private closure was not accepted -- docker logs: {r.stdout[-400:]})")
        if "ARTIFACT-ATTEST-FAIL" in r.stdout:
            raise RuntimeError(f"in-container attestation FAILED for {name} on {host} "
                                f"(container refused to start the real role binary -- "
                                f"docker logs: {r.stdout[-400:]})")
        if marker in r.stdout:
            return
        time.sleep(0.5)
    tail = r.stdout[-400:] if r is not None else "(no docker logs output collected)"
    raise RuntimeError(f"in-container attestation marker {marker!r} never appeared for {name} "
                        f"on {host} within {timeout}s (docker logs: {tail!r})")

def up(worker_hosts, plan):
    """Launch the cluster from an ALREADY-validated LaunchPlan -- up()
    itself no longer resolves or preflights anything: every role in this
    plan was already identity-bound, for every host, inside
    resolve_launch_plan() (and, atomically with respect to any mutation,
    re-bound by revalidate_entire_plan()'s whole-plan barrier) before
    this function was ever called. up() therefore performs ONLY the
    mutating actions (docker_rm, scratch_prepare, docker_run_detached),
    each immediately preceded by revalidate_before_mutation() -- a cheap,
    zero-docker-run re-check that the SAME immutable root already
    validated is still exactly what it was (defense-in-depth on top of
    the barrier: the barrier proves the WHOLE plan clean at one instant
    immediately before the FIRST mutation; this closes the window a
    later role is still exposed to while an earlier role in this same
    loop is being launched, which takes real wall-clock time). Identity
    is no longer separately reconfirmed AFTER each container starts
    (the round-3 design, verify_launched_container_identity() /
    `docker exec ... sha256sum`): each container's own launch command now
    carries an ATTESTATION PREFIX (_attestation_prefix()) that hashes
    every tracked file from INSIDE the container, through its own /work
    mount, and refuses to exec the real role binary at all on any
    mismatch -- so identity is proven (or the container dies trying)
    strictly BEFORE the role process can start, not after. up() waits for
    each container's ARTIFACT-ATTEST-OK marker (wait_for_attestation())
    immediately after starting it, purely to fail fast with a clear
    RuntimeError rather than silently racing ahead. up() can no longer
    discover a bad role partway through a launch that already tore down
    or started an earlier one (the defect LO/BO both flagged on
    02622ab6)."""
    sip = HOSTS[SCHED_HOST]["ip"]
    print(f"UP: scheduler on {SCHED_HOST}({sip}):{SCHED_PORT}  workers={worker_hosts}  "
          f"S-tree={plan.s_tree}  F-trees={[t for t, _ in plan.f_resolved]}")
    # scheduler (--network host so it binds the LAN IP). The daemon/scheduler drop privileges to
    # icecc at startup, so the -l log dir must be writable by that user => useradd icecc + 1777 dir.
    revalidate_before_mutation(SCHED_HOST, plan.binary_set_s)
    docker_rm(SCHED_HOST, "farm-sched")
    scratch_prepare(SCHED_HOST, f"{SCRATCH}/farm", f"{SCRATCH}/farm/sched.log")
    s_token = _attestation_token()
    s_stage_token = _attestation_token()
    docker_run_detached(SCHED_HOST, "farm-sched", plan.s_tree, plan.s_img,
        _artifact_stage_prefix(plan.binary_set_s, s_stage_token) +
        _attestation_prefix(plan.binary_set_s, s_token, "obj/scheduler/icecc-scheduler") +
        f"useradd -r icecc 2>/dev/null; exec /work/obj/scheduler/icecc-scheduler -p {SCHED_PORT} -n {NET} -l /scratch/farm/sched.log -vvv")
    wait_for_attestation(SCHED_HOST, "farm-sched", s_token)
    time.sleep(3)
    # one worker per F -- every host's role was already resolved, in order,
    # inside resolve_launch_plan(), before this loop (or anything else in
    # this function) performed a single docker/scratch action.
    for i, h in enumerate(worker_hosts):
        wp = 12000 + i
        f_tree, f_img = plan.f_resolved[i]
        revalidate_before_mutation(h, plan.binary_set_f)
        docker_rm(h, "farm-worker")
        # chmod only the mickg-owned dir (not -R: stale daemon files are uid-999, unchmod-able by host user; rm clears them)
        scratch_prepare(h, f"{SCRATCH}/farm/envs", f"{SCRATCH}/farm/worker.log")
        f_token = _attestation_token()
        f_stage_token = _attestation_token()
        docker_run_detached(h, "farm-worker", f_tree, f_img,
            _artifact_stage_prefix(plan.binary_set_f, f_stage_token) +
            _attestation_prefix(plan.binary_set_f, f_token, "obj/daemon/iceccd") +
            f"useradd -r -s /usr/sbin/nologin icecc 2>/dev/null; "
            f"chown icecc /scratch/farm/envs; "  # pre-chown as root: daemon's cleanup_cache runs post-drop (no CAP_CHOWN)
            f"exec /work/obj/daemon/iceccd -m 8 -s {sip}:{SCHED_PORT} -n {NET} -N {h}w -b /scratch/farm/envs "
            f"-p {wp} -l /scratch/farm/worker.log -vvv")
        wait_for_attestation(h, "farm-worker", f_token)
    # wait for all workers to register
    want = len(worker_hosts)
    got = 0
    for _ in range(40):
        r = sh(SCHED_HOST, f"grep -c 'login' {SCRATCH}/farm/sched.log 2>/dev/null; true")
        try: got = int((r.stdout.strip() or "0").splitlines()[0])
        except Exception: got = 0
        if got >= want: break
        time.sleep(1)
    r = sh(SCHED_HOST, f"grep -iE 'login|Login|connect|node' {SCRATCH}/farm/sched.log 2>/dev/null | tail -8; true")
    print(f"UP: registrations={got}/{want}")
    print("SCHED LOG (tail):"); print(r.stdout.rstrip())
    if got < want:
        for h in worker_hosts:
            wl = sh(h, f"grep -iE 'scheduler|connect|login|fail|error' {SCRATCH}/farm/worker.log 2>/dev/null | tail -6; true")
            print(f"WORKER {h} LOG:"); print(wl.stdout.rstrip() or "(empty)")
    return got >= want

SCRIPTS = ["farm_client.sh", "replay.py"]   # pushed to the client host's /scratch before a test
# The checkout's OWN farmharness/ directory -- NEVER an ambient path.
# Round-4 finding (BO): this used to be a hardcoded ambient session-
# scratch path, meaning the compatibility/JOIN-replay cells executed
# bytes entirely outside the reviewed/committed SHA -- an execution-
# provenance gap, even though (confirmed) the ambient copies' CONTENT
# happened to still agree with the committed ones. Sourcing from the
# checkout itself, plus verify_harness_scripts() below, makes that
# agreement a verified invariant instead of a coincidence.
HUB_DIR = os.path.dirname(os.path.abspath(__file__))
# Frozen at commit f5d13fd9 (the original harness-freeze commit that
# first checked farm_client.sh/replay.py into this exact repo, alongside
# farm.py itself) -- these two scripts are the compatibility/JOIN-replay
# half of the test harness and aren't covered by any role-manifest, so
# their own integrity is pinned here instead, the same way every tracked
# role binary's is pinned in role-manifests/*.json.
FROZEN_HARNESS_HASHES = {
    "farm_client.sh": "f502f74e7d246569fca16c231ae5a6e43809bd68cbe4401c4c72d45798b55aa7",
    "replay.py": "427ac52d777bfdf49c98cc502d06d4279fbb373618b88b01e0e319b072d816ef",
}

def verify_harness_scripts():
    """PURE, LOCAL, read-only check: every file in SCRIPTS, read from
    HUB_DIR (the checkout's own farmharness/ directory), must hash to its
    FROZEN_HARNESS_HASHES entry EXACTLY. Raises RuntimeError -- fail-
    closed, no fallback path -- on any mismatch or on a missing/unreadable
    file. Called from run_client() before _build_harness_bundle() ever
    runs, so a tampered or drifted checkout copy of either script is
    refused before a single byte of it is even bundled, let alone
    streamed to any host or executed inside a container."""
    for s in SCRIPTS:
        path = os.path.join(HUB_DIR, s)
        try:
            data = open(path, "rb").read()
        except OSError as exc:
            raise RuntimeError(f"harness script {s!r} unreadable at {path!r}: {exc}")
        actual = hashlib.sha256(data).hexdigest()
        expect = FROZEN_HARNESS_HASHES.get(s)
        if actual != expect:
            raise RuntimeError(f"harness script {s!r} at {path!r} sha256 mismatch "
                                f"(frozen {expect!r}, actual {actual!r}) -- refusing to stage or "
                                f"execute it on any host")

def _build_harness_bundle():
    """Build a deterministic, in-memory, uncompressed tar of SCRIPTS
    (farm_client.sh, replay.py), read from HUB_DIR -- the exact same
    bytes verify_harness_scripts() (the caller's own first statement,
    always run immediately before this) just confirmed match
    FROZEN_HARNESS_HASHES. Every member gets a FIXED mtime/uid/gid/mode
    (never the checkout's own filesystem metadata, which varies run to
    run and machine to machine) so these tar bytes are a pure function
    of the two scripts' CONTENT alone -- this is what gets streamed
    directly into the client container's own stdin (round-5, Deep
    Reviewer's promoted harness-private-staging fix), never written to
    any mutable host path first."""
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w") as tf:
        for s in SCRIPTS:
            data = open(os.path.join(HUB_DIR, s), "rb").read()
            info = tarfile.TarInfo(name=s)
            info.size = len(data)
            info.mtime = 0
            info.uid = info.gid = 0
            info.uname = info.gname = ""
            info.mode = 0o555
            tf.addfile(info, io.BytesIO(data))
    return buf.getvalue()

def _harness_stage_verify(stage_token):
    """Bash snippet -- safe to embed inside a single-quoted `bash -c
    '...'` docker argument -- run INSIDE the container immediately
    after the streamed bundle has been extracted into (container-
    private, tmpfs) /scratch, BEFORE farm_client.sh is ever reached.
    Verifies what actually LANDED in tmpfs -- never the bytes that were
    sent, never the hub's own checkout copy -- matches
    FROZEN_HARNESS_HASHES exactly: exact member count (no extras,
    nothing missing), every tracked script a regular non-symlink file,
    sha256 exact. Emits HARNESS-STAGED-OK-<token> on success,
    HARNESS-STAGE-FAIL + `exit 96` on any mismatch -- structurally the
    same idea as _attestation_prefix(), just for the harness bundle
    instead of the role binaries: verify what you are about to RUN, not
    what you SENT. Because this runs as part of the container's own
    PID 1 script, a failure here terminates the container immediately,
    before farm_client.sh (let alone replay.py, let alone any role
    binary it invokes) is ever reached."""
    parts = []
    parts.append("scnt=$(find /scratch -mindepth 1 -maxdepth 1 | wc -l)")
    parts.append(f'if [ "$scnt" != "{len(SCRIPTS)}" ]; then echo HARNESS-STAGE-FAIL; exit 96; fi')
    for s in SCRIPTS:
        remote = f"/scratch/{s}"
        expect = FROZEN_HARNESS_HASHES[s]
        parts.append(f'if [ -L "{remote}" ] || [ ! -f "{remote}" ]; then echo HARNESS-STAGE-FAIL; exit 96; fi')
        parts.append(f'h=$(sha256sum "{remote}"); h=${{h%% *}}')
        parts.append(f'if [ "$h" != "{expect}" ]; then echo HARNESS-STAGE-FAIL; exit 96; fi')
    parts.append(f"echo HARNESS-STAGED-OK-{stage_token}")
    return "; ".join(parts) + "; "

def docker_run_foreground_staged(host, name, tree, img, stdin_bytes, inner_cmd, timeout=1800):
    """Run a container in the FOREGROUND (`docker run -i --rm`, never
    detached) with `stdin_bytes` piped directly into its stdin. The
    ONLY docker-mutating primitive run_client() uses for the client
    role (round-5 redesign, Deep Reviewer's promoted harness-private-
    staging fix -- supersedes the old detached-holder + `docker exec`
    + host-scratch `push_file()` design).

    `/work` is `:ro` -- role-artifact binaries, same reasoning as
    docker_run_detached(). `/scratch` is CONTAINER-PRIVATE tmpfs --
    NEVER a host bind -- the streamed harness bundle is extracted there
    and verified from INSIDE the container (_harness_stage_verify()),
    never touching mutable host storage at any point; tmpfs is also
    mounted `noexec` by default, which structurally forbids anything
    landing there from ever being exec'd directly (irrelevant to this
    design either way -- farm_client.sh/replay.py are always INVOKED
    via an interpreter, `bash .../farm_client.sh` and, internally,
    `python3 /scratch/replay.py`, never exec'd as their own standalone
    process -- confirmed live: `bash -n /scratch/farm_client.sh`
    succeeds under noexec, a direct `/scratch/farm_client.sh` invocation
    is refused with "Permission denied"). `/hostscratch` is the SAME
    host scratch directory docker_run_detached()'s callers use for
    logging, but READ-ONLY and at a DIFFERENT mount point -- the pre-
    populated, reusable fixtures this design still needs (a cmake
    install, project sources like fmt/rocksdb) live there, kept
    structurally separate from the tmpfs-only harness-staging area, and
    never writable by this container.

    `--rm` (not a persistent named container docker_rm() tears down
    later) since this is a single foreground invocation whose own exit
    ends its container's lifetime; a defensive `docker rm -f` still
    runs first, in case a stale container from an interrupted prior run
    left this name occupied."""
    MUTATIONS.mark()
    sh(host, f"docker rm -f {name} 2>/dev/null; true")
    if "ARTIFACT-STAGED-OK-" in inner_cmd:
        work_mount = f"-v {tree}:/artifact-source:ro --tmpfs /work:rw,exec"
    else:
        work_mount = f"-v {tree}:/work:ro"
    cmd = (f"docker run -i --rm --pull=never --name {name} --network host "
           f"{work_mount} --tmpfs /scratch -v {SCRATCH}:/hostscratch:ro "
           f"-u 0:0 {img} bash -c '{inner_cmd}'")
    return sh_stdin(host, cmd, stdin_bytes, timeout=timeout)

def run_client(client_host, project_dir, mode, maxtu, jobs, prefer, plan):
    """Run the client against an ALREADY-validated LaunchPlan's C
    resolution -- like up(), run_client() no longer resolves or
    preflights anything itself; that already happened inside
    resolve_launch_plan()/revalidate_entire_plan(), before up() was even
    called, let alone this function. revalidate_before_mutation() runs
    first, before docker_run_foreground_staged() -- the ONE mutating
    action this function performs.

    Round-5 redesign (Deep Reviewer's promoted harness-private-staging
    fix, superseding round-4's minimum shape): the OLD design pushed
    farm_client.sh/replay.py into `~/farm-scratch` -- a MUTABLE, host-
    bind-mounted, read-write directory -- before ever starting the
    container, then separately started a DETACHED idle holder and
    `docker exec`'d into it. That left a real window (push-to-exec, not
    merely hash-to-exec) during which the STAGED copies on the host sat
    writable and host-visible, with nothing re-verifying them at the
    point they actually ran. Fixed: the harness bundle
    (_build_harness_bundle(), built from the SAME checkout copies
    verify_harness_scripts() just confirmed) is streamed DIRECTLY into a
    single FOREGROUND `docker run -i`'s stdin (docker_run_foreground_
    staged()) and extracted into CONTAINER-PRIVATE tmpfs -- never
    touching any mutable host path at all -- then _harness_stage_verify()
    checks what actually landed in that tmpfs, from INSIDE the
    container, before farm_client.sh is ever reached. The pre-populated,
    reusable fixtures the old design also kept under `~/farm-scratch`
    (a cmake install, project sources like fmt/rocksdb) are still needed
    but are now mounted READ-ONLY at the SEPARATE path `/hostscratch`
    (see docker_run_foreground_staged()) -- `project_dir` callers pass
    is expected to already be `/hostscratch/<name>`, and `farm_client.sh`
    itself is never modified (it already honors a `CMAKE` environment
    override, exported here rather than editing the frozen script).

    The selected artifact root is staged into the same container-private
    `/work` tmpfs for C as for S/F.  Thus farm_client.sh and replay.py's
    repeated invocations of icecc cannot observe a host-owned live bind.
    The stage covers the full manifest closure, including icecc-create-env
    and iceccd, before the frozen harness is reached.

    `--rm` (docker_run_foreground_staged()'s own design) means this
    container is already gone by the time this function returns on any
    path -- no separate teardown needed, but a defensive `docker rm -f`
    still runs first inside that primitive in case a stale name is
    occupied.

    verify_harness_scripts() runs first, before anything else in this
    function including revalidate_before_mutation() -- it's pure and
    local (no host touched, no MUTATIONS mark), checking the harness
    scripts THIS invocation is about to bundle; a tampered or drifted
    checkout copy is refused before a single byte of it is even
    bundled, let alone streamed to any host or executed inside a
    container."""
    verify_harness_scripts()
    revalidate_before_mutation(client_host, plan.binary_set_c)
    sched = f"{HOSTS[SCHED_HOST]['ip']}:{SCHED_PORT}"
    bundle = _build_harness_bundle()
    stage_token = _attestation_token()
    c_token = _attestation_token()
    artifact_stage_token = _attestation_token()
    stage_verify = _harness_stage_verify(stage_token)
    artifact_stage = _artifact_stage_prefix(plan.binary_set_c, artifact_stage_token)
    attest = _attestation_prefix(plan.binary_set_c, c_token)
    real_cmd = (f"export CMAKE=/hostscratch/cmake/bin/cmake; "
                f"bash /scratch/farm_client.sh {sched} {NET} {project_dir} {mode} {maxtu} {jobs} {prefer}")
    inner_cmd = f"mkdir -p /scratch && tar -xf - -C /scratch && {artifact_stage}{stage_verify}{attest}{real_cmd}"
    print(f"TEST: client={client_host} project={project_dir} mode={mode} maxtu={maxtu} jobs={jobs} prefer={prefer} "
          f"C-tree={plan.c_tree}")
    r = docker_run_foreground_staged(client_host, "farm-client", plan.c_tree, plan.c_img, bundle, inner_cmd, timeout=1800)
    if "HARNESS-STAGE-FAIL" in (r.stdout or ""):
        raise RuntimeError(f"in-container harness-bundle staging FAILED for farm-client on {client_host} -- "
                            f"farm_client.sh was never reached: {r.stdout[-400:]}")
    if f"HARNESS-STAGED-OK-{stage_token}" not in (r.stdout or ""):
        raise RuntimeError(f"in-container harness-stage marker missing for farm-client on {client_host} "
                            f"(expected HARNESS-STAGED-OK-{stage_token}); refusing to trust this run's output: "
                            f"{r.stdout[-400:]}")
    if "ARTIFACT-STAGE-FAIL" in (r.stdout or ""):
        raise RuntimeError(f"in-container artifact staging FAILED for farm-client on {client_host} -- "
                           f"farm_client.sh was never reached: {r.stdout[-400:]}")
    if plan.binary_set_c is not None and f"ARTIFACT-STAGED-OK-{artifact_stage_token}" not in (r.stdout or ""):
        raise RuntimeError(f"in-container artifact-stage marker missing for farm-client on {client_host} "
                           f"(expected ARTIFACT-STAGED-OK-{artifact_stage_token}); refusing to trust this run's output: "
                           f"{r.stdout[-400:]}")
    if "ARTIFACT-ATTEST-FAIL" in (r.stdout or ""):
        raise RuntimeError(f"in-container attestation FAILED for farm-client on {client_host} -- "
                            f"farm_client.sh was never reached: {r.stdout[-400:]}")
    if plan.binary_set_c is not None and f"ARTIFACT-ATTEST-OK-{c_token}" not in (r.stdout or ""):
        raise RuntimeError(f"in-container attestation marker missing for farm-client on {client_host} "
                            f"(expected ARTIFACT-ATTEST-OK-{c_token}); refusing to trust this run's output: "
                            f"{r.stdout[-400:]}")
    print(r.stdout.rstrip())
    if r.stderr.strip():
        print("TEST STDERR:", r.stderr.strip()[:400])
    return r

def dump_worker_evidence(worker_hosts, client_stdout, client_rc, binary_set_c=None):
    """Bijection verdict -- STRENGTHENED per BO's cell-verdict fix.

    The round-4 finding: main() used to discard this function's own
    return value entirely (never assigned, never combined into `ok`) and
    never inspected run_client()'s returncode either -- so a cell whose
    client script itself failed, or whose JOIN bijection was broken,
    still made the whole `farm.py` process exit 0. This function alone
    can no longer cause that: it now requires ALL of --

    - EXACTLY ONE parseable `CELL: project TUs=N mode=M` header line
      (zero or more-than-one is a malformed/replayed-log situation, not
      a clean single run);
    - EXACTLY ONE terminal `CELL: PASS` line (a `CELL: FAIL`, or a
      missing terminal line entirely -- e.g. a crash mid-replay -- is a
      hard fail here, never silently ignored);
    - the JOINROW count equals N * (2 if mode == "sequence" else 1) --
      replay.py's own `sequence` mode genuinely replays every TU TWICE
      (once cold, once warm; see its `main()`), so this is the correct
      expected cardinality, not an arbitrary guess;
    - every row's accepted/exact/mode fields are exactly "1"/"1"/"remote";
    - client-reported job IDs are unique and none is -1 (a JOINROW with
      jobid=-1 means replay.py's own assignment-line regex never matched
      anything for that TU -- genuinely missing evidence, not a valid id);
    - EXACT set equality between the client's reported job IDs and the
      union of every F host's OWN requested-job-id log entries --
      PREVIOUSLY a SUBSET check (`<=`), which could not detect an F host
      reporting job IDs the client never actually claimed (e.g. stale
      entries surviving from an earlier, different run on a shared host);
    - the sum of every F host's own completion count equals the total
      JOINROW count (no orphaned completions, none missing).

    `client_rc` (run_client()'s own `r.returncode`) is accepted here
    PURELY for inclusion in the printed CELL-VERDICT line -- it is
    deliberately NOT part of this function's own returned boolean (BO's
    exact spec keeps `client_ok`/`join_ok` as two separately-combined
    values in main(): `ok = ok and client_ok and join_ok`).

    ANY malformed input (unparseable rows, missing fields, non-integer
    IDs, a transport error while querying an F host's log) is caught and
    turned into a FALSE verdict -- this function must NEVER raise, since
    an uncaught exception here would propagate past main()'s own
    result-combination logic as an uncontrolled crash (a different,
    equally bad way for a real failure to not end up reflected as a
    normal, deliberate nonzero process exit), even though the
    `finally: down()` teardown itself would still run either way."""
    try:
        return _dump_worker_evidence_impl(worker_hosts, client_stdout, client_rc, binary_set_c)
    except Exception as exc:
        print(f"CELL-VERDICT: join_ok=False client_rc={client_rc!r} error={exc!r} "
              f"-- malformed/missing evidence, refusing to call this a pass")
        return False

def _dump_worker_evidence_impl(worker_hosts, client_stdout, client_rc, binary_set_c):
    lines = client_stdout.splitlines()

    headers = [ln for ln in lines if re.match(r"^CELL: project TUs=\d+ mode=\S+", ln)]
    header_ok = len(headers) == 1
    n_expected, mode = None, None
    if header_ok:
        hm = re.match(r"^CELL: project TUs=(\d+) mode=(\S+)", headers[0])
        n_expected, mode = int(hm.group(1)), hm.group(2)

    terminal_ok = sum(1 for ln in lines if ln == "CELL: PASS") == 1

    rows = []
    for ln in lines:
        if ln.startswith("JOINROW "):
            rows.append(dict(kv.split("=", 1) for kv in ln.split()[1:] if "=" in kv))
    n = len(rows)

    expected_rows = n_expected * (2 if mode == "sequence" else 1) if header_ok else None
    rows_ok = header_ok and expected_rows is not None and n == expected_rows

    cl_ids = [int(r0["jobid"]) for r0 in rows]
    ok_rows = bool(rows) and all(r0.get("accepted") == "1" and r0.get("exact") == "1"
                                  and r0.get("mode") == "remote" for r0 in rows)
    uniq = len(set(cl_ids)) == n and -1 not in cl_ids

    fmap = {}   # host -> (requested job-id set, completions)
    for h in worker_hosts:
        r = sh(h, f"grep -oE 'request for job [0-9]+' {SCRATCH}/farm/worker.log 2>/dev/null | grep -oE '[0-9]+'; true")
        ids = set(int(x) for x in r.stdout.split()) if r.stdout.strip() else set()
        c = sh(h, f"grep -c 'Remote compilation completed with exit code 0' {SCRATCH}/farm/worker.log 2>/dev/null; true")
        try: comp = int((c.stdout.strip().splitlines() or ["0"])[0])
        except Exception: comp = 0
        fmap[h] = (ids, comp)
        print(f"  F={h}: requested-job-ids={len(ids)} completions(exit0)={comp}")

    f_union = set().union(*(v[0] for v in fmap.values())) if fmap else set()
    set_equal = set(cl_ids) == f_union   # EXACT equality -- was `<=` (subset), the bug BO found
    completions = sum(v[1] for v in fmap.values())
    completions_ok = completions == n

    join_ok = (header_ok and terminal_ok and rows_ok and ok_rows and uniq
               and set_equal and completions_ok and n > 0)

    print(f"CELL-JOIN: rows={n} expected_rows={expected_rows} header_ok={header_ok} "
          f"terminal_ok={terminal_ok} all-accepted-exact-remote={ok_rows} jobids-unique={uniq} "
          f"F-set-equal={set_equal} completions={completions}/{n} -> BIJECTION {'OK' if join_ok else 'FAIL'}")
    print(f"CELL-VERDICT: join_ok={join_ok} client_rc={client_rc!r} expected_rows={expected_rows!r} "
          f"actual_rows={n} set_equal={set_equal} f_completions={completions} "
          f"product={binary_set_c!r} harness=f5d13fd9 run={int(time.time())}-{os.getpid()}")
    return join_ok

def down(worker_hosts, client_host=None):
    print("DOWN: tearing down cluster")
    docker_rm(SCHED_HOST, "farm-sched")
    for h in worker_hosts:
        docker_rm(h, "farm-worker")
    if client_host:
        docker_rm(client_host, "farm-client")

def distribute(sets, hosts):
    """`python3 farm.py distribute --sets p43,p50 --hosts research6,research7,q2`
    -- the ONLY code path allowed to write role-artifacts onto a host
    (never called from resolve_role()/up()/run_client()/
    revalidate_before_mutation(); those only ever preflight-check and
    refuse). Publishes the CONTENT-ADDRESSED immutable_root(binary_set) on
    each host: already-published-and-verified is a pure no-op; anything
    else is published fresh via publish_immutable_root() (temp-sibling
    extract, full verify, atomic rename -- never an in-place edit of an
    existing immutable directory). q3 is included (not skipped) -- it also
    needs its own immutable-named copy published from its local tar, same
    as every other host, so preflight/resolve_role never has to special-
    case where a root's content actually lives. Exits nonzero if any
    (set, host) pair ends in a hard failure, INCLUDING an existing
    immutable path that fails verification (a content-addressing
    invariant violation this function deliberately does not auto-repair --
    see publish_immutable_root()'s docstring)."""
    failures = []
    for binary_set in sets:
        if binary_set not in KNOWN_SETS:
            failures.append(f"{binary_set}: unknown binary set (choices: {KNOWN_SETS})")
            continue
        for host in hosts:
            if host not in HOSTS:
                failures.append(f"{binary_set}/{host}: unknown host (choices: {sorted(HOSTS)})")
                continue
            err, status = publish_immutable_root(host, binary_set)
            if err:
                print(f"DISTRIBUTE FAIL {binary_set}->{host}: {err}")
                failures.append(f"{binary_set}/{host}: {err}")
            else:
                print(f"DISTRIBUTE OK {binary_set}->{host}: {status}")
    if failures:
        print(f"DISTRIBUTE: {len(failures)} failure(s)")
        sys.exit(1)
    print("DISTRIBUTE: all sets/hosts current")

def main():
    if len(sys.argv) > 1 and sys.argv[1] == "distribute":
        # Separate, minimal parser so this new subcommand can't perturb the
        # existing flag surface below in any way (no shared ArgumentParser).
        dp = argparse.ArgumentParser(prog="farm.py distribute")
        dp.add_argument("--sets", required=True, help="comma list, e.g. p43,p50")
        dp.add_argument("--hosts", required=True, help="comma list of target hosts")
        da = dp.parse_args(sys.argv[2:])
        distribute(da.sets.split(","), da.hosts.split(","))
        return
    ap = argparse.ArgumentParser()
    ap.add_argument("--workers", default="research6", help="comma list of worker hosts")
    ap.add_argument("--client", default="q3", help="client host (runs the submitter + compiles)")
    ap.add_argument("--project", default="/hostscratch/fmt")
    ap.add_argument("--mode", default="simultaneous")
    ap.add_argument("--maxtu", default="0")
    ap.add_argument("--jobs", default="16")
    ap.add_argument("--phase", default="up-test-down", choices=["up","up-down","up-test-down"])
    ap.add_argument("--binary-set-S", dest="binary_set_s", default=None, choices=KNOWN_SETS,
                     help="scheduler role binary set (default: existing hardcoded TREE, unchanged)")
    ap.add_argument("--binary-set-C", dest="binary_set_c", default=None, choices=KNOWN_SETS,
                     help="client role binary set (default: existing hardcoded TREE, unchanged)")
    ap.add_argument("--binary-set-F", dest="binary_set_f", default=None, choices=KNOWN_SETS,
                     help="worker(F) role binary set, applied to every worker host (default: existing hardcoded TREE, unchanged)")
    a = ap.parse_args()
    workers = a.workers.split(",")
    prefer = (workers[0] + "w") if len(workers) == 1 else ""   # pin for 1 F; let scheduler balance for >1 F
    want_client = a.phase == "up-test-down"
    ok = False
    # MUTATIONS.started replaces the old `down_needed = True`, which used
    # to be set immediately after resolve_launch_plan() succeeded -- i.e.
    # BEFORE revalidate_entire_plan()'s whole-plan barrier, or up(), had
    # performed a single real action. That meant a barrier-revalidation
    # failure still left down_needed True, and down() would then
    # docker-rm the fixed farm-{sched,worker,client} names unconditionally,
    # which could disturb a pre-existing, unrelated, exact-name running
    # cluster while this invocation was simply declining to start a NEW
    # one (LO/BO's finding). MUTATIONS.started only becomes True from
    # INSIDE the actual mutating primitives (docker_rm, scratch_prepare,
    # docker_run_detached, docker_run_foreground_staged) the first time
    # any of them actually runs -- see up()/run_client() -- so a refusal
    # at ANY point before that (resolve_launch_plan(), the race-gate
    # seam, or revalidate_entire_plan()) can never trigger down().
    MUTATIONS.started = False
    try:
        try:
            plan = resolve_launch_plan(workers, a.binary_set_s, a.binary_set_f,
                                        a.client if want_client else None, a.binary_set_c)
        except RuntimeError as exc:
            # Nothing was resolved-and-launched, and nothing was even
            # resolved for EVERY role (a later role's failure means
            # earlier roles' successful resolutions were never acted on
            # either) -- refuse cleanly, zero docker/scratch/push actions
            # taken, down() never invoked.
            print(f"REFUSED: {exc}")
        else:
            # Test-only seam, inert by default (see _race_gate_pause()):
            # lets an external harness pause a REAL main() here, inject
            # genuine concurrent activity against the plan just resolved,
            # then let this invocation proceed into the barrier below and
            # prove it refuses cleanly. Zero effect on any normal run.
            _race_gate_pause()
            try:
                revalidate_entire_plan(workers, plan, a.client if want_client else None)
            except RuntimeError as exc:
                # Whole-plan barrier refused. resolve_launch_plan() proved
                # every role valid AT ITS OWN resolution time, but nothing
                # has mutated anything yet -- MUTATIONS.started is still
                # False here, by construction (nothing that sets it has
                # run). Exactly like a resolve_launch_plan() failure: a
                # clean refusal, zero docker/scratch/push actions taken,
                # down() never invoked.
                print(f"REFUSED: {exc}")
            else:
                try:
                    ok = up(workers, plan)
                except RuntimeError as exc:
                    # Real launch actions had already begun (up() always
                    # marks a mutation before this can raise) -- this is a
                    # genuine command failure mid-launch, not a
                    # plan-validation refusal.
                    print(f"LAUNCH FAILED: {exc}")
                    ok = False
                else:
                    print("CLUSTER:", "REGISTERED-OK" if ok else "REGISTRATION-FAILED")
                    if ok and want_client:
                        try:
                            r = run_client(a.client, a.project, a.mode, a.maxtu, a.jobs, prefer, plan)
                        except RuntimeError as exc:
                            print(f"LAUNCH FAILED: {exc}")
                            ok = False
                        else:
                            # Round-4 cell-verdict fix (BO): r.returncode and
                            # dump_worker_evidence()'s own bijection verdict
                            # used to both be silently discarded here, so a
                            # cell whose client script failed, or whose JOIN
                            # was broken, still exited 0 -- false-green. Both
                            # are now real, separately-named values combined
                            # into `ok`, which alone controls the process's
                            # final exit code below.
                            client_ok = (r.returncode == 0)
                            join_ok = dump_worker_evidence(workers, r.stdout, r.returncode, a.binary_set_c)
                            ok = ok and client_ok and join_ok
    finally:
        if MUTATIONS.started and a.phase in ("up-down", "up-test-down"):
            down(workers, a.client)
    sys.exit(0 if ok else 2)

# S4: guarded so role_tree()/HOSTS/KNOWN_SETS can be imported (e.g. by
# artifact_selection_test.sh) without triggering a live cluster deploy.
if __name__ == "__main__":
    main()
