#!/usr/bin/env bash
set -euo pipefail

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
FAILURES=0

require_count() {
    local expected="$1" pattern="$2" path="$3" label="$4" actual
    actual="$(grep -F -c -- "$pattern" "$ROOT/$path" || true)"
    if [ "$actual" -ne "$expected" ]; then
        echo "not ok - $label (expected $expected, found $actual)" >&2
        FAILURES=$((FAILURES + 1))
    else
        echo "ok - $label"
    fi
}

for distro in ubuntu22.04 ubuntu24.04; do
    script="package_builder/$distro/build_deb.sh"
    for package in libboost-dev libxxhash-dev libzstd-dev liblzo2-dev libarchive-dev; do
        require_count 1 "    $package \\" "$script" \
            "$distro explicitly installs $package"
    done
    require_count 1 'probe_build_requirements.sh" | bash' "$script" \
        "$distro executes the committed pre-build probe"
    require_count 1 'release_tarball_sha256=' "$script" \
        "$distro records release archive identity"
done

for package in boost-devel xxhash-devel libzstd-devel lzo-devel libarchive-devel; do
    require_count 1 "    $package \\" package_builder/fedora-latest/build_rpm.sh \
        "Fedora latest explicitly installs $package"
done
require_count 1 'probe_build_requirements.sh" | bash' \
    package_builder/fedora-latest/build_rpm.sh \
    'Fedora latest executes the committed pre-build probe'
require_count 1 'release_tarball_sha256=' \
    package_builder/fedora-latest/build_rpm.sh \
    'Fedora latest records release archive identity'

require_count 1 '#if BOOST_VERSION < 107400' \
    package_builder/probe_build_requirements.sh \
    'probe enforces Boost 1.74 minimum'
require_count 1 '#if XXH_VERSION_NUMBER < 801' \
    package_builder/probe_build_requirements.sh \
    'probe enforces xxHash 0.8.1 minimum'
require_count 1 '-lxxhash -lzstd -llzo2 -larchive' \
    package_builder/probe_build_requirements.sh \
    'probe links every mandatory non-Boost library'

for script in package_builder/fedora28/build_rpm.sh package_builder/fedora28/verify_rpm.sh; do
    require_count 1 'UNSUPPORTED: Fedora 28' "$script" \
        "$(basename "$script") refuses the unsupported Fedora 28 row"
    require_count 1 'exit 78' "$script" \
        "$(basename "$script") uses the deterministic unsupported status"
done

if [ "$FAILURES" -ne 0 ]; then
    echo "FAIL: $FAILURES package-builder dependency contract failure(s)" >&2
    exit 1
fi

echo 'PASS: package-builder dependency contract is deletion-sensitive'
