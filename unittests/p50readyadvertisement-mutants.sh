#!/bin/sh
# Compile and execute four policy mutants; every one must be killed by the
# same behavioral test used by make check.
set -eu

src=${ICECC_TEST_TOP_SRCDIR:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
build_dir=${ICECC_TEST_BUILDDIR:-${builddir:-$(pwd)}}
cxx=${ICECC_TEST_CXX:-${CXX:-c++}}
standard=${ICECC_TEST_CXX_STANDARD_FLAG:-${ICE_CXX_STANDARD_FLAG:--std=c++23}}
mutant_dir=$(mktemp -d "$build_dir/p50readyadvertisement-mutants.XXXXXX")
trap 'rm -rf "$mutant_dir"' EXIT HUP INT TERM

compile_and_kill() {
    name=$1
    expression=$2
    replacement=$3
    mutant="$mutant_dir/$name.cpp"
    binary="$mutant_dir/$name"

    python3 - "$src/cache/p50_ready_advertisement.cpp" "$mutant" \
        "$expression" "$replacement" <<'PY'
import pathlib
import sys

source = pathlib.Path(sys.argv[1]).read_text()
old, new = sys.argv[3], sys.argv[4]
if source.count(old) != 1:
    raise SystemExit(f"mutation anchor count for {old!r}: {source.count(old)}")
pathlib.Path(sys.argv[2]).write_text(source.replace(old, new))
PY

    "$cxx" "$standard" -Wall -Wextra -Wpedantic -Werror \
        -I"$src" -I"$src/cache" -I"$src/services" \
        "$src/unittests/p50_ready_advertisement_test.cpp" "$mutant" \
        -o "$binary"
    if "$binary" >"$mutant_dir/$name.log" 2>&1; then
        echo "FAIL: $name mutant survived" >&2
        cat "$mutant_dir/$name.log" >&2
        exit 1
    fi
    echo "ok - $name mutant killed"
}

compile_and_kill ready-gate \
    'const bool ready = observation.supervisor_state == sidecar::State::Ready;' \
    'const bool ready = true;'
compile_and_kill transient-auth-reintroduced \
    '&& observation.current_lease_matches;' \
    '&& observation.current_lease_matches && observation.private_relationship_authenticated;'
compile_and_kill current-lease-gate \
    '&& observation.current_lease_matches;' \
    '&& true;'
compile_and_kill crash-withdrawal \
    'if (crashed && current_.present()) {' \
    'if ((static_cast<void>(crashed), false) && current_.present()) {'
compile_and_kill runnable-profile \
    'CACHE_WIRE_PROTOCOL_V1, CACHE_PROFILE_ZSTD_TU' \
    'CACHE_WIRE_PROTOCOL_V1, CACHE_PROFILE_P29'
compile_and_kill counter-saturation \
    '== std::numeric_limits<uint64_t>::max()) {' \
    '== std::numeric_limits<uint64_t>::max() && false) {'

echo 'PASS: READY advertisement behavioral mutants are all red'
