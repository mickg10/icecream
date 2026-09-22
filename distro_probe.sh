#!/bin/sh
# distro_probe.sh -- S1b exit-roster distro probes (2026-08-23 run).
#
# Reproduces the three distro-probe rows in S1B_EXIT_MANIFEST.md: install
# build deps for a distro, configure the icecc-1.5.0 dist tarball
# out-of-tree, build services+cache+client, run both release-identity
# tests. Run on a host with docker and the extracted dist tarball
# available; pass the extracted source dir as $1 and a scratch work dir as
# $2.
#
# IMPORTANT: every step for one distro must run inside a SINGLE `docker
# run` invocation. `docker run --rm` starts a fresh container each time --
# packages installed in one invocation do not persist into the next. (This
# script exists partly because the first live run of the ubuntu24 probe
# got this wrong and failed with "no acceptable C compiler found";
# see the manifest for that verbatim failure and the corrected rerun.)
set -eu

SRC=${1:?usage: distro_probe.sh SRC_DIR WORK_DIR DISTRO, DISTRO one of ubuntu22, ubuntu24, fedora40}
WORK=${2:?usage: distro_probe.sh SRC_DIR WORK_DIR DISTRO, DISTRO one of ubuntu22, ubuntu24, fedora40}
DISTRO=${3:?usage: distro_probe.sh SRC_DIR WORK_DIR DISTRO, DISTRO one of ubuntu22, ubuntu24, fedora40}
SRC=$(CDPATH= cd -- "$SRC" && pwd)
mkdir -p "$WORK"
WORK=$(CDPATH= cd -- "$WORK" && pwd)

run_probe() {
    image_ref=$1; expected_id=$2; install_cmd=$3
    if actual_id=$(docker image inspect "$image_ref" --format '{{.Id}}' 2>/dev/null); then
        :
    else
        actual_id=""
    fi
    if [ "$actual_id" != "$expected_id" ]; then
        echo "FAIL: immutable probe image $image_ref resolved to '${actual_id:-ABSENT}', expected '$expected_id'" >&2
        exit 1
    fi
    docker run --rm --pull=never -v "$SRC:/src:ro" -v "$WORK:/work" -u 0:0 "$image_ref" bash -c "
        set -e
        $install_cmd
        mkdir -p /work/build && cd /work/build
        /src/configure > configure.log 2>&1; echo CONFIGURE-EXIT=\$?
        make -C services -j8 > services.log 2>&1; echo SERVICES-EXIT=\$?
        make -C cache -j8 > cache.log 2>&1; echo CACHE-EXIT=\$?
        make -C client -j8 icecc libclient.a > client.log 2>&1; echo CLIENT-EXIT=\$?
        ./client/icecc --version
        ICECC_TEST_TOP_SRCDIR=/src /src/unittests/releaseidentity-source.sh; echo SOURCE_TEST_EXIT=\$?
        ICECC_RELEASE_IDENTITY_BIN=/work/build/client/icecc /src/unittests/releaseidentity-artifact.sh; echo ARTIFACT_TEST_EXIT=\$?
    "
}

case "$DISTRO" in
    ubuntu22)
        # This row used the pinned farm-node image, whose system packages
        # already cover every icecc build dependency -- no apt-get needed.
        run_probe sha256:fe001a6138f017608b8846b43bf268a76a9d7a5b66c3364ba3f881da2ff0c54b \
            sha256:fe001a6138f017608b8846b43bf268a76a9d7a5b66c3364ba3f881da2ff0c54b ':'
        ;;
    ubuntu24)
        run_probe ubuntu@sha256:33ceb71981b602c1a7443a53469e4dba065f7503eab3078a2d7a57a2ab987517 \
            sha256:a6f81fb630d51837271b89f8193810a5fc493fa4f30a55d7ebcdb3a66f3cc63a '
            apt-get update >/dev/null 2>&1
            DEBIAN_FRONTEND=noninteractive apt-get install -y g++ gcc make autoconf automake libtool pkg-config \
                libzstd-dev liblzo2-dev libarchive-dev libboost-dev libcap-ng-dev libxxhash-dev >/dev/null 2>&1
        '
        ;;
    fedora40)
        run_probe fedora@sha256:3c86d25fef9d2001712bc3d9b091fc40cf04be4767e48f1aa3b785bf58d300ed \
            sha256:b368d29df3b50e2acc0d6622493a29dafedbbc5a58ad03cab73bddca16c23858 '
            dnf install -y gcc gcc-c++ make autoconf automake libtool pkgconf-pkg-config \
                libzstd-devel lzo-devel libarchive-devel boost-devel libcap-ng-devel xxhash-devel >/dev/null 2>&1
        '
        ;;
    *)
        echo "unknown distro: $DISTRO (want ubuntu22, ubuntu24, or fedora40)" >&2
        exit 2
        ;;
esac
