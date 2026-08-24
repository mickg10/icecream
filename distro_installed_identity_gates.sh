#!/bin/bash
# distro_installed_identity_gates.sh -- S1b local-oracle + BigOracle HOLD
# successor on 1f316208, updated per BigOracle's follow-up implementation-
# shape guidance, then per BigOracle's definitive S1b blueprint. Gates,
# each mutation-tested to prove it can actually go RED:
#
# 1. MEMBERSHIP (packaging): from a fresh no-Git archive of the exact
#    commit under test, a real `make dist` must carry exactly one copy
#    each of distro_installed_identity.sh, distro_installed_identity_gates.sh
#    (the gate ships ITSELF too), distro_probe.sh, and S1B_EXIT_MANIFEST.md
#    -- byte-identical to the exact Git blob, with no .git anywhere in
#    either extraction, and both shell scripts pass `sh -n` from BOTH their
#    standalone per-member extraction AND the full source tree extraction
#    that the rest of this script actually runs the producer from. An
#    EXTRA_DIST-deletion mutant (one registration line removed from a
#    scratch copy of Makefile.am) must red this gate's membership count --
#    and does so BEFORE any docker-touching gate below ever starts.
# 2. SENTINEL MUTATION (freshness): distro_installed_identity.sh's real
#    clean step is temporarily neutered in a scratch copy of the
#    dist-extracted producer -- specifically ONLY the `find ... -exec rm
#    -rf -- {} +` line INSIDE empty_root(), leaving both call sites
#    (`root=/build; empty_root` / `root=/destdir; empty_root`), the
#    emptiness assertion, and the entire configure/build/install/probe
#    pipeline completely intact and genuinely live. Running
#    --sentinel-control against that mutated copy must go RED specifically
#    by tripping the POST-CLEAN-EMPTY assertion (post_clean_empty=NO),
#    proving the assertion is reachable and load-bearing, not merely
#    proving SOME later stage happened to notice stale content. An earlier
#    version of this mutation neutered the two CALL SITES instead --
#    confirmed broken (empty_root() itself, including its own assertion,
#    was then never invoked at all, so POST-CLEAN-EMPTY=YES fired
#    unconditionally regardless of sentinel survival). The same
#    --sentinel-control run against the unmutated, dist-packaged copy must
#    go GREEN. Run on ALL THREE distros.
# 3. IMAGE IDENTITY, TWO PIN CHANNELS: registry-pulled images (ubuntu24,
#    fedora40) pin by DIGEST-QUALIFIED REF (repo@sha256:...) + --pull=never,
#    UNRESOLVED/empty RepoDigests/tag-digest disagreement fatal. The pinned
#    ubuntu22 farm-node image has NO registry anywhere -- confirmed operator
#    fact (icecream/farm-node's RepoDigests is unconditionally empty on
#    every host running the classic, non-containerd docker store; there is
#    no "build/run it somewhere else" fix) -- so it pins by IMAGE ID
#    instead: the tag must currently resolve to a recorded, hard-coded
#    PINNED_IMAGE_ID. This is EQUIVALENT binding strength through a
#    different provenance channel, not a weaker check -- an image ID is
#    itself a content-addressed config digest. Both channels feed the SAME
#    downstream guarantee: the normal-mode docker run is mutated to launch
#    a locally-built decoy image instead of $IMAGE_REF, while the
#    fact-computation code (which still inspects the real per-distro tag
#    and prints the real, correct image_digest/image_ref/image_id, or
#    image_id_pin_check on the local-id channel) is left completely
#    untouched. This must red via container_run_image_id -- the producer's
#    post-run check of what the container actually ran from -- even though
#    the printed pre-run facts still describe the real, correct image.
#    Proves the pre-run facts alone are not the thing keeping this gate
#    honest, on EITHER channel: the image-identity mutant runs against both
#    MATRIX_DISTRO (registry-digest) and ubuntu22 (local-id), and a
#    separate corrupt-control=image-digest row against ubuntu22 confirms
#    the local-id channel's OWN pre-run gate (image_id_pin_check, not
#    image_digest) is independently deletion-sensitive too.
# 4. PER-ARTIFACT/COMPETING-IDENTITY MATRIX (BigOracle): reusing a GREEN sentinel
#    run's own build/destdir (already proven indistinguishable from a
#    plain normal run's output), --corrupt-control=ARTIFACT is run once
#    per artifact/predicate -- icecc, icecc-create-env, iceccd
#    absent/wrong plus competing identity, scheduler absent/wrong plus
#    competing identity, libicecc.a, icecc.pc wrong plus competing Version,
#    image authority, package inventory, build log -- each changing exactly
#    that one artifact and requiring the run to fail, NAMING it. After each
#    row, the read-only-mounted source
#    tree ($SRCTREE) is re-hashed and required to remain byte-for-byte
#    identical to its pre-matrix snapshot -- the corruption must land only
#    in build/destdir, never in source.
#
# All of gates 2-4 run the producer ONLY from the full dist-tarball
# extraction ($SRCTREE / $PRODUCER below) -- never the ambient worktree.
#
# Usage:
#   distro_installed_identity_gates.sh <git-repo> <commit-ish> <workdir> \
#       <matrix-distro> [--skip-sentinel] [--sentinel-distros=D1,D2,D3]
#
#   MEMBERSHIP builds the dist tarball (autogen/configure/make dist-gzip)
#   inside the pinned ubuntu22 farm-node image rather than on the bare
#   host: that image already carries a real Boost >= 1.74 (with the Asio
#   coroutine APIs this tree's configure.ac requires) and a full autotools
#   toolchain, so this sidesteps whatever autotools/Boost package versions
#   happen to be on the host running this script. Set
#   S1B_DIST_CONFIGURE_ARGS to add/override configure flags (mirroring
#   registry-dist-gate.sh's REGISTRY_DIST_CONFIGURE_ARGS); the default is
#   --without-man because this pinned producer image intentionally has no
#   asciidoc/a2x toolchain, while its Boost is already new enough.
#   MEMBERSHIP therefore needs docker + the farm-node image reachable;
#   pass --skip-sentinel to additionally skip gates 2-4 (SENTINEL, IMAGE
#   IDENTITY, and the per-artifact matrix), which need docker reachability
#   to every distro image involved, not just the farm-node one.
#   <matrix-distro> is the one distro gates 3-4 run against; the sentinel
#   mutation control (gate 2) always runs against all three (ubuntu22,
#   ubuntu24, fedora40) unless overridden via
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

