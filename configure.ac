# quite some macros are taken from distcc

AC_PREREQ([2.71])

# ====================
# Version informations
# ====================
# Stable versions: x.y.z , where z < 50
# Development versions: x.y.90
# Pre-release versions: x.y.z, where z = 90 + X in rcX (1.1rc1 = 1.1.91)
m4_define([icecream_version_major],[1])
m4_define([icecream_version_minor],[4])
m4_define([icecream_version_micro],[92])
m4_ifnblank(icecream_version_micro,
        [m4_define([icecream_version],[icecream_version_major.icecream_version_minor.icecream_version_micro])],
        [m4_define([icecream_version],[icecream_version_major.icecream_version_minor])])

# =============
# Automake init
# =============
AC_INIT([icecc],[icecream_version])
AC_CONFIG_MACRO_DIR([m4])
AC_CONFIG_HEADERS([config.h])
AM_INIT_AUTOMAKE([1.11 foreign dist-xz])
AM_SILENT_RULES([yes])
AC_LANG([C++])

# ===========================
# Find required base packages
# ===========================
AC_PROG_CC
AC_PROG_CXX

AC_ARG_VAR([ICE_CXX_STANDARD_FLAG],
    [compiler flag selecting C++23 mode (default: -std=c++23)])
AS_IF([test -z "$ICE_CXX_STANDARD_FLAG"], [ICE_CXX_STANDARD_FLAG=-std=c++23])

AC_MSG_CHECKING([for the required C++23 language and library features ($ICE_CXX_STANDARD_FLAG)])
CXXFLAGS="$CXXFLAGS $ICE_CXX_STANDARD_FLAG"
AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[
        #include <compare>
        #include <cstddef>
        #include <optional>
        #include <span>
        #include <variant>
        #include <vector>

        #if __cplusplus < 202100L
        # error compiler is not in C++23 language mode
        #endif

        struct OrderedValue {
            unsigned long long value = 0;
            auto operator<=>(const OrderedValue&) const = default;
        };

        constexpr unsigned long long span_sum(std::span<const unsigned long long> values) {
            unsigned long long result = 0;
            for (unsigned long long value : values) result += value;
            return result;
        }
    ]], [[
        const std::vector<unsigned long long> values{1, 2, 3};
        const std::optional<OrderedValue> selected{OrderedValue{span_sum(values)}};
        const std::variant<unsigned long long, OrderedValue> tagged{*selected};
        return std::get<OrderedValue>(tagged).value == 6 ? 0 : 1;
    ]])],[
        AC_MSG_RESULT(yes)
    ],[ AC_MSG_ERROR([a compiler with the required C++23 mode, std::span, std::optional, std::variant, and defaulted comparisons is required])
])

if test "$GCC" = yes; then
    cast_align=-Wcast-align
    wshadow=
    AC_MSG_CHECKING([if GCC is actually Clang])
    AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[]], [[
            #ifdef __clang__
            return 0;
            #else
            fail
            #endif
        ]])],[
            AC_MSG_RESULT(yes)
            # The code has numerous cast alignment warnings that only clang warns about,
            # and it probably(?) doesn't matter in practice.
            cast_align=
            # Clang is not so overzealous like GCC and doesn't warn about cases which
            # are very unlikely to be problems (e.g. function argument shadowing
            # a function elsewhere), so it's not so annoying.
            wshadow=-Wshadow
        ],[ AC_MSG_RESULT(no) ])
    CFLAGS="-g -Wall -Wextra \
-Wshadow -Wpointer-arith $cast_align -Wwrite-strings \
-Waggregate-return -Wstrict-prototypes -Wmissing-prototypes \
-Wnested-externs -Wundef $CFLAGS"
    CXXFLAGS=" -g -Wall -Wextra -Wpointer-arith $cast_align $wshadow -Wwrite-strings -Wundef $CXXFLAGS"
    AC_MSG_NOTICE([Adding gcc options: $CFLAGS])
fi

AC_ARG_ENABLE(debug,
    AS_HELP_STRING([--enable-debug],
        [Enable debug mode (enable asserts, disable optimizations).]))
AC_MSG_CHECKING([whether to enable debug module])
if test "$enable_debug" = "yes"; then
    CFLAGS="$CFLAGS -g -O0"
    CXXFLAGS="$CXXFLAGS -g -O0"
    AC_MSG_RESULT(yes)
else
    CFLAGS="$CFLAGS -DNDEBUG"
    CXXFLAGS="$CXXFLAGS -DNDEBUG"
    AC_MSG_RESULT(no)
