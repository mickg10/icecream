TESTS = testargs lockexec backpressure schedpolicy utf8test statewritertest fastesttest connectivity-run.sh schedbp-quick.sh schedstress-quick.sh remoteice-quick.sh

AM_CPPFLAGS = -I$(top_srcdir)/client -I$(top_srcdir)/services -I$(top_srcdir)/
testargs_LDADD = ../client/libclient.a ../services/libicecc.la

check_PROGRAMS = testargs lockexec backpressure schedpolicy utf8test statewritertest schedbp connectivity fastesttest
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

# Connectivity-probe state machine regression (issue #4 item 4): drives
# CompileServer directly, with failure injection from conn_shim.so.
connectivity_SOURCES = connectivity.cpp ../scheduler/compileserver.cpp ../scheduler/job.cpp ../scheduler/jobstat.cpp
connectivity_CPPFLAGS = $(AM_CPPFLAGS) -I$(top_srcdir)/scheduler
connectivity_LDADD = ../services/libicecc.la

fastesttest_SOURCES = fastesttest.cpp
schedbp_CXXFLAGS = -pthread
schedbp_LDFLAGS = -pthread
schedbp_LDADD = ../services/libicecc.la

check-local: sndbuf_shim.so conn_shim.so
sndbuf_shim.so: $(srcdir)/sndbuf_shim.c
	$(CC) -shared -fPIC $(srcdir)/sndbuf_shim.c -ldl -o $@
conn_shim.so: $(srcdir)/conn_shim.c
	$(CC) -shared -fPIC $(srcdir)/conn_shim.c -ldl -o $@

connectivity_DEPENDENCIES = conn_shim.so

CLEANFILES = sndbuf_shim.so conn_shim.so
EXTRA_DIST = sndbuf_shim.c conn_shim.c connectivity-run.sh schedbp-quick.sh schedstress-quick.sh remoteice-quick.sh

# Make the tests also print the test log if they fail.
check: export VERBOSE=1
