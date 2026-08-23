#!/bin/sh
# distro_installed_identity.sh -- S1b BigOracle-HOLD successor on 050845ba,
# hardened per local-oracle + BigOracle HOLD on 1f316208, then per
# BigOracle's follow-up implementation-shape guidance (no new verdict,
# exact shapes to adopt).
#
# The prior distro rows (distro_probe.sh) proved BUILD-TREE identity
# (./client/icecc --version run straight out of the build dir) and reused
# `mkdir -p` build/DESTDIR paths across runs -- not fresh-by-construction.
# This script proves INSTALLED identity instead: `make install
# DESTDIR=...` runs INSIDE the distro container (same invocation as
# configure/build, per the u24 --rm lesson), against a build dir AND
# DESTDIR that are unconditionally emptied and re-verified empty
# immediately beforehand, so no cross-run or cross-image residue can
# satisfy the gate.
#
# Usage:
#   distro_installed_identity.sh SRC_DIR WORK_DIR DISTRO \
#       [--stale-control | --sentinel-control | --corrupt-control=ARTIFACT]
#
# SRC_DIR    the extracted icecc-1.5.90 dist tree (read-only bind; reusing
#            the SOURCE across runs is fine, only build/DESTDIR must be
#            fresh -- the HOLD is about build-output freshness).
# WORK_DIR   per-distro scratch root; build/ and destdir/ live under it.
# DISTRO     one of: ubuntu22 (pinned farm-node image), ubuntu24, fedora40.
# --stale-control
#            KNOWN-CAUGHT CONTROL, run AFTER a normal pass has already
#            populated WORK_DIR: reuses that build/destdir WITHOUT wiping
#            them (the omitted-clean-step scenario), plants a wrong-version
#            fake icecc at the exact installed path, runs ONLY the identity
#            probe (no configure/build/install), and requires the exact
#            "ICECC 1.5.90" check to FAIL. This alone does NOT prove the
#            clean step (BigOracle: a rebuild can overwrite the stale
#            client without the clean step ever running) -- see
#            --sentinel-control for the check that does.
# --sentinel-control
#            KNOWN-CAUGHT CONTROL for the clean step itself. Plants
#            .s1b-stale-build-sentinel in build/ and
#            .s1b-stale-install-sentinel in destdir/, then runs the SAME
#            normal-mode path as a plain invocation (no branch-around: the
#            real empty_root() clean, the real POST-CLEAN-EMPTY assertion,
#            the real configure/build/install) and requires both sentinels
#            to be gone afterward. The companion
#            distro_installed_identity_gates.sh mutation-tests this
#            directly by neutering the real empty_root() calls and
#            confirming this row goes red AT the POST-CLEAN-EMPTY
#            assertion (not some later, indirect symptom), on all three
#            distros.
# --corrupt-control=ARTIFACT
#            KNOWN-CAUGHT per-artifact deletion/corruption matrix. Run
#            AFTER a normal pass has already populated WORK_DIR: reuses
#            that build/destdir WITHOUT wiping or rebuilding, corrupts (or
#            deletes) EXACTLY ONE artifact, then re-runs every installed-
#            identity assertion (not just that one) and requires the run
#            to FAIL, NAMING the corrupted artifact. ARTIFACT is one of:
#            icecc, iceccd, icecc-scheduler, libicecc.a, icecc.pc,
#            image-digest, package-inventory, build-log.
set -eu

SRC=${1:?usage: distro_installed_identity.sh SRC_DIR WORK_DIR DISTRO [--stale-control|--sentinel-control|--corrupt-control=ARTIFACT]}
WORK=${2:?usage: distro_installed_identity.sh SRC_DIR WORK_DIR DISTRO [--stale-control|--sentinel-control|--corrupt-control=ARTIFACT]}
DISTRO=${3:?usage: distro_installed_identity.sh SRC_DIR WORK_DIR DISTRO [--stale-control|--sentinel-control|--corrupt-control=ARTIFACT]}
MODE=normal
SENTINEL_CONTROL=false
CORRUPT_ARTIFACT=""
case "${4:-}" in
    --stale-control)      MODE=stale-control ;;
    --sentinel-control)   SENTINEL_CONTROL=true ;;
    --corrupt-control=*)
        MODE=corrupt-control
        CORRUPT_ARTIFACT=${4#--corrupt-control=}
        case "$CORRUPT_ARTIFACT" in
            icecc|iceccd|icecc-scheduler|libicecc.a|icecc.pc|image-digest|package-inventory|build-log) ;;
            *) echo "unknown --corrupt-control artifact: $CORRUPT_ARTIFACT (want one of: icecc iceccd icecc-scheduler libicecc.a icecc.pc image-digest package-inventory build-log)" >&2
               exit 2 ;;
        esac
        ;;
    "") ;;
    *) echo "unknown 4th argument: ${4}" >&2; exit 2 ;;
