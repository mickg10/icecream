#!/bin/sh

# Prove that --with-boost's library directory is confined to the Boost
# feature probes and the explicit BOOST_* target variables.  The temporary
# Boost prefix deliberately also carries unrelated static archives.  A
# leaked -L option makes either configure's lzo probe or the regular build
# choose those incomplete archives and fail.

set -eu

: "${ICECC_TEST_TOP_SRCDIR:?ICECC_TEST_TOP_SRCDIR is required}"

icecc_test_cc=${ICECC_TEST_CC:-cc}
icecc_test_cxx=${ICECC_TEST_CXX:-c++}
icecc_test_ar=${ICECC_TEST_AR:-ar}
icecc_test_root=${TMPDIR:-/tmp}/icecc-boost-prefix-scope.$$
icecc_test_prefix=$icecc_test_root/boost-prefix
icecc_test_build=$icecc_test_root/build

cleanup()
{
    rm -rf "$icecc_test_root"
}
trap cleanup EXIT HUP INT TERM

mkdir -p "$icecc_test_prefix/include/boost" "$icecc_test_prefix/lib" \
    "$icecc_test_build"

# The wrapper makes the exact Boost selected by the outer configure visible
# behind the temporary prefix without copying a full Boost installation.
printf '%s\n' '#include_next <boost/version.hpp>' \
    > "$icecc_test_prefix/include/boost/version.hpp"

printf '%s\n' 'void icecc_unrelated_archive_marker(void) {}' \
    > "$icecc_test_root/unrelated.c"
"$icecc_test_cc" -c "$icecc_test_root/unrelated.c" \
    -o "$icecc_test_root/unrelated.o"
for icecc_test_library in lzo2 zstd archive; do
    "$icecc_test_ar" cr "$icecc_test_prefix/lib/lib${icecc_test_library}.a" \
        "$icecc_test_root/unrelated.o"
done

(
    cd "$icecc_test_build"
    CPPFLAGS="${ICECC_TEST_BOOST_CPPFLAGS:-} ${ICECC_TEST_CPPFLAGS:-}" \
        LDFLAGS="${ICECC_TEST_LDFLAGS:-}" \
        CC="$icecc_test_cc" CXX="$icecc_test_cxx" \
        ICE_CXX_STANDARD_FLAG="${ICECC_TEST_CXX_STANDARD_FLAG:-}" \
        LIBCAP_NG_CFLAGS="${ICECC_TEST_LIBCAP_NG_CFLAGS:-}" \
        LIBCAP_NG_LIBS="${ICECC_TEST_LIBCAP_NG_LIBS:-}" \
        LIBARCHIVE_CFLAGS="${ICECC_TEST_LIBARCHIVE_CFLAGS:-}" \
        LIBARCHIVE_LIBS="${ICECC_TEST_LIBARCHIVE_LIBS:-}" \
        LIBZSTD_CFLAGS="${ICECC_TEST_LIBZSTD_CFLAGS:-}" \
        LIBZSTD_LIBS="${ICECC_TEST_LIBZSTD_LIBS:-}" \
        "$ICECC_TEST_TOP_SRCDIR/configure" \
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

if ! make -C "$icecc_test_build" -j2 V=1 all \
        > "$icecc_test_root/build.log" 2>&1; then
    cat "$icecc_test_root/build.log" >&2
    exit 1
fi
if grep -F -- "-isystem $icecc_test_prefix/include" \
        "$icecc_test_root/build.log" >/dev/null || \
        grep -F -- "-L$icecc_test_prefix/lib" \
        "$icecc_test_root/build.log" >/dev/null; then
    echo "a non-Boost build target inherited the Boost prefix" >&2
    exit 1
fi