S1B_MEMBERS="distro_installed_identity.sh distro_installed_identity_gates.sh distro_probe.sh S1B_EXIT_MANIFEST.md"
DIST_IMAGE=icecream/farm-node:ubuntu22-gcc11-boost174
S1B_DIST_CONFIGURE_ARGS=${S1B_DIST_CONFIGURE_ARGS:---without-man}
S1B_SOURCE_DATE_EPOCH=$(git -C "$REPO" show -s --format=%ct "$COMMIT")
case "$S1B_SOURCE_DATE_EPOCH" in
    ''|*[!0-9]*) echo "RED: exact commit has no numeric source-date epoch" >&2; exit 1 ;;
esac

dist_build() {
    # dist_build SOURCE_DIR BUILD_DIR LOG_FILE -- autogen.sh (needs write
    # access to SOURCE_DIR, which is why it isn't :ro) + out-of-tree
    # configure + make dist-gzip, all inside $DIST_IMAGE so this depends
    # on a real, matched Boost/autotools toolchain instead of whatever the
    # host running this gate script happens to have installed.
    # Runs as the host UID:GID, not root -- autoconf/automake/make need no
    # privilege, and this keeps every generated file host-user-owned and
    # cleanable by later plain `rm -rf` (a root-owned autom4te.cache/ from
    # an earlier -u 0:0 attempt needed a root container just to delete).
    src=$1 build=$2 log=$3
    docker run --rm --pull=never -v "$src:/dsrc" -v "$build:/dbuild" -u "$(id -u):$(id -g)" "$DIST_IMAGE" bash -c "
        set -e
        export LC_ALL=C TZ=UTC SOURCE_DATE_EPOCH=$S1B_SOURCE_DATE_EPOCH
        export TAR_OPTIONS='--sort=name --mtime=@$S1B_SOURCE_DATE_EPOCH --owner=0 --group=0 --numeric-owner'
        umask 022
        cd /dsrc && ./autogen.sh
        cd /dbuild && /dsrc/configure $S1B_DIST_CONFIGURE_ARGS
        make dist-gzip
    " >"$log" 2>&1
}

rm -rf "$WORK"; mkdir -p "$WORK/source" "$WORK/build"
git -C "$REPO" archive "$COMMIT" | tar -x -C "$WORK/source" || { echo "RED: archive failed"; exit 1; }
if find "$WORK/source" -name .git -print -quit | grep -q .; then
    echo "RED: git-archive extraction at $WORK/source contains a .git entry -- expected none"; exit 1
fi
echo "ok - git-archive extraction contains no .git"