fi

AC_PROG_MAKE_SET
AC_PROG_INSTALL
PKG_PROG_PKG_CONFIG
AC_C_INLINE
AC_DISABLE_SHARED
AC_ENABLE_STATIC
LT_INIT

# The cache endpoint uses Boost.Asio's C++20 coroutine and asynchronous TCP APIs.
# Discovery is central so all later targets consume one dependency contract.
AC_ARG_WITH([boost],
    [AS_HELP_STRING([--with-boost=PREFIX],
        [use Boost installed under PREFIX (Boost >= 1.74 with Boost.Asio coroutine support is required)])],
    [boost_prefix="$withval"],
    [boost_prefix=""])
AS_IF([test "x$boost_prefix" = xno],
    [AC_MSG_ERROR([Boost is a mandatory dependency and cannot be disabled])])

BOOST_CPPFLAGS=
BOOST_LDFLAGS=
AS_IF([test -n "$boost_prefix" && test "x$boost_prefix" != xyes], [
    AS_IF([test -f "$boost_prefix/boost/version.hpp"], [
        BOOST_CPPFLAGS="-isystem $boost_prefix"
    ], [test -f "$boost_prefix/include/boost/version.hpp"], [
        BOOST_CPPFLAGS="-isystem $boost_prefix/include"
    ], [
        AC_MSG_ERROR([--with-boost=$boost_prefix does not contain boost/version.hpp])
    ])
    icecc_boost_multiarch=`$CXX -print-multiarch 2>/dev/null`
    AS_IF([test -n "$icecc_boost_multiarch" &&
           test -d "$boost_prefix/lib/$icecc_boost_multiarch"], [
        BOOST_LDFLAGS="-L$boost_prefix/lib/$icecc_boost_multiarch"
    ], [test -d "$boost_prefix/lib64"], [
        BOOST_LDFLAGS="-L$boost_prefix/lib64"
    ], [test -d "$boost_prefix/lib"], [
        BOOST_LDFLAGS="-L$boost_prefix/lib"
    ])
])

# Boost 1.74's Clang feature detection still looks for the Coroutines TS
# header even when the compiler and standard library provide C++20
# <coroutine>. The repository requires C++23 and the link probe below uses
# the exact standard-coroutine APIs, so select that supported Asio path
# centrally for every target.
BOOST_CPPFLAGS="$BOOST_CPPFLAGS -DBOOST_ASIO_HAS_CO_AWAIT=1 -DBOOST_ASIO_HAS_STD_COROUTINE=1"

# The Boost prefix belongs only to the Boost feature probes below.  In
# particular, do not make its library directory part of the global search
# path: later lzo, zstd, libarchive and platform probes must keep using their
# own dependency contracts.  Future Boost-using targets opt in through the
# substituted BOOST_* variables.
icecc_boost_save_CPPFLAGS="$CPPFLAGS"
icecc_boost_save_LDFLAGS="$LDFLAGS"
icecc_boost_save_LIBS="$LIBS"
CPPFLAGS="$BOOST_CPPFLAGS $icecc_boost_save_CPPFLAGS"
LDFLAGS="$BOOST_LDFLAGS $icecc_boost_save_LDFLAGS"

AC_MSG_CHECKING([for Boost version 1.74 or newer])
AC_COMPUTE_INT([icecc_boost_version], [BOOST_VERSION], [[
    #include <boost/version.hpp>
]], [AC_MSG_ERROR([Boost headers were not found; install Boost >= 1.74 or use --with-boost=PREFIX])])
AS_IF([test "$icecc_boost_version" -lt 107400], [
    AC_MSG_ERROR([Boost >= 1.74 is required; found BOOST_VERSION=$icecc_boost_version])
])
icecc_boost_major=`expr "$icecc_boost_version" / 100000`
icecc_boost_minor=`expr \( "$icecc_boost_version" / 100 \) % 1000`
icecc_boost_patch=`expr "$icecc_boost_version" % 100`
AC_MSG_RESULT([$icecc_boost_major.$icecc_boost_minor.$icecc_boost_patch])

