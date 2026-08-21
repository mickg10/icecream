TESTS = testargs p50wire digest128bench

AM_CPPFLAGS = -I$(top_srcdir)/client -I$(top_srcdir)/services -I$(top_srcdir)/
testargs_LDADD = ../client/libclient.a ../services/libicecc.la
p50wire_LDADD = ../cache/libprotocol50.a ../services/libicecc.la
digest128bench_LDADD = ../services/libicecc.la

check_PROGRAMS = testargs p50wire digest128bench
testargs_SOURCES = args.cpp
p50wire_SOURCES = p50_wire_test.cpp
digest128bench_SOURCES = digest128_bench.cpp

# Make the tests also print the test log if they fail.
check: export VERBOSE=1
