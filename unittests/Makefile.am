TESTS = testargs lockexec occupancy backpressure schedcredit-run.sh

AM_CPPFLAGS = -I$(top_srcdir)/client -I$(top_srcdir)/services -I$(top_srcdir)/
testargs_LDADD = ../client/libclient.a ../services/libicecc.la

check_PROGRAMS = testargs lockexec occupancy backpressure schedcredit
testargs_SOURCES = args.cpp
lockexec_SOURCES = lockexec.cpp
lockexec_LDADD = ../client/libclient.a ../services/libicecc.la
occupancy_SOURCES = occupancy.cpp
backpressure_SOURCES = backpressure.cpp
backpressure_LDADD = ../services/libicecc.la
schedcredit_SOURCES = schedcredit.cpp
schedcredit_LDADD = ../services/libicecc.la

# Every source archive must carry the gate's inputs no matter where the
# archive is produced; the built shim must not survive make clean.
EXTRA_DIST = schedgate-run.sh sndbuf_shim.c schedcredit-run.sh
CLEANFILES = sndbuf_shim.so

# The scheduler integration gate needs an LD_PRELOAD shim to manufacture
# its clog; both are built -- and the test run at all -- only where that
# mechanism exists.  Gating at build time matters: the shim compile itself
# is Linux-specific, so an in-script skip would come too late.
if IS_LINUX
TESTS += schedgate-run.sh
check_PROGRAMS += schedgate
schedgate_SOURCES = schedgate.cpp
schedgate_LDADD = ../services/libicecc.la
# Under parallel make check, check-TESTS could otherwise start before the
# shim exists; make it a prerequisite of the binary itself -- ALONGSIDE the
# linked library, since an explicit _DEPENDENCIES replaces the inferred
# list and would otherwise stop relinking on libicecc changes.
schedgate_DEPENDENCIES = $(schedgate_LDADD) sndbuf_shim.so

sndbuf_shim.so: $(srcdir)/sndbuf_shim.c
	$(CC) -shared -fPIC $(srcdir)/sndbuf_shim.c -ldl -o $@
endif

# Make the tests also print the test log if they fail.
check: export VERBOSE=1
