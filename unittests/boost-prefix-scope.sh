#!/bin/sh

# Prove that --with-boost's library directory is confined to the Boost
# feature probes and the explicit BOOST_* target variables.  The temporary
# Boost prefix deliberately also carries unrelated static archives.  A
# leaked -L option makes either configure's lzo probe or the regular build
# choose those incomplete archives and fail.

set -eu

: "${ICECC_TEST_TOP_SRCDIR:?ICECC_TEST_TOP_SRCDIR is required}"
: "${ICECC_TEST_TOP_BUILDDIR:?ICECC_TEST_TOP_BUILDDIR is required}"

icecc_test_make=${ICECC_TEST_MAKE:-make}
icecc_test_cc=${ICECC_TEST_CC:-cc}
icecc_test_cxx=${ICECC_TEST_CXX:-c++}
icecc_test_ar=${ICECC_TEST_AR:-ar}
icecc_test_ranlib=${ICECC_TEST_RANLIB:-ranlib}
icecc_test_root=${TMPDIR:-/tmp}/icecc-boost-prefix-scope.$$
icecc_test_prefix=$icecc_test_root/boost-prefix
icecc_test_build=$icecc_test_root/build
icecc_test_distdir=icecc-boost-prefix-scope-source.$$
icecc_test_source=$ICECC_TEST_TOP_BUILDDIR/$icecc_test_distdir

# Autoconf accepts compiler and tool variables as make-style commands, so a
# value such as "gcc -m64" is valid.  Split those commands into words without
# evaluating shell syntax; each original argument passed to the helper remains
# a separate argument.
icecc_run_make()
{
    # shellcheck disable=SC2086
    $icecc_test_make "$@"
}

icecc_run_cc()
{
    # shellcheck disable=SC2086
    $icecc_test_cc "$@"
}

icecc_run_ar()
{
    # shellcheck disable=SC2086
    $icecc_test_ar "$@"
}

cleanup()
{
    rm -rf "$icecc_test_root"
    rm -rf "$icecc_test_source"
}
trap cleanup EXIT HUP INT TERM

mkdir -p "$icecc_test_prefix/include/boost" "$icecc_test_prefix/lib" \
    "$icecc_test_build"

# A configured source directory cannot itself serve as the source of a nested
# out-of-tree configure.  Ask Automake for a clean, independent distribution
# tree.  This follows the same path for both in-source and out-of-tree parent
# builds and also exercises the files that a release archive carries.
if ! icecc_run_make -C "$ICECC_TEST_TOP_BUILDDIR" distdir \
        distdir="$icecc_test_distdir" \
        > "$icecc_test_root/distdir.log" 2>&1; then
    cat "$icecc_test_root/distdir.log" >&2
    exit 1
fi

# The wrapper makes the exact Boost selected by the outer configure visible
# behind the temporary prefix without copying a full Boost installation.
printf '%s\n' '#include_next <boost/version.hpp>' \
    > "$icecc_test_prefix/include/boost/version.hpp"

printf '%s\n' 'int icecc_unrelated_archive_marker;' \
    > "$icecc_test_root/unrelated.c"
# Compiler variables and flags use make's word-splitting convention.  This
# supports configured commands such as "gcc -m64" and preserves sysroot or
# other target-selection flags required by the outer build.
# shellcheck disable=SC2086
icecc_run_cc ${ICECC_TEST_CPPFLAGS:-} ${ICECC_TEST_CFLAGS:-} \
    -c "$icecc_test_root/unrelated.c" \
    -o "$icecc_test_root/unrelated.o"
for icecc_test_library in lzo2 zstd archive; do
    icecc_run_ar cr "$icecc_test_prefix/lib/lib${icecc_test_library}.a" \
        "$icecc_test_root/unrelated.o"
done