esac

SRC=$(CDPATH= cd -- "$SRC" && pwd)

# One-time SRC timestamp normalization. A dist tarball extracted over an SSH
# `cat | tar -x` pipe can land with scrambled mtimes; automake's Makefile.in
# auto-remake rule (`$(srcdir)/Makefile.in: $(srcdir)/Makefile.am
# $(am__configure_deps)`, unconditionally present -- this tree does not use
# AM_MAINTAINER_MODE so --disable-maintainer-mode is not an option) fires
# whenever it judges ANY prerequisite not-older-than the generated file, and
# its recipe runs `automake --foreign` INSIDE srcdir, which needs to create
# autom4te.cache there -- a write, which the deliberately-:ro source mount
# correctly refuses. Rather than chase exact-equality timestamp races (tried;
# not reliably sufficient), enforce two clearly-ordered tiers, both safely in
# the past (a future-dated generated tier was tried too and fails a DIFFERENT
# way: autoconf's own environment-sanity check rejects a newly-created file
# being older than a distributed file). This is idempotent and touches only
# metadata, never content, so re-running it changes nothing observable about
# the source; it is required for the read-only mount design to work at all,
# not a workaround being smuggled past the freshness requirement.
( cd "$SRC" &&
  find . -name 'Makefile.am' -o -name 'configure.ac' -o -path './m4/*.m4' -o -name 'acconfig.h' \
    | xargs touch -d '2020-01-01T00:00:00' -- &&
  find . -name 'Makefile.in' -o -name 'aclocal.m4' -o -name 'configure' -o -name 'config.h.in' \
    -o -name 'compile' -o -name 'depcomp' -o -name 'install-sh' -o -name 'missing' \
    -o -name 'ltmain.sh' -o -name 'config.guess' -o -name 'config.sub' -o -name 'test-driver' \
    | xargs touch -d '2020-01-01T00:10:00' --
)

mkdir -p "$WORK"
WORK=$(CDPATH= cd -- "$WORK" && pwd)
BUILD="$WORK/build"
DESTDIR="$WORK/destdir"
# sentinel-control stays MODE=normal (it exercises the real normal path,
# not a separate branch -- see the header) but must not silently overwrite
# a plain normal run's evidence files with the same name; corrupt-control
# is keyed by which artifact it corrupted, for the same reason.
RUN_SUFFIX="$MODE"
[ "$SENTINEL_CONTROL" = true ] && RUN_SUFFIX="${MODE}-sentinel"
[ "$MODE" = corrupt-control ] && RUN_SUFFIX="${MODE}-${CORRUPT_ARTIFACT}"
FACTS="$WORK/facts-$RUN_SUFFIX.txt"
: > "$FACTS"
fact() { printf '%s\t%s\n' "$1" "$2" >> "$FACTS"; }

# -- Fail-closed verifier helpers. One owner for pass/fail: every artifact
# assertion below goes through exactly one of these, each fails IMMEDIATELY
# and NAMES the artifact/check that failed; the script only ever reaches
# its final "PASS" line if every single one of these passed. --
require_file() {
    # require_file LABEL PATH -- exists AND non-empty.
    label=$1 path=$2
    if [ ! -e "$path" ]; then
        echo "FAIL: $DISTRO $label: $path does not exist" >&2
        exit 1
    fi
    if [ ! -s "$path" ]; then
        echo "FAIL: $DISTRO $label: $path exists but is empty" >&2
        exit 1
    fi
}
require_exact() {
    # require_exact LABEL ACTUAL EXPECTED -- byte-exact match.
    label=$1 actual=$2 expected=$3
    if [ "$actual" != "$expected" ]; then
        echo "FAIL: $DISTRO $label expected exactly '$expected', got '$actual'" >&2
        exit 1
    fi
}
require_prefix() {
    # require_prefix LABEL ACTUAL PREFIX -- ACTUAL must start with PREFIX
    # (used where the exact trailing content isn't itself part of the
    # identity being asserted, e.g. a pkg-config Version: line).
    label=$1 actual=$2 prefix=$3
    case "$actual" in
        "$prefix"*) ;;
        *) echo "FAIL: $DISTRO $label expected to start with '$prefix', got '$actual'" >&2
           exit 1 ;;
    esac
}
require_sha() {
    # require_sha LABEL PATH -- PATH must exist and be hashable; records
    # (via the caller's own fact() call, not here) and returns the sha256.
    # Existence/non-emptiness is enforced first via require_file so a
    # missing file never silently hashes to an empty-input digest.
    label=$1 path=$2
    require_file "$label" "$path"
    sha256sum "$path" | awk '{print $1}'
}

