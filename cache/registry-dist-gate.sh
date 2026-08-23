#!/bin/bash
# S0.3 deletion-sensitive distribution row (bigoracle bounded successor):
# from a fresh no-Git archive of the exact commit under test, a source
# distribution must carry exactly one cache/PROFILE-REGISTRY.md whose bytes
# equal the exact Git blob. Removing the EXTRA_DIST registration must make
# this row red. EXTRA_DIST-only; not wired into make check (requires
# autotools + a configured deps environment, like cache/formal/run_tlc.sh).
#
# Usage: registry-dist-gate.sh <git-repo> <commit-ish> <workdir>
#   The configure environment (CPPFLAGS/LDFLAGS/PKG_CONFIG_PATH/--with-boost
#   etc.) is taken from REGISTRY_DIST_CONFIGURE_ARGS + the caller's exported
#   env so the gate runs on any host with the project's build deps.
set -u
REPO=${1:?git repo}; COMMIT=${2:?commit}; WORK=${3:?workdir}
rm -rf "$WORK"; mkdir -p "$WORK/source" "$WORK/build"
git -C "$REPO" archive "$COMMIT" | tar -x -C "$WORK/source" || { echo "RED: archive failed"; exit 1; }
( cd "$WORK/source" && ./autogen.sh ) >"$WORK/log" 2>&1 || { echo "RED: autogen failed"; exit 1; }
( cd "$WORK/build" && "$WORK/source/configure" ${REGISTRY_DIST_CONFIGURE_ARGS:-} ) >>"$WORK/log" 2>&1 \
  || { echo "RED: configure failed"; exit 1; }
( cd "$WORK/build" && make dist-gzip ) >>"$WORK/log" 2>&1 || { echo "RED: make dist failed"; exit 1; }
TARBALL=$(ls "$WORK"/build/icecc-*.tar.gz 2>/dev/null | head -1)
[ -n "$TARBALL" ] || { echo "RED: no dist tarball"; exit 1; }
COUNT=$(tar -tzf "$TARBALL" | grep -cF "cache/PROFILE-REGISTRY.md")
[ "$COUNT" = "1" ] || { echo "RED: cache/PROFILE-REGISTRY.md entries in dist = $COUNT (require exactly 1)"; exit 1; }
tar -xzf "$TARBALL" -C "$WORK" --wildcards "*/cache/PROFILE-REGISTRY.md"
EXTRACTED=$(find "$WORK"/icecc-* -name PROFILE-REGISTRY.md | head -1)
git -C "$REPO" show "$COMMIT:cache/PROFILE-REGISTRY.md" | cmp -s - "$EXTRACTED" \
  || { echo "RED: distributed registry differs from the exact Git blob"; exit 1; }
echo "GREEN: dist carries exactly one cache/PROFILE-REGISTRY.md, byte-identical to $COMMIT"