AC_MSG_CHECKING([for required Boost.Asio coroutine and asynchronous TCP APIs])
BOOST_LIBS=
AC_LINK_IFELSE([AC_LANG_PROGRAM([[
        #include <array>
        #include <utility>
        #include <boost/asio/awaitable.hpp>
        #include <boost/asio/co_spawn.hpp>
        #include <boost/asio/detached.hpp>
        #include <boost/asio/io_context.hpp>
        #include <boost/asio/ip/tcp.hpp>
        #include <boost/asio/read.hpp>
        #include <boost/asio/use_awaitable.hpp>
        #include <boost/asio/write.hpp>

        namespace asio = boost::asio;
        using tcp = asio::ip::tcp;

        asio::awaitable<void> icecc_asio_probe(tcp::acceptor& acceptor,
                                               tcp::socket& socket) {
            std::array<unsigned char, 1> byte{};
            co_await acceptor.async_accept(socket, asio::use_awaitable);
            co_await socket.async_connect(tcp::endpoint{}, asio::use_awaitable);
            co_await asio::async_read(socket, asio::buffer(byte), asio::use_awaitable);
            co_await asio::async_write(socket, asio::buffer(byte), asio::use_awaitable);
        }
    ]], [[
        boost::asio::io_context context;
        boost::asio::ip::tcp::acceptor acceptor(context);
        boost::asio::ip::tcp::socket socket(context);
        boost::asio::co_spawn(context, icecc_asio_probe(acceptor, socket),
                              boost::asio::detached);
        context.poll();
        return 0;
    ]])], [
        AC_MSG_RESULT([yes (header-only Boost.System)])
    ], [
        LIBS="-lboost_system $icecc_boost_save_LIBS"
        AC_LINK_IFELSE([AC_LANG_PROGRAM([[
                #include <array>
                #include <utility>
                #include <boost/asio/awaitable.hpp>
                #include <boost/asio/co_spawn.hpp>
                #include <boost/asio/detached.hpp>
                #include <boost/asio/io_context.hpp>
                #include <boost/asio/ip/tcp.hpp>
                #include <boost/asio/read.hpp>
                #include <boost/asio/use_awaitable.hpp>
                #include <boost/asio/write.hpp>

                namespace asio = boost::asio;
                using tcp = asio::ip::tcp;

                asio::awaitable<void> icecc_asio_probe(tcp::acceptor& acceptor,
                                                       tcp::socket& socket) {
                    std::array<unsigned char, 1> byte{};
                    co_await acceptor.async_accept(socket, asio::use_awaitable);
                    co_await socket.async_connect(tcp::endpoint{}, asio::use_awaitable);
                    co_await asio::async_read(socket, asio::buffer(byte), asio::use_awaitable);
                    co_await asio::async_write(socket, asio::buffer(byte), asio::use_awaitable);
                }
            ]], [[
                boost::asio::io_context context;
                boost::asio::ip::tcp::acceptor acceptor(context);
                boost::asio::ip::tcp::socket socket(context);
                boost::asio::co_spawn(context, icecc_asio_probe(acceptor, socket),
                                      boost::asio::detached);
                context.poll();
                return 0;
            ]])], [
                BOOST_LIBS=-lboost_system
                AC_MSG_RESULT([yes (links Boost.System)])
            ], [
                AC_MSG_ERROR([Boost.Asio awaitable, co_spawn, use_awaitable, and asynchronous TCP read/write/accept/connect APIs are required])
            ])
    ])
CPPFLAGS="$icecc_boost_save_CPPFLAGS"
LDFLAGS="$icecc_boost_save_LDFLAGS"
LIBS="$icecc_boost_save_LIBS"

AC_DEFINE_UNQUOTED([ICECC_BOOST_VERSION], [$icecc_boost_version],
    [Numeric Boost version used to build Icecream])
AC_SUBST([BOOST_CPPFLAGS])
AC_SUBST([BOOST_LDFLAGS])
AC_SUBST([BOOST_LIBS])
AC_SUBST([ICE_CXX_STANDARD_FLAG])
AC_MSG_NOTICE([C++ language mode: $ICE_CXX_STANDARD_FLAG])
AC_MSG_NOTICE([Boost: $icecc_boost_major.$icecc_boost_minor.$icecc_boost_patch; Asio coroutine/TCP APIs: yes; link components: ${BOOST_LIBS:-none}])

