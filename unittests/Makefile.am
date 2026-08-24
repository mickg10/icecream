TESTS = testargs lockexec backpressure schedpolicy utf8test statewritertest fastesttest hallflowtest jobidalloctest inputpump compilerinput p49assignment p50assignment p50cacheadvertisement p50assignment-source.sh p50cacheadvertisement-source.sh p50localtransport-source.sh packagebuilderdeps.sh p49daemon-run.sh p49scheduler-run.sh connectivity-run.sh schedbp-quick.sh schedstress-quick.sh remoteice-quick.sh p50assignment-remote.sh daemonlogin-run.sh daemonbatch-run.sh cachehandoffdaemon-run.sh boost-prefix-scope.sh p50wire p50slice0 p50zstd p50zstdwindow p50inputrecord p50inputrecordlate p50inputrecordreopen p50endpoint p50localtransport digest128bench p50cacheservice p50_trace_check.sh

AM_TESTS_ENVIRONMENT = \
	ICECC_TEST_BUILDDIR='$(abs_builddir)'; export ICECC_TEST_BUILDDIR; \
	ICECC_TEST_TOP_BUILDDIR='$(abs_top_builddir)'; export ICECC_TEST_TOP_BUILDDIR; \
	ICECC_TEST_TOP_SRCDIR='$(abs_top_srcdir)'; export ICECC_TEST_TOP_SRCDIR; \
	ICECC_TEST_MAKE='$(MAKE)'; export ICECC_TEST_MAKE; \
	ICECC_TEST_CC='$(CC)'; export ICECC_TEST_CC; \
	ICECC_TEST_CXX='$(CXX)'; export ICECC_TEST_CXX; \
	ICECC_TEST_AR='$(AR)'; export ICECC_TEST_AR; \
	ICECC_TEST_RANLIB='$(RANLIB)'; export ICECC_TEST_RANLIB; \
	ICECC_TEST_PKG_CONFIG='$(PKG_CONFIG)'; export ICECC_TEST_PKG_CONFIG; \
	ICECC_TEST_PKG_CONFIG_PATH='$(PKG_CONFIG_PATH)'; export ICECC_TEST_PKG_CONFIG_PATH; \
	ICECC_TEST_PKG_CONFIG_LIBDIR='$(PKG_CONFIG_LIBDIR)'; export ICECC_TEST_PKG_CONFIG_LIBDIR; \
	ICECC_TEST_CACHE_SERVICE='$(abs_top_builddir)/cache/icecc-cache-service'; export ICECC_TEST_CACHE_SERVICE; \
	ICECC_TEST_CXX_STANDARD_FLAG='$(ICE_CXX_STANDARD_FLAG)'; export ICECC_TEST_CXX_STANDARD_FLAG; \
	ICECC_TEST_CFLAGS='$(CFLAGS)'; export ICECC_TEST_CFLAGS; \
	ICECC_TEST_CXXFLAGS='$(CXXFLAGS)'; export ICECC_TEST_CXXFLAGS; \
	ICECC_TEST_CPPFLAGS='$(CPPFLAGS)'; export ICECC_TEST_CPPFLAGS; \
	ICECC_TEST_LDFLAGS='$(LDFLAGS)'; export ICECC_TEST_LDFLAGS; \
	ICECC_TEST_LIBS='$(LIBS)'; export ICECC_TEST_LIBS; \
	ICECC_TEST_BOOST_CPPFLAGS='$(BOOST_CPPFLAGS)'; export ICECC_TEST_BOOST_CPPFLAGS; \
	ICECC_TEST_LIBCAP_NG_CFLAGS='$(LIBCAP_NG_CFLAGS)'; export ICECC_TEST_LIBCAP_NG_CFLAGS; \
	ICECC_TEST_LIBCAP_NG_LIBS='$(LIBCAP_NG_LIBS)'; export ICECC_TEST_LIBCAP_NG_LIBS; \
	ICECC_TEST_LIBARCHIVE_CFLAGS='$(LIBARCHIVE_CFLAGS)'; export ICECC_TEST_LIBARCHIVE_CFLAGS; \
	ICECC_TEST_LIBARCHIVE_LIBS='$(LIBARCHIVE_LIBS)'; export ICECC_TEST_LIBARCHIVE_LIBS; \
	ICECC_TEST_LIBZSTD_CFLAGS='$(LIBZSTD_CFLAGS)'; export ICECC_TEST_LIBZSTD_CFLAGS; \
	ICECC_TEST_LIBZSTD_LIBS='$(LIBZSTD_LIBS)'; export ICECC_TEST_LIBZSTD_LIBS; \
	ICECC_TEST_XXHASH_CFLAGS='$(XXHASH_CFLAGS)'; export ICECC_TEST_XXHASH_CFLAGS; \
	ICECC_TEST_XXHASH_LIBS='$(XXHASH_LIBS)'; export ICECC_TEST_XXHASH_LIBS;
TEST_EXTENSIONS = .sh
SH_LOG_COMPILER = $(SHELL)

AM_CPPFLAGS = -I$(top_srcdir)/client -I$(top_srcdir)/services -I$(top_srcdir)/ \
	$(LIBZSTD_CFLAGS)
