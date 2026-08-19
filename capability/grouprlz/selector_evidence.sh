# selector_evidence.sh — per-ACCEPTED-cell evidence retention, sourced by the launchers.
#
# The fail-closed work made the launchers retain FAILED cells.  That is the wrong half to keep
# if the goal is a record: a number that gets published comes from a cell that PASSED, and
# until now the passing cells' stderr and both physical streams were deleted along with the
# working directory.  A result you cannot re-examine is a result you are asking to be trusted.
#
# Per accepted cell this retains: stdout, stderr, the C->F and F->C stream artifacts, a
# manifest of every retained file with its size and SHA-256, and the exact command line.  Once
# per run it retains provenance: host, date, the source commit (and whether the tree was
# dirty), and each binary's path, size and SHA-256 -- so a number can be tied to the exact
# bytes that produced it rather than to a branch name that has since moved.
#
# Usage:
#   . "$HERE/selector_evidence.sh"
#   selector_evidence_init "$OUT" "$BIN" [more binaries...]
#   selector_evidence_cell "$OUT" "<slug>" "<command line>" file1 file2 ...

selector_evidence_init() {
  local out=$1; shift
  mkdir -p "$out"
  local p=$out/provenance.txt
  {
    echo "date       $(date -Is)"
    echo "host       $(hostname)"
    echo "user       $(id -un)"
    # The commit the HARNESS came from, plus whether it was modified -- a clean SHA that does
    # not match the working tree would be worse than no SHA at all.
    if [ -n "${SELECTOR_SOURCE_COMMIT:-}" ]; then
      echo "commit     $SELECTOR_SOURCE_COMMIT (declared via SELECTOR_SOURCE_COMMIT)"
    elif git -C "${HERE:-.}" rev-parse --git-dir >/dev/null 2>&1; then
      echo "commit     $(git -C "${HERE:-.}" rev-parse HEAD)"
      if [ -n "$(git -C "${HERE:-.}" status --porcelain -- "${HERE:-.}" 2>/dev/null)" ]; then
        echo "tree       DIRTY (the retained scripts differ from that commit)"
      else
        echo "tree       clean"
      fi
    else
      echo "commit     unknown (not a git checkout; set SELECTOR_SOURCE_COMMIT to declare one)"
    fi
    # Hash the harness itself.  These runs are normally staged into a scratch directory on a
    # compute host rather than launched from the checkout, so the commit is often unknown --
    # but the SCRIPT BYTES are always knowable, and they are what actually produced the result.
    for f in "${BASH_SOURCE[1]:-}" "${BASH_SOURCE[0]}"; do
      [ -n "$f" ] && [ -e "$f" ] || continue
      echo "harness    $(basename "$f")  sha256=$(sha256sum "$f" | cut -d' ' -f1)"
    done
    for b in "$@"; do
      if [ -e "$b" ]; then
        echo "binary     $b  bytes=$(stat -c %s "$b")  sha256=$(sha256sum "$b" | cut -d' ' -f1)"
      else
        echo "binary     $b  MISSING"
      fi
    done
  } >"$p"
}

selector_evidence_cell() {
  local out=$1 slug=$2 cmdline=$3; shift 3
  local dir=$out/cells/$slug
  rm -rf "$dir"; mkdir -p "$dir"
  printf '%s\n' "$cmdline" >"$dir/command.txt"
  local manifest=$dir/MANIFEST.tsv
  printf 'file\tbytes\tsha256\n' >"$manifest"
  local f base
  for f in "$@"; do
    [ -e "$f" ] || continue
    base=$(basename "$f")
    cp "$f" "$dir/$base"
    printf '%s\t%s\t%s\n' "$base" "$(stat -c %s "$f")" "$(sha256sum "$f" | cut -d' ' -f1)" >>"$manifest"
  done
}
