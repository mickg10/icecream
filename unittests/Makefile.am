TESTS = testargs lockexec

AM_CPPFLAGS = -I$(top_srcdir)/client -I$(top_srcdir)/services -I$(top_srcdir)/
testargs_LDADD = ../client/libclient.a ../services/libicecc.la

check_PROGRAMS = testargs lockexec
testargs_SOURCES = args.cpp
lockexec_SOURCES = lockexec.cpp
lockexec_LDADD = ../client/libclient.a ../services/libicecc.la

# Make the tests also print the test log if they fail.
check: export VERBOSE=1