testargs_LDADD = ../client/libclient.a ../services/libicecc.la
p50wire_LDADD = ../cache/libprotocol50.a ../services/libicecc.la
p50slice0_LDADD = ../cache/libprotocol50.a ../services/libicecc.la
p50zstd_LDADD = ../cache/libprotocol50.a ../services/libicecc.la $(LIBZSTD_LIBS)
p50zstdwindow_LDADD = ../cache/libprotocol50.a ../services/libicecc.la $(LIBZSTD_LIBS)
p50inputrecord_LDADD = ../cache/libprotocol50.a ../services/libicecc.la
p50inputrecordlate_LDADD = ../cache/libprotocol50.a ../services/libicecc.la
p50inputrecordreopen_LDADD = ../cache/libprotocol50.a ../services/libicecc.la
p50endpoint_CPPFLAGS = $(AM_CPPFLAGS) $(BOOST_CPPFLAGS)
p50endpoint_LDFLAGS = $(BOOST_LDFLAGS)
p50endpoint_LDADD = ../cache/libp50endpoint.a ../cache/libprotocol50.a \
	../services/libicecc.la \
	$(LIBZSTD_LIBS) $(BOOST_LIBS)
p50localtransport_SOURCES = p50_local_transport_test.cpp ../cache/p50_local_transport.cpp
p50localtransport_CPPFLAGS = $(AM_CPPFLAGS) -DICECC_P50_LOCAL_TRANSPORT_TEST_HOOKS
p50localtransport_CXXFLAGS = -pthread
p50localtransport_LDFLAGS = -pthread
digest128bench_LDADD = ../services/libicecc.la

check_PROGRAMS = testargs lockexec backpressure schedpolicy utf8test statewritertest schedbp daemonlogin daemonbatch cachehandoffdaemon p49daemon p49scheduler connectivity fastesttest hallflowtest jobidalloctest inputpump compilerinput p49assignment p50assignment p50cacheadvertisement p50wire p50slice0 p50zstd p50zstdwindow p50inputrecord p50inputrecordlate p50inputrecordreopen p50endpoint p50localtransport digest128bench p50cacheservice
testargs_SOURCES = args.cpp
p50wire_SOURCES = p50_wire_test.cpp
p50slice0_SOURCES = \
	p50_slice0_test.cpp
p50zstd_SOURCES = p50_zstd_test.cpp
p50zstdwindow_SOURCES = p50_zstd_window_test.cpp
p50inputrecord_SOURCES = p50_input_record_test.cpp
p50inputrecordlate_SOURCES = p50_input_record_late_commit_test.cpp
p50inputrecordreopen_SOURCES = p50_input_record_closed_reopen_test.cpp
p50endpoint_SOURCES = p50_endpoint_test.cpp
digest128bench_SOURCES = digest128_bench.cpp
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
cachehandoffdaemon_SOURCES = cachehandoffdaemon.cpp
cachehandoffdaemon_LDADD = ../services/libicecc.la
daemonbatch_SOURCES = daemonbatch.cpp
daemonbatch_LDADD = ../services/libicecc.la
p49daemon_SOURCES = p49daemon.cpp
p49daemon_LDADD = ../services/libicecc.la
p49scheduler_SOURCES = p49scheduler.cpp
p49scheduler_LDADD = ../services/libicecc.la
p49scheduler_DEPENDENCIES = sndbuf_shim.so

# Connectivity-probe state machine regression (issue #4 item 4): drives
# CompileServer directly, with failure injection from conn_shim.so.
connectivity_SOURCES = connectivity.cpp ../scheduler/compileserver.cpp ../scheduler/job.cpp ../scheduler/jobstat.cpp
connectivity_CPPFLAGS = $(AM_CPPFLAGS) -I$(top_srcdir)/scheduler
connectivity_LDADD = ../services/libicecc.la

fastesttest_SOURCES = fastesttest.cpp

hallflowtest_SOURCES = hallflowtest.cpp

jobidalloctest_SOURCES = jobidalloctest.cpp
p49assignment_SOURCES = p49assignment.cpp
p49assignment_CXXFLAGS = -pthread
p49assignment_LDFLAGS = -pthread
p49assignment_LDADD = ../services/libicecc.la
p50assignment_SOURCES = p50assignment.cpp
p50assignment_CXXFLAGS = -pthread
p50assignment_LDFLAGS = -pthread
p50assignment_LDADD = ../services/libicecc.la
p50cacheadvertisement_SOURCES = p50cacheadvertisement.cpp
p50cacheadvertisement_CXXFLAGS = -pthread
p50cacheadvertisement_LDFLAGS = -pthread
p50cacheadvertisement_LDADD = ../cache/libprotocol50.a ../services/libicecc.la
p50cacheservice_SOURCES = p50cacheservice.cpp
p50cacheservice_LDADD = ../cache/libp50localtransport.a
p50cacheservice_DEPENDENCIES = ../cache/icecc-cache-service
inputpump_SOURCES = input_pump_test.cpp
inputpump_CXXFLAGS = -pthread
inputpump_LDFLAGS = -pthread
inputpump_LDADD = ../client/libclient.a ../services/libicecc.la
compilerinput_SOURCES = compiler_input_test.cpp ../daemon/compiler_input.cpp
compilerinput_CPPFLAGS = $(AM_CPPFLAGS) -I$(top_srcdir)/daemon
compilerinput_CXXFLAGS = -pthread
compilerinput_LDFLAGS = -pthread
compilerinput_LDADD = ../services/libicecc.la
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
EXTRA_DIST = sndbuf_shim.c conn_shim.c connectivity-run.sh schedbp-quick.sh schedstress-quick.sh remoteice-quick.sh p50assignment-remote.sh daemonlogin-run.sh daemonbatch-run.sh cachehandoffdaemon-run.sh p49daemon-run.sh p49scheduler-run.sh boost-prefix-scope.sh

EXTRA_DIST += p50_trace_check.sh p50assignment-source.sh p50cacheadvertisement-source.sh p50localtransport-source.sh packagebuilderdeps.sh

# Make the tests also print the test log if they fail.
check: export VERBOSE=1
