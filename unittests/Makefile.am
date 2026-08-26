TESTS = testargs lockexec backpressure schedpolicy utf8test statewritertest fastesttest hallflowtest jobidalloctest inputpump compilerinput p49assignment p50assignment p50cacheadvertisement p50cachesession p50daemoncachedispatch p50assignment-source.sh p50cacheadvertisement-source.sh p50cachesession-source.sh p50daemoncachedispatch-source.sh p50daemoncachedispatch-sanitize.sh p50localtransport-source.sh p50localtransport-sanitize.sh p50sidecarsupervisor-source.sh p50inputattachment-source.sh p50inputattachment-mutants.sh p50_input_attachment_sanitize.sh p50readyadvertisement-source.sh p50readyadvertisement-mutants.sh p50endpoint-source.sh p50fdhandoff-source.sh p50cacheservice-source.sh p50cacheservice-sanitize.sh p50fdhandoff-sanitize.sh packagebuilderdeps.sh p49daemon-run.sh p49scheduler-run.sh connectivity-run.sh schedbp-quick.sh schedstress-quick.sh remoteice-quick.sh p50assignment-remote.sh daemonlogin-run.sh daemonbatch-run.sh cachehandoffdaemon-run.sh daemoncachedispatch-run.sh boost-prefix-scope.sh p50wire p50slice0 p50zstd p50zstdwindow p50inputrecord p50inputrecordlate p50inputrecordreopen p50inputattachment p50endpoint p50localtransport p50sidecarsupervisor p50readyadvertisement p50cacheservice p50fdhandoff digest128bench p50_trace_check.sh
TESTS += p50sourcearm p50sourcearm-source.sh
TESTS += p50sourcearmwire p50sourcearmwire-source.sh
TESTS += p50storeidentity-semantic.sh
TESTS += p50sourcearmlease
TESTS += p50sourcearm-wait-mutants.sh
TESTS += p50sourcearm-live-source.sh p50sourcearm-live-run.sh
TESTS += p50storeidentitywire p50storeidentitywire-mutants.sh
TESTS += p50sourceingress p50sourceingress-source.sh p50sourceingress-sanitize.sh
TESTS += p50phaseopen p50phaseopen-source.sh
TESTS += p50reversefdretry p50reversefdretry-source.sh
TESTS += p50reversefdretry-mutants.sh p50_reverse_fd_retry_sanitize.sh
TESTS += p50s2restartreplay p50s2restartreplay-source.sh
TESTS += p50_s2_restart_replay_sanitize.sh
TESTS += p50sidecarsupervisor-sanitize.sh
TESTS += p50sidecarlifecycle p50sidecarlifecycle-source.sh p50sidecarlifecycle-mutants.sh p50sidecarlifecycle-sanitize.sh
TESTS += p50connectionprovenance
TESTS += p50daemonsidecaradapter
TESTS += p50daemonsidecaradapter-source.sh
TESTS += p50daemonsidecaradapter-sanitize.sh
TESTS += p50daemonpositive-source.sh p50daemonpositive-run.sh
TESTS += p50inputfd-source.sh p50inputfd-sanitize.sh
TESTS += p50inputfd
TESTS += p50inputlifecycle-source.sh p50inputlifecycle-sanitize.sh
TESTS += p50inputlifecycle
TESTS += p50compilebinding
TESTS += p50compilee2e-source.sh p50compilee2e-run.sh p50resultdisposition
TESTS += p50_completion_record
TESTS += p50daemontaskcount p50daemontaskcount-source.sh
TESTS += p50forkfdhygiene p50forkfdhygiene-source.sh p50forkfdhygiene-mutants.sh p50_fork_fd_hygiene_sanitize.sh p50forkfdhygiene-oot.sh p50forkfdhygiene-package.sh
TESTS += p50completionflow-source.py
TESTS += p50completionrecord-sanitize.sh
TESTS += p50completionflow-run.sh
TESTS += p50profile p50profile-source.sh p50_profile_sanitize.sh
TESTS += p50roleowner p50tworole-source.sh
TESTS += p50sessionjoin p50cachesessionjoin-source.sh \
	p50cachesessionjoin-mutants.sh p50cachesessionjoin-sanitize.sh
