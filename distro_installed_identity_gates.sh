#!/bin/bash
# distro_installed_identity_gates.sh -- S1b local-oracle + BigOracle HOLD
# successor on 1f316208, updated per BigOracle's follow-up implementation-
# shape guidance. Three independent deletion-sensitive gates, MEMBERSHIP
# modeled directly on the S0.3 pattern (cache/registry-dist-gate.sh):
#
# 1. MEMBERSHIP (packaging): from a fresh no-Git archive of the exact
#    commit under test, a real `make dist` must carry exactly one copy
#    each of distro_installed_identity.sh, distro_installed_identity_gates.sh
#    (the gate ships ITSELF too), distro_probe.sh, and S1B_EXIT_MANIFEST.md
#    -- a self-contained evidence set -- byte-identical to the exact Git
#    blob, AND both shell scripts must pass `sh -n`/`bash -n` from their
#    extracted copies. Removing any one file's EXTRA_DIST registration in
#    Makefile.am must red its NAMED count before anything else runs against
#    that file.
# 2. SENTINEL MUTATION (freshness): distro_installed_identity.sh's real
#    clean step (the two `root=/build; empty_root` / `root=/destdir;
#    empty_root` call sites) is temporarily neutered in a scratch copy;
#    running --sentinel-control against that mutated copy must go RED,
#    specifically by tripping the explicit POST-CLEAN-EMPTY assertion (not
#    merely a later, indirect symptom) -- BigOracle explicitly rejected the
#    earlier --stale-control shape as proof of this because it branches
#    around the real normal pipeline entirely and never reaches that
#    assertion; --sentinel-control does not branch around it. The same
#    --sentinel-control run against the unmutated, dist-packaged copy
#    (extracted alongside the source it builds, from the SAME tarball --
#    not the ambient worktree) must go GREEN. Run on ALL THREE distros.
# 3. PER-ARTIFACT DELETION MATRIX (BigOracle): reusing a GREEN sentinel
#    run's own build/destdir (already proven indistinguishable from a
#    plain normal run's output), --corrupt-control=ARTIFACT is run once
#    per artifact -- icecc, iceccd, icecc-scheduler, libicecc.a, icecc.pc,
#    image-digest, package-inventory, build-log -- each corrupting/deleting
#    exactly that one artifact and requiring the run to fail, NAMING it.
#    All from the same extracted successor archive as gates 1-2.
#
# Usage:
#   distro_installed_identity_gates.sh <git-repo> <commit-ish> <workdir> \
#       <matrix-distro> [--skip-sentinel] [--sentinel-distros=D1,D2,D3]
#
#   MEMBERSHIP needs autotools + a configured deps environment (export
#   S1B_DIST_CONFIGURE_ARGS, mirroring registry-dist-gate.sh's
#   REGISTRY_DIST_CONFIGURE_ARGS). SENTINEL and the per-artifact matrix
#   additionally need docker and network reachability to every distro
#   image involved -- pass --skip-sentinel to run membership only (e.g. in
#   an environment without docker; this also skips the per-artifact
#   matrix, which depends on a sentinel run's build/destdir).
#   <matrix-distro> is the one distro the per-artifact matrix (gate 3)
#   runs against; the sentinel mutation control (gate 2) always runs
#   against all three (ubuntu22, ubuntu24, fedora40) unless overridden via
#   --sentinel-distros=... (comma list; e.g. to skip a distro whose image
#   is not reachable from the current host).
set -u
REPO=${1:?git repo}; COMMIT=${2:?commit}; WORK=${3:?workdir}; MATRIX_DISTRO=${4:?matrix distro}
SKIP_SENTINEL=false
SENTINEL_DISTROS="ubuntu22,ubuntu24,fedora40"
for arg in "${@:5}"; do
    case "$arg" in
        --skip-sentinel) SKIP_SENTINEL=true ;;
        --sentinel-distros=*) SENTINEL_DISTROS=${arg#--sentinel-distros=} ;;
    esac
done

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
for member in distro_installed_identity.sh distro_installed_identity_gates.sh distro_probe.sh S1B_EXIT_MANIFEST.md; do
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
    case "$member" in
        *.sh)
            if ! sh -n "$EXTRACTED" 2>"$WORK/synchk-$member.err"; then
                echo "RED: extracted $member fails sh -n: $(cat "$WORK/synchk-$member.err")"; FAIL=1; continue
            fi
            ;;
    esac
    echo "GREEN: dist carries exactly one $member, byte-identical to $COMMIT$( [ "${member##*.}" = sh ] && echo ", sh -n clean" )"
done
[ "$FAIL" = "0" ] || { echo "MEMBERSHIP GATE: RED"; exit 1; }
echo "MEMBERSHIP GATE: GREEN"

if [ "$SKIP_SENTINEL" = true ]; then
    echo "SENTINEL GATE: skipped (--skip-sentinel)"
    echo "PER-ARTIFACT MATRIX: skipped (--skip-sentinel)"
    exit 0
fi

# Extract the FULL tarball once more (this time not filtered to one
# member) so the producer script sits alongside the exact source tree it
# builds -- both members of the SAME dist tarball, the thing BigOracle's
# finding required running the producer from ("the EXACT EXTRACTED
# SUCCESSOR ARCHIVE"), not the ambient worktree copy.
rm -rf "$WORK/full-extract"; mkdir -p "$WORK/full-extract"
tar -xzf "$TARBALL" -C "$WORK/full-extract" || { echo "RED: full extract failed"; exit 1; }
SRCTREE=$(find "$WORK/full-extract" -maxdepth 1 -mindepth 1 -type d | head -1)
PRODUCER="$SRCTREE/distro_installed_identity.sh"
[ -x "$PRODUCER" ] || { echo "RED: extracted distro_installed_identity.sh missing or not executable at $PRODUCER"; exit 1; }

SNAPSHOT="$WORK/producer.pre-mutant-snapshot"
cp "$PRODUCER" "$SNAPSHOT"

sentinel_gate_one_distro() {
    d=$1
    echo
    echo "== SENTINEL GATE ($d) =="
    echo "-- sentinel-control (unmutated, dist-packaged producer): must go GREEN --"
    GREEN_WORK="$WORK/sentinel-green-$d"
    rm -rf "$GREEN_WORK"
    "$PRODUCER" "$SRCTREE" "$GREEN_WORK" "$d" --sentinel-control >"$WORK/sentinel-green-$d.log" 2>&1
    GREEN_RC=$?
    tail -20 "$WORK/sentinel-green-$d.log"
    if [ "$GREEN_RC" != "0" ]; then
        echo "RED ($d): unmutated --sentinel-control exited $GREEN_RC (expected 0/GREEN) -- see $WORK/sentinel-green-$d.log" >&2
        return 1
    fi
    grep -qE '^post_clean_empty[[:space:]]+YES' "$GREEN_WORK/facts-normal-sentinel.txt" \
      || { echo "RED ($d): unmutated run did not record post_clean_empty=YES" >&2; return 1; }
    grep -qE '^sentinel_survivors[[:space:]]+none' "$GREEN_WORK/facts-normal-sentinel.txt" \
      || { echo "RED ($d): unmutated run did not record sentinel_survivors=none" >&2; return 1; }
    echo "GREEN ($d): unmutated producer's real clean step removed both planted sentinels (post_clean_empty=YES, sentinel_survivors=none)"

    echo
    echo "-- mutating the dist-packaged producer's OWN copy ($d): neuter both empty_root call sites --"
    python3 - "$PRODUCER" <<'PY'
import sys
path = sys.argv[1]
src = open(path).read()
old = "        root=/build; empty_root\n        root=/destdir; empty_root\n"
new = "        root=/build; true # MUTATED-OUT-FOR-TEST (real clean neutered)\n        root=/destdir; true # MUTATED-OUT-FOR-TEST (real clean neutered)\n"
count = src.count(old)
if count != 1:
    print(f"FAIL: expected exactly 1 occurrence of the empty_root call-site pair, found {count}", file=sys.stderr)
    sys.exit(1)
open(path, "w").write(src.replace(old, new, 1))
print("mutation applied")
PY
    if [ $? -ne 0 ]; then
        echo "RED ($d): could not apply the clean-step-neutering mutation" >&2
        cp "$SNAPSHOT" "$PRODUCER"
        return 1
    fi

    echo
    echo "-- sentinel-control (mutated: real clean neutered, $d): must go RED at the POST-CLEAN-EMPTY assertion, not branch around it --"
    RED_WORK="$WORK/sentinel-red-$d"
    rm -rf "$RED_WORK"
    "$PRODUCER" "$SRCTREE" "$RED_WORK" "$d" --sentinel-control >"$WORK/sentinel-red-$d.log" 2>&1
    RED_RC=$?
    tail -20 "$WORK/sentinel-red-$d.log"
    cp "$SNAPSHOT" "$PRODUCER"
    if cmp -s "$PRODUCER" "$SNAPSHOT"; then
        echo "ok ($d) - producer restored byte-exact after mutation"
    else
        echo "RED ($d): producer restoration is NOT byte-exact -- manual cleanup needed at $PRODUCER" >&2
        return 1
    fi
    if [ "$RED_RC" = "0" ]; then
        echo "RED ($d): mutated (clean-neutered) --sentinel-control exited 0 (expected non-zero/RED) -- the mutation had no effect" >&2
        return 1
    fi
    # The precise, checkable proof BigOracle asked for: the run must have
    # actually REACHED and TRIPPED the post-clean-empty assertion (recorded
    # NO, not merely absent because some earlier, unrelated thing failed).
    if ! grep -qE '^post_clean_empty[[:space:]]+NO' "$RED_WORK/facts-normal-sentinel.txt" 2>/dev/null; then
        echo "RED ($d): mutated run did not record post_clean_empty=NO -- did not demonstrably reach the assertion it is supposed to trip" >&2
        return 1
    fi
    if grep -qE '^sentinel_survivors[[:space:]]+none' "$RED_WORK/facts-normal-sentinel.txt" 2>/dev/null; then
        echo "RED ($d): mutated run still recorded sentinel_survivors=none -- the mutation had no effect" >&2
        return 1
    fi
    echo "GREEN ($d): REDDENED as required -- with the real clean step neutered, the POST-CLEAN-EMPTY assertion itself caught the surviving sentinels (post_clean_empty=NO) before configure ever ran (exit $RED_RC)"
    return 0
}

SENTINEL_FAIL=0
OLD_IFS=$IFS; IFS=','
for d in $SENTINEL_DISTROS; do
    IFS=$OLD_IFS
    sentinel_gate_one_distro "$d" || SENTINEL_FAIL=1
    IFS=','
done
IFS=$OLD_IFS
[ "$SENTINEL_FAIL" = "0" ] || { echo; echo "SENTINEL GATE: RED"; exit 1; }
echo
echo "SENTINEL GATE: GREEN on all of: $SENTINEL_DISTROS (both directions each: unmutated clean/rebuild removes every sentinel and asserts it; clean-neutered mutant is caught at the assertion itself)"

# PER-ARTIFACT DELETION MATRIX. Reuses the matrix distro's own GREEN
# sentinel run build/destdir as the clean baseline -- that run already
# proved it ends in a state indistinguishable from a plain normal run's
# output (sentinel_survivors=none), and --corrupt-control never rebuilds,
# so copying it is safe and avoids a redundant full rebuild per artifact.
GREEN_WORK="$WORK/sentinel-green-$MATRIX_DISTRO"
[ -d "$GREEN_WORK/build" ] || { echo "RED: no GREEN sentinel run build/destdir for matrix distro $MATRIX_DISTRO (was it in --sentinel-distros?)" >&2; exit 1; }
echo
echo "== per-artifact deletion/corruption matrix (BigOracle HOLD on 1f316208), distro=$MATRIX_DISTRO =="
MATRIX_FAIL=0
matrix_label() {
    case "$1" in
        icecc)              echo installed_icecc_version_output ;;
        iceccd)              echo installed_iceccd_version_probe ;;
        icecc-scheduler)     echo installed_scheduler_version_probe ;;
        libicecc.a)          echo installed_libicecc_a_file ;;
        icecc.pc)            echo installed_icecc_pc_version_line ;;
        image-digest)        echo image_digest ;;
        package-inventory)   echo package_inventory_file ;;
        build-log)           echo configure_log_file ;;
    esac
}
for artifact in icecc iceccd icecc-scheduler libicecc.a icecc.pc image-digest package-inventory build-log; do
    MATRIX_WORK="$WORK/matrix-$artifact"
    rm -rf "$MATRIX_WORK"; mkdir -p "$MATRIX_WORK"
    if [ "$artifact" != image-digest ]; then
        cp -a "$GREEN_WORK/build" "$MATRIX_WORK/build"
        cp -a "$GREEN_WORK/destdir" "$MATRIX_WORK/destdir"
    fi
    OUT=$("$PRODUCER" "$SRCTREE" "$MATRIX_WORK" "$MATRIX_DISTRO" "--corrupt-control=$artifact" 2>&1)
    RC=$?
    LABEL=$(matrix_label "$artifact")
    echo "-- corrupt-control=$artifact (expect FAIL naming $LABEL) --"
    if [ "$RC" = "0" ]; then
        echo "RED: --corrupt-control=$artifact exited 0 (expected non-zero/RED)" >&2
        echo "$OUT" | tail -15
        MATRIX_FAIL=1
        continue
    fi
    FAILLINE=$(echo "$OUT" | grep -E '^FAIL: ' | head -1)
    if [ -z "$FAILLINE" ]; then
        echo "RED: --corrupt-control=$artifact exited $RC but produced no 'FAIL: ...' line" >&2
        echo "$OUT" | tail -15
        MATRIX_FAIL=1
        continue
    fi
    case "$FAILLINE" in
        *"$LABEL"*)
            echo "ok - $FAILLINE"
            ;;
        *)
            echo "RED: --corrupt-control=$artifact failed, but not naming $LABEL -- got: $FAILLINE" >&2
            MATRIX_FAIL=1
            ;;
    esac
done
[ "$MATRIX_FAIL" = "0" ] || { echo "PER-ARTIFACT MATRIX: RED"; exit 1; }
echo
echo "PER-ARTIFACT MATRIX: GREEN (all 8 artifacts, corrupted one at a time, each failed naming itself)"