case "$DISTRO" in
    ubuntu22)
        IMAGE=icecream/farm-node:ubuntu22-gcc11-boost174
        DEP_INSTALL=':'
        DEP_QUERY='dpkg-query -W -f '\''${Package}=${Version}\n'\'' g++ gcc make autoconf automake libtool pkg-config libzstd-dev liblzo2-dev libarchive-dev libboost-dev libcap-ng-dev libxxhash-dev 2>/dev/null'
        ;;
    ubuntu24)
        IMAGE=ubuntu:24.04
        # binutils (strings) is needed by the iceccd identity probe below --
        # ubuntu22's pinned farm-node image happens to ship it already, bare
        # ubuntu:24.04 does not.
        DEP_INSTALL='apt-get update >/dev/null 2>&1 && DEBIAN_FRONTEND=noninteractive apt-get install -y g++ gcc make autoconf automake libtool pkg-config libzstd-dev liblzo2-dev libarchive-dev libboost-dev libcap-ng-dev libxxhash-dev binutils >/dev/null 2>&1'
        DEP_QUERY='dpkg-query -W -f '\''${Package}=${Version}\n'\'' g++ gcc make autoconf automake libtool pkg-config libzstd-dev liblzo2-dev libarchive-dev libboost-dev libcap-ng-dev libxxhash-dev binutils 2>/dev/null'
        ;;
    fedora40)
        IMAGE=fedora:40
        DEP_INSTALL='dnf install -y gcc gcc-c++ make autoconf automake libtool pkgconf-pkg-config libzstd-devel lzo-devel libarchive-devel boost-devel libcap-ng-devel xxhash-devel binutils >/dev/null 2>&1'
        DEP_QUERY='rpm -q gcc gcc-c++ make autoconf automake libtool pkgconf-pkg-config libzstd-devel lzo-devel libarchive-devel boost-devel libcap-ng-devel xxhash-devel binutils 2>/dev/null'
        ;;
    *)
        echo "unknown distro: $DISTRO (want ubuntu22, ubuntu24, or fedora40)" >&2
        exit 2
        ;;
esac

if [ "$MODE" = corrupt-control ] && [ "$CORRUPT_ARTIFACT" = image-digest ]; then
    # Substitute a KNOWN-locally-built-only image for JUST this digest
    # computation: a fresh `docker build` output is guaranteed to have
    # empty RepoDigests (it has never been pulled/pushed through any
    # registry), regardless of whether the real per-distro $IMAGE on THIS
    # host happens to be properly pinned or not -- deterministic, not
    # dependent on ambient host image state. `RUN true` forces a genuinely
    # new image ID (a bare `FROM X` with no other instructions can just
    # alias X's own existing ID, which would inherit X's real RepoDigests
    # and defeat the point).
    DIGEST_CTX="$WORK/digest-corrupt-ctx"
    rm -rf "$DIGEST_CTX"; mkdir -p "$DIGEST_CTX"
    printf 'FROM %s\nRUN true\n' "$IMAGE" > "$DIGEST_CTX/Dockerfile"
    docker build -q -t s1b-digest-corrupt-control:local "$DIGEST_CTX" >/dev/null
    IMAGE=s1b-digest-corrupt-control:local
    fact corrupt_artifact "$CORRUPT_ARTIFACT"
fi