TESTS += p50cachesessionwire p50cachesessionwire-source.sh \
	p50cachesessionwire-mutants.sh p50cachesessionwire-sanitize.sh
TESTS += p50daemoncontrol p50daemoncontrol-source.sh \
	p50daemoncontrol-mutants.sh p50_daemon_control_sanitize.sh

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
	ICECC_TEST_READY_CLOSE_SHIM='$(abs_builddir)/readyclose_shim.so'; export ICECC_TEST_READY_CLOSE_SHIM; \
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
P50_CLIENT_LINK_LIBS = ../client/libp50zstdsender.a \
	../cache/libp50endpoint.a ../cache/libprotocol50.a \
	../services/libicecc.la $(LIBZSTD_LIBS) $(XXHASH_LIBS) $(BOOST_LIBS)
testargs_LDFLAGS = $(BOOST_LDFLAGS) -pthread
testargs_LDADD = ../client/libclient.a $(P50_CLIENT_LINK_LIBS)
p50wire_LDADD = ../cache/libprotocol50.a ../services/libicecc.la
p50slice0_LDADD = ../cache/libprotocol50.a ../services/libicecc.la
p50zstd_LDADD = ../cache/libprotocol50.a ../services/libicecc.la $(LIBZSTD_LIBS)
p50zstdwindow_LDADD = ../cache/libprotocol50.a ../services/libicecc.la $(LIBZSTD_LIBS)
p50inputrecord_LDADD = ../cache/libprotocol50.a ../services/libicecc.la
p50inputrecordlate_LDADD = ../cache/libprotocol50.a ../services/libicecc.la
p50inputrecordreopen_LDADD = ../cache/libprotocol50.a ../services/libicecc.la
p50inputattachment_LDADD = ../cache/libprotocol50.a ../services/libicecc.la
p50endpoint_CPPFLAGS = $(AM_CPPFLAGS) $(BOOST_CPPFLAGS)
p50endpoint_LDFLAGS = $(BOOST_LDFLAGS)
p50endpoint_LDADD = ../cache/libp50endpoint.a ../cache/libprotocol50.a \
	../services/libicecc.la \
	$(LIBZSTD_LIBS) $(BOOST_LIBS)
p50localtransport_SOURCES = p50_local_transport_test.cpp ../cache/p50_local_transport.cpp
p50localtransport_CPPFLAGS = $(AM_CPPFLAGS) -DICECC_P50_LOCAL_TRANSPORT_TEST_HOOKS
p50localtransport_CXXFLAGS = -pthread
p50localtransport_LDFLAGS = -pthread
p50sidecarsupervisor_SOURCES = p50_sidecar_supervisor_test.cpp
p50sidecarsupervisor_CXXFLAGS = -pthread
p50sidecarsupervisor_LDFLAGS = -pthread
p50sidecarsupervisor_LDADD = ../cache/libp50sidecarsupervisor.a \
	../cache/libp50localtransport.a ../services/libicecc.la
p50sidecarlifecycle_SOURCES = p50_sidecar_lifecycle_test.cpp
p50sidecarlifecycle_CXXFLAGS = -pthread
p50sidecarlifecycle_LDFLAGS = -pthread
p50sidecarlifecycle_LDADD = ../cache/libp50sidecarlifecycle.a \
	../cache/libp50sidecarsupervisor.a ../cache/libp50localtransport.a \
	../services/libicecc.la
p50roleowner_SOURCES = p50_role_owner_test.cpp ../cache/p50_role_owner.cpp
p50roleowner_CXXFLAGS = -pthread
p50roleowner_LDFLAGS = -pthread
p50sessionjoin_SOURCES = p50_cache_session_join_test.cpp \
	../daemon/connection_provenance.cpp
p50sessionjoin_CPPFLAGS = $(AM_CPPFLAGS)
p50sessionjoin_LDADD = ../cache/libp50sessionjoin.a \
	../cache/libprotocol50.a ../services/libicecc.la \
	$(LIBZSTD_LIBS) $(XXHASH_LIBS)
