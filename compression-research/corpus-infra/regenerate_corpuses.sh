#!/bin/bash
# regenerate_corpuses.sh -- rebuild any of the 16 preprocessed-TU corpora from scratch.
#
#   ./regenerate_corpuses.sh                  # all 16
#   ./regenerate_corpuses.sh corpus7 corpus9  # just those
#   ./regenerate_corpuses.sh --force corpus7  # redo configure + preprocess even if present
#   ./regenerate_corpuses.sh --outroot DIR --srcroot DIR corpus7
#
# Pipeline per corpus:  clone -> configure -> preprocess (-E) -> manifest -> verify TU
# Every stage is skipped when its output already exists, so a re-run is cheap and a
# partial run resumes.  --force redoes configure and preprocess (never the clone).
#
# The per-corpus knobs live in recipes/<corpus>.sh; this file only knows how to drive
# the four generic modes (ccjson / multi / scons / savetemps).
set -uo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
PP="$HERE/preprocess_corpus.py"

SRCROOT_DEFAULT=$HERE/sources
OUTROOT_DEFAULT=$HERE/corpora
SRCROOT=$SRCROOT_DEFAULT
OUTROOT=$OUTROOT_DEFAULT
FORCE=0
JOBS=${JOBS:-8}
ALL=(corpus corpus2 corpus3 corpus4 corpus5 corpus6 corpus7 corpus8 corpus9 \
     corpus10 corpus11 corpus12 corpus13 corpus14 corpus15 corpus16)

WANT=()
while [ $# -gt 0 ]; do
  case "$1" in
    --force)   FORCE=1; shift ;;
    --srcroot) SRCROOT=$2; shift 2 ;;
    --outroot) OUTROOT=$2; shift 2 ;;
    --jobs)    JOBS=$2; shift 2 ;;
    -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
    -*)        echo "unknown option $1" >&2; exit 2 ;;
    *)         WANT+=("$1"); shift ;;
  esac
