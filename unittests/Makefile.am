TESTS = testargs backpressure

AM_CPPFLAGS = -I$(top_srcdir)/client -I$(top_srcdir)/services -I$(top_srcdir)/
testargs_LDADD = ../client/libclient.a ../services/libicecc.la

check_PROGRAMS = testargs backpressure
testargs_SOURCES = args.cpp

backpressure_SOURCES = backpressure.cpp
backpressure_LDADD = ../services/libicecc.la

# Make the tests also print the test log if they fail.
check: export VERBOSE=1
