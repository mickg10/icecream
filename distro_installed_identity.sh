#!/bin/sh
# distro_installed_identity.sh -- S1b BigOracle-HOLD successor on 050845ba,
# hardened per local-oracle + BigOracle HOLD on 1f316208, BigOracle's
# implementation-shape follow-up, and BigOracle's definitive blueprint.
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
#            SECONDARY identity-predicate control only (per BigOracle),
#            run AFTER a normal pass has already populated WORK_DIR:
#            reuses that build/destdir WITHOUT wiping them (the omitted-
#            clean-step scenario), plants a wrong-version fake icecc at
#            the exact installed path, runs ONLY the identity probe (no
#            configure/build/install), and requires the exact
#            "ICECC 1.5.90" check to FAIL. This alone does NOT prove the
#            clean step (a rebuild can overwrite the stale client without
#            the clean step ever running) -- see --sentinel-control.
# --sentinel-control
#            KNOWN-CAUGHT CONTROL for the clean step itself. Plants
#            .s1b-stale-build-sentinel in build/ and
#            .s1b-stale-install-sentinel in destdir/, then runs the SAME
#            normal-mode path as a plain invocation (no branch-around: the
#            real empty_root() clean, the real POST-CLEAN-EMPTY assertion,
#            the real, COMPLETE configure/build/install/probe pipeline)
#            and requires both sentinels to be gone afterward. The
#            companion distro_installed_identity_gates.sh mutation-tests
#            this by neutering ONLY the empty_root() rm -- retaining the
#            emptiness assertion and the entire rest of the pipeline
#            unmodified -- and confirming this row goes red AT the
#            POST-CLEAN-EMPTY assertion, on all three distros.
# --corrupt-control=ARTIFACT
#            KNOWN-CAUGHT per-artifact deletion/corruption matrix. Run
#            AFTER a normal pass has already populated WORK_DIR: reuses
#            that build/destdir WITHOUT wiping or rebuilding, corrupts (or
#            deletes) EXACTLY ONE artifact, then re-runs every installed-
#            identity assertion (not just that one) and requires the run
#            to FAIL, NAMING the corrupted artifact. ARTIFACT is one of:
#            icecc, icecc-create-env, iceccd, icecc-scheduler, libicecc.a,
#            icecc.pc, image-digest, package-inventory, build-log.
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
            icecc|icecc-create-env|iceccd|icecc-scheduler|libicecc.a|icecc.pc|image-digest|package-inventory|build-log) ;;
            *) echo "unknown --corrupt-control artifact: $CORRUPT_ARTIFACT" >&2
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
# the past. This is idempotent and touches only metadata.
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
    label=$1 actual=$2 expected=$3
    if [ "$actual" != "$expected" ]; then
        echo "FAIL: $DISTRO $label expected exactly '$expected', got '$actual'" >&2
        exit 1
    fi
}
require_prefix() {
    label=$1 actual=$2 prefix=$3
    case "$actual" in
        "$prefix"*) ;;
        *) echo "FAIL: $DISTRO $label expected to start with '$prefix', got '$actual'" >&2
           exit 1 ;;
    esac
}
require_count1() {
    # require_count1 LABEL COUNT CONTEXT -- exactly-cardinality: COUNT must
    # be exactly 1 (not "at least one, take the first and ignore the
    # rest" -- a second, competing line must be a failure, not silently
    # dropped by e.g. `| head -1`).
    label=$1 count=$2 context=$3
    if [ "$count" != "1" ]; then
        echo "FAIL: $DISTRO $label: expected exactly 1 matching $context, found $count" >&2
        exit 1
    fi
}
require_sha256_format() {
    label=$1 sha=$2
    if ! echo "$sha" | grep -qE '^[0-9a-f]{64}$'; then
        echo "FAIL: $DISTRO $label: '$sha' is not exactly 64 lowercase hex characters" >&2
        exit 1
    fi
}
read_or() {
    path=$1 fallback=$2
    if value=$(cat "$path" 2>/dev/null); then
        printf '%s' "$value"
    else
        printf '%s' "$fallback"
    fi
}
presence() {
    if [ -n "$1" ]; then
        printf '%s' present
    else
        printf '%s' ABSENT
    fi
}

