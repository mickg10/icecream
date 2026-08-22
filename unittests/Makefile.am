TESTS = testargs p50wire p50slice0 p50zstd p50zstdwindow p50endpoint p50preparereplay digest128bench p50_trace_check.sh
TEST_EXTENSIONS = .sh
SH_LOG_COMPILER = $(SHELL)

AM_CPPFLAGS = -I$(top_srcdir)/client -I$(top_srcdir)/services -I$(top_srcdir)/ \
	$(LIBZSTD_CFLAGS)
testargs_LDADD = ../client/libclient.a ../services/libicecc.la
p50wire_LDADD = ../cache/libprotocol50.a ../services/libicecc.la
p50slice0_LDADD = ../cache/libprotocol50.a ../services/libicecc.la
p50zstd_LDADD = ../cache/libprotocol50.a ../services/libicecc.la $(LIBZSTD_LIBS)
p50zstdwindow_LDADD = ../cache/libprotocol50.a ../services/libicecc.la $(LIBZSTD_LIBS)
p50endpoint_LDFLAGS = $(BOOST_LDFLAGS)
p50endpoint_LDADD = ../cache/libprotocol50.a ../services/libicecc.la \
	$(LIBZSTD_LIBS) $(BOOST_LIBS)
p50preparereplay_CPPFLAGS = $(AM_CPPFLAGS) $(BOOST_CPPFLAGS)
p50preparereplay_LDFLAGS = $(BOOST_LDFLAGS)
p50preparereplay_LDADD = ../cache/libprotocol50.a ../services/libicecc.la \
	$(LIBZSTD_LIBS) $(BOOST_LIBS)
digest128bench_LDADD = ../services/libicecc.la

check_PROGRAMS = testargs p50wire p50slice0 p50zstd p50zstdwindow p50endpoint p50preparereplay digest128bench
testargs_SOURCES = args.cpp
p50wire_SOURCES = p50_wire_test.cpp
p50slice0_SOURCES = \
	p50_slice0_test.cpp
p50zstd_SOURCES = p50_zstd_test.cpp
p50zstdwindow_SOURCES = p50_zstd_window_test.cpp
p50endpoint_SOURCES = p50_endpoint_test.cpp
p50preparereplay_SOURCES = p50_prepare_replay_contract_test.cpp
digest128bench_SOURCES = digest128_bench.cpp

EXTRA_DIST = p50_trace_check.sh

# Make the tests also print the test log if they fail.
check: export VERBOSE=1