# The retained build-tree compatibility probe is not part of the installed
# EXIT verdict, but because it is shipped as a reproducer it must not retain
# a mutable-image escape hatch.  Bind all three shipped authorities together
# before any Docker row.
for authority in \
    sha256:fe001a6138f017608b8846b43bf268a76a9d7a5b66c3364ba3f881da2ff0c54b \
    ubuntu@sha256:33ceb71981b602c1a7443a53469e4dba065f7503eab3078a2d7a57a2ab987517 \
    fedora@sha256:3c86d25fef9d2001712bc3d9b091fc40cf04be4767e48f1aa3b785bf58d300ed
do
    for authority_file in distro_installed_identity.sh distro_probe.sh S1B_EXIT_MANIFEST.md; do
        grep -qF "$authority" "$WORK/source/$authority_file" || {
            echo "RED: $authority_file is not bound to committed image authority $authority" >&2
            exit 1
        }
    done
done
grep -qF 'docker run --rm --pull=never' "$WORK/source/distro_probe.sh" || {
    echo "RED: retained distro_probe.sh does not fail closed on implicit pulls" >&2
    exit 1
}
python3 - "$WORK/source/distro_installed_identity.sh" <<'PY'
import sys

text = open(sys.argv[1]).read()
anchors = {
    'require_exact "${label}_mode" "$mode" "$expected_mode"': 1,
    'rows = json.load(stream)': 1,
    'set(paths) != expected': 1,
    'require_exact image_digest_authority "$IMAGE_DIGEST" "$PINNED_IMAGE_REF"': 1,
    'require_count1 installed_iceccd_identity_total_count': 2,
    'require_count1 installed_scheduler_identity_total_count': 2,
    'require_count1 installed_icecc_pc_version_total_count': 2,
}
for needle, expected in anchors.items():
    actual = text.count(needle)
    if actual != expected:
        raise SystemExit(
            f"RED: installed-identity production anchor {needle!r}: "
            f"expected {expected}, found {actual}")
print("ok - installed manifest, mode, image-authority and exact-cardinality anchors are load-bearing")
PY
echo "ok - producer, retained probe and contract share immutable image authorities"

# EXTRA_DIST-deletion mutant, BEFORE any docker-touching gate begins: a
# SEPARATE fresh archive of the same commit, with exactly one EXTRA_DIST
# registration line removed from Makefile.am, must make its OWN `make
# dist` carry zero copies of that member -- proving the membership count
# below is actually sensitive to the registration, not just counting
# something that would be there regardless (e.g. via an unrelated
# `dist_` rule, or being swept in by a wildcard).
echo
echo "== EXTRA_DIST-deletion mutants (must RED before any docker row) =="
for MUT_MEMBER in $S1B_MEMBERS; do
    MUTWORK="$WORK/extra-dist-deletion-mutant-$MUT_MEMBER"
    rm -rf "$MUTWORK"; mkdir -p "$MUTWORK/source" "$MUTWORK/build"
    git -C "$REPO" archive "$COMMIT" | tar -x -C "$MUTWORK/source" || { echo "RED: mutant archive failed for $MUT_MEMBER"; exit 1; }
    python3 - "$MUTWORK/source/Makefile.am" "$MUT_MEMBER" <<'PY'
import sys
path, member = sys.argv[1:]
lines = open(path).readlines()
matches = [i for i, line in enumerate(lines) if line.strip().rstrip('\\').strip() == member]
if len(matches) != 1:
    print(f"FAIL: expected exactly 1 EXTRA_DIST line for {member!r}, found {len(matches)}", file=sys.stderr)
    sys.exit(1)
del_index = matches[0]
if not lines[del_index].strip().endswith('\\') and del_index > 0 and lines[del_index - 1].rstrip().endswith('\\'):
    lines[del_index - 1] = lines[del_index - 1].rstrip()[:-1] + "\n"
del lines[del_index]
open(path, "w").writelines(lines)
print(f"mutation applied: removed {member}'s EXTRA_DIST line")
PY
    if [ $? -ne 0 ]; then
        echo "RED: could not apply the EXTRA_DIST-deletion mutation for $MUT_MEMBER" >&2
        exit 1
    fi
    dist_build "$MUTWORK/source" "$MUTWORK/build" "$MUTWORK/log" || { echo "RED: mutant dist build failed for $MUT_MEMBER"; tail -30 "$MUTWORK/log" >&2; exit 1; }
    MUTTARBALL=$(ls "$MUTWORK"/build/icecc-*.tar.gz 2>/dev/null | head -1)
    [ -n "$MUTTARBALL" ] || { echo "RED: mutant produced no dist tarball for $MUT_MEMBER"; exit 1; }
    MUTCOUNT=$(tar -tzf "$MUTTARBALL" | grep -cF "/$MUT_MEMBER")
    if [ "$MUTCOUNT" != "0" ]; then
        echo "RED: EXTRA_DIST-deletion mutant for $MUT_MEMBER still carries $MUTCOUNT copy/copies (expected 0 -- deletion had no effect)" >&2
        exit 1
    fi
    echo "GREEN: EXTRA_DIST-deletion mutant correctly dropped $MUT_MEMBER (0 copies) -- membership is sensitive to its registration"