# Protocol-50's common 128-bit digest uses the stable XXH3 streaming and one-shot APIs.
# This is an M0 dependency, separate from the B0 C++23/Boost baseline above.
PKG_CHECK_MODULES([XXHASH], [libxxhash >= 0.8.0], [], [
    AC_MSG_ERROR([libxxhash >= 0.8.0 with XXH3-128 support is required])
])
AC_MSG_CHECKING([for required XXH3-128 streaming APIs])
icecc_save_CPPFLAGS="$CPPFLAGS"
icecc_save_LIBS="$LIBS"
CPPFLAGS="$XXHASH_CFLAGS $CPPFLAGS"
LIBS="$XXHASH_LIBS $LIBS"
AC_COMPUTE_INT([icecc_xxhash_version], [XXH_VERSION_NUMBER], [[
    #include <xxhash.h>
]], [AC_MSG_ERROR([xxhash.h does not expose XXH_VERSION_NUMBER])])
AS_IF([test "$icecc_xxhash_version" -lt 800], [
    AC_MSG_ERROR([libxxhash >= 0.8.0 is required; found XXH_VERSION_NUMBER=$icecc_xxhash_version])
])
icecc_xxhash_major=`expr "$icecc_xxhash_version" / 10000`
icecc_xxhash_minor=`expr \( "$icecc_xxhash_version" / 100 \) % 100`
icecc_xxhash_patch=`expr "$icecc_xxhash_version" % 100`
XXHASH_VERSION="$icecc_xxhash_major.$icecc_xxhash_minor.$icecc_xxhash_patch"
AC_LINK_IFELSE([AC_LANG_PROGRAM([[
        #include <xxhash.h>
    ]], [[
        XXH3_state_t* state = XXH3_createState();
        if (state == nullptr) return 1;
        const char byte = 0;
        if (XXH3_128bits_reset(state) != XXH_OK ||
            XXH3_128bits_update(state, &byte, 1) != XXH_OK) return 2;
        const XXH128_hash_t digest = XXH3_128bits_digest(state);
        XXH3_freeState(state);
        (void)digest;
        return 0;
    ]])], [
        AC_MSG_RESULT([yes])
    ], [
        AC_MSG_ERROR([libxxhash is present but the XXH3-128 streaming API is unavailable])
    ])
CPPFLAGS="$icecc_save_CPPFLAGS"
LIBS="$icecc_save_LIBS"
AC_MSG_NOTICE([XXHash: $XXHASH_VERSION; link flags: $XXHASH_LIBS])

AS_CASE([$host], [*linux*], [LIBCAP_NG_DEFAULT=yes], [LIBCAP_NG_DEFAULT=auto])
PKG_HAVE_DEFINE_WITH_MODULES([LIBCAP_NG], [libcap-ng], , $LIBCAP_NG_DEFAULT)

# =============
# Documentation
# =============
AC_ARG_WITH(man,
    [AS_HELP_STRING([--without-man], [Do not build manpage])],
    [with_man="$withval"],
    [with_man=yes]
)
AS_IF([test "x$with_man" != "xno"], [
    build_man=yes
    AC_PATH_PROGS(ASCIIDOC, [asciidoc])
    AS_IF([test -z "$ASCIIDOC"], [
        AC_MSG_WARN([asciidoc is missing. Install asciidoc package.])
        ASCIIDOC='echo asciidoc is missing. Install asciidoc package.'])
    AC_PATH_PROGS(A2X, [a2x])
    AS_IF([test -z "$A2X"], [
        AC_MSG_WARN([a2x is missing. Install a2x package.])
        A2X='echo a2x is missing. Install a2x package.'])
], [build_man=no])
AC_SUBST(ASCIIDOC)
AC_SUBST(A2X)
AM_CONDITIONAL([WITH_ICECREAM_MAN], [test "x$build_man" != "xno"])

########################################################################
### Checks for header files

# Some of these are needed by popt (or other libraries included in the future).

AC_CHECK_HEADERS([signal.h ifaddrs.h kinfo.h sys/param.h devstat.h])
AC_CHECK_HEADERS([sys/socketvar.h sys/vfs.h])
AC_CHECK_HEADERS([mach/host_info.h])
AC_CHECK_HEADERS([arpa/nameser.h], [], [],
[#include <sys/types.h>
])
AC_CHECK_HEADERS([resolv.h], [], [],
[#include <sys/types.h>
#include <netinet/in.h>
#if HAVE_ARPA_NAMESER_H
# include <arpa/nameser.h>
#endif
])

AC_CHECK_HEADERS([netinet/tcp_var.h], [], [],
[#include <sys/types.h>
#if HAVE_SYS_SOCKETVAR_H
# include <sys/socketvar.h>
#endif
#include <netinet/in.h>
#include <netinet/tcp.h>
])

AC_CHECK_HEADERS([sys/user.h])

