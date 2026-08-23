#!/bin/sh
# distro_installed_identity.sh -- S1b BigOracle-HOLD successor on 050845ba.
#
# The prior distro rows (distro_probe.sh) proved BUILD-TREE identity
# (./client/icecc --version run straight out of the build dir) and reused
# `mkdir -p` build/DESTDIR paths across runs -- not fresh-by-construction.
# This script proves INSTALLED identity instead: `make install
# DESTDIR=...` runs INSIDE the distro container (same invocation as
# configure/build, per the u24 --rm lesson), against a build dir AND
# DESTDIR that are unconditionally wiped and recreated empty immediately
# beforehand, so no cross-run or cross-image residue can satisfy the gate.
#
# Usage:
#   distro_installed_identity.sh SRC_DIR WORK_DIR DISTRO [--stale-control|--sentinel-control]
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
#            "ICECC 1.5.90" check to FAIL. Proves the probe is a genuine
#            content check, not something mere file presence would satisfy
#            -- i.e. omitting the clean step cannot silently pass, because
#            the check that runs afterward is intolerant of stale content.
#            This deliberately does NOT exercise the real cleanup line (it
#            skips configure/build/install entirely) -- see
#            --sentinel-control below for the check that does.
# --sentinel-control
#            KNOWN-CAUGHT CONTROL for the clean step itself. Plants visible
#            AND hidden (dotfile/dot-directory) sentinel junk into build/
#            and destdir/, then runs the SAME normal-mode path as a
#            plain invocation (no branch-around: the real `find ...
#            -mindepth 1 -delete` cleanup line, the real configure/build/
#            install) and requires every planted sentinel to be gone
#            afterward. A plain `rm -rf $dir/*` would silently leave
#            dotfiles behind (`*` does not match them) and this row would
#            catch that regression; the companion
#            distro_installed_identity_gates.sh mutation-tests this
#            directly by neutering the real cleanup line and confirming
#            this row goes red.
set -eu

