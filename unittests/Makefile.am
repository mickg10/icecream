TESTS = testargs lockexec backpressure schedpolicy utf8test statewritertest fastesttest hallflowtest jobidalloctest connectivity-run.sh schedbp-quick.sh schedstress-quick.sh remoteice-quick.sh daemonlogin-run.sh daemonbatch-run.sh boost-prefix-scope.sh

AM_TESTS_ENVIRONMENT = \
	ICECC_TEST_BUILDDIR='$(abs_builddir)'; export ICECC_TEST_BUILDDIR; \
	ICECC_TEST_TOP_BUILDDIR='$(abs_top_builddir)'; export ICECC_TEST_TOP_BUILDDIR; \
	ICECC_TEST_TOP_SRCDIR='$(abs_top_srcdir)'; export ICECC_TEST_TOP_SRCDIR; \
	ICECC_TEST_CC='$(CC)'; export ICECC_TEST_CC; \
	ICECC_TEST_CXX='$(CXX)'; export ICECC_TEST_CXX; \
	ICECC_TEST_AR='$(AR)'; export ICECC_TEST_AR; \
	ICECC_TEST_CXX_STANDARD_FLAG='$(ICE_CXX_STANDARD_FLAG)'; export ICECC_TEST_CXX_STANDARD_FLAG; \
	ICECC_TEST_CPPFLAGS='$(CPPFLAGS)'; export ICECC_TEST_CPPFLAGS; \
	ICECC_TEST_LDFLAGS='$(LDFLAGS)'; export ICECC_TEST_LDFLAGS; \
	ICECC_TEST_BOOST_CPPFLAGS='$(BOOST_CPPFLAGS)'; export ICECC_TEST_BOOST_CPPFLAGS; \
	ICECC_TEST_LIBCAP_NG_CFLAGS='$(LIBCAP_NG_CFLAGS)'; export ICECC_TEST_LIBCAP_NG_CFLAGS; \
	ICECC_TEST_LIBCAP_NG_LIBS='$(LIBCAP_NG_LIBS)'; export ICECC_TEST_LIBCAP_NG_LIBS; \
	ICECC_TEST_LIBARCHIVE_CFLAGS='$(LIBARCHIVE_CFLAGS)'; export ICECC_TEST_LIBARCHIVE_CFLAGS; \
	ICECC_TEST_LIBARCHIVE_LIBS='$(LIBARCHIVE_LIBS)'; export ICECC_TEST_LIBARCHIVE_LIBS; \
	ICECC_TEST_LIBZSTD_CFLAGS='$(LIBZSTD_CFLAGS)'; export ICECC_TEST_LIBZSTD_CFLAGS; \
	ICECC_TEST_LIBZSTD_LIBS='$(LIBZSTD_LIBS)'; export ICECC_TEST_LIBZSTD_LIBS;

AM_CPPFLAGS = -I$(top_srcdir)/client -I$(top_srcdir)/services -I$(top_srcdir)/
testargs_LDADD = ../client/libclient.a ../services/libicecc.la

check_PROGRAMS = testargs lockexec backpressure schedpolicy utf8test statewritertest schedbp daemonlogin daemonbatch connectivity fastesttest hallflowtest jobidalloctest
testargs_SOURCES = args.cpp
lockexec_SOURCES = lockexec.cpp
lockexec_LDADD = ../client/libclient.a ../services/libicecc.la

schedpolicy_SOURCES = schedpolicy.cpp

backpressure_SOURCES = backpressure.cpp

utf8test_SOURCES = utf8test.cpp

statewritertest_SOURCES = statewritertest.cpp ../daemon/statewriter.cpp
statewritertest_CPPFLAGS = $(AM_CPPFLAGS) -I$(top_srcdir)/daemon
# statewriter.cpp now logs its one-shot death notice via the shared logger
statewritertest_LDADD = ../services/libicecc.la
backpressure_CXXFLAGS = -pthread
backpressure_LDFLAGS = -pthread
backpressure_LDADD = ../services/libicecc.la

# Integration harness for scheduler backpressure (issue #1).  Built by
# `make check` so it cannot bit-rot; execution is manual because it starts a
# real scheduler and takes minutes -- see the header of schedbp.cpp.
schedbp_SOURCES = schedbp.cpp

# Login-attempt/ConfCS activation lifecycle gate (issue #4 G4).
daemonlogin_SOURCES = daemonlogin.cpp
daemonlogin_LDADD = ../services/libicecc.la
daemonbatch_SOURCES = daemonbatch.cpp
daemonbatch_LDADD = ../services/libicecc.la

# Connectivity-probe state machine regression (issue #4 item 4): drives
# CompileServer directly, with failure injection from conn_shim.so.
connectivity_SOURCES = connectivity.cpp ../scheduler/compileserver.cpp ../scheduler/job.cpp ../scheduler/jobstat.cpp
connectivity_CPPFLAGS = $(AM_CPPFLAGS) -I$(top_srcdir)/scheduler
connectivity_LDADD = ../services/libicecc.la

fastesttest_SOURCES = fastesttest.cpp

hallflowtest_SOURCES = hallflowtest.cpp

jobidalloctest_SOURCES = jobidalloctest.cpp
schedbp_CXXFLAGS = -pthread
schedbp_LDFLAGS = -pthread
schedbp_LDADD = ../services/libicecc.la

check-local: sndbuf_shim.so conn_shim.so
sndbuf_shim.so: $(srcdir)/sndbuf_shim.c
	$(CC) -shared -fPIC $(srcdir)/sndbuf_shim.c -ldl -o $@
conn_shim.so: $(srcdir)/conn_shim.c
	$(CC) -shared -fPIC $(srcdir)/conn_shim.c -ldl -o $@

connectivity_DEPENDENCIES = conn_shim.so

CLEANFILES = sndbuf_shim.so conn_shim.so schedbp-scheduler-*.log
EXTRA_DIST = sndbuf_shim.c conn_shim.c connectivity-run.sh schedbp-quick.sh schedstress-quick.sh remoteice-quick.sh daemonlogin-run.sh daemonbatch-run.sh boost-prefix-scope.sh

# Make the tests also print the test log if they fail.
check: export VERBOSE=1
