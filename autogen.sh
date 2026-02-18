#!/bin/sh
TESTLIBTOOLIZE="glibtoolize libtoolize"

LIBTOOLIZEFOUND="0"

srcdir=$(dirname $0)
test -z "$srcdir" && srcdir=.

cd $srcdir

autoconf --version > /dev/null 2> /dev/null || {
    echo "error: autoconf not found"
    exit 1
}

AUTOCONF_VER="$(autoconf --version | head -n1 | awk '{print $NF}')"
AUTOCONF_MAJOR="$(echo "$AUTOCONF_VER" | awk -F. '{print $1}')"
AUTOCONF_MINOR="$(echo "$AUTOCONF_VER" | awk -F. '{print $2}')"
REQUIRED_AUTOCONF_MAJOR="2"
REQUIRED_AUTOCONF_MINOR="71"

if [ "${AUTOCONF_MAJOR:-0}" -lt "$REQUIRED_AUTOCONF_MAJOR" ] || {
   [ "${AUTOCONF_MAJOR:-0}" -eq "$REQUIRED_AUTOCONF_MAJOR" ] && [ "${AUTOCONF_MINOR:-0}" -lt "$REQUIRED_AUTOCONF_MINOR" ];
}; then
    if [ -x "./configure" ]; then
        echo "warning: autoconf >= ${REQUIRED_AUTOCONF_MAJOR}.${REQUIRED_AUTOCONF_MINOR} required (${AUTOCONF_VER} found); using existing ./configure" >&2
        exit 0
    fi
    echo "error: autoconf >= ${REQUIRED_AUTOCONF_MAJOR}.${REQUIRED_AUTOCONF_MINOR} required (${AUTOCONF_VER} found) and ./configure is missing" >&2
    exit 1
fi

aclocal --version > /dev/null 2> /dev/null || {
    echo "error: aclocal not found"
    exit 1
}
automake --version > /dev/null 2> /dev/null || {
    echo "error: automake not found"
    exit 1
}

for i in $TESTLIBTOOLIZE; do
	if which $i > /dev/null 2>&1; then
		LIBTOOLIZE=$i
		LIBTOOLIZEFOUND="1"
		break
	fi
done

if [ "$LIBTOOLIZEFOUND" = "0" ]; then
	echo "$0: need libtoolize tool to build icecream" >&2
	exit 1
fi

if automake --version | grep -F 'automake (GNU automake) 1.5' > /dev/null; then # grep -q is non-portable
    echo "warning: you appear to be using automake 1.5"
    echo "         this version has a bug - GNUmakefile.am dependencies are not generated"
fi

rm -rf autom4te*.cache

$LIBTOOLIZE --force --copy || {
    echo "error: libtoolize failed"
    exit 1
}
aclocal -I m4 $ACLOCAL_FLAGS || {
    echo "error: aclocal -I m4 $ACLOCAL_FLAGS failed"
    exit 1
}
autoheader || {
    echo "error: autoheader failed"
    exit 1
}
automake -a -c --foreign || {
    echo "warning: automake failed"
}
autoconf || {
    echo "error: autoconf failed"
    exit 1
}