done

dist_build "$WORK/source" "$WORK/build" "$WORK/log" || { echo "RED: dist build failed"; tail -30 "$WORK/log" >&2; exit 1; }
TARBALL=$(ls "$WORK"/build/icecc-*.tar.gz 2>/dev/null | head -1)
[ -n "$TARBALL" ] || { echo "RED: no dist tarball"; exit 1; }
echo "dist tarball: $(basename "$TARBALL") sha256=$(sha256sum "$TARBALL" | cut -d' ' -f1)"

# A hash-bound archive is meaningful only if two isolated productions of
# the exact commit produce the same bytes.  Re-export, regenerate and
# compare before trusting the displayed digest.
mkdir -p "$WORK/repro-source" "$WORK/repro-build"
git -C "$REPO" archive "$COMMIT" | tar -x -C "$WORK/repro-source" || {
    echo "RED: reproducibility archive export failed"; exit 1;
}
dist_build "$WORK/repro-source" "$WORK/repro-build" "$WORK/repro-log" || {
    echo "RED: reproducibility dist build failed"; tail -30 "$WORK/repro-log" >&2; exit 1;
}
REPRO_TARBALL=$(ls "$WORK"/repro-build/icecc-*.tar.gz 2>/dev/null | head -1)
[ -n "$REPRO_TARBALL" ] || { echo "RED: reproducibility run produced no dist tarball"; exit 1; }
if ! cmp -s "$TARBALL" "$REPRO_TARBALL"; then
    echo "RED: two isolated make-dist productions of $COMMIT differ: $(sha256sum "$TARBALL" "$REPRO_TARBALL" | tr '\n' ' ')" >&2
    exit 1
fi
echo "REPRODUCIBLE-DIST-SHA256=$(sha256sum "$TARBALL" | cut -d' ' -f1)"

FAIL=0
for member in $S1B_MEMBERS; do
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
    echo "IMAGE-IDENTITY GATE: skipped (--skip-sentinel)"
    echo "PER-ARTIFACT MATRIX: skipped (--skip-sentinel)"
    exit 0
fi

# Extract the FULL tarball once more (this time not filtered to one
# member) so the producer script sits alongside the exact source tree it
# builds -- both members of the SAME dist tarball ("the EXACT EXTRACTED
# SUCCESSOR ARCHIVE"), not the ambient worktree copy. Gates 2-4 run
# EXCLUSIVELY out of this extraction from here on.
rm -rf "$WORK/full-extract"; mkdir -p "$WORK/full-extract"
tar -xzf "$TARBALL" -C "$WORK/full-extract" || { echo "RED: full extract failed"; exit 1; }
if find "$WORK/full-extract" -name .git -print -quit | grep -q .; then
    echo "RED: full dist-tarball extraction at $WORK/full-extract contains a .git entry -- expected none"; exit 1
fi
echo "ok - full dist-tarball extraction contains no .git"
SRCTREE=$(find "$WORK/full-extract" -maxdepth 1 -mindepth 1 -type d | head -1)
PRODUCER="$SRCTREE/distro_installed_identity.sh"
[ -x "$PRODUCER" ] || { echo "RED: extracted distro_installed_identity.sh missing or not executable at $PRODUCER"; exit 1; }

# Re-run the byte-compare and sh -n against the copies IN $SRCTREE itself
# -- the ones every gate below actually executes -- not just the
# standalone per-member extraction above (a different tar -x invocation,
# same tarball, but not proof this specific copy matches).
for member in $S1B_MEMBERS; do
    INTREE=$(find "$SRCTREE" -maxdepth 1 -name "$member")
    [ -n "$INTREE" ] || { echo "RED: $member absent from full extraction at $SRCTREE"; exit 1; }
    if ! git -C "$REPO" show "$COMMIT:$member" | cmp -s - "$INTREE"; then
        echo "RED: $SRCTREE/$member (the copy gates 2-4 actually run) differs from the exact Git blob"; exit 1
    fi
    case "$member" in
        *.sh)
            if ! sh -n "$INTREE" 2>"$WORK/synchk-intree-$member.err"; then
                echo "RED: $SRCTREE/$member fails sh -n: $(cat "$WORK/synchk-intree-$member.err")"; exit 1
            fi
            ;;
    esac
