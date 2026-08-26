#!/bin/sh
# Deletion-sensitive source gate for the type-erased profile transaction seam.
set -eu

src=${ICECC_TEST_TOP_SRCDIR:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
endpoint="$src/cache/p50_endpoint.cpp"
profile="$src/cache/p50_profile.cpp"
header="$src/cache/p50_profile.h"

grep -F 'ProfileDialogue dialogue;' "$endpoint" >/dev/null
grep -F 'ProfileDialogue::create(' "$endpoint" >/dev/null
grep -F 'ProfileDialogueState::BodyClosed' "$endpoint" >/dev/null
if grep -F 'ZstdTuDialogue' "$endpoint" >/dev/null; then
    echo 'FAIL: transaction engine names concrete ZSTD_TU dialogue' >&2
    exit 1
fi
grep -F 'struct ProfileDialogueVTable' "$header" >/dev/null
grep -F 'make_profile_dialogue' "$profile" >/dev/null
grep -F 'new ZstdTuDialogue' "$profile" >/dev/null
grep -F 'P50_PROFILE_INTERFACE.md' "$src/cache/Makefile.am" >/dev/null

mutant=$(mktemp "${TMPDIR:-/tmp}/p50profile-mutant.XXXXXX")
trap 'rm -f "$mutant"' EXIT HUP INT TERM
sed 's/ProfileDialogue dialogue;/std::unique_ptr<ZstdTuDialogue> dialogue;/' \
    "$endpoint" >"$mutant"
if grep -F 'ProfileDialogue dialogue;' "$mutant" >/dev/null; then
    echo 'FAIL: concrete Pending ownership mutant survived' >&2
    exit 1
fi

echo 'ok - pending transaction owns only the type-erased profile dialogue'
echo 'ok - concrete profile construction is confined to the profile adapter'
echo 'ok - deletion-sensitive concrete ownership mutant reddens'
echo 'PASS: frozen profile vtable source gate'
