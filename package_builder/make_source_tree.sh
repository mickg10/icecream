#!/usr/bin/env bash
# Clean-export source staging shared by every package builder (PKG-1/PKG-2).
#
#   make_source_tree.sh <git-checkout> <dest-dir> [--bootstrap]
#
# Populates <dest-dir> with `git archive HEAD` of <git-checkout> -- exactly
# the committed revision: no ignored files, no host-built binaries, no
# generated Autotools output, no local workspace state.  The previous
# rsync-the-workspace staging made package builds depend on whatever
# untracked state the developer tree happened to contain (stale configure,
# built iceccd/icecc/icecc-scheduler executables, editor droppings), so a
# clean clone and a dirty tree produced different Source0 contents.
#
# With --bootstrap, runs `autoreconf -fi` inside the export afterwards, so
# the tree additionally carries generated configure/Makefile.in for build
# roots whose own Autotools are too old to bootstrap (Fedora 28).  The
# caller's environment must have modern autoconf/automake/libtool.
set -euo pipefail

SRC_GIT=${1:?git checkout to export}
DEST=${2:?destination directory}
BOOTSTRAP=${3:-}

command -v git >/dev/null || { echo "ERROR: git is required for a clean export" >&2; exit 2; }

# Per-command config only: appending to the global git configuration on
# every run leaks state between builds.  Bind-mounted checkouts are
# typically owned by another uid, hence safe.directory.
GIT=(git -c "safe.directory=$SRC_GIT" -C "$SRC_GIT")

# Resolve ONCE, then archive that exact object id: archiving HEAD after
# recording the revision races with concurrent commits, producing content
# and .source-revision that disagree.
REV=$("${GIT[@]}" rev-parse HEAD)
if ! "${GIT[@]}" diff --quiet || ! "${GIT[@]}" diff --cached --quiet; then
    echo "WARNING: checkout has uncommitted changes; the package is built from $REV WITHOUT them" >&2
fi

# A pre-existing destination could mix stale files into the export.
if [ -e "$DEST" ] && [ -n "$(ls -A "$DEST" 2>/dev/null)" ]; then
    echo "ERROR: destination $DEST exists and is not empty" >&2
    exit 2
fi
mkdir -p "$DEST"
"${GIT[@]}" archive --format=tar "$REV" | tar -x -C "$DEST"
echo "$REV" > "$DEST/.source-revision"

if [ "$BOOTSTRAP" = "--bootstrap" ]; then
    command -v autoreconf >/dev/null \
        || { echo "ERROR: --bootstrap requires autoconf/automake/libtool" >&2; exit 2; }
    ( cd "$DEST" && autoreconf -fi >/dev/null )
fi

echo "staged clean export of $REV at $DEST"