p50cachesessionwire_SOURCES = p50_cache_session_wire_test.cpp
p50cachesessionwire_CXXFLAGS = -pthread
p50cachesessionwire_LDFLAGS = -pthread
p50cachesessionwire_LDADD = ../services/libicecc.la
p50daemoncontrol_SOURCES = p50_daemon_control_test.cpp
p50daemoncontrol_CXXFLAGS = -pthread
p50daemoncontrol_LDFLAGS = -pthread
p50daemoncontrol_LDADD = ../cache/libp50localtransport.a
p50readyadvertisement_LDADD = ../cache/libp50readyadvertisement.a
digest128bench_LDADD = ../services/libicecc.la

check_PROGRAMS = testargs lockexec backpressure schedpolicy utf8test statewritertest schedbp daemonlogin daemonbatch cachehandoffdaemon daemoncachedispatch p50daemoncachedispatch p49daemon p49scheduler connectivity fastesttest hallflowtest jobidalloctest inputpump compilerinput p49assignment p50assignment p50cacheadvertisement p50cachesession p50wire p50slice0 p50zstd p50zstdwindow p50inputrecord p50inputrecordlate p50inputrecordreopen p50inputattachment p50endpoint p50localtransport p50sidecarsupervisor p50readyadvertisement p50cacheservice p50fdhandoff digest128bench p50zstdsender p50compilebinding
check_PROGRAMS += p50daemonsidecaradapter p50daemonpositive
check_PROGRAMS += p50inputfd
check_PROGRAMS += p50inputlifecycle
check_PROGRAMS += p50resultdisposition
check_PROGRAMS += p50_completion_record
check_PROGRAMS += p50daemontaskcount
check_PROGRAMS += p50forkfdhygiene
check_PROGRAMS += p50connectionprovenance
check_PROGRAMS += p50profile
testargs_SOURCES = args.cpp
check_PROGRAMS += p50sourcearm p50phaseopen
check_PROGRAMS += p50sourceingress
check_PROGRAMS += p50sourcearmwire
check_PROGRAMS += p50sourcearmlease
check_PROGRAMS += p50sourcearm-live
check_PROGRAMS += p50storeidentitywire
check_PROGRAMS += p50reversefdretry
check_PROGRAMS += p50s2restartreplay
check_PROGRAMS += p50roleowner
check_PROGRAMS += p50sessionjoin
check_PROGRAMS += p50cachesessionwire
check_PROGRAMS += p50sidecarlifecycle
check_PROGRAMS += p50daemoncontrol
p50wire_SOURCES = p50_wire_test.cpp
p50sourceingress_SOURCES = p50_source_ingress_test.cpp
p50sourceingress_CXXFLAGS = -pthread
p50sourceingress_LDFLAGS = -pthread
p50sourceingress_LDADD = ../cache/libprotocol50.a ../services/libicecc.la
p50storeidentitywire_SOURCES = p50_store_identity_wire_test.cpp
p50slice0_SOURCES = \
	p50_slice0_test.cpp
p50zstd_SOURCES = p50_zstd_test.cpp
p50profile_SOURCES = p50_profile_test.cpp
p50profile_LDADD = ../cache/libprotocol50.a ../services/libicecc.la $(LIBZSTD_LIBS)
p50zstdwindow_SOURCES = p50_zstd_window_test.cpp
p50inputrecord_SOURCES = p50_input_record_test.cpp
p50inputrecordlate_SOURCES = p50_input_record_late_commit_test.cpp
p50inputrecordreopen_SOURCES = p50_input_record_closed_reopen_test.cpp
p50inputattachment_SOURCES = p50_input_attachment_test.cpp
p50inputfd_SOURCES = p50_input_fd_attachment_test.cpp \
	../cache/p50_input_fd_attachment.cpp ../cache/p50_input_record.cpp \
	../cache/p50_local_transport.cpp ../cache/p50_fd_handoff.cpp \
	../cache/protocol50.cpp
