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
#   distro_installed_identity.sh SRC_DIR WORK_DIR DISTRO [--stale-control]
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
set -eu

SRC=${1:?usage: distro_installed_identity.sh SRC_DIR WORK_DIR DISTRO [--stale-control]}
WORK=${2:?usage: distro_installed_identity.sh SRC_DIR WORK_DIR DISTRO [--stale-control]}
DISTRO=${3:?usage: distro_installed_identity.sh SRC_DIR WORK_DIR DISTRO [--stale-control]}
MODE=normal
[ "${4:-}" = "--stale-control" ] && MODE=stale-control

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
FACTS="$WORK/facts-$MODE.txt"
: > "$FACTS"
fact() { printf '%s\t%s\n' "$1" "$2" >> "$FACTS"; }

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

if [ "$MODE" = normal ]; then
    # Fresh-by-construction: unconditional wipe, inside the SAME container
    # invocation as configure/build/install (the clean step is not a
    # separate host-side rm -- root-owned files from -u 0:0 containers
    # cannot always be removed by a non-root host shell; doing it inside
    # the container as root sidesteps that entirely).
    mkdir -p "$BUILD" "$DESTDIR"
    docker run --rm -v "$SRC:/src:ro" -v "$BUILD:/build" -v "$DESTDIR:/destdir" -u 0:0 "$IMAGE" bash -c "
        set -e
        rm -rf /build/* /destdir/*
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
        strings /destdir/usr/local/sbin/iceccd 2>/dev/null | grep -F 'ICECREAM daemon' | head -1 > iceccd-version-probe.txt || true
        /destdir/usr/local/sbin/icecc-scheduler --version 2>&1 | grep -F 'ICECREAM scheduler' > scheduler-version-probe.txt || true
    " 2>&1 | tee "$WORK/container-run-$MODE.log"
    cp "$WORK/container-run-$MODE.log" "$WORK/container-run.log" 2>/dev/null || true

    for stage in CONFIGURE SERVICES CACHE DAEMON SCHEDULER CLIENT INSTALL; do
        rc=$(grep -oE "^${stage}-EXIT=[0-9]+" "$WORK/container-run-$MODE.log" | tail -1 | cut -d= -f2)
        fact "${stage}_exit" "${rc:-MISSING}"
        [ "${rc:-1}" = "0" ] || { echo "FAIL: $DISTRO $stage-EXIT=$rc (expected 0)" >&2; exit 1; }
    done
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
    # the control mode's whole point is the one check above) --
    for rel in usr/local/sbin/iceccd usr/local/sbin/icecc-scheduler usr/local/lib/libicecc.a usr/local/lib/pkgconfig/icecc.pc; do
        key=$(printf '%s' "$rel" | tr '/.' '__')
        path="$DESTDIR/$rel"
        if [ -f "$path" ]; then
            fact "installed_${key}_sha256" "$(sha256sum "$path" | awk '{print $1}')"
        else
            fact "installed_${key}_sha256" "ABSENT"
        fi
    done
    ICECCD_PROBE=$(cat "$BUILD/iceccd-version-probe.txt" 2>/dev/null || echo "MISSING: iceccd-version-probe.txt not produced")
    fact installed_iceccd_version_probe "$ICECCD_PROBE"
    SCHED_PROBE=$(cat "$BUILD/scheduler-version-probe.txt" 2>/dev/null || echo "MISSING: scheduler-version-probe.txt not produced")
    fact installed_scheduler_version_probe "$SCHED_PROBE"
    PC_VERSION_LINE=$(grep "^Version:" "$DESTDIR/usr/local/lib/pkgconfig/icecc.pc" 2>/dev/null || echo "ABSENT")
    fact installed_icecc_pc_version_line "$PC_VERSION_LINE"

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
