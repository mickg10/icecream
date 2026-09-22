#!/bin/sh
# Fail-closed built-artifact identity gate for the release
# identity (S1b), split out from releaseidentity-source.sh per the
# bigoracle/local-oracle HOLD on 97ef314f.
#
# 97ef314f's original test folded this check into the source test, and it
# had two bugs: the binary-not-built-yet path printed "ok - skipped ..."
# and let the whole test PASS -- so an artifact-identity claim was
# satisfied even when no artifact existed -- and the comparison was a
# `case *1.5.90*` substring/glob match, which a corrupted build like
# "ICECC 11.5.900" would still satisfy.
#
# This test makes only the artifact claim, and makes it exactly: the
# environment must name a real, executable binary (ICECC_RELEASE_IDENTITY_BIN,
# wired by unittests/Makefile.am's AM_TESTS_ENVIRONMENT to
# $(abs_top_builddir)/client/icecc -- i.e. `make -C services && make -C
# cache && make -C client icecc` must already have run), and its
# `--version` output must equal the literal string "ICECC 1.5.0", not
# merely contain it. Missing, non-executable, or wrong output is RED; there
# is no skip path.
set -eu

: "${ICECC_RELEASE_IDENTITY_BIN:?required artifact path is absent}"

if [ ! -e "$ICECC_RELEASE_IDENTITY_BIN" ]; then
    echo "FAIL: $ICECC_RELEASE_IDENTITY_BIN does not exist -- build client/icecc first" >&2
    exit 1
fi
if [ ! -x "$ICECC_RELEASE_IDENTITY_BIN" ]; then
    echo "FAIL: $ICECC_RELEASE_IDENTITY_BIN exists but is not executable" >&2
    exit 1
fi
echo "ok - $ICECC_RELEASE_IDENTITY_BIN exists and is executable"

out=$("$ICECC_RELEASE_IDENTITY_BIN" --version)
if [ "$out" != "ICECC 1.5.0" ]; then
    echo "FAIL: $ICECC_RELEASE_IDENTITY_BIN --version printed '$out', expected exactly 'ICECC 1.5.0'" >&2
    exit 1
fi
echo "ok - $ICECC_RELEASE_IDENTITY_BIN --version is exactly 'ICECC 1.5.0'"

echo 'PASS: built release identity artifact is exactly ICECC 1.5.0'