fact distro "$DISTRO"
fact mode "$MODE"
fact image_tag "$IMAGE"
# Captured SEPARATELY from the UNRESOLVED fallback, not `cmd || echo
# UNRESOLVED` in one substitution: when the template fails (empty
# RepoDigests), `docker image inspect` still writes a blank line to
# STDOUT before the error goes to stderr -- `$(cmd || echo X)` would then
# concatenate that leading blank line with X into "\nX", which is never
# equal to the literal string "UNRESOLVED" and would let this whole gate
# silently pass on exactly the case it exists to catch (found by
# --corrupt-control=image-digest, which is the first thing to have ever
# actually exercised this fallback path). `|| true` (not bare, and not
# `|| echo ...`) is required here too: under `set -e`, a plain
# `VAR=$(failing_cmd)` assignment aborts the script immediately at this
# line -- `|| true` absorbs that without adding any output of its own, so
# a failed lookup correctly collapses to an empty string (all-whitespace
# command-substitution output is stripped entirely, not just trailing
# newlines-after-content) for the presence check below to catch.
IMAGE_DIGEST=$(docker image inspect "$IMAGE" --format '{{index .RepoDigests 0}}' 2>/dev/null || true)
[ -n "$IMAGE_DIGEST" ] || IMAGE_DIGEST="UNRESOLVED"
fact image_digest "$IMAGE_DIGEST"
if [ "$IMAGE_DIGEST" = "UNRESOLVED" ]; then
    echo "FAIL: $DISTRO image_digest is ABSENT/UNRESOLVED (docker image inspect returned no RepoDigest for $IMAGE) -- an unresolved digest is a gate failure, not an accepted observation" >&2
    echo
    echo "=== facts ($FACTS) ==="
    cat "$FACTS"
    exit 1
fi
# Digest-qualified reference, used for EVERY docker run below instead of
# the mutable tag -- no inspect-vs-run tag drift: what we just verified
# the digest of is what actually launches, not "whatever this tag
# currently points at by the time we get around to running it". A
# RepoDigests entry is ALREADY a complete "repo@sha256:..." reference (not
# a bare hash) -- IMAGE_DIGEST IS the reference; reconstructing it by
# prepending the repo name again (as an earlier version of this line did)
# produces an invalid doubled "repo@repo@sha256:..." string that `docker
# run` rejects outright ("invalid reference format").
IMAGE_REF="$IMAGE_DIGEST"
fact image_ref "$IMAGE_REF"
IMAGE_ID=$(docker image inspect "$IMAGE" --format '{{.Id}}' 2>/dev/null || true)
fact image_id "${IMAGE_ID:-ABSENT}"
require_exact image_ref_nonempty "$([ -n "$IMAGE_REF" ] && echo present || echo ABSENT)" present
require_exact image_id_nonempty "$([ -n "$IMAGE_ID" ] && echo present || echo ABSENT)" present
# image-digest's whole test is the gate above; nothing else about this
# artifact needs (or can meaningfully use) a DESTDIR/BUILD, so stop here.
if [ "$MODE" = corrupt-control ] && [ "$CORRUPT_ARTIFACT" = image-digest ]; then
    echo "BUG: corrupt-control=image-digest reached past the digest gate without it firing" >&2
    exit 3
fi

if [ "$SENTINEL_CONTROL" = true ]; then
    # ONE hidden sentinel per root, exact names -- proves the dotfile-
    # inclusive clean actually removes hidden content (LO's original
    # finding: a shell-glob-based `rm -rf DIR/*` silently skips dotfiles).
    # Both dirs must exist for this, but need not come from a prior run --
    # planting into freshly-mkdir'd dirs exercises the exact same property
    # (pre-existing content, however it got there, must not survive the
    # real clean step).
    mkdir -p "$BUILD" "$DESTDIR"
    : > "$BUILD/.s1b-stale-build-sentinel"
    : > "$DESTDIR/.s1b-stale-install-sentinel"
    fact sentinel_planted ".s1b-stale-build-sentinel (build/), .s1b-stale-install-sentinel (destdir/)"
fi