p50inputfd_CPPFLAGS = $(AM_CPPFLAGS) -DICECC_P50_INPUT_FD_ATTACHMENT_TEST_HOOKS
p50inputfd_CXXFLAGS = -pthread
p50inputfd_LDFLAGS = -pthread
p50inputfd_LDADD = ../services/libicecc.la $(LIBZSTD_LIBS) $(XXHASH_LIBS)
p50inputlifecycle_SOURCES = p50_input_lifecycle_test.cpp
p50forkfdhygiene_SOURCES = p50_fork_fd_hygiene_test.cpp \
	../daemon/p50_fork_fd_hygiene.cpp
p50forkfdhygiene_CPPFLAGS = $(AM_CPPFLAGS) -DICECC_P50_FORK_FD_HYGIENE_TEST_HOOKS
p50inputlifecycle_CXXFLAGS = -pthread
p50inputlifecycle_LDFLAGS = -pthread
p50inputlifecycle_LDADD = ../cache/libp50inputlifecycle.a \
	../cache/libprotocol50.a ../cache/libp50localtransport.a \
	../services/libicecc.la $(LIBZSTD_LIBS) $(XXHASH_LIBS)
p50endpoint_SOURCES = p50_endpoint_test.cpp
p50zstdsender_SOURCES = p50_zstd_sender_test.cpp
p50zstdsender_CPPFLAGS = $(AM_CPPFLAGS) $(BOOST_CPPFLAGS)
p50zstdsender_CXXFLAGS = -pthread
p50zstdsender_LDFLAGS = $(BOOST_LDFLAGS) -pthread
p50zstdsender_LDADD = ../client/libp50zstdsender.a ../cache/libp50endpoint.a \
	../cache/libprotocol50.a ../services/libicecc.la $(LIBZSTD_LIBS) $(BOOST_LIBS)
p50compilebinding_SOURCES = p50_compile_binding_test.cpp
p50compilebinding_CPPFLAGS = $(AM_CPPFLAGS) $(BOOST_CPPFLAGS)
p50compilebinding_CXXFLAGS = -pthread
p50compilebinding_LDFLAGS = $(BOOST_LDFLAGS) -pthread
p50compilebinding_LDADD = ../client/libp50zstdsender.a ../cache/libp50endpoint.a \
	../cache/libprotocol50.a ../services/libicecc.la $(LIBZSTD_LIBS) $(BOOST_LIBS)
p50resultdisposition_SOURCES = p50_result_disposition_test.cpp
p50resultdisposition_CXXFLAGS = -pthread
p50resultdisposition_LDFLAGS = -pthread
p50resultdisposition_LDADD = ../services/libicecc.la
p50readyadvertisement_SOURCES = p50_ready_advertisement_test.cpp
p50daemonsidecaradapter_SOURCES = p50_daemon_sidecar_adapter_test.cpp \
	../cache/p50_daemon_cache_dispatch.cpp ../cache/p50_fd_handoff.cpp
p50daemonsidecaradapter_CPPFLAGS = $(AM_CPPFLAGS) \
	-DICECC_P50_DAEMON_SIDECAR_ADAPTER_TEST_HOOKS
p50daemonsidecaradapter_CXXFLAGS = -pthread
p50daemonsidecaradapter_LDFLAGS = -pthread
p50daemonsidecaradapter_LDADD = ../cache/libp50daemonsidecaradapter.a \
	../cache/libp50inputfd.a ../cache/libp50inputlifecycle.a \
	../cache/libprotocol50.a \
	../cache/libp50readyadvertisement.a ../cache/libp50sidecarsupervisor.a \
	../cache/libp50localtransport.a ../services/libicecc.la \
	$(LIBZSTD_LIBS) $(XXHASH_LIBS)
p50daemonsidecaradapter_DEPENDENCIES = \
	../cache/libp50daemonsidecaradapter.a ../cache/libp50inputfd.a \
	../cache/libp50inputlifecycle.a \
	../cache/libprotocol50.a ../cache/libp50readyadvertisement.a \
	../cache/libp50sidecarsupervisor.a ../cache/libp50localtransport.a \
	../services/libicecc.la ../cache/icecc-cache-service