SRC=${1:?usage: distro_installed_identity.sh SRC_DIR WORK_DIR DISTRO [--stale-control|--sentinel-control]}
WORK=${2:?usage: distro_installed_identity.sh SRC_DIR WORK_DIR DISTRO [--stale-control|--sentinel-control]}
DISTRO=${3:?usage: distro_installed_identity.sh SRC_DIR WORK_DIR DISTRO [--stale-control|--sentinel-control]}
MODE=normal
SENTINEL_CONTROL=false
case "${4:-}" in
    --stale-control)    MODE=stale-control ;;
    --sentinel-control) SENTINEL_CONTROL=true ;;
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
# a plain normal run's evidence files with the same name.
RUN_SUFFIX="$MODE"
[ "$SENTINEL_CONTROL" = true ] && RUN_SUFFIX="${MODE}-sentinel"
FACTS="$WORK/facts-$RUN_SUFFIX.txt"
: > "$FACTS"
fact() { printf '%s\t%s\n' "$1" "$2" >> "$FACTS"; }
# require_exact LABEL ACTUAL EXPECTED -- a gate, not an observation: FAILS
# the whole run immediately if ACTUAL != EXPECTED. Every per-artifact
# identity check below goes through this (not just icecc's) so that
# "recorded as a fact but never actually asserted" cannot happen again for
# iceccd/icecc-scheduler/icecc.pc the way it did before this successor.
require_exact() {
    label=$1 actual=$2 expected=$3
    if [ "$actual" != "$expected" ]; then
        echo "FAIL: $DISTRO $label expected exactly '$expected', got '$actual'" >&2
        exit 1
    fi
}
# require_present LABEL VALUE -- FAILS if VALUE is the ABSENT sentinel
# fact() records for a missing file. Presence alone is weaker than
# require_exact but is what libicecc.a (a static archive with no embedded
# version string to assert against) has to be judged on.
require_present() {
    label=$1 value=$2
    if [ "$value" = "ABSENT" ]; then
        echo "FAIL: $DISTRO $label is ABSENT (expected to exist and hash successfully)" >&2
        exit 1
    fi
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

fact distro "$DISTRO"
fact mode "$MODE"
fact image_tag "$IMAGE"
IMAGE_DIGEST=$(docker image inspect "$IMAGE" --format '{{index .RepoDigests 0}}' 2>/dev/null || echo "UNRESOLVED")
fact image_digest "$IMAGE_DIGEST"
if [ "$IMAGE_DIGEST" = "UNRESOLVED" ]; then
    echo "FAIL: $DISTRO image digest did not resolve (docker image inspect returned no RepoDigest for $IMAGE) -- an unresolved digest is a gate failure, not an accepted observation" >&2
    exit 1
fi

if [ "$SENTINEL_CONTROL" = true ]; then
    # Plant visible AND hidden junk into build/ and destdir/ BEFORE the real
    # cleanup line runs (see below). Both dirs must exist for this, but
    # need not come from a prior run -- planting into freshly-mkdir'd dirs
    # exercises the exact same property (pre-existing content, however it
    # got there, must not survive the real clean step).
    mkdir -p "$BUILD" "$DESTDIR"
    for d in "$BUILD" "$DESTDIR"; do
        : > "$d/stale-visible-file"
        : > "$d/.stale-hidden-file"
        mkdir -p "$d/.stale-hidden-dir"
        : > "$d/.stale-hidden-dir/nested-stale"
    done
    fact sentinel_planted "stale-visible-file, .stale-hidden-file, .stale-hidden-dir/nested-stale (both build/ and destdir/)"
fi

if [ "$MODE" = normal ]; then
    # Fresh-by-construction: unconditional wipe, inside the SAME container
    # invocation as configure/build/install (the clean step is not a
    # separate host-side rm -- root-owned files from -u 0:0 containers
    # cannot always be removed by a non-root host shell; doing it inside
    # the container as root sidesteps that entirely).
    mkdir -p "$BUILD" "$DESTDIR"
    docker run --rm -v "$SRC:/src:ro" -v "$BUILD:/build" -v "$DESTDIR:/destdir" -u 0:0 "$IMAGE" bash -c "
        set -e
        # -mindepth 1 -delete on each EXPLICIT, validated mount target
        # (never a derived/guessed path) -- NOT 'rm -rf /build/* /destdir/*',
        # which is a shell glob and silently skips dotfiles/dot-directories
        # (a bare '*' does not match names starting with '.' unless dotglob
        # is set, which it is not here). A stale .deps/, a hidden leftover
        # object, or any other dotfile survives 'rm -rf DIR/*' untouched --
        # exactly the false-green LO's planted hidden sentinels caught.
        # --sentinel-control (see the script header) proves this line
        # actually removes hidden content, and the companion
        # distro_installed_identity_gates.sh mutation-tests this exact line.
        find /build -mindepth 1 -delete
        find /destdir -mindepth 1 -delete
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
        { $DEP_QUERY ; } > package-inventory.txt || true
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

    for stage in CONFIGURE SERVICES CACHE DAEMON SCHEDULER CLIENT INSTALL; do
        rc=$(grep -oE "^${stage}-EXIT=[0-9]+" "$WORK/container-run-$RUN_SUFFIX.log" | tail -1 | cut -d= -f2)
        fact "${stage}_exit" "${rc:-MISSING}"
        [ "${rc:-1}" = "0" ] || { echo "FAIL: $DISTRO $stage-EXIT=$rc (expected 0)" >&2; exit 1; }
    done

    if [ "$SENTINEL_CONTROL" = true ]; then
        # Prove the real cleanup line actually removed every planted
        # sentinel -- not "no *new* sentinel-named file happens to exist"
        # but "none of the specific paths planted before this run survived
        # it", checked against the SAME build/ and destdir/ the real
        # configure/build/install just ran against (fresh-by-construction
        # already implies these ran in one continuous container invocation
        # with the cleanup line first).
        survivors=""
        for d in "$BUILD" "$DESTDIR"; do
            for rel in stale-visible-file .stale-hidden-file .stale-hidden-dir .stale-hidden-dir/nested-stale; do
                [ -e "$d/$rel" ] && survivors="$survivors $d/$rel"
            done
        done
        if [ -n "$survivors" ]; then
            fact sentinel_survivors "$survivors"
            echo "FAIL: $DISTRO sentinel-control: the following planted sentinels survived the real cleanup step:$survivors" >&2
            exit 1
        fi
        fact sentinel_survivors "none (all planted visible+hidden sentinels correctly removed by the real cleanup line)"
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
    docker run --rm -v "$DESTDIR:/destdir" -u 0:0 "$IMAGE" bash -c "
        printf '#!/bin/sh\necho \"ICECC 0.0.0-STALE-PLANTED\"\n' > /destdir/usr/local/bin/icecc
        chmod 755 /destdir/usr/local/bin/icecc
    "
    fact planted_artifact "/destdir/usr/local/bin/icecc overwritten with a fake script reporting ICECC 0.0.0-STALE-PLANTED"
fi

# -- identity probe (both modes run this; only its outcome differs) --
if [ "$MODE" = normal ]; then
    # Read back the probe result captured INSIDE the build container (see
    # above) -- must not spawn a fresh --rm container here, it would lack
    # the runtime shared libraries $DEP_INSTALL put in the build container.
    ICECC_OUT=$(cat "$BUILD/icecc-version-output.txt" 2>/dev/null || echo "MISSING: icecc-version-output.txt not produced")
else
    # stale-control: the planted artifact is a dependency-free #!/bin/sh
    # script (see below), so a fresh minimal probe container is fine here --
    # there is no dynamic-linking requirement to sidestep.
    ICECC_OUT=$(docker run --rm -v "$DESTDIR:/destdir" -u 0:0 "$IMAGE" /destdir/usr/local/bin/icecc --version 2>&1 || true)
fi
fact installed_icecc_version_output "$ICECC_OUT"
ICECC_SHA=$(sha256sum "$DESTDIR/usr/local/bin/icecc" 2>/dev/null | awk '{print $1}')
fact installed_icecc_sha256 "${ICECC_SHA:-ABSENT}"

if [ "$ICECC_OUT" = "ICECC 1.5.90" ]; then
    fact installed_icecc_identity_check "PASS"
else
    fact installed_icecc_identity_check "FAIL (expected exactly 'ICECC 1.5.90', got '$ICECC_OUT')"
fi

if [ "$MODE" = normal ]; then
    # -- the rest of the installed-identity evidence (normal mode only;
    # the control mode's whole point is the one check above). Every one of
    # these is a GATE (require_exact/require_present -> exit 1 on
    # mismatch), not merely a recorded observation: LO's finding was that
    # only installed_icecc_version_output was ever actually asserted here,
    # while iceccd/icecc-scheduler/libicecc.a/icecc.pc could show ABSENT or
    # arbitrary text and the run would still report PASS.
    for rel in usr/local/sbin/iceccd usr/local/sbin/icecc-scheduler usr/local/lib/libicecc.a usr/local/lib/pkgconfig/icecc.pc; do
        key=$(printf '%s' "$rel" | tr '/.' '__')
        path="$DESTDIR/$rel"
        if [ -f "$path" ]; then
            sha="$(sha256sum "$path" | awk '{print $1}')"
        else
            sha="ABSENT"
        fi
        fact "installed_${key}_sha256" "$sha"
        require_present "installed_${key}_sha256" "$sha"
    done
    ICECCD_PROBE=$(cat "$BUILD/iceccd-version-probe.txt" 2>/dev/null || echo "MISSING: iceccd-version-probe.txt not produced")
    fact installed_iceccd_version_probe "$ICECCD_PROBE"
    require_exact installed_iceccd_version_probe "$ICECCD_PROBE" "ICECREAM daemon 1.5.90"
    SCHED_PROBE=$(cat "$BUILD/scheduler-version-probe.txt" 2>/dev/null || echo "MISSING: scheduler-version-probe.txt not produced")
    fact installed_scheduler_version_probe "$SCHED_PROBE"
    require_exact installed_scheduler_version_probe "$SCHED_PROBE" "ICECREAM scheduler 1.5.90"
    PC_VERSION_LINE=$(grep "^Version:" "$DESTDIR/usr/local/lib/pkgconfig/icecc.pc" 2>/dev/null || echo "ABSENT")
    fact installed_icecc_pc_version_line "$PC_VERSION_LINE"
    require_exact installed_icecc_pc_version_line "$PC_VERSION_LINE" "Version: 1.5.90"

    for log in configure services cache daemon scheduler client install; do
        f="$BUILD/$log.log"
        if [ -f "$f" ]; then
            fact "${log}_log_sha256" "$(sha256sum "$f" | awk '{print $1}')"
        fi
    done
    if [ -f "$BUILD/package-inventory.txt" ]; then
        fact package_inventory_sha256 "$(sha256sum "$BUILD/package-inventory.txt" | awk '{print $1}')"
        while IFS= read -r line; do
            fact "package_inventory_line" "$line"
        done < "$BUILD/package-inventory.txt"
    fi
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