if [ "$MODE" = normal ]; then
    # Fresh-by-construction: unconditional empty, inside the SAME container
    # invocation as configure/build/install (the clean step is not a
    # separate host-side rm -- root-owned files from -u 0:0 containers
    # cannot always be removed by a non-root host shell; doing it inside
    # the container as root sidesteps that entirely).
    mkdir -p "$BUILD" "$DESTDIR"
    docker run --rm -v "$SRC:/src:ro" -v "$BUILD:/build" -v "$DESTDIR:/destdir" -u 0:0 "$IMAGE_REF" bash -c "
        set -e
        # empty_root(): clear every top-level entry under \\\$root, then
        # verify NOTHING remains (not just trust the rm) -- an explicit,
        # checkable assertion, not merely running a clean command. Not a
        # hand-maintained glob set: -mindepth 1 -maxdepth 1 lists every
        # top-level entry (files AND dotfiles/dot-directories alike --
        # unlike a shell glob 'DIR/*', which silently skips names starting
        # with '.'), 'rm -rf --' recursively removes each one regardless of
        # type, and the second find (-print -quit: stop at the first hit)
        # re-checks the root is genuinely empty afterward.
        fail() { echo POST-CLEAN-EMPTY=NO; exit 1; }
        empty_root() {
            find \"\$root\" -mindepth 1 -maxdepth 1 -exec rm -rf -- {} +
            [ -z \"\$(find \"\$root\" -mindepth 1 -maxdepth 1 -print -quit)\" ] || fail
        }
        root=/build; empty_root
        root=/destdir; empty_root
        echo POST-CLEAN-EMPTY=YES
        $DEP_INSTALL
        cd /build
        /src/configure --without-man > configure.log 2>&1; echo CONFIGURE-EXIT=\$?
        make -C services -j8 > services.log 2>&1; echo SERVICES-EXIT=\$?
        make -C cache -j8 > cache.log 2>&1; echo CACHE-EXIT=\$?
        make -C daemon -j8 > daemon.log 2>&1; echo DAEMON-EXIT=\$?
        make -C scheduler -j8 > scheduler.log 2>&1; echo SCHEDULER-EXIT=\$?
        make -C client -j8 icecc libclient.a > client.log 2>&1; echo CLIENT-EXIT=\$?
        make install DESTDIR=/destdir > install.log 2>&1; echo INSTALL-EXIT=\$?
        find /destdir -type f | sort > destdir-listing.txt
        { $DEP_QUERY ; } > package-inventory.txt; echo PACKAGE-INVENTORY-EXIT=\$?
        # Identity probes run HERE, inside this same container filesystem,
        # not in a later fresh --rm container -- the runtime shared libraries
        # (liblzo2, libzstd, libboost, ...) were installed by \$DEP_INSTALL
        # into THIS container only; a later separate 'docker run --rm' would
        # start from the bare image again and the installed icecc binary
        # would fail to even launch (dynamic linker: liblzo2.so.2 not found).
        # This is the exact same single-invocation lesson the u24 --rm build
        # hiccup taught, recurring one step later in the pipeline.
        /destdir/usr/local/bin/icecc --version > icecc-version-output.txt 2>&1 || true
        # -oE against an anchored version pattern, not -F against the whole
        # line -- a clean, exact 'ICECREAM daemon X.Y.Z' string to assert
        # against, not whatever else happens to share that line.
        strings /destdir/usr/local/sbin/iceccd 2>/dev/null | grep -oE 'ICECREAM daemon [0-9]+\.[0-9]+\.[0-9]+' | head -1 > iceccd-version-probe.txt || true
        /destdir/usr/local/sbin/icecc-scheduler --version 2>&1 | grep -oE 'ICECREAM scheduler [0-9]+\.[0-9]+\.[0-9]+' | head -1 > scheduler-version-probe.txt || true
    " 2>&1 | tee "$WORK/container-run-$RUN_SUFFIX.log"
    cp "$WORK/container-run-$RUN_SUFFIX.log" "$WORK/container-run.log" 2>/dev/null || true

    pce=$(grep -oE '^POST-CLEAN-EMPTY=(YES|NO)' "$WORK/container-run-$RUN_SUFFIX.log" | tail -1 | cut -d= -f2)
    fact post_clean_empty "${pce:-MISSING}"
    [ "${pce:-MISSING}" = "YES" ] || { echo "FAIL: $DISTRO post-clean-empty assertion did not pass (got '${pce:-MISSING}')" >&2; exit 1; }

    for stage in CONFIGURE SERVICES CACHE DAEMON SCHEDULER CLIENT INSTALL PACKAGE-INVENTORY; do
        rc=$(grep -oE "^${stage}-EXIT=[0-9]+" "$WORK/container-run-$RUN_SUFFIX.log" | tail -1 | cut -d= -f2)
        fact "${stage}_exit" "${rc:-MISSING}"
        [ "${rc:-1}" = "0" ] || { echo "FAIL: $DISTRO $stage-EXIT=$rc (expected 0)" >&2; exit 1; }
    done

    if [ "$SENTINEL_CONTROL" = true ]; then
        # Prove the real clean actually removed both planted sentinels --
        # checked against the SAME build/ and destdir/ the real
        # configure/build/install just ran against (fresh-by-construction
        # already implies these ran in one continuous container invocation
        # with empty_root() first).
        survivors=""
        [ -e "$BUILD/.s1b-stale-build-sentinel" ] && survivors="$survivors $BUILD/.s1b-stale-build-sentinel"
        [ -e "$DESTDIR/.s1b-stale-install-sentinel" ] && survivors="$survivors $DESTDIR/.s1b-stale-install-sentinel"
        if [ -n "$survivors" ]; then
            fact sentinel_survivors "$survivors"
            echo "FAIL: $DISTRO sentinel-control: the following planted sentinels survived the real clean step:$survivors" >&2
            exit 1
        fi
        fact sentinel_survivors "none (both planted sentinels correctly removed by the real clean step)"
    fi
elif [ "$MODE" = stale-control ]; then
    [ -d "$DESTDIR/usr/local/bin" ] || {
        echo "distro_installed_identity.sh: --stale-control needs a prior normal run's DESTDIR to reuse (none found at $DESTDIR); run without --stale-control first" >&2
        exit 2
    }
    fact reused_destdir_wiped "false (deliberate -- this is the control)"
    # Plant a wrong-version fake artifact directly at the installed path,
    # then do NOT reconfigure/rebuild/reinstall -- this models "the
    # clean+rebuild+install step was omitted or skipped" and asks whether
    # the identity probe alone still catches it.
    docker run --rm -v "$DESTDIR:/destdir" -u 0:0 "$IMAGE_REF" bash -c "
        printf '#!/bin/sh\necho \"ICECC 0.0.0-STALE-PLANTED\"\n' > /destdir/usr/local/bin/icecc
        chmod 755 /destdir/usr/local/bin/icecc
    "
    fact planted_artifact "/destdir/usr/local/bin/icecc overwritten with a fake script reporting ICECC 0.0.0-STALE-PLANTED"
elif [ "$MODE" = corrupt-control ]; then
    # Per-artifact deletion/corruption matrix (all artifacts except
    # image-digest, handled and exited on above). Reuses an existing
    # DESTDIR/BUILD WITHOUT wiping or rebuilding -- corrupts exactly ONE
    # artifact, then does every check ONE combined container invocation
    # can do live (so the untouched, genuine, real-compiled siblings of
    # the corrupted artifact can still be probed -- they need the SAME
    # runtime shared libraries a bare, deps-free --rm container would lack,
    # per the single-invocation lesson noted above).
    [ -d "$DESTDIR/usr/local/bin" ] || {
        echo "distro_installed_identity.sh: --corrupt-control needs a prior normal run's DESTDIR/BUILD to reuse (none found at $DESTDIR); run without a control flag first" >&2
        exit 2
    }
    fact reused_destdir_wiped "false (deliberate -- this is the per-artifact corruption control)"
    fact corrupt_artifact "$CORRUPT_ARTIFACT"
    docker run --rm -v "$BUILD:/build" -v "$DESTDIR:/destdir" -u 0:0 "$IMAGE_REF" bash -c "
        set -e
        $DEP_INSTALL
        case '$CORRUPT_ARTIFACT' in
            icecc)
                printf '#!/bin/sh\necho \"ICECC 0.0.0-STALE-PLANTED\"\n' > /destdir/usr/local/bin/icecc
                chmod 755 /destdir/usr/local/bin/icecc
                ;;
            iceccd)
                printf 'not a real daemon binary\n' > /destdir/usr/local/sbin/iceccd
                ;;
            icecc-scheduler)
                printf '#!/bin/sh\necho WRONG-SCHEDULER-VERSION\n' > /destdir/usr/local/sbin/icecc-scheduler
                chmod 755 /destdir/usr/local/sbin/icecc-scheduler
                ;;
            libicecc.a)
                rm -f /destdir/usr/local/lib/libicecc.a
                ;;
            icecc.pc)
                printf 'Name: icecc\nVersion: 0.0.0-CORRUPTED\n' > /destdir/usr/local/lib/pkgconfig/icecc.pc
                ;;
            package-inventory)
                rm -f /build/package-inventory.txt
                ;;
            build-log)
                rm -f /build/configure.log
                ;;
        esac
        /destdir/usr/local/bin/icecc --version > /build/corrupt-icecc-version-output.txt 2>&1 || true
        strings /destdir/usr/local/sbin/iceccd 2>/dev/null | grep -oE 'ICECREAM daemon [0-9]+\.[0-9]+\.[0-9]+' | head -1 > /build/corrupt-iceccd-version-probe.txt || true
        /destdir/usr/local/sbin/icecc-scheduler --version 2>&1 | grep -oE 'ICECREAM scheduler [0-9]+\.[0-9]+\.[0-9]+' | head -1 > /build/corrupt-scheduler-version-probe.txt || true
    " 2>&1 | tee "$WORK/container-run-$RUN_SUFFIX.log"

    # This mode runs its OWN complete verification (every artifact, not
    # just icecc's) and exits from inside this branch -- it does not fall
    # through to the shared normal/stale-control checks below, which read
    # from different (pre-corruption) evidence files.
    ICECC_OUT=$(cat "$BUILD/corrupt-icecc-version-output.txt" 2>/dev/null || echo "MISSING")
    fact installed_icecc_version_output "$ICECC_OUT"
    require_file installed_icecc_file "$DESTDIR/usr/local/bin/icecc"
    fact installed_icecc_sha256 "$(sha256sum "$DESTDIR/usr/local/bin/icecc" | awk '{print $1}')"
    require_exact installed_icecc_version_output "$ICECC_OUT" "ICECC 1.5.90"

    require_file installed_iceccd_file "$DESTDIR/usr/local/sbin/iceccd"
    fact installed_iceccd_sha256 "$(sha256sum "$DESTDIR/usr/local/sbin/iceccd" | awk '{print $1}')"
    ICECCD_PROBE=$(cat "$BUILD/corrupt-iceccd-version-probe.txt" 2>/dev/null || echo "MISSING")
    fact installed_iceccd_version_probe "$ICECCD_PROBE"
    require_exact installed_iceccd_version_probe "$ICECCD_PROBE" "ICECREAM daemon 1.5.90"

    require_file installed_scheduler_file "$DESTDIR/usr/local/sbin/icecc-scheduler"
    fact installed_scheduler_sha256 "$(sha256sum "$DESTDIR/usr/local/sbin/icecc-scheduler" | awk '{print $1}')"
    SCHED_PROBE=$(cat "$BUILD/corrupt-scheduler-version-probe.txt" 2>/dev/null || echo "MISSING")
    fact installed_scheduler_version_probe "$SCHED_PROBE"
    require_exact installed_scheduler_version_probe "$SCHED_PROBE" "ICECREAM scheduler 1.5.90"

    require_file installed_libicecc_a_file "$DESTDIR/usr/local/lib/libicecc.a"
    fact installed_libicecc_a_sha256 "$(sha256sum "$DESTDIR/usr/local/lib/libicecc.a" | awk '{print $1}')"

    require_file installed_icecc_pc_file "$DESTDIR/usr/local/lib/pkgconfig/icecc.pc"
    fact installed_icecc_pc_sha256 "$(sha256sum "$DESTDIR/usr/local/lib/pkgconfig/icecc.pc" | awk '{print $1}')"
    PC_VERSION_LINE=$(grep "^Version:" "$DESTDIR/usr/local/lib/pkgconfig/icecc.pc" 2>/dev/null || echo "ABSENT")
    fact installed_icecc_pc_version_line "$PC_VERSION_LINE"
    require_prefix installed_icecc_pc_version_line "$PC_VERSION_LINE" "Version: 1.5.90"

    require_file package_inventory_file "$BUILD/package-inventory.txt"
    fact package_inventory_sha256 "$(sha256sum "$BUILD/package-inventory.txt" | awk '{print $1}')"

    for log in configure services cache daemon scheduler client install; do
        require_file "${log}_log_file" "$BUILD/$log.log"
        fact "${log}_log_sha256" "$(sha256sum "$BUILD/$log.log" | awk '{print $1}')"
    done

    echo
    echo "=== facts ($FACTS) ==="
    cat "$FACTS"
    echo
    echo "BUG: corrupt-control=$CORRUPT_ARTIFACT did not trigger any gate -- the corruption had no detectable effect" >&2
    exit 3