done
echo "ok - all $S1B_MEMBERS in the full extraction ($SRCTREE, what gates 2-4 actually execute) are byte-identical to $COMMIT, both .sh members sh -n clean"

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
    echo "-- mutating the dist-packaged producer's OWN copy ($d): neuter ONLY the rm inside empty_root(), keep both call sites + the emptiness assertion + the full pipeline live --"
    python3 - "$PRODUCER" <<'PY'
import sys
path = sys.argv[1]
lines = open(path).readlines()
target_substr = "-exec rm -rf -- {} +"
matches = [i for i, l in enumerate(lines) if target_substr in l]
if len(matches) != 1:
    print(f"FAIL: expected exactly 1 line containing {target_substr!r}, found {len(matches)}", file=sys.stderr)
    sys.exit(1)
i = matches[0]
indent = lines[i][:len(lines[i]) - len(lines[i].lstrip())]
lines[i] = indent + "true # MUTATED-OUT-FOR-TEST (only the rm neutered here; empty_root's call sites, its own emptiness assertion, and the complete configure/build/install/probe pipeline all stay genuinely live and unmodified)\n"
open(path, "w").writelines(lines)
print("mutation applied: neutered ONLY the rm inside empty_root(), line", i + 1)
PY
    if [ $? -ne 0 ]; then
        echo "RED ($d): could not apply the clean-step-neutering mutation" >&2
        cp "$SNAPSHOT" "$PRODUCER"
        return 1
    fi

    echo
    echo "-- sentinel-control (mutated: only empty_root's rm neutered, $d): must go RED at the POST-CLEAN-EMPTY assertion, not branch around it --"
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
        echo "RED ($d): mutated (rm-neutered) --sentinel-control exited 0 (expected non-zero/RED) -- the mutation had no effect" >&2
        return 1
    fi
    # The precise, checkable proof BigOracle asked for: the run must have
    # actually REACHED and TRIPPED the post-clean-empty assertion (recorded
    # NO, not merely absent because some earlier, unrelated thing failed),
    # via the REAL call sites and the REAL assertion code, not a branch
    # around them.
    if ! grep -qE '^post_clean_empty[[:space:]]+NO' "$RED_WORK/facts-normal-sentinel.txt" 2>/dev/null; then
        echo "RED ($d): mutated run did not record post_clean_empty=NO -- did not demonstrably reach the assertion it is supposed to trip" >&2
        return 1
    fi
    if grep -qE '^sentinel_survivors[[:space:]]+none' "$RED_WORK/facts-normal-sentinel.txt" 2>/dev/null; then
        echo "RED ($d): mutated run still recorded sentinel_survivors=none -- the mutation had no effect" >&2
        return 1
    fi
    echo "GREEN ($d): REDDENED as required -- with ONLY empty_root's rm neutered (call sites + assertion + full pipeline otherwise live), the POST-CLEAN-EMPTY assertion itself caught the surviving sentinels (post_clean_empty=NO) before configure ever ran (exit $RED_RC)"
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
echo "SENTINEL GATE: GREEN on all of: $SENTINEL_DISTROS (both directions each: unmutated clean/rebuild removes every sentinel and asserts it; rm-neutered mutant is caught at the assertion itself, call sites and pipeline otherwise untouched)"

# IMAGE-IDENTITY MUTANT: the normal-mode docker run is switched from
# $IMAGE_REF to a locally-built decoy image, while every line that
# computes/prints image_digest/image_ref/image_id (registry-digest
# channel) or image_id_pin_check (local-id channel) is left untouched --
# proving those pre-run facts are not, by themselves, what keeps this
# gate honest; the producer's own post-run container_run_image_id check
# (comparing what the container actually ran from against the $IMAGE_ID
# captured before the run) has to be the thing that catches it. Run
# against BOTH pin channels -- registry-digest (MATRIX_DISTRO) and
# local-id (ubuntu22, the pinned farm-node image, which has no registry
# anywhere so it pins by image ID instead; see the producer's PIN_CHANNEL
# branch) -- so both are proven deletion-sensitive, not just one.
image_identity_mutant_one_distro() {
    d=$1
    echo
    echo "== IMAGE-IDENTITY MUTANT ($d): docker run switched to a decoy image, facts-computation code untouched =="
    case "$d" in
        ubuntu22) DECOY_BASE=icecream/farm-node:ubuntu22-gcc11-boost174 ;;
        ubuntu24) DECOY_BASE=ubuntu:24.04 ;;
        fedora40) DECOY_BASE=fedora:40 ;;
        *) echo "RED: unknown distro $d for decoy base" >&2; return 1 ;;
    esac
    DECOY_CTX="$WORK/image-identity-decoy-ctx-$d"
    rm -rf "$DECOY_CTX"; mkdir -p "$DECOY_CTX"
    # FROM the real distro's own image + RUN true: guaranteed a genuinely
    # different image ID (not aliasing the real image's ID) while
    # remaining a valid, --pull=never-runnable image with no extra
    # network fetch.
    printf 'FROM %s\nRUN true\n' "$DECOY_BASE" > "$DECOY_CTX/Dockerfile"
    docker build -q -t "s1b-image-identity-decoy-$d:local" "$DECOY_CTX" >"$WORK/image-identity-decoy-build-$d.log" 2>&1 \
      || { echo "RED ($d): could not build the decoy image for the image-identity mutant" >&2; cat "$WORK/image-identity-decoy-build-$d.log" >&2; return 1; }

    python3 - "$PRODUCER" "s1b-image-identity-decoy-$d:local" <<'PY'
