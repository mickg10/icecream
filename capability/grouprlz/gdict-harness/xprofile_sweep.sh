#!/bin/bash
# Cross-docker-profile transfer sweep: xprofile_sweep.sh <project> [...]
set -uo pipefail
B=/home/ttuser/gdict; C=$B/cells
export BIN=codec50-bigcap-mt CPUS=${CPUS:-0-23}
D=debian-gcc; N=conan-gcc; L=linuxbrew; F=fedora-clang-libcxx

one() {  # one <tag> <target-profile-man> [prior-man]
  local tag="$1" tgt="$2" pri="${3:-}"
  [ -f "$B/runs/$tag.out" ] && grep -q "TOTAL=" "$B/runs/$tag.out" && { echo "skip $tag"; return; }
  if [ -n "$pri" ]; then python3 "$B/mkman2.py" "$tag.man" "$tgt" "$pri" >/dev/null
  else python3 "$B/mkman2.py" "$tag.man" "$tgt" >/dev/null; fi
  ( cd "$B" && ./run_alt.sh "$tag" "manifests/$tag.man" >/dev/null 2>&1 )
  rm -rf "$B/plans/$tag"
  echo "$tag $(grep -o TOTAL=[0-9]* "$B/runs/$tag.out" | head -1) $(grep -c byte-exact=OK "$B/runs/$tag.out")"
}

for p in "$@"; do
  m() { echo "$C/${p}__$1.man"; }
  [ -f "$(m $D)" ] || { echo "no cells for $p"; continue; }
  # target conan-gcc (gcc14/libstdc++14)
  one xp_${p}_conan_cold        "$(m $N)"
  one xp_${p}_conan_warmsame    "$(m $N)" "$(m $N)"
  one xp_${p}_conan_from_debian "$(m $N)" "$(m $D)"
  one xp_${p}_conan_from_brew   "$(m $N)" "$(m $L)"
  one xp_${p}_conan_from_fedora "$(m $N)" "$(m $F)"
  # target fedora-clang-libcxx (clang20/libc++)
  one xp_${p}_fedora_cold        "$(m $F)"
  one xp_${p}_fedora_warmsame    "$(m $F)" "$(m $F)"
  one xp_${p}_fedora_from_debian "$(m $F)" "$(m $D)"
  # target debian-gcc (gcc12/libstdc++12)
  one xp_${p}_debian_cold       "$(m $D)"
  one xp_${p}_debian_warmsame   "$(m $D)" "$(m $D)"
  one xp_${p}_debian_from_brew  "$(m $D)" "$(m $L)"
  one xp_${p}_debian_from_conan "$(m $D)" "$(m $N)"
done
