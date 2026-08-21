TESTS = testargs p50wire p50slice0 digest128bench p50_trace_check.sh
TEST_EXTENSIONS = .sh
SH_LOG_COMPILER = $(SHELL)

AM_CPPFLAGS = -I$(top_srcdir)/client -I$(top_srcdir)/services -I$(top_srcdir)/
testargs_LDADD = ../client/libclient.a ../services/libicecc.la
p50wire_LDADD = ../cache/libprotocol50.a ../services/libicecc.la
p50slice0_LDADD = ../cache/libprotocol50.a ../services/libicecc.la
digest128bench_LDADD = ../services/libicecc.la

check_PROGRAMS = testargs p50wire p50slice0 digest128bench
testargs_SOURCES = args.cpp
p50wire_SOURCES = p50_wire_test.cpp
p50slice0_SOURCES = \
	p50_slice0_test.cpp
digest128bench_SOURCES = digest128_bench.cpp

EXTRA_DIST = p50_trace_check.sh

# Make the tests also print the test log if they fail.
check: export VERBOSE=1