fi

# -- identity probe (normal and stale-control only reach here;
# corrupt-control has its own dedicated block above and always exits from
# inside it) --
if [ "$MODE" = normal ]; then
    # Read back the probe result captured INSIDE the build container (see
    # above) -- must not spawn a fresh --rm container here, it would lack
    # the runtime shared libraries $DEP_INSTALL put in the build container.
    ICECC_OUT=$(cat "$BUILD/icecc-version-output.txt" 2>/dev/null || echo "MISSING: icecc-version-output.txt not produced")
else
    # stale-control: the planted artifact is a dependency-free #!/bin/sh
    # script (see below), so a fresh minimal probe container is fine here --
    # there is no dynamic-linking requirement to sidestep.
    ICECC_OUT=$(docker run --rm -v "$DESTDIR:/destdir" -u 0:0 "$IMAGE_REF" /destdir/usr/local/bin/icecc --version 2>&1 || true)
fi
fact installed_icecc_version_output "$ICECC_OUT"
require_file installed_icecc_file "$DESTDIR/usr/local/bin/icecc"
fact installed_icecc_sha256 "$(sha256sum "$DESTDIR/usr/local/bin/icecc" | awk '{print $1}')"

if [ "$ICECC_OUT" = "ICECC 1.5.90" ]; then
    fact installed_icecc_identity_check "PASS"