# manifest_entries accumulates one JSON object per required artifact,
# emitted (sorted) into a SINGLE per-distro machine-readable installed
# manifest {path,type,mode,size,sha256,identity} that the run's final PASS
# is bound to (require_file'd like everything else, not a side effect).
manifest_entries=""
record_artifact() {
    # record_artifact LABEL PATH TYPE IDENTITY -- require_file's PATH,
    # records mode/size/sha256 facts (sha256 format-validated), and
    # appends a manifest entry. Returns the sha256 on stdout for callers
    # that also want to assert an exact/prefix identity on it.
    label=$1 path=$2 type=$3 identity=$4
    require_file "$label" "$path"
    mode=$(stat -c %a "$path")
    size=$(stat -c %s "$path")
    sha=$(sha256sum "$path" | awk '{print $1}')
    require_sha256_format "${label}_sha256" "$sha"
    fact "${label}_mode" "$mode"
    fact "${label}_size" "$size"
    fact "${label}_sha256" "$sha"
    manifest_entries="${manifest_entries}{\"path\":\"${path#"$WORK"/}\",\"type\":\"$type\",\"mode\":\"$mode\",\"size\":$size,\"sha256\":\"$sha\",\"identity\":\"$identity\"}
"
}
write_manifest() {
    MANIFEST="$WORK/installed-manifest-$RUN_SUFFIX.json"
    printf '%s' "$manifest_entries" | sort > "$MANIFEST.lines"
    { printf '[\n'; sed '$!s/$/,/' "$MANIFEST.lines" | sed 's/^/  /'; printf ']\n'; } > "$MANIFEST"
    rm -f "$MANIFEST.lines"
    fact installed_manifest_path "$MANIFEST"
    require_file installed_manifest_file "$MANIFEST"
    fact installed_manifest_sha256 "$(sha256sum "$MANIFEST" | awk '{print $1}')"
}

case "$DISTRO" in
    ubuntu22)
        IMAGE=icecream/farm-node:ubuntu22-gcc11-boost174
        # LOCAL-ID pin channel: icecream/farm-node has no registry
        # anywhere (confirmed across every host running the classic
        # ZFS-driver store, not just this one -- there is no "build/run it
        # somewhere else" fix); RepoDigests is unconditionally empty. Pin
        # by IMAGE ID instead -- see the PIN_CHANNEL branch below for why
        # this is equivalent binding strength, not a weaker check.
        PIN_CHANNEL=local-id
        PINNED_IMAGE_ID=sha256:fe001a6138f017608b8846b43bf268a76a9d7a5b66c3364ba3f881da2ff0c54b
        DEP_INSTALL=':'
        DEP_PACKAGES='g++ gcc make autoconf automake libtool pkg-config libzstd-dev liblzo2-dev libarchive-dev libboost-dev libcap-ng-dev libxxhash-dev'
        DEP_QUERY='dpkg-query -W -f '\''${Package}=${Version}\n'\'' '"$DEP_PACKAGES"' 2>/dev/null'
        ;;
    ubuntu24)
        IMAGE=ubuntu:24.04
        PIN_CHANNEL=registry-digest
        DEP_PACKAGES='g++ gcc make autoconf automake libtool pkg-config libzstd-dev liblzo2-dev libarchive-dev libboost-dev libcap-ng-dev libxxhash-dev binutils'
        DEP_INSTALL="apt-get update >/dev/null 2>&1 && DEBIAN_FRONTEND=noninteractive apt-get install -y $DEP_PACKAGES >/dev/null 2>&1"
        DEP_QUERY='dpkg-query -W -f '\''${Package}=${Version}\n'\'' '"$DEP_PACKAGES"' 2>/dev/null'
        ;;
    fedora40)
        IMAGE=fedora:40
        PIN_CHANNEL=registry-digest
        DEP_PACKAGES='gcc gcc-c++ make autoconf automake libtool pkgconf-pkg-config libzstd-devel lzo-devel libarchive-devel boost-devel libcap-ng-devel xxhash-devel binutils'
        DEP_INSTALL="dnf install -y $DEP_PACKAGES >/dev/null 2>&1"
        DEP_QUERY='rpm -q '"$DEP_PACKAGES"' 2>/dev/null'
        ;;
    *)
        echo "unknown distro: $DISTRO (want ubuntu22, ubuntu24, or fedora40)" >&2
        exit 2
        ;;
esac
set -- $DEP_PACKAGES; DEP_PACKAGE_COUNT=$#

