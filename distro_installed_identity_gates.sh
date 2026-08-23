#!/bin/bash
# distro_installed_identity_gates.sh -- S1b local-oracle-HOLD successor on
# 1f316208. Two independent deletion-sensitive gates, modeled directly on
# the S0.3 pattern (cache/registry-dist-gate.sh):
#
# 1. MEMBERSHIP (packaging): from a fresh no-Git archive of the exact
#    commit under test, a real `make dist` must carry exactly one copy
#    each of distro_installed_identity.sh and S1B_EXIT_MANIFEST.md,
#    byte-identical to the exact Git blob. Removing either file's
#    EXTRA_DIST registration in Makefile.am must make this row red.
# 2. SENTINEL MUTATION (freshness): distro_installed_identity.sh's real
#    dotfile-inclusive cleanup line (`find $dir -mindepth 1 -delete`, at
#    both the build/ and destdir/ call sites) is temporarily neutered in a
#    scratch copy; running --sentinel-control against that mutated copy
#    must go RED (planted visible+hidden sentinels survive); the same
#    --sentinel-control run against the unmutated, dist-packaged copy
#    (extracted alongside the source it builds, from the SAME tarball --
#    not the ambient worktree) must go GREEN.
#
# Usage:
#   distro_installed_identity_gates.sh <git-repo> <commit-ish> <workdir> \
#       <distro> [--skip-sentinel]
#
#   MEMBERSHIP needs autotools + a configured deps environment (export
#   S1B_DIST_CONFIGURE_ARGS, mirroring registry-dist-gate.sh's
#   REGISTRY_DIST_CONFIGURE_ARGS). SENTINEL additionally needs docker and
#   network reachability to the DISTRO image, exactly like
#   distro_installed_identity.sh itself -- pass --skip-sentinel to run
#   membership only (e.g. in an environment without docker).
set -u
REPO=${1:?git repo}; COMMIT=${2:?commit}; WORK=${3:?workdir}; DISTRO=${4:?distro}
SKIP_SENTINEL=false
[ "${5:-}" = "--skip-sentinel" ] && SKIP_SENTINEL=true

rm -rf "$WORK"; mkdir -p "$WORK/source" "$WORK/build"
git -C "$REPO" archive "$COMMIT" | tar -x -C "$WORK/source" || { echo "RED: archive failed"; exit 1; }
( cd "$WORK/source" && ./autogen.sh ) >"$WORK/log" 2>&1 || { echo "RED: autogen failed"; exit 1; }
( cd "$WORK/build" && "$WORK/source/configure" ${S1B_DIST_CONFIGURE_ARGS:-} ) >>"$WORK/log" 2>&1 \
  || { echo "RED: configure failed"; exit 1; }
( cd "$WORK/build" && make dist-gzip ) >>"$WORK/log" 2>&1 || { echo "RED: make dist failed"; exit 1; }
TARBALL=$(ls "$WORK"/build/icecc-*.tar.gz 2>/dev/null | head -1)
[ -n "$TARBALL" ] || { echo "RED: no dist tarball"; exit 1; }
echo "dist tarball: $(basename "$TARBALL") sha256=$(sha256sum "$TARBALL" | cut -d' ' -f1)"

FAIL=0
for member in distro_installed_identity.sh S1B_EXIT_MANIFEST.md; do
    COUNT=$(tar -tzf "$TARBALL" | grep -cF "/$member")
    if [ "$COUNT" != "1" ]; then
        echo "RED: $member entries in dist = $COUNT (require exactly 1)"; FAIL=1; continue
    fi
    rm -rf "$WORK/extract-$member"; mkdir -p "$WORK/extract-$member"
    tar -xzf "$TARBALL" -C "$WORK/extract-$member" --wildcards "*/$member"
    EXTRACTED=$(find "$WORK/extract-$member" -name "$member" | head -1)
    if ! git -C "$REPO" show "$COMMIT:$member" | cmp -s - "$EXTRACTED"; then
        echo "RED: distributed $member differs from the exact Git blob"; FAIL=1; continue
    fi
    echo "GREEN: dist carries exactly one $member, byte-identical to $COMMIT"
done
[ "$FAIL" = "0" ] || { echo "MEMBERSHIP GATE: RED"; exit 1; }
echo "MEMBERSHIP GATE: GREEN"

if [ "$SKIP_SENTINEL" = true ]; then
    echo "SENTINEL GATE: skipped (--skip-sentinel)"
    exit 0
fi