######################################################################
dnl Checks for types

AC_CHECK_TYPES([sa_family_t, socklen_t, in_port_t, in_addr_t], , ,
	       [
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#if HAVE_ARPA_NAMESER_H
# include <arpa/nameser.h>
#endif
])

AC_CHECK_MEMBER([struct ifreq.ifr_dstaddr],
                [AC_DEFINE(HAVE_IFR_DSTADDR, 1, [Set to 1 if struct ifr_ifru has member ifr_dstaddr] )],
                [AC_DEFINE(HAVE_IFR_DSTADDR, 0, [Set to 0 if struct ifr_ifru has no ifr_dstaddr] )],
                [
#include <net/if.h>
])

########################################################################
### Checks for libraries.

# The following test taken from the cvs sources via Samba:
# If we can't find connect, try looking in -lsocket, -lnsl, and -linet.
# The Irix 5 libc.so has connect and gethostbyname, but Irix 5 also has
# libsocket.so which has a bad implementation of gethostbyname (it
# only looks in /etc/hosts), so we only look for -lsocket if we need
# it.
AC_CHECK_FUNCS(connect)
if test x"$ac_cv_func_connect" = x"no"; then
    case "$LIBS" in
    *-lnsl*) ;;
    *) AC_CHECK_LIB(nsl_s, printf) ;;
    esac
    case "$LIBS" in
    *-lnsl*) ;;
    *) AC_CHECK_LIB(nsl, printf) ;;
    esac
    case "$LIBS" in
    *-lsocket*) ;;
    *) AC_CHECK_LIB(socket, connect) ;;
    esac
    case "$LIBS" in
    *-linet*) ;;
    *) AC_CHECK_LIB(inet, connect) ;;
    esac
    dnl We can't just call AC_CHECK_FUNCS(connect) here, because the value
    dnl has been cached.
    if test x"$ac_cv_lib_socket_connect" = x"yes" || 
       test x"$ac_cv_lib_inet_connect" = x"yes"; then
        # ac_cv_func_connect=yes
        # don't!  it would cause AC_CHECK_FUNC to succeed next time configure is run
        AC_DEFINE(HAVE_CONNECT,1,[Whether the system has connect()])
    fi
fi

AC_CHECK_FUNCS([flock lockf])
AC_CHECK_FUNCS([strsignal])
AC_CHECK_FUNCS([getloadavg])

AC_CHECK_LIB(lzo2, lzo1x_1_compress, LZO_LDADD=-llzo2,
	AC_MSG_ERROR([Could not find lzo2 library - please install lzo-devel]))
AC_SUBST(LZO_LDADD)

PKG_CHECK_MODULES([LIBZSTD], [libzstd])

AC_CHECK_LIB([dl], [dlsym], [DL_LDADD=-ldl])
AC_SUBST([DL_LDADD])

PKG_CHECK_MODULES([LIBARCHIVE], [libarchive])

AC_MSG_CHECKING([whether libarchive has archive_read_support_filter_xz()])
AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[
    #include <archive.h>
    ]], [[
        struct archive *a;
        archive_read_support_filter_xz(a);
    ]])],[
        AC_MSG_RESULT(yes)
        AC_DEFINE(HAVE_LIBARCHIVE_XZ, 1, [Whether libarchive has archive_read_support_filter_xz()])
    ],[ AC_MSG_RESULT(no) ])
AC_MSG_CHECKING([whether libarchive has archive_read_support_filter_zstd()])
AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[
    #include <archive.h>
    ]], [[
        struct archive *a;
        archive_read_support_filter_zstd(a);
    ]])],[
        AC_MSG_RESULT(yes)
        AC_DEFINE(HAVE_LIBARCHIVE_ZSTD, 1, [Whether libarchive has archive_read_support_filter_zstd()])
    ],[ AC_MSG_RESULT(no) ])

# In DragonFlyBSD daemon needs to be linked against libkinfo.
case $host_os in
  dragonfly*) LIB_KINFO="-lkinfo" ;;
  *) LIB_KINFO="" ;;
esac
AC_SUBST(LIB_KINFO)

AC_CHECK_PROG(CLANG,clang,clang)