(
    cd "$icecc_test_build"
    CPPFLAGS="${ICECC_TEST_BOOST_CPPFLAGS:-} ${ICECC_TEST_CPPFLAGS:-}" \
        CFLAGS="${ICECC_TEST_CFLAGS:-}" \
        CXXFLAGS="${ICECC_TEST_CXXFLAGS:-}" \
        LDFLAGS="${ICECC_TEST_LDFLAGS:-}" \
        LIBS="${ICECC_TEST_LIBS:-}" \
        CC="$icecc_test_cc" CXX="$icecc_test_cxx" \
        AR="$icecc_test_ar" RANLIB="$icecc_test_ranlib" \
        ICE_CXX_STANDARD_FLAG="${ICECC_TEST_CXX_STANDARD_FLAG:-}" \
        LIBCAP_NG_CFLAGS="${ICECC_TEST_LIBCAP_NG_CFLAGS:-}" \
        LIBCAP_NG_LIBS="${ICECC_TEST_LIBCAP_NG_LIBS:-}" \
        LIBARCHIVE_CFLAGS="${ICECC_TEST_LIBARCHIVE_CFLAGS:-}" \
        LIBARCHIVE_LIBS="${ICECC_TEST_LIBARCHIVE_LIBS:-}" \
        LIBZSTD_CFLAGS="${ICECC_TEST_LIBZSTD_CFLAGS:-}" \
        LIBZSTD_LIBS="${ICECC_TEST_LIBZSTD_LIBS:-}" \
        XXHASH_CFLAGS="${ICECC_TEST_XXHASH_CFLAGS:-}" \
        XXHASH_LIBS="${ICECC_TEST_XXHASH_LIBS:-}" \
        "$icecc_test_source/configure" \
        --without-man --without-libcap-ng \
        --with-boost="$icecc_test_prefix"
)

# BOOST_LDFLAGS must remember the explicit prefix for a future target, while
# the ordinary CPPFLAGS and LDFLAGS contracts must not inherit it.
grep -F "BOOST_CPPFLAGS='-isystem $icecc_test_prefix/include " \
    "$icecc_test_build/config.log" >/dev/null
grep -F "BOOST_LDFLAGS='-L$icecc_test_prefix/lib'" \
    "$icecc_test_build/config.log" >/dev/null
if grep "^CPPFLAGS='.*-isystem $icecc_test_prefix/include" \
        "$icecc_test_build/config.log" >/dev/null; then
    echo "ordinary CPPFLAGS inherited the Boost prefix" >&2
    exit 1
fi
if grep "^LDFLAGS='.*-L$icecc_test_prefix/lib" \
        "$icecc_test_build/config.log" >/dev/null; then
    echo "ordinary LDFLAGS inherited the Boost prefix" >&2
    exit 1
fi

if ! icecc_run_make -C "$icecc_test_build" -j2 V=1 all \
        > "$icecc_test_root/build.log" 2>&1; then
    cat "$icecc_test_root/build.log" >&2
    exit 1
fi
if ! icecc_run_make -C "$icecc_test_build/unittests" -j2 V=1 p50endpoint \
        >> "$icecc_test_root/build.log" 2>&1; then
    cat "$icecc_test_root/build.log" >&2
    exit 1
fi
if ! grep -F -- "-isystem $icecc_test_prefix/include" \
        "$icecc_test_root/build.log" | grep -E \
        'libp50endpoint_a-|p50endpoint-p50_endpoint_test' >/dev/null; then
    echo "the P50 endpoint consumers did not receive BOOST_CPPFLAGS" >&2
    exit 1
fi
if ! grep -F -- "-L$icecc_test_prefix/lib" \
        "$icecc_test_root/build.log" | grep -F 'p50endpoint' >/dev/null; then
    echo "the P50 endpoint link did not receive BOOST_LDFLAGS" >&2
    exit 1
fi
if grep -F -- "-isystem $icecc_test_prefix/include" \
        "$icecc_test_root/build.log" | grep -Ev \
        'libp50endpoint_a-|p50endpoint-p50_endpoint_test' >/dev/null || \
        grep -F -- "-L$icecc_test_prefix/lib" \
        "$icecc_test_root/build.log" | grep -Fv 'p50endpoint' >/dev/null; then
    echo "a non-endpoint build target inherited the Boost prefix" >&2
    exit 1
fi