p50daemonpositive_SOURCES = p50daemonpositive.cpp
p50daemonpositive_LDADD = ../services/libicecc.la
p50daemonpositive_DEPENDENCIES = ../daemon/iceccd ../cache/icecc-cache-service
digest128bench_SOURCES = digest128_bench.cpp
lockexec_SOURCES = lockexec.cpp
lockexec_LDFLAGS = $(BOOST_LDFLAGS) -pthread
lockexec_LDADD = ../client/libclient.a $(P50_CLIENT_LINK_LIBS)

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
daemoncachedispatch_SOURCES = daemoncachedispatch.cpp
daemoncachedispatch_LDADD = ../services/libicecc.la
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
p50_completion_record_SOURCES = p50_completion_record_test.cpp ../daemon/p50_completion_record.cpp
p50_completion_record_CPPFLAGS = $(AM_CPPFLAGS) -I$(top_srcdir)/daemon
p50_completion_record_CXXFLAGS = -pthread
p50_completion_record_LDFLAGS = -pthread
p50_completion_record_LDADD = ../services/libicecc.la

p50daemontaskcount_SOURCES = p50_daemon_task_count_test.cpp ../daemon/p50_task_count.cpp
p50daemontaskcount_CPPFLAGS = $(AM_CPPFLAGS) -I$(top_srcdir)
p50daemontaskcount_CXXFLAGS = -pthread
p50daemontaskcount_LDFLAGS = -pthread
p50assignment_CXXFLAGS = -pthread
p50assignment_LDFLAGS = -pthread
p50assignment_LDADD = ../services/libicecc.la
p50cacheadvertisement_SOURCES = p50cacheadvertisement.cpp
p50cacheadvertisement_CXXFLAGS = -pthread
p50cacheadvertisement_LDFLAGS = -pthread
p50cacheadvertisement_LDADD = ../cache/libprotocol50.a ../services/libicecc.la
p50cachesession_SOURCES = p50cachesession.cpp
p50cachesession_CXXFLAGS = -pthread
p50cachesession_LDFLAGS = -pthread
p50cachesession_LDADD = ../services/libicecc.la
p50daemoncachedispatch_SOURCES = p50_daemon_cache_dispatch_test.cpp \
	../cache/p50_daemon_cache_dispatch.cpp \
	../cache/p50_fd_handoff.cpp ../cache/p50_local_transport.cpp
p50daemoncachedispatch_CPPFLAGS = $(AM_CPPFLAGS)
p50daemoncachedispatch_CXXFLAGS = -pthread
p50daemoncachedispatch_LDFLAGS = -pthread
p50daemoncachedispatch_LDADD = ../cache/libprotocol50.a ../services/libicecc.la
p50connectionprovenance_SOURCES = p50_connection_provenance_test.cpp ../daemon/connection_provenance.cpp
p50connectionprovenance_CPPFLAGS = $(AM_CPPFLAGS) -DICECC_CONNECTION_PROVENANCE_TEST_HOOKS
p50sourcearm_SOURCES = p50_source_arm_wait_test.cpp \
	../cache/p50_source_identity.cpp ../cache/p50_input_wait.cpp
p50sourcearm_CPPFLAGS = $(AM_CPPFLAGS) -I$(top_srcdir)/cache
p50sourcearm_CXXFLAGS = -pthread
p50sourcearm_LDFLAGS = -pthread
p50sourcearm_LDADD = ../services/libicecc.la
p50sourcearmwire_SOURCES = p50_source_arm_wire_test.cpp
p50sourcearmwire_CXXFLAGS = -pthread
p50sourcearmwire_LDFLAGS = -pthread
p50sourcearmwire_LDADD = ../services/libicecc.la
p50sourcearmlease_SOURCES = p50_source_arm_wait_lease_test.cpp
p50sourcearmlease_CPPFLAGS = $(AM_CPPFLAGS) -I$(top_srcdir)/daemon -I$(top_srcdir)/cache
p50sourcearmlease_CXXFLAGS = -pthread
p50sourcearmlease_LDFLAGS = -pthread
p50sourcearmlease_LDADD = ../services/libicecc.la
p50sourcearm_live_SOURCES = p50_source_arm_live_wait_test.cpp
p50sourcearm_live_CPPFLAGS = $(AM_CPPFLAGS) -I$(top_srcdir)/cache
p50sourcearm_live_CXXFLAGS = -pthread
p50sourcearm_live_LDFLAGS = -pthread
p50sourcearm_live_LDADD = ../services/libicecc.la
p50phaseopen_SOURCES = p50_phase_open_test.cpp
p50phaseopen_CPPFLAGS = $(AM_CPPFLAGS)
p50phaseopen_CXXFLAGS = -pthread
p50phaseopen_LDFLAGS = -pthread
p50phaseopen_LDADD = ../cache/libprotocol50.a ../services/libicecc.la
p50reversefdretry_SOURCES = p50_reverse_fd_retry_test.cpp
p50reversefdretry_CPPFLAGS = $(AM_CPPFLAGS)
p50reversefdretry_CXXFLAGS = -pthread
p50reversefdretry_LDFLAGS = -pthread
p50reversefdretry_LDADD = ../cache/libp50localtransport.a ../cache/libprotocol50.a \
	../services/libicecc.la