if [ "$MODE" = corrupt-control ] && [ "$CORRUPT_ARTIFACT" = image-digest ]; then
    # Substitute a KNOWN-locally-built-only image for JUST this digest
    # computation: a fresh `docker build` output is guaranteed to have
    # empty RepoDigests (never pulled/pushed through any registry),
    # regardless of whether the real per-distro $IMAGE on THIS host
    # happens to be properly pinned. `RUN true` forces a genuinely new
    # image ID (a bare `FROM X` can alias X's own ID and inherit its real
    # RepoDigests, defeating the point).
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
fact image_pin_channel "$PIN_CHANNEL"

if [ "$PIN_CHANNEL" = registry-digest ]; then
    # Registry-pulled images (ubuntu24, fedora40): pin by DIGEST-QUALIFIED
    # REF. IMAGE_DIGEST captured SEPARATELY from the UNRESOLVED fallback:
    # `docker image inspect` writes a blank line to STDOUT before a
    # template error goes to stderr, so a combined fallback command would
    # concatenate into "\nX", never equal to the literal "UNRESOLVED" --
    # and a bare `VAR=$(failing_cmd)` under `set -e` aborts immediately,
    # before any fallback check runs. The explicit if/else below absorbs
    # the expected lookup failure without adding output, collapsing a
    # failed lookup to an empty string for the presence check below.
    if IMAGE_DIGEST=$(docker image inspect "$IMAGE" --format '{{index .RepoDigests 0}}' 2>/dev/null); then
        :
    else
        IMAGE_DIGEST=""
    fi
    [ -n "$IMAGE_DIGEST" ] || IMAGE_DIGEST="UNRESOLVED"
    fact image_digest "$IMAGE_DIGEST"
    if [ "$IMAGE_DIGEST" = "UNRESOLVED" ]; then
        echo "FAIL: $DISTRO image_digest is ABSENT/UNRESOLVED (docker image inspect returned no RepoDigest for $IMAGE) -- an unresolved digest is a gate failure, not an accepted observation" >&2
        echo; echo "=== facts ($FACTS) ==="; cat "$FACTS"
        exit 1
    fi
    # Digest-qualified reference, used for EVERY docker run below instead
    # of the mutable tag -- no inspect-vs-run tag drift: what we just
    # verified the digest of is what actually launches. A RepoDigests
    # entry is ALREADY a complete "repo@sha256:..." reference (not a bare
    # hash) -- IMAGE_DIGEST IS the reference.
    IMAGE_REF="$IMAGE_DIGEST"
    fact image_ref "$IMAGE_REF"
    if IMAGE_ID=$(docker image inspect "$IMAGE" --format '{{.Id}}' 2>/dev/null); then
        :
    else
        IMAGE_ID=""
    fi
    fact image_id "${IMAGE_ID:-ABSENT}"
    require_exact image_ref_nonempty "$(presence "$IMAGE_REF")" present
    require_exact image_id_nonempty "$(presence "$IMAGE_ID")" present
    # Local existence of the digest-qualified reference itself (not just
    # the tag) -- and tag/digest agreement: inspecting IMAGE_REF must
    # resolve to the SAME image ID as inspecting the tag, so there is no
    # daylight between "what we verified" and "what --pull=never below
    # will actually launch".
    if IMAGE_REF_ID=$(docker image inspect "$IMAGE_REF" --format '{{.Id}}' 2>/dev/null); then
        :
    else
        IMAGE_REF_ID=""
    fi
    require_exact image_ref_id "${IMAGE_REF_ID:-ABSENT}" "$IMAGE_ID"
elif [ "$PIN_CHANNEL" = local-id ]; then
    # LOCAL-ID channel (the pinned ubuntu22 farm-node image): no
    # RepoDigest can ever exist for this image on any host running a
    # classic (non-containerd) docker store -- confirmed operator fact,
    # not something a retry or a different host fixes. Binding comes from
    # a different, equally content-addressed channel instead: the image
    # ID itself IS the config digest. The tag must currently resolve to
    # the recorded pinned ID, and -- exactly like the registry-digest
    # channel's IMAGE_REF_ID cross-check -- the post-run
    # container_run_image_id check below (against this same $IMAGE_ID)
    # independently confirms the container that actually ran matches it.
    # No digest-qualified ref exists on this channel, so $IMAGE_REF is the
    # tag; --pull=never below still applies (no image named $IMAGE exists
    # in any registry to begin with, so a silent pull could never
    # succeed, but the flag stays for uniformity and defense in depth).
    fact image_digest "N/A (local-id channel -- see image_id_pin_channel/image_id_pin_check)"
    IMAGE_REF="$IMAGE"
    fact image_ref "$IMAGE_REF (tag; local-id channel has no digest-qualified ref)"
    if IMAGE_ID=$(docker image inspect "$IMAGE" --format '{{.Id}}' 2>/dev/null); then
        :
    else
        IMAGE_ID=""
    fi
    fact image_id "${IMAGE_ID:-ABSENT}"
    require_exact image_id_nonempty "$(presence "$IMAGE_ID")" present
    fact image_id_pin_expected "$PINNED_IMAGE_ID"
    require_exact image_id_pin_check "${IMAGE_ID:-ABSENT}" "$PINNED_IMAGE_ID"