import sys
path, decoy = sys.argv[1], sys.argv[2]
lines = open(path).readlines()
matches = [i for i, l in enumerate(lines) if 'docker run --name "$CONTAINER_NAME"' in l]
if len(matches) != 1:
    print(f"FAIL: expected exactly 1 'docker run --name \"$CONTAINER_NAME\"' line, found {len(matches)}", file=sys.stderr)
    sys.exit(1)
i = matches[0]
if '"$IMAGE_REF"' not in lines[i]:
    print("FAIL: expected that line to reference \"$IMAGE_REF\"", file=sys.stderr)
    sys.exit(1)
lines[i] = lines[i].replace('"$IMAGE_REF"', f'"{decoy}"', 1)
open(path, "w").writelines(lines)
print("mutation applied: normal-mode docker run now launches the decoy image instead of $IMAGE_REF, line", i + 1)
PY
    if [ $? -ne 0 ]; then
        echo "RED ($d): could not apply the image-identity decoy mutation" >&2
        cp "$SNAPSHOT" "$PRODUCER"
        return 1
    fi

    TAGMUT_WORK="$WORK/image-identity-mutant-$d"
    rm -rf "$TAGMUT_WORK"
    "$PRODUCER" "$SRCTREE" "$TAGMUT_WORK" "$d" >"$WORK/image-identity-mutant-$d.log" 2>&1
    TAGMUT_RC=$?
    tail -25 "$WORK/image-identity-mutant-$d.log"
    cp "$SNAPSHOT" "$PRODUCER"
    if cmp -s "$PRODUCER" "$SNAPSHOT"; then
        echo "ok ($d) - producer restored byte-exact after the image-identity mutation"
    else
        echo "RED ($d): producer restoration is NOT byte-exact after the image-identity mutation -- manual cleanup needed at $PRODUCER" >&2
        return 1
    fi
    if [ "$TAGMUT_RC" = "0" ]; then
        echo "RED ($d): image-identity mutant (decoy image at the actual docker run) exited 0 (expected non-zero/RED)" >&2
        return 1
    fi
    if ! grep -qE '^FAIL: .*container_run_image_id' "$WORK/image-identity-mutant-$d.log"; then
        echo "RED ($d): image-identity mutant failed, but not by naming container_run_image_id -- got:" >&2
        grep -E '^FAIL: ' "$WORK/image-identity-mutant-$d.log" | tail -5 >&2
        return 1
    fi
    # The pre-run facts must still look entirely normal for THIS distro's
    # channel -- registry-digest prints image_digest, local-id prints
    # image_id_pin_check PASS-shaped output (require_exact doesn't emit a
    # PASS fact, but it must not have failed there either, or this would
    # be failing at the wrong gate for the wrong reason).
    case "$d" in
        ubuntu22)
            if ! grep -qE '^image_id_pin_expected[[:space:]]' "$TAGMUT_WORK/facts-normal.txt" 2>/dev/null; then
                echo "RED ($d): image-identity mutant did not even reach fact computation -- not a faithful test of 'facts still look right but the run used something else'" >&2
                return 1
            fi
            ;;
        *)
            if ! grep -qE '^image_digest[[:space:]]' "$TAGMUT_WORK/facts-normal.txt" 2>/dev/null; then
                echo "RED ($d): image-identity mutant did not even reach fact computation -- not a faithful test of 'facts still look right but the run used something else'" >&2
                return 1
            fi
            ;;
    esac
    echo "GREEN ($d): image-identity mutant correctly REDDENED at container_run_image_id, with the pre-run identity facts computed and printed exactly as a normal run would (fact-computation code was never touched) -- the pre-run facts alone would have looked entirely correct; only the post-run check of what the container actually ran from caught the substitution"
    return 0
}