p50s2restartreplay_SOURCES = p50_s2_restart_replay_test.cpp
p50s2restartreplay_CPPFLAGS = $(AM_CPPFLAGS)
p50s2restartreplay_CXXFLAGS = -pthread
p50s2restartreplay_LDFLAGS = -pthread
p50s2restartreplay_LDADD = ../cache/libp50sidecarsupervisor.a \
	../cache/libp50localtransport.a ../cache/libprotocol50.a \
	../services/libicecc.la
p50cacheservice_SOURCES = p50cacheservice.cpp ../cache/p50_cache_service.cpp
p50cacheservice_CPPFLAGS = $(AM_CPPFLAGS) $(BOOST_CPPFLAGS) -DICECC_P50_CACHE_SERVICE_NO_MAIN
p50cacheservice_CXXFLAGS = -pthread
p50cacheservice_LDFLAGS = $(BOOST_LDFLAGS) -pthread
p50cacheservice_LDADD = ../cache/libp50endpoint.a ../cache/libp50inputfd.a \
	../cache/libp50inputlifecycle.a \
	../cache/libprotocol50.a ../cache/libp50localtransport.a \
	../services/libicecc.la $(LIBZSTD_LIBS) $(XXHASH_LIBS) $(BOOST_LIBS)
p50cacheservice_DEPENDENCIES = ../cache/icecc-cache-service \
	../cache/libp50inputfd.a ../cache/libp50inputlifecycle.a readyclose_shim.so
p50fdhandoff_SOURCES = p50fdhandoff.cpp ../cache/p50_fd_handoff.cpp \
	../cache/p50_local_transport.cpp
p50fdhandoff_CPPFLAGS = $(AM_CPPFLAGS) -DICECC_P50_FD_HANDOFF_TEST_HOOKS
p50fdhandoff_CXXFLAGS = -pthread
p50fdhandoff_LDFLAGS = -pthread
inputpump_SOURCES = input_pump_test.cpp
inputpump_CXXFLAGS = -pthread
inputpump_LDFLAGS = $(BOOST_LDFLAGS) -pthread
inputpump_LDADD = ../client/libclient.a $(P50_CLIENT_LINK_LIBS)
compilerinput_SOURCES = compiler_input_test.cpp ../daemon/compiler_input.cpp
compilerinput_CPPFLAGS = $(AM_CPPFLAGS) -I$(top_srcdir)/daemon
compilerinput_CXXFLAGS = -pthread
compilerinput_LDFLAGS = -pthread
compilerinput_LDADD = ../services/libicecc.la
schedbp_CXXFLAGS = -pthread
schedbp_LDFLAGS = -pthread
schedbp_LDADD = ../services/libicecc.la

check-local: sndbuf_shim.so conn_shim.so readyclose_shim.so
sndbuf_shim.so: $(srcdir)/sndbuf_shim.c
	$(CC) -shared -fPIC $(srcdir)/sndbuf_shim.c -ldl -o $@
conn_shim.so: $(srcdir)/conn_shim.c
	$(CC) -shared -fPIC $(srcdir)/conn_shim.c -ldl -o $@