else
    echo "FAIL: $DISTRO unknown PIN_CHANNEL '$PIN_CHANNEL' (script bug)" >&2
    exit 1
fi
# image-digest's whole test is the gate above (either sub-branch);
# nothing else about this artifact needs (or can meaningfully use) a
# DESTDIR/BUILD, so stop here.
if [ "$MODE" = corrupt-control ] && [ "$CORRUPT_ARTIFACT" = image-digest ]; then
    echo "BUG: corrupt-control=image-digest reached past the digest/ID-pin gate without it firing" >&2
    exit 3
fi

if [ "$SENTINEL_CONTROL" = true ]; then
    # ONE hidden sentinel per root, exact names -- proves the dotfile-
    # inclusive clean actually removes hidden content (LO's original
    # finding: a shell-glob-based `rm -rf DIR/*` silently skips dotfiles).
    mkdir -p "$BUILD" "$DESTDIR"
    : > "$BUILD/.s1b-stale-build-sentinel"
    : > "$DESTDIR/.s1b-stale-install-sentinel"
    fact sentinel_planted ".s1b-stale-build-sentinel (build/), .s1b-stale-install-sentinel (destdir/)"
fi

if [ "$MODE" = normal ]; then
    # Fresh-by-construction: unconditional empty, inside the SAME container
    # invocation as configure/build/install (root-owned files from a
    # -u 0:0 container cannot always be removed by a non-root host shell;
    # doing it inside the container as root sidesteps that entirely).
    # --pull=never on every docker run in this file: the reference is
    # already digest-qualified and verified to exist locally above, so a
    # silent implicit pull (which could fetch something unexpected, or
    # mask a reference that does not actually exist locally) must never
    # happen -- an absent local image is a hard failure, not a fetch.
    mkdir -p "$BUILD" "$DESTDIR"
    # Named (not --rm) so the container survives just long enough to be
    # inspected below: a pre-flight `docker image inspect` of a reference
    # string only proves what THAT LOOKUP resolved to, not what the
    # container that actually ran was created from -- a mutable tag can
    # drift between the pre-flight check and the run. `docker inspect
    # <container> --format '{{.Image}}'` reports the concrete image ID
    # Docker actually instantiated, independent of which reference string
    # (tag or digest) was passed to `docker run`, closing that gap.
    CONTAINER_NAME="s1b-${DISTRO}-${RUN_SUFFIX}-$$"
    if docker rm -f "$CONTAINER_NAME" >/dev/null 2>&1; then
        :
    else
        :
    fi
    if docker run --name "$CONTAINER_NAME" --pull=never -v "$SRC:/src:ro" -v "$BUILD:/build" -v "$DESTDIR:/destdir" -u 0:0 "$IMAGE_REF" bash -c "
        set -e
        # empty_root(): clear every top-level entry under \\\$root, then
        # verify NOTHING remains (not just trust the rm) -- an explicit,
        # checkable assertion, not merely running a clean command.
        # -mindepth 1 -maxdepth 1 lists every top-level entry (files AND
        # dotfiles/dot-directories alike -- unlike a shell glob 'DIR/*',
        # which silently skips names starting with '.'), 'rm -rf --'
        # recursively removes each one regardless of type, and the second
        # find (-print -quit: stop at the first hit) re-checks the root is
        # genuinely empty afterward.
        fail() { echo POST-CLEAN-EMPTY=NO; exit 1; }
        empty_root() {
            root=\$1
            find \"\$root\" -mindepth 1 -maxdepth 1 -exec rm -rf -- {} +
            if [ -n \"\$(find \"\$root\" -mindepth 1 -maxdepth 1 -print -quit)\" ]; then fail; fi
        }
        empty_root /build
        empty_root /destdir
        echo POST-CLEAN-EMPTY=YES
        $DEP_INSTALL
        cd /build
        /src/configure --without-man > configure.log 2>&1; echo CONFIGURE-EXIT=\$?
        make -C services -j8 > services.log 2>&1; echo SERVICES-EXIT=\$?
        make -C cache -j8 > cache.log 2>&1; echo CACHE-EXIT=\$?
        make -C daemon -j8 > daemon.log 2>&1; echo DAEMON-EXIT=\$?
        make -C scheduler -j8 > scheduler.log 2>&1; echo SCHEDULER-EXIT=\$?
        make -C client -j8 icecc icecc-create-env libclient.a > client.log 2>&1; echo CLIENT-EXIT=\$?
        make install DESTDIR=/destdir > install.log 2>&1; echo INSTALL-EXIT=\$?
        find /destdir -type f | sort > destdir-listing.txt; echo DESTDIR-LISTING-EXIT=\$?
        { $DEP_QUERY ; } > package-inventory.txt; echo PACKAGE-INVENTORY-EXIT=\$?
        # Identity probes run HERE, inside this same container filesystem,
        # not in a later fresh --rm container -- the runtime shared
        # libraries (liblzo2, libzstd, libboost, ...) were installed by
        # \$DEP_INSTALL into THIS container only.
        if /destdir/usr/local/bin/icecc --version > icecc-version-output.txt 2>&1; then :; else :; fi
        if strings /destdir/usr/local/sbin/iceccd 2>/dev/null | grep -oE 'ICECREAM daemon [0-9]+\.[0-9]+\.[0-9]+' > iceccd-version-matches.txt; then :; else :; fi
        if /destdir/usr/local/sbin/icecc-scheduler --version 2>&1 | grep -oE 'ICECREAM scheduler [0-9]+\.[0-9]+\.[0-9]+' > scheduler-version-matches.txt; then :; else :; fi
    " >"$WORK/container-run-$RUN_SUFFIX.log" 2>&1; then
        RUN_RC=0
    else
        RUN_RC=$?
    fi
    cat "$WORK/container-run-$RUN_SUFFIX.log"
    if [ "$RUN_RC" != "0" ]; then
        echo "FAIL: $DISTRO build/install container exited $RUN_RC" >&2
        exit 1
    fi
    if CONTAINER_RUN_IMAGE=$(docker inspect "$CONTAINER_NAME" --format '{{.Image}}' 2>/dev/null); then
        :
    else
        CONTAINER_RUN_IMAGE=""
    fi
    if docker rm "$CONTAINER_NAME" >/dev/null 2>&1; then
        :
    else
        :
    fi
    fact container_run_image_id "${CONTAINER_RUN_IMAGE:-ABSENT}"
    # Bound to $IMAGE_ID, captured EARLIER (before this run) and held in a
    # shell variable for the rest of the script -- not re-derived from the
    # tag at this point. If the docker-run call were ever changed to launch
    # a mutable tag instead of $IMAGE_REF, and that tag had drifted since
    # $IMAGE_ID was captured, this is what would catch it: the pre-run
    # facts alone (image_digest/image_ref/image_id) cannot, since nothing
    # about computing and printing them requires the run to have actually
    # used what they describe.
    require_exact container_run_image_id "${CONTAINER_RUN_IMAGE:-ABSENT}" "$IMAGE_ID"
    if cp "$WORK/container-run-$RUN_SUFFIX.log" "$WORK/container-run.log" 2>/dev/null; then
        :
    else
        echo "FAIL: $DISTRO container-run transcript could not be copied" >&2
        exit 1
    fi

    pce=$(grep -oE '^POST-CLEAN-EMPTY=(YES|NO)' "$WORK/container-run-$RUN_SUFFIX.log" | tail -1 | cut -d= -f2)
    fact post_clean_empty "${pce:-MISSING}"
    [ "${pce:-MISSING}" = "YES" ] || { echo "FAIL: $DISTRO post-clean-empty assertion did not pass (got '${pce:-MISSING}')" >&2; exit 1; }

    for stage in CONFIGURE SERVICES CACHE DAEMON SCHEDULER CLIENT INSTALL DESTDIR-LISTING PACKAGE-INVENTORY; do
        rc=$(grep -oE "^${stage}-EXIT=[0-9]+" "$WORK/container-run-$RUN_SUFFIX.log" | tail -1 | cut -d= -f2)
        fact "${stage}_exit" "${rc:-MISSING}"
        [ "${rc:-1}" = "0" ] || { echo "FAIL: $DISTRO $stage-EXIT=$rc (expected 0)" >&2; exit 1; }
    done

    if [ "$SENTINEL_CONTROL" = true ]; then
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
    docker run --rm --pull=never -v "$DESTDIR:/destdir" -u 0:0 "$IMAGE_REF" bash -c "
        printf '#!/bin/sh\necho \"ICECC 0.0.0-STALE-PLANTED\"\n' > /destdir/usr/local/bin/icecc
        chmod 755 /destdir/usr/local/bin/icecc
    "
    fact planted_artifact "/destdir/usr/local/bin/icecc overwritten with a fake script reporting ICECC 0.0.0-STALE-PLANTED"
elif [ "$MODE" = corrupt-control ]; then
    # Per-artifact deletion/corruption matrix (all artifacts except
    # image-digest, handled and exited on above). Reuses an existing
    # DESTDIR/BUILD WITHOUT wiping or rebuilding -- corrupts exactly ONE
    # artifact, then does every check ONE combined container invocation
    # can do live (untouched, genuine, real-compiled siblings need the
    # SAME runtime shared libraries a bare, deps-free --rm container would
    # lack).
    [ -d "$DESTDIR/usr/local/bin" ] || {
        echo "distro_installed_identity.sh: --corrupt-control needs a prior normal run's DESTDIR/BUILD to reuse (none found at $DESTDIR); run without a control flag first" >&2
        exit 2
    }
    fact reused_destdir_wiped "false (deliberate -- this is the per-artifact corruption control)"
    fact corrupt_artifact "$CORRUPT_ARTIFACT"
    if docker run --rm --pull=never -v "$BUILD:/build" -v "$DESTDIR:/destdir" -u 0:0 "$IMAGE_REF" bash -c "
        set -e
        $DEP_INSTALL
        case '$CORRUPT_ARTIFACT' in
            icecc)
                printf '#!/bin/sh\necho \"ICECC 0.0.0-STALE-PLANTED\"\n' > /destdir/usr/local/bin/icecc
                chmod 755 /destdir/usr/local/bin/icecc
                ;;
            icecc-create-env)
                rm -f /destdir/usr/local/bin/icecc-create-env
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
        if /destdir/usr/local/bin/icecc --version > /build/corrupt-icecc-version-output.txt 2>&1; then :; else :; fi
        if strings /destdir/usr/local/sbin/iceccd 2>/dev/null | grep -oE 'ICECREAM daemon [0-9]+\.[0-9]+\.[0-9]+' > /build/corrupt-iceccd-version-matches.txt; then :; else :; fi
        if /destdir/usr/local/sbin/icecc-scheduler --version 2>&1 | grep -oE 'ICECREAM scheduler [0-9]+\.[0-9]+\.[0-9]+' > /build/corrupt-scheduler-version-matches.txt; then :; else :; fi
    " >"$WORK/container-run-$RUN_SUFFIX.log" 2>&1; then
        RUN_RC=0
    else
        RUN_RC=$?
    fi
    cat "$WORK/container-run-$RUN_SUFFIX.log"
    if [ "$RUN_RC" != "0" ]; then
        echo "FAIL: $DISTRO corruption container exited $RUN_RC" >&2
        exit 1
    fi

    # This mode runs its OWN complete verification (every artifact, not
    # just icecc's) and exits from inside this branch.
    ICECC_OUT=$(read_or "$BUILD/corrupt-icecc-version-output.txt" MISSING)
    fact installed_icecc_version_output "$ICECC_OUT"
    record_artifact installed_icecc "$DESTDIR/usr/local/bin/icecc" file "$ICECC_OUT" >/dev/null
    require_exact installed_icecc_version_output "$ICECC_OUT" "ICECC 1.5.90"

    record_artifact installed_icecc_create_env "$DESTDIR/usr/local/bin/icecc-create-env" file "(script, no version marker)" >/dev/null
    [ -x "$DESTDIR/usr/local/bin/icecc-create-env" ] || { echo "FAIL: $DISTRO installed_icecc_create_env not executable" >&2; exit 1; }

    ICECCD_MATCHES=$(read_or "$BUILD/corrupt-iceccd-version-matches.txt" "")
    if ICECCD_COUNT=$(printf '%s\n' "$ICECCD_MATCHES" | grep -cE 'ICECREAM daemon 1\.5\.90'); then :; else :; fi
    record_artifact installed_iceccd "$DESTDIR/usr/local/sbin/iceccd" file "$(printf '%s' "$ICECCD_MATCHES" | tr '\n' ';')" >/dev/null
    fact installed_iceccd_1590_match_count "$ICECCD_COUNT"
    require_count1 installed_iceccd_1590_match_count "$ICECCD_COUNT" "'ICECREAM daemon 1.5.90' line"

    SCHED_MATCHES=$(read_or "$BUILD/corrupt-scheduler-version-matches.txt" "")
    if SCHED_COUNT=$(printf '%s\n' "$SCHED_MATCHES" | grep -cxE 'ICECREAM scheduler 1\.5\.90'); then :; else :; fi
    record_artifact installed_scheduler "$DESTDIR/usr/local/sbin/icecc-scheduler" file "$(printf '%s' "$SCHED_MATCHES" | tr '\n' ';')" >/dev/null
    fact installed_scheduler_1590_match_count "$SCHED_COUNT"
    require_count1 installed_scheduler_1590_match_count "$SCHED_COUNT" "exact 'ICECREAM scheduler 1.5.90' line"

    record_artifact installed_libicecc_a "$DESTDIR/usr/local/lib/libicecc.a" file "(static archive)" >/dev/null

    record_artifact installed_icecc_pc "$DESTDIR/usr/local/lib/pkgconfig/icecc.pc" file "" >/dev/null
    if PC_VERSION_MATCHES=$(grep -cxE 'Version: 1\.5\.90' "$DESTDIR/usr/local/lib/pkgconfig/icecc.pc" 2>/dev/null); then :; else :; fi
    fact installed_icecc_pc_1590_match_count "$PC_VERSION_MATCHES"
    require_count1 installed_icecc_pc_1590_match_count "$PC_VERSION_MATCHES" "exact 'Version: 1.5.90' line"

    record_artifact package_inventory "$BUILD/package-inventory.txt" file "" >/dev/null
    PKG_LINES=$(wc -l < "$BUILD/package-inventory.txt" | tr -d ' ')
    fact package_inventory_line_count "$PKG_LINES"
    require_exact package_inventory_line_count "$PKG_LINES" "$DEP_PACKAGE_COUNT"

    record_artifact destdir_listing "$BUILD/destdir-listing.txt" file "" >/dev/null

    for log in configure services cache daemon scheduler client install; do
        record_artifact "${log}_log" "$BUILD/$log.log" file "" >/dev/null
    done

    write_manifest

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
    ICECC_OUT=$(read_or "$BUILD/icecc-version-output.txt" "MISSING: icecc-version-output.txt not produced")
