#!/bin/sh
# Validate the operator-provisioned scratch directory without creating it.
set -eu

if [ "${ICEFARM_TMPDIR-}" = "" ]; then
	echo "ICEFARM_TMPDIR is required; set it to an existing writable absolute scratch directory" >&2
	exit 2
fi
case "$ICEFARM_TMPDIR" in
	/*) ;;
	*) echo "ICEFARM_TMPDIR must be an absolute path: $ICEFARM_TMPDIR" >&2; exit 2 ;;
esac
if [ ! -d "$ICEFARM_TMPDIR" ]; then
	echo "ICEFARM_TMPDIR must name an existing directory: $ICEFARM_TMPDIR" >&2
	exit 2
fi
physical_dir=$(CDPATH= cd -- "$ICEFARM_TMPDIR" && pwd -P) || {
	echo "ICEFARM_TMPDIR cannot be resolved: $ICEFARM_TMPDIR" >&2
	exit 2
}
if [ "$physical_dir" = "/" ]; then
	echo "ICEFARM_TMPDIR must not resolve to the filesystem root" >&2
	exit 2
fi

probe_dir=$(umask 077; mktemp -d "$ICEFARM_TMPDIR/.icefarm-write-probe.XXXXXX" 2>/dev/null) || {
	echo "ICEFARM_TMPDIR is not writable: $ICEFARM_TMPDIR" >&2
	exit 2
}
cleanup() {
	rmdir -- "$probe_dir"
}
trap cleanup EXIT
trap 'exit 2' HUP INT TERM