# 2. SENTINEL MUTATION. Extract the FULL tarball once more (this time not
# filtered to one member) so the producer script sits alongside the exact
# source tree it builds -- both members of the SAME dist tarball, the
# thing team-lead's finding required running the producer from, not the
# ambient worktree copy.
rm -rf "$WORK/full-extract"; mkdir -p "$WORK/full-extract"
tar -xzf "$TARBALL" -C "$WORK/full-extract" || { echo "RED: full extract failed"; exit 1; }
SRCTREE=$(find "$WORK/full-extract" -maxdepth 1 -mindepth 1 -type d | head -1)
PRODUCER="$SRCTREE/distro_installed_identity.sh"
[ -x "$PRODUCER" ] || { echo "RED: extracted distro_installed_identity.sh missing or not executable at $PRODUCER"; exit 1; }

echo
echo "-- sentinel-control (unmutated, dist-packaged producer): must go GREEN --"
GREEN_WORK="$WORK/sentinel-green"
rm -rf "$GREEN_WORK"
"$PRODUCER" "$SRCTREE" "$GREEN_WORK" "$DISTRO" --sentinel-control >"$WORK/sentinel-green.log" 2>&1
GREEN_RC=$?
tail -20 "$WORK/sentinel-green.log"
if [ "$GREEN_RC" != "0" ]; then
    echo "RED: unmutated --sentinel-control exited $GREEN_RC (expected 0/GREEN) -- see $WORK/sentinel-green.log" >&2
    exit 1
fi
grep -qE '^sentinel_survivors[[:space:]]+none' "$GREEN_WORK/facts-normal-sentinel.txt" \
  || { echo "RED: unmutated run did not record sentinel_survivors=none" >&2; exit 1; }
echo "GREEN: unmutated producer's real cleanup line removed every planted sentinel"

echo
echo "-- mutating the dist-packaged producer's OWN copy: neuter both find -mindepth 1 -delete call sites --"
SNAPSHOT="$WORK/producer.pre-mutant-snapshot"
cp "$PRODUCER" "$SNAPSHOT"
python3 - "$PRODUCER" <<'PY'
import sys
path = sys.argv[1]
src = open(path).read()
old = "find /build -mindepth 1 -delete\n        find /destdir -mindepth 1 -delete\n"
new = "true # MUTATED-OUT-FOR-TEST (real cleanup neutered)\n        true # MUTATED-OUT-FOR-TEST (real cleanup neutered)\n"
count = src.count(old)
if count != 1:
    print(f"FAIL: expected exactly 1 occurrence of the cleanup line pair, found {count}", file=sys.stderr)
    sys.exit(1)
open(path, "w").write(src.replace(old, new, 1))
print("mutation applied")
PY
if [ $? -ne 0 ]; then
    echo "RED: could not apply the cleanup-neutering mutation" >&2
    cp "$SNAPSHOT" "$PRODUCER"
    exit 1
fi

echo
echo "-- sentinel-control (mutated: real cleanup neutered): must go RED --"
RED_WORK="$WORK/sentinel-red"
rm -rf "$RED_WORK"
"$PRODUCER" "$SRCTREE" "$RED_WORK" "$DISTRO" --sentinel-control >"$WORK/sentinel-red.log" 2>&1
RED_RC=$?
tail -20 "$WORK/sentinel-red.log"
cp "$SNAPSHOT" "$PRODUCER"
if cmp -s "$PRODUCER" "$SNAPSHOT"; then
    echo "ok - producer restored byte-exact after mutation"
else
    echo "RED: producer restoration is NOT byte-exact -- manual cleanup needed at $PRODUCER" >&2
    exit 1
fi
if [ "$RED_RC" = "0" ]; then
    echo "RED: mutated (cleanup-neutered) --sentinel-control exited 0 (expected non-zero/RED) -- the mutation had no effect, this gate is not exercising the intended line" >&2
    exit 1
fi
if grep -qE '^sentinel_survivors[[:space:]]+none' "$RED_WORK/facts-normal-sentinel.txt" 2>/dev/null; then
    echo "RED: mutated run still recorded sentinel_survivors=none -- the mutation had no effect" >&2
    exit 1
fi
echo "GREEN: REDDENED as required -- with the real cleanup line neutered, planted sentinels survived and --sentinel-control correctly caught it (exit $RED_RC)"

echo
echo "SENTINEL GATE: GREEN (both directions: unmutated clean/rebuild removes every sentinel; cleanup-neutered mutant is caught)"