else
    # stale-control: the planted artifact is a dependency-free #!/bin/sh
    # script, so a fresh minimal probe container is fine here -- no
    # dynamic-linking requirement to sidestep.
    if ICECC_OUT=$(docker run --rm --pull=never -v "$DESTDIR:/destdir" -u 0:0 "$IMAGE_REF" /destdir/usr/local/bin/icecc --version 2>&1); then
        :
    else
        ICECC_OUT=""
    fi
fi
fact installed_icecc_version_output "$ICECC_OUT"
record_artifact installed_icecc "$DESTDIR/usr/local/bin/icecc" file "$ICECC_OUT" >/dev/null

if [ "$ICECC_OUT" = "ICECC 1.5.90" ]; then
    fact installed_icecc_identity_check "PASS"
else
    fact installed_icecc_identity_check "FAIL (expected exactly 'ICECC 1.5.90', got '$ICECC_OUT')"
fi

if [ "$MODE" = normal ]; then
    # -- the rest of the installed-identity evidence (normal mode only).
    # Every one of these is a GATE, not merely a recorded observation.
    require_exact installed_icecc_version_output "$ICECC_OUT" "ICECC 1.5.90"

    record_artifact installed_icecc_create_env "$DESTDIR/usr/local/bin/icecc-create-env" file "(script, no version marker)" >/dev/null
    [ -x "$DESTDIR/usr/local/bin/icecc-create-env" ] || { echo "FAIL: $DISTRO installed_icecc_create_env not executable" >&2; exit 1; }

    ICECCD_MATCHES=$(read_or "$BUILD/iceccd-version-matches.txt" "")
    if ICECCD_COUNT=$(printf '%s\n' "$ICECCD_MATCHES" | grep -cE 'ICECREAM daemon 1\.5\.90'); then :; else :; fi
    record_artifact installed_iceccd "$DESTDIR/usr/local/sbin/iceccd" file "$(printf '%s' "$ICECCD_MATCHES" | tr '\n' ';')" >/dev/null
    fact installed_iceccd_1590_match_count "$ICECCD_COUNT"
    require_count1 installed_iceccd_1590_match_count "$ICECCD_COUNT" "'ICECREAM daemon 1.5.90' line"

    SCHED_MATCHES=$(read_or "$BUILD/scheduler-version-matches.txt" "")
    if SCHED_COUNT=$(printf '%s\n' "$SCHED_MATCHES" | grep -cxE 'ICECREAM scheduler 1\.5\.90'); then :; else :; fi
    record_artifact installed_scheduler "$DESTDIR/usr/local/sbin/icecc-scheduler" file "$(printf '%s' "$SCHED_MATCHES" | tr '\n' ';')" >/dev/null
    fact installed_scheduler_1590_match_count "$SCHED_COUNT"
    require_count1 installed_scheduler_1590_match_count "$SCHED_COUNT" "exact 'ICECREAM scheduler 1.5.90' line"

    record_artifact installed_libicecc_a "$DESTDIR/usr/local/lib/libicecc.a" file "(static archive)" >/dev/null

    record_artifact installed_icecc_pc "$DESTDIR/usr/local/lib/pkgconfig/icecc.pc" file "" >/dev/null
    if PC_VERSION_MATCHES=$(grep -cxE 'Version: 1\.5\.90' "$DESTDIR/usr/local/lib/pkgconfig/icecc.pc" 2>/dev/null); then :; else :; fi
    fact installed_icecc_pc_1590_match_count "$PC_VERSION_MATCHES"
    require_count1 installed_icecc_pc_1590_match_count "$PC_VERSION_MATCHES" "exact 'Version: 1.5.90' line"
    PC_VERSION_LINE=$(grep "^Version:" "$DESTDIR/usr/local/lib/pkgconfig/icecc.pc")
    fact installed_icecc_pc_version_line "$PC_VERSION_LINE"
    require_prefix installed_icecc_pc_version_line "$PC_VERSION_LINE" "Version: 1.5.90"

    record_artifact package_inventory "$BUILD/package-inventory.txt" file "" >/dev/null
    PKG_LINES=$(wc -l < "$BUILD/package-inventory.txt" | tr -d ' ')
    fact package_inventory_line_count "$PKG_LINES"
    require_exact package_inventory_line_count "$PKG_LINES" "$DEP_PACKAGE_COUNT"
    while IFS= read -r line; do
        fact "package_inventory_line" "$line"
    done < "$BUILD/package-inventory.txt"

    record_artifact destdir_listing "$BUILD/destdir-listing.txt" file "" >/dev/null

    for log in configure services cache daemon scheduler client install; do
        record_artifact "${log}_log" "$BUILD/$log.log" file "" >/dev/null
    done

    write_manifest
