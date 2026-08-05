TESTS = testargs backpressure schedpolicy schedbp-quick.sh remoteice-quick.sh

AM_CPPFLAGS = -I$(top_srcdir)/client -I$(top_srcdir)/services -I$(top_srcdir)/
testargs_LDADD = ../client/libclient.a ../services/libicecc.la

check_PROGRAMS = testargs backpressure schedpolicy schedbp
testargs_SOURCES = args.cpp

schedpolicy_SOURCES = schedpolicy.cpp

backpressure_SOURCES = backpressure.cpp
backpressure_CXXFLAGS = -pthread
backpressure_LDFLAGS = -pthread
backpressure_LDADD = ../services/libicecc.la

# Integration harness for scheduler backpressure (issue #1).  Built by
# `make check` so it cannot bit-rot; execution is manual because it starts a
# real scheduler and takes minutes -- see the header of schedbp.cpp.
schedbp_SOURCES = schedbp.cpp
schedbp_CXXFLAGS = -pthread
schedbp_LDFLAGS = -pthread
schedbp_LDADD = ../services/libicecc.la

check-local: sndbuf_shim.so
sndbuf_shim.so: $(srcdir)/sndbuf_shim.c
	$(CC) -shared -fPIC $(srcdir)/sndbuf_shim.c -ldl -o $@

CLEANFILES = sndbuf_shim.so
EXTRA_DIST = sndbuf_shim.c schedbp-quick.sh remoteice-quick.sh

# Make the tests also print the test log if they fail.
check: export VERBOSE=1