else
    fact installed_icecc_identity_check "FAIL (expected exactly 'ICECC 1.5.90', got '$ICECC_OUT')"
fi

if [ "$MODE" = normal ]; then
    # -- the rest of the installed-identity evidence (normal mode only;
    # the control modes' whole point is narrower checks above). Every one
    # of these is a GATE (require_file/require_exact/require_prefix ->
    # exit 1 on mismatch), not merely a recorded observation: LO's finding
    # was that only installed_icecc_version_output was ever actually
    # asserted here, while iceccd/icecc-scheduler/libicecc.a/icecc.pc could
    # show ABSENT or arbitrary text and the run would still report PASS.
    require_exact installed_icecc_version_output "$ICECC_OUT" "ICECC 1.5.90"

    require_file installed_iceccd_file "$DESTDIR/usr/local/sbin/iceccd"
    fact installed_iceccd_sha256 "$(sha256sum "$DESTDIR/usr/local/sbin/iceccd" | awk '{print $1}')"
    ICECCD_PROBE=$(cat "$BUILD/iceccd-version-probe.txt" 2>/dev/null || echo "MISSING: iceccd-version-probe.txt not produced")
    fact installed_iceccd_version_probe "$ICECCD_PROBE"
    require_exact installed_iceccd_version_probe "$ICECCD_PROBE" "ICECREAM daemon 1.5.90"

    require_file installed_scheduler_file "$DESTDIR/usr/local/sbin/icecc-scheduler"
    fact installed_scheduler_sha256 "$(sha256sum "$DESTDIR/usr/local/sbin/icecc-scheduler" | awk '{print $1}')"
    SCHED_PROBE=$(cat "$BUILD/scheduler-version-probe.txt" 2>/dev/null || echo "MISSING: scheduler-version-probe.txt not produced")
    fact installed_scheduler_version_probe "$SCHED_PROBE"
    require_exact installed_scheduler_version_probe "$SCHED_PROBE" "ICECREAM scheduler 1.5.90"

    require_file installed_libicecc_a_file "$DESTDIR/usr/local/lib/libicecc.a"
    fact installed_libicecc_a_sha256 "$(sha256sum "$DESTDIR/usr/local/lib/libicecc.a" | awk '{print $1}')"

    require_file installed_icecc_pc_file "$DESTDIR/usr/local/lib/pkgconfig/icecc.pc"
    fact installed_icecc_pc_sha256 "$(sha256sum "$DESTDIR/usr/local/lib/pkgconfig/icecc.pc" | awk '{print $1}')"
    PC_VERSION_LINE=$(grep "^Version:" "$DESTDIR/usr/local/lib/pkgconfig/icecc.pc" 2>/dev/null || echo "ABSENT")
    fact installed_icecc_pc_version_line "$PC_VERSION_LINE"
    require_prefix installed_icecc_pc_version_line "$PC_VERSION_LINE" "Version: 1.5.90"

    require_file package_inventory_file "$BUILD/package-inventory.txt"
    fact package_inventory_sha256 "$(sha256sum "$BUILD/package-inventory.txt" | awk '{print $1}')"
    while IFS= read -r line; do
        fact "package_inventory_line" "$line"
    done < "$BUILD/package-inventory.txt"

    for log in configure services cache daemon scheduler client install; do
        require_file "${log}_log_file" "$BUILD/$log.log"
        fact "${log}_log_sha256" "$(sha256sum "$BUILD/$log.log" | awk '{print $1}')"
    done
fi

echo
echo "=== facts ($FACTS) ==="
cat "$FACTS"

if [ "$MODE" = stale-control ]; then
    grep -qE '^installed_icecc_identity_check[[:space:]]+FAIL' "$FACTS" && {
        echo
        echo "STALE-CONTROL RESULT: RED as required (planted stale artifact was correctly rejected)"
        exit 1
    }
    echo
    echo "STALE-CONTROL BUG: the planted stale artifact was NOT rejected -- control did not go red" >&2
    exit 3
fi

grep -qE '^installed_icecc_identity_check[[:space:]]+PASS' "$FACTS" || {
    echo "FAIL: $DISTRO installed icecc identity check did not pass" >&2
    exit 1
}
echo
echo "PASS: $DISTRO installed identity verified fresh-by-construction"