fi

echo
echo "=== facts ($FACTS) ==="
cat "$FACTS"

if [ "$MODE" = stale-control ]; then
    if grep -qE '^installed_icecc_identity_check[[:space:]]+FAIL' "$FACTS"; then
        echo
        echo "STALE-CONTROL RESULT: RED as required (planted stale artifact was correctly rejected)"
        exit 1
    fi
    echo
    echo "STALE-CONTROL BUG: the planted stale artifact was NOT rejected -- control did not go red" >&2
    exit 3
fi

if ! grep -qE '^installed_icecc_identity_check[[:space:]]+PASS' "$FACTS"; then
    echo "FAIL: $DISTRO installed icecc identity check did not pass" >&2
    exit 1
fi
if [ "$MODE" = normal ]; then
    # require_file (called on $MANIFEST inside write_manifest) is a pure
    # gate and never itself writes a fact -- installed_manifest_path is
    # the fact write_manifest actually records, so that's what's checked
    # here (checking for a never-written key would make this always fail).
    if ! grep -qE '^installed_manifest_path[[:space:]]' "$FACTS"; then
        echo "FAIL: $DISTRO installed manifest was not produced" >&2
        exit 1
    fi
fi
echo
echo "PASS: $DISTRO installed identity verified fresh-by-construction"