readyclose_shim.so: $(srcdir)/readyclose_shim.c
	$(CC) -shared -fPIC $(srcdir)/readyclose_shim.c -ldl -o $@

connectivity_DEPENDENCIES = conn_shim.so

CLEANFILES = sndbuf_shim.so conn_shim.so readyclose_shim.so schedbp-scheduler-*.log
EXTRA_DIST = sndbuf_shim.c conn_shim.c readyclose_shim.c connectivity-run.sh schedbp-quick.sh schedstress-quick.sh remoteice-quick.sh p50assignment-remote.sh daemonlogin-run.sh daemonbatch-run.sh cachehandoffdaemon-run.sh daemoncachedispatch-run.sh p49daemon-run.sh p49scheduler-run.sh boost-prefix-scope.sh

EXTRA_DIST += p50_trace_check.sh p50assignment-source.sh p50cacheadvertisement-source.sh p50cachesession-source.sh p50daemoncachedispatch-source.sh p50daemoncachedispatch-sanitize.sh p50localtransport-source.sh p50localtransport-sanitize.sh p50sidecarsupervisor-source.sh p50inputattachment-source.sh p50inputattachment-mutants.sh p50_input_attachment_sanitize.sh p50readyadvertisement-source.sh p50readyadvertisement-mutants.sh p50endpoint-source.sh p50fdhandoff-source.sh p50cacheservice-source.sh p50cacheservice-sanitize.sh p50fdhandoff-sanitize.sh p50inputfd-source.sh p50inputfd-sanitize.sh p50inputlifecycle-source.sh p50inputlifecycle-sanitize.sh packagebuilderdeps.sh p50daemonpositive-source.sh p50daemonpositive-run.sh p50zstdsender-source.sh
EXTRA_DIST += p50sourcearm-source.sh p50sourcearm-wait-mutants.sh p50phaseopen-source.sh
EXTRA_DIST += p50sourcearmwire-source.sh p50storeidentitywire-mutants.sh
EXTRA_DIST += p50storeidentity-semantic.sh \
	p50_store_identity_validator_semantic_test.cpp
EXTRA_DIST += p50sourcearm-live-source.sh p50sourcearm-live-run.sh
EXTRA_DIST += p50sourceingress-source.sh p50sourceingress-sanitize.sh
EXTRA_DIST += p50reversefdretry-source.sh p50reversefdretry-mutants.sh \
	p50_reverse_fd_retry_sanitize.sh
EXTRA_DIST += p50s2restartreplay-source.sh p50_s2_restart_replay_sanitize.sh
EXTRA_DIST += p50sidecarsupervisor-sanitize.sh p50sidecarlifecycle-source.sh \
	p50sidecarlifecycle-mutants.sh p50sidecarlifecycle-sanitize.sh
EXTRA_DIST += p50daemonsidecaradapter-source.sh p50daemonsidecaradapter-sanitize.sh
EXTRA_DIST += p50profile-source.sh p50_profile_sanitize.sh
EXTRA_DIST += p50compilee2e-source.sh p50compilee2e-run.sh
EXTRA_DIST += p50completionflow-source.py
EXTRA_DIST += p50completionrecord-sanitize.sh
EXTRA_DIST += p50completionflow-run.sh
EXTRA_DIST += p50tworole-source.sh
EXTRA_DIST += p50daemontaskcount-source.sh
EXTRA_DIST += p50forkfdhygiene-source.sh p50forkfdhygiene-mutants.sh \
	p50_fork_fd_hygiene_sanitize.sh p50forkfdhygiene-oot.sh \
	p50forkfdhygiene-package.sh
EXTRA_DIST += p50cachesessionjoin-source.sh p50cachesessionjoin-mutants.sh \
	p50cachesessionjoin-sanitize.sh
EXTRA_DIST += p50cachesessionwire-source.sh p50cachesessionwire-mutants.sh \
	p50cachesessionwire-sanitize.sh
EXTRA_DIST += p50daemoncontrol-source.sh p50daemoncontrol-mutants.sh \
	p50_daemon_control_sanitize.sh

# Make the tests also print the test log if they fail.
check: export VERBOSE=1