IMAGE_IDENTITY_FAIL=0
image_identity_mutant_one_distro "$MATRIX_DISTRO" || IMAGE_IDENTITY_FAIL=1
if [ "$MATRIX_DISTRO" != ubuntu22 ]; then
    image_identity_mutant_one_distro ubuntu22 || IMAGE_IDENTITY_FAIL=1
fi
[ "$IMAGE_IDENTITY_FAIL" = "0" ] || { echo; echo "IMAGE-IDENTITY GATE: RED"; exit 1; }
echo
echo "IMAGE-IDENTITY GATE: GREEN on both pin channels (registry-digest via $MATRIX_DISTRO, local-id via ubuntu22)"

# EXPECTED-DIGEST AUTHORITY MUTANT: a different valid registry image has
# a perfectly nonempty RepoDigest and a self-consistent image ID.  Dynamic
# "accept the tag's first digest" code would therefore pass it.  Change
# only the convenience tag while retaining the committed expected ref/ID;
# the producer must fail before a build at image_digest_authority.
REGISTRY_AUTH_DISTRO=$MATRIX_DISTRO
[ "$REGISTRY_AUTH_DISTRO" = ubuntu22 ] && REGISTRY_AUTH_DISTRO=ubuntu24
case "$REGISTRY_AUTH_DISTRO" in
    ubuntu24) REGISTRY_AUTH_FROM='IMAGE=ubuntu:24.04'; REGISTRY_AUTH_TO='IMAGE=fedora:40' ;;
    fedora40) REGISTRY_AUTH_FROM='IMAGE=fedora:40'; REGISTRY_AUTH_TO='IMAGE=ubuntu:24.04' ;;
    *) echo "RED: no registry authority mutant for $REGISTRY_AUTH_DISTRO" >&2; exit 1 ;;
esac
echo
echo "== EXPECTED-DIGEST AUTHORITY MUTANT ($REGISTRY_AUTH_DISTRO): valid alternate registry tag must RED =="
python3 - "$PRODUCER" "$REGISTRY_AUTH_FROM" "$REGISTRY_AUTH_TO" <<'PY'
import sys
path, old, new = sys.argv[1:]
text = open(path).read()
if text.count(old) != 1:
    raise SystemExit(f"expected exactly one {old!r}, found {text.count(old)}")
open(path, "w").write(text.replace(old, new, 1))
PY
REGISTRY_AUTH_WORK="$WORK/registry-authority-mutant-$REGISTRY_AUTH_DISTRO"
rm -rf "$REGISTRY_AUTH_WORK"
REGISTRY_AUTH_OUT=$("$PRODUCER" "$SRCTREE" "$REGISTRY_AUTH_WORK" "$REGISTRY_AUTH_DISTRO" 2>&1)
REGISTRY_AUTH_RC=$?
cp "$SNAPSHOT" "$PRODUCER"
if ! cmp -s "$PRODUCER" "$SNAPSHOT"; then
    echo "RED: producer restoration is not byte-exact after expected-digest mutant" >&2
    exit 1
fi
if [ "$REGISTRY_AUTH_RC" = 0 ]; then
    echo "RED: alternate valid registry tag was accepted as authority" >&2
    exit 1
fi
REGISTRY_AUTH_FAIL=$(printf '%s\n' "$REGISTRY_AUTH_OUT" | grep -E '^FAIL: ' | head -1)
case "$REGISTRY_AUTH_FAIL" in
    *image_digest_authority*)
        echo "GREEN: alternate valid registry tag rejected by committed authority -- $REGISTRY_AUTH_FAIL"
        ;;
    *)
        echo "RED: alternate valid registry tag failed at the wrong check: $REGISTRY_AUTH_FAIL" >&2
        exit 1
        ;;
esac

# LOCAL-ID CHANNEL DECOY: mirrors the registry-digest channel's
# corrupt-control=image-digest row (which substitutes a locally-built
# decoy image to force empty RepoDigests -> UNRESOLVED), but against
# ubuntu22 specifically -- must fail by naming image_id_pin_check (the
# decoy's .Id genuinely differs from PINNED_IMAGE_ID), not image_digest
# (which does not gate anything on this channel). corrupt-control's
# image-digest handling exits before ever touching BUILD/DESTDIR, so this
# needs no pre-existing green baseline -- a fresh empty WORK is enough.
echo
echo "== LOCAL-ID CHANNEL decoy (ubuntu22): corrupt-control=image-digest must fail naming image_id_pin_check =="
U22_DIGEST_WORK="$WORK/ubuntu22-image-digest-corrupt"
rm -rf "$U22_DIGEST_WORK"; mkdir -p "$U22_DIGEST_WORK"
U22_OUT=$("$PRODUCER" "$SRCTREE" "$U22_DIGEST_WORK" ubuntu22 "--corrupt-control=image-digest" 2>&1)
U22_RC=$?
if [ "$U22_RC" = "0" ]; then
    echo "RED: ubuntu22 corrupt-control=image-digest exited 0 (expected non-zero/RED)" >&2
    echo "$U22_OUT" | tail -15
    exit 1
