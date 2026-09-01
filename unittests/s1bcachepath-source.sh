#!/bin/sh
set -eu

top_src=${ICECC_TEST_TOP_SRCDIR:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
tmp_root=$(mktemp -d "${TMPDIR:-/tmp}/s1bcachepath-source.XXXXXX")
cleanup() {
    rm -rf -- "$tmp_root"
}
trap cleanup EXIT HUP INT TERM

# The producer expects a generated distribution tree.  A minimal generated
# marker is enough to reach its host-side cache validation without invoking
# Docker, so this fixture remains entirely local.
fixture="$tmp_root/source"
mkdir -p "$fixture"
touch "$fixture/Makefile.am" "$fixture/Makefile.in"

run_expect() {
    label=$1 expected_status=$2 expected_text=$3 cache=$4 work=$5
    mkdir -p "$work"
    set +e
    S1B_APT_CACHE="$cache" S1B_APT_CACHE_READY=true \
        "$top_src/distro_installed_identity.sh" "$fixture" "$work" ubuntu24 \
        >"$work.out" 2>&1
    status=$?
    set -e
    if [ "$status" -ne "$expected_status" ]; then
        echo "FAIL: $label returned $status, expected $expected_status" >&2
        cat "$work.out" >&2
        exit 1
    fi
    grep -qF "$expected_text" "$work.out" || {
        echo "FAIL: $label did not name '$expected_text'" >&2
        cat "$work.out" >&2
        exit 1
    }
}

safe_cache="$tmp_root/safe-cache"
safe_work="$tmp_root/safe-work"
run_expect safe-cache 1 'apt cache has 0 .deb payloads' "$safe_cache" "$safe_work"
test -d "$safe_cache/lists"
test -d "$safe_cache/archives"
test -f "$safe_cache/docker-clean"
test ! -L "$safe_cache/docker-clean"
echo 'ok - safe cache path is accepted and prepared before the payload gate'

alias_target="$tmp_root/alias-target"
mkdir -p "$alias_target"
alias_path="$tmp_root/cache-alias"
ln -s "$alias_target" "$alias_path"
run_expect symlink-alias 2 'symlink component' "$alias_path" "$tmp_root/alias-work"
test -z "$(find "$alias_target" -mindepth 1 -print -quit)"
echo 'ok - symlink cache alias is rejected before target writes'

component_cache="$tmp_root/component-cache"
mkdir -p "$component_cache"
component_target="$tmp_root/component-target"
mkdir -p "$component_target"
ln -s "$component_target" "$component_cache/lists"
run_expect symlink-component 2 'S1B_APT_CACHE entry is a symlink' "$component_cache" "$tmp_root/component-work"
test ! -e "$component_cache/archives"
echo 'ok - symlink lists component is rejected before sibling creation'

file_cache="$tmp_root/file-cache"
mkdir -p "$file_cache"
file_target="$tmp_root/control-target"
printf '%s\n' 'not the gate control file' > "$file_target"
ln -s "$file_target" "$file_cache/docker-clean"
run_expect symlink-control-file 2 'docker-clean must be a regular file' "$file_cache" "$tmp_root/file-work"
echo 'ok - symlink docker-clean file is rejected before writes'

echo 'PASS: S1b apt-cache path ownership fixtures hold'