done
[ ${#WANT[@]} -eq 0 ] && WANT=("${ALL[@]}")

mkdir -p "$SRCROOT" "$OUTROOT"
log () { printf '[%s] %s\n' "$(date +%T)" "$*"; }
die () { echo "ERROR: $*" >&2; return 1; }

# ---------------------------------------------------------------- clone
# clone_one <checkout-name> <url> [ref] [extra-clone-args]
clone_one () {
  local name="$1" url="$2" ref="${3:-}" extra="${4:-}" dir="$SRCROOT/$1"
  if [ -d "$dir/.git" ]; then
    log "  clone: $name present @ $(git -C "$dir" rev-parse --short HEAD)"
    return 0
  fi
  if [ -n "$ref" ]; then
    # A pinned commit needs a fetch-by-sha, not --depth 1 of the default branch.
    log "  clone: $name @ $ref"
    git init -q "$dir" \
      && git -C "$dir" remote add origin "$url" \
      && git -C "$dir" fetch -q --depth 1 origin "$ref" \
      && git -C "$dir" checkout -q FETCH_HEAD || return 1
    [ -n "$extra" ] && git -C "$dir" submodule update -q --init --recursive --depth 1
  else
    log "  clone: $name (shallow, default branch)"
    # shellcheck disable=SC2086
    nice -19 git clone -q --depth 1 $extra "$url" "$dir" || return 1
  fi
  log "  clone: $name -> $(git -C "$dir" rev-parse --short HEAD)"
}

# ------------------------------------------------------------ configure
# cmake_configure <src> <build> <extra-cmake-args...>
cmake_configure () {
  local src="$1" bld="$2"; shift 2
  nice -19 cmake -S "$src" -B "$bld" -G Ninja \
    -DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-Release}" \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON "$@"
}

# ------------------------------------------------------- cc-json filter
# Drop vendored test frameworks and CMake-fetched dependencies so a corpus only ever
# contains its OWN project's TUs (see corpus14/corpus4 recipe comments).
filter_ccjson () {
  local src="$1" dst="$2" keep="${3:-}"
  python3 - "$src" "$dst" "$keep" <<'PYEOF'
import json, os, sys
src, dst, keep = sys.argv[1], sys.argv[2], sys.argv[3]
EXCLUDE = ("/third_party/googletest/", "/third_party/benchmark/",
           "/googletest/", "/google-benchmark/", "/_deps/", "/.git/")
kept = []
for e in json.load(open(src)):
    f = os.path.normpath(e.get("file", "")).replace(os.sep, "/")
    if any(x in f for x in EXCLUDE):
        continue
    if keep and keep not in f:
        continue
    kept.append(e)
json.dump(kept, open(dst, "w"))
print(f"  filter: {len(kept)} entries kept")
PYEOF
}

# ------------------------------------------------------------ manifest
write_manifest () {
  local outdir="$1" expected="$2" name="$3"
  find "$outdir" -name '*.ii' -size +0c | sort > "$outdir/manifest.txt"
  local n; n=$(wc -l < "$outdir/manifest.txt")
  if [ "$n" -eq "$expected" ]; then
    log "  VERIFY $name: TU=$n (expected $expected) OK"
  else
    log "  VERIFY $name: TU=$n (expected $expected) MISMATCH"
  fi
}

# -------------------------------------------------- preprocess (-E) run
preprocess () {
  local ccjson="$1" outdir="$2" logf="$3" limit="${4:-0}"
  local limit_arg=(); [ "$limit" != "0" ] && limit_arg=(--limit "$limit")
  nice -19 python3 "$PP" --cc-json "$ccjson" --outdir "$outdir" \
       --jobs "$JOBS" --log "$logf" "${limit_arg[@]}"
}

# =====================================================================
do_corpus () {
  local c="$1"
  local recipe="$HERE/recipes/$c.sh"
  [ -f "$recipe" ] || { die "no recipe for $c"; return 1; }

  # recipe vars are re-declared per corpus so nothing leaks between them
  local PROJECT CHECKOUT GIT_URL GIT_REF CLONE_ARGS SRC_SUBDIR MODE FILTER \
        PP_LIMIT EXPECTED_TU CMAKE_BUILD_TYPE BUILD_SECONDS PROTOBUF_KEEP_PREFIX
  local -a CMAKE_ARGS=() SCONS_ARGS=() ABSEIL_CMAKE_ARGS=() PROTOBUF_CMAKE_ARGS=()
  unset -f postconfigure
  CMAKE_BUILD_TYPE=Release; FILTER=1; PP_LIMIT=0; GIT_REF=; CLONE_ARGS=; SRC_SUBDIR=.
  # shellcheck disable=SC1090
  source "$recipe"

  local outdir="$OUTROOT/$c"
  log "=== $c ($PROJECT) mode=$MODE ==="

  if [ -s "$outdir/manifest.txt" ] && [ "$FORCE" -eq 0 ]; then
    log "  manifest present ($(wc -l < "$outdir/manifest.txt") TUs) -- skip (use --force to redo)"
    return 0
  fi

  case "$MODE" in
  # ---------------------------------------------------------------- ccjson
  ccjson)
    clone_one "$CHECKOUT" "$GIT_URL" "$GIT_REF" "$CLONE_ARGS" || return 1
    local src="$SRCROOT/$CHECKOUT/$SRC_SUBDIR" bld="$SRCROOT/$CHECKOUT/build"
    if [ ! -f "$bld/compile_commands.json" ] || [ "$FORCE" -eq 1 ]; then
      log "  configure"
      rm -rf "$bld"
      cmake_configure "$src" "$bld" "${CMAKE_ARGS[@]}" > "$HERE/logs/$c.configure.log" 2>&1 \
        || { tail -30 "$HERE/logs/$c.configure.log"; die "$c configure failed"; return 1; }
    else
      log "  configure: compile_commands.json present -- skip"
    fi
    [ -f "$bld/compile_commands.json" ] || { die "$c: no compile_commands.json"; return 1; }
    if declare -F postconfigure >/dev/null; then
      log "  postconfigure (generated sources)"
      postconfigure "$bld" > "$HERE/logs/$c.postconfigure.log" 2>&1 \
        || log "  postconfigure returned $? (see logs/$c.postconfigure.log)"
    fi
    local cc="$bld/compile_commands.json"
    if [ "$FILTER" = "1" ]; then
      filter_ccjson "$cc" "$bld/compile_commands.filtered.json" ""
      cc="$bld/compile_commands.filtered.json"
    fi
    log "  preprocess -> $outdir"
    rm -rf "$outdir"; mkdir -p "$outdir"
    preprocess "$cc" "$outdir" "$HERE/logs/$c.pp.log" "$PP_LIMIT"
    ;;

  # ----------------------------------------------------------------- multi
  # corpus4 only: two CMake projects preprocessed into one corpus.
  multi)
    local names=($CHECKOUT) urls=($GIT_URL) i
    for i in "${!names[@]}"; do
      clone_one "${names[$i]}" "${urls[$i]}" "" "$CLONE_ARGS" || return 1
    done
    rm -rf "$outdir"; mkdir -p "$outdir"
    # -- abseil
    local ab_args=("${ABSEIL_CMAKE_ARGS[@]//@SRCROOT@/$SRCROOT}")
    if [ ! -f "$SRCROOT/abseil-cpp/build/compile_commands.json" ] || [ "$FORCE" -eq 1 ]; then
      log "  configure abseil"
      rm -rf "$SRCROOT/abseil-cpp/build"
      cmake_configure "$SRCROOT/abseil-cpp" "$SRCROOT/abseil-cpp/build" "${ab_args[@]}" \
        > "$HERE/logs/$c.abseil.configure.log" 2>&1 \
        || { tail -30 "$HERE/logs/$c.abseil.configure.log"; die "abseil configure failed"; return 1; }
    else
      log "  configure abseil: compile_commands.json present -- skip"
    fi
    filter_ccjson "$SRCROOT/abseil-cpp/build/compile_commands.json" \
                  "$SRCROOT/abseil-cpp/build/cc_filtered.json" ""
    log "  preprocess abseil"
    preprocess "$SRCROOT/abseil-cpp/build/cc_filtered.json" "$outdir/abseil" \
               "$HERE/logs/$c.abseil.pp.log" 0
    # -- protobuf (src/ only)
    if [ ! -f "$SRCROOT/protobuf/build/compile_commands.json" ] || [ "$FORCE" -eq 1 ]; then
      log "  configure protobuf"
      rm -rf "$SRCROOT/protobuf/build"
      cmake_configure "$SRCROOT/protobuf" "$SRCROOT/protobuf/build" "${PROTOBUF_CMAKE_ARGS[@]}" \
        > "$HERE/logs/$c.protobuf.configure.log" 2>&1 \
        || { tail -30 "$HERE/logs/$c.protobuf.configure.log"; die "protobuf configure failed"; return 1; }
    else
      log "  configure protobuf: compile_commands.json present -- skip"
    fi
    filter_ccjson "$SRCROOT/protobuf/build/compile_commands.json" \
                  "$SRCROOT/protobuf/build/cc_pbsrc.json" "${PROTOBUF_KEEP_PREFIX//@SRCROOT@/$SRCROOT}"
    log "  preprocess protobuf"
    preprocess "$SRCROOT/protobuf/build/cc_pbsrc.json" "$outdir/protobuf" \
               "$HERE/logs/$c.protobuf.pp.log" 0
    ;;

  # ----------------------------------------------------------------- scons
  scons)
    clone_one "$CHECKOUT" "$GIT_URL" "$GIT_REF" "$CLONE_ARGS" || return 1
    local gd="$SRCROOT/$CHECKOUT"
    command -v scons >/dev/null || { die "scons not on PATH (pip install scons)"; return 1; }
    if [ ! -f "$gd/compile_commands.json" ] || [ "$FORCE" -eq 1 ]; then
      log "  scons full build (needed: .gen.h codegen)"
      ( cd "$gd" && nice -19 scons "${SCONS_ARGS[@]}" -j"$JOBS" ) \
        > "$HERE/logs/$c.build.log" 2>&1 \
        || { tail -30 "$HERE/logs/$c.build.log"; die "godot build failed"; return 1; }
      log "  scons compiledb"
      ( cd "$gd" && nice -19 scons "${SCONS_ARGS[@]}" compiledb ) \
        > "$HERE/logs/$c.compiledb.log" 2>&1 || { die "godot compiledb failed"; return 1; }
    else
      log "  build: compile_commands.json present -- skip"
    fi
    local cc="$gd/compile_commands.json"
    if [ "$FILTER" = "1" ]; then
      filter_ccjson "$cc" "$gd/compile_commands.filtered.json" ""
      cc="$gd/compile_commands.filtered.json"
    fi
    log "  preprocess -> $outdir"
    rm -rf "$outdir"; mkdir -p "$outdir"
    preprocess "$cc" "$outdir" "$HERE/logs/$c.pp.log" "$PP_LIMIT"
    ;;

  # ------------------------------------------------------------- savetemps
  # LLVM: -save-temps=obj drops a .ii beside every object during a real build.
  savetemps)
    clone_one "$CHECKOUT" "$GIT_URL" "$GIT_REF" "$CLONE_ARGS" || return 1
    mkdir -p "$outdir"
    local src="$SRCROOT/$CHECKOUT/$SRC_SUBDIR" bld="$outdir/build"
    log "  configure (-save-temps=obj)"
    cmake_configure "$src" "$bld" "${CMAKE_ARGS[@]}" > "$HERE/logs/$c.configure.log" 2>&1 \
      || { tail -30 "$HERE/logs/$c.configure.log"; die "llvm configure failed"; return 1; }
    log "  bounded build (${BUILD_SECONDS}s) to harvest .ii"
    timeout "$BUILD_SECONDS" nice -19 ninja -C "$bld" -j"$JOBS" \
      > "$HERE/logs/$c.build.log" 2>&1
    log "  ninja rc=$? (124 = hit the time bound, expected)"
    ;;
  *) die "$c: unknown MODE=$MODE"; return 1 ;;
  esac

  write_manifest "$outdir" "$EXPECTED_TU" "$c"
  echo "$PROJECT @ $(git -C "$SRCROOT/${CHECKOUT%% *}" rev-parse --short HEAD 2>/dev/null)" > "$outdir/NAME"
}

mkdir -p "$HERE/logs"
rc=0
for c in "${WANT[@]}"; do
  do_corpus "$c" || { rc=1; log "!! $c FAILED"; }
done
log "regenerate done (rc=$rc); corpora under $OUTROOT"
exit $rc