fi
U22_FAILLINE=$(echo "$U22_OUT" | grep -E '^FAIL: ' | head -1)
case "$U22_FAILLINE" in
    *image_id_pin_check*)
        echo "GREEN: ubuntu22 corrupt-control=image-digest correctly failed naming image_id_pin_check (local-id channel is deletion-sensitive too) -- $U22_FAILLINE"
        ;;
    *)
        echo "RED: ubuntu22 corrupt-control=image-digest failed, but not naming image_id_pin_check -- got: $U22_FAILLINE" >&2
        exit 1
        ;;
esac

# PER-ARTIFACT DELETION MATRIX. Reuses the matrix distro's own GREEN
# sentinel run build/destdir as the clean baseline -- that run already
# proved it ends in a state indistinguishable from a plain normal run's
# output (sentinel_survivors=none), and --corrupt-control never rebuilds,
# so copying it is safe and avoids a redundant full rebuild per artifact.
GREEN_WORK="$WORK/sentinel-green-$MATRIX_DISTRO"
[ -d "$GREEN_WORK/build" ] || { echo "RED: no GREEN sentinel run build/destdir for matrix distro $MATRIX_DISTRO (was it in --sentinel-distros?)" >&2; exit 1; }

# Baseline hash of the read-only-mounted source tree, taken once before
# any corrupt-control row runs, so every row below can prove it left
# $SRCTREE untouched -- corruption must land only in the copied
# build/destdir, never in source (which is bind-mounted :ro, but this
# proves it rather than assuming the mount flag alone is sufficient).
SRCTREE_BASELINE_HASH=$(find "$SRCTREE" -type f -exec sha256sum {} + | sort | sha256sum | awk '{print $1}')
echo
echo "SRCTREE baseline hash (must be unchanged after every matrix row): $SRCTREE_BASELINE_HASH"

echo
echo "== per-artifact deletion/corruption matrix (BigOracle), distro=$MATRIX_DISTRO =="
MATRIX_FAIL=0
matrix_label() {
    case "$1" in
        icecc)               echo installed_icecc_version_output ;;
        icecc-create-env)     echo installed_icecc_create_env ;;
        iceccd|iceccd-competing)
                              echo installed_iceccd_identity_total_count ;;
        icecc-scheduler|icecc-scheduler-competing)
                              echo installed_scheduler_identity_total_count ;;
        libicecc.a)           echo installed_libicecc_a ;;
        icecc.pc)             echo installed_icecc_pc_version_line ;;
        icecc.pc-competing)   echo installed_icecc_pc_version_total_count ;;
        image-digest)
            if [ "$MATRIX_DISTRO" = ubuntu22 ]; then
                echo image_id_pin_check
            else
                echo image_digest
            fi
            ;;
        package-inventory)    echo package_inventory ;;
        build-log)            echo configure_log ;;
    esac
}
for artifact in icecc icecc-create-env iceccd iceccd-competing icecc-scheduler icecc-scheduler-competing libicecc.a icecc.pc icecc.pc-competing image-digest package-inventory build-log; do
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
            continue
            ;;
    esac
    SRCTREE_ROW_HASH=$(find "$SRCTREE" -type f -exec sha256sum {} + | sort | sha256sum | awk '{print $1}')
    if [ "$SRCTREE_ROW_HASH" != "$SRCTREE_BASELINE_HASH" ]; then
        echo "RED: SRCTREE hash changed after --corrupt-control=$artifact (baseline $SRCTREE_BASELINE_HASH, now $SRCTREE_ROW_HASH) -- the read-only source tree must never be modified" >&2
        MATRIX_FAIL=1
        continue
    fi
    echo "ok - SRCTREE unchanged after --corrupt-control=$artifact"
done
[ "$MATRIX_FAIL" = "0" ] || { echo "PER-ARTIFACT MATRIX: RED"; exit 1; }
echo
echo "PER-ARTIFACT MATRIX: GREEN (all 12 absence/wrong/competing-identity controls, corrupted one at a time, each failed naming itself, SRCTREE byte-identical to its pre-matrix baseline after every row)"