AC_ARG_ENABLE(clang-rewrite-includes,
    AS_HELP_STRING([--enable-clang-rewrite-includes],
        [Use by default Clang's -frewrite-includes option.]))

AC_ARG_ENABLE(clang-wrappers,
    AS_HELP_STRING([--enable-clang-wrappers],
        [Use symlink wrappers for clang/clang++.]))

CLANG_SYMLINK_WRAPPERS=
if test "$enable_clang_wrappers" = "yes"; then
    CLANG_SYMLINK_WRAPPERS='clang clang++'
elif test "$enable_clang_wrappers" = "no"; then
    true # do not enable
else
    if test -n "$CLANG"; then
        CLANG_SYMLINK_WRAPPERS='clang clang++'
    fi
fi
AC_SUBST(CLANG_SYMLINK_WRAPPERS)

AC_ARG_ENABLE(gcc-color-diagnostics,
    AS_HELP_STRING([--enable-gcc-color-diagnostics],
        [Assume by default GCC has -fdiagnostics-color=auto option.]))

if test "$enable_gcc_color_diagnostics" = "yes"; then
    AC_DEFINE(HAVE_GCC_COLOR_DIAGNOSTICS, 1, [Define to 1 if gcc supports -fdiagnostics-color=auto])
else
    true  # do not enable
fi

AC_ARG_ENABLE(gcc-show-caret,
    AS_HELP_STRING([--enable-gcc-show-caret],
        [Assume by default GCC has -fdiagnostics-show-caret option.]))

if test "$enable_gcc_show_caret" = "yes"; then
    AC_DEFINE(HAVE_GCC_SHOW_CARET, 1, [Define to 1 if gcc supports -fdiagnostics-show-caret])
elif test "$enable_gcc_show_caret" = "no"; then
    true  # do not enable
else
    AC_CHECK_PROG(GCC_BIN,gcc,gcc)
    if test -n "$GCC_BIN"; then
        AC_MSG_CHECKING([for gcc -fdiagnostics-show-caret])
        $GCC_BIN -Werror -E -fdiagnostics-show-caret - >/dev/null 2>/dev/null
        if test $? -eq 0; then
            AC_MSG_RESULT(yes)
            AC_DEFINE(HAVE_GCC_SHOW_CARET, 1, [Define to 1 if gcc supports -fdiagnostics-show-caret])
        else
            AC_MSG_RESULT(no)
        fi
    fi
fi

AC_MSG_CHECKING([for -fsanitize= usage])
if echo "$CXXFLAGS" | grep -q -- -fsanitize; then
    AC_DEFINE(SANITIZER_USED, 1, [Define to 1 if compiled with -fsanitize option(s)])
    AC_MSG_RESULT(yes)
else
    AC_MSG_RESULT(no)
fi

AC_CONFIG_FILES([ Makefile ])
AC_CONFIG_FILES([ client/Makefile ])
AC_CONFIG_FILES([ daemon/Makefile ])
AC_CONFIG_FILES([ doc/Makefile ])
AC_CONFIG_FILES([ services/Makefile ])
AC_CONFIG_FILES([ services/icecc.pc ])
AC_CONFIG_FILES([ cache/Makefile ])
AC_CONFIG_FILES([ suse/Makefile ])
AC_CONFIG_FILES([ compilerwrapper/Makefile ])
AC_CONFIG_FILES([ scheduler/Makefile ])
AC_CONFIG_FILES([ tests/Makefile ])
AC_CONFIG_FILES([ unittests/Makefile ])
AC_CONFIG_FILES([ client/icecc-create-env ])
AC_CONFIG_FILES([ tests/test-setup.sh ])
AC_CONFIG_FILES([ suse/icecream.spec ])
AC_OUTPUT
if test "$prefix" = NONE; then
  prefix=$ac_default_prefix
fi

AC_DEFUN([KDE_EXPAND_MAKEVAR], [
savex=$exec_prefix
test "x$exec_prefix" = xNONE && exec_prefix=$prefix
tmp=$$2
while $1=`eval echo "$tmp"`; test "x$$1" != "x$tmp"; do tmp=$$1; done
exec_prefix=$savex
])

KDE_EXPAND_MAKEVAR(mybindir, bindir)
AC_DEFINE_UNQUOTED(BINDIR, "$mybindir", [Where to look for icecc])

myorundir='${localstatedir}/run'
KDE_EXPAND_MAKEVAR(myrundir, myorundir)
AC_DEFINE_UNQUOTED(RUNDIR, "$myrundir", [Where to place pid files])

KDE_EXPAND_MAKEVAR(mylibexecdir, libexecdir)
PKGLIBEXECDIR="$mylibexecdir/$PACKAGE"
AC_SUBST(PKGLIBEXECDIR)

AC_OUTPUT
