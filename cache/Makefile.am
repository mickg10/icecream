noinst_LIBRARIES = libprotocol50.a libp50endpoint.a libp50localtransport.a \
	libp50sidecar.a libp50daemonsidecaradapter.a libp50input.a \
	libp50adoptedoutcomewriter.a
check_LIBRARIES = libp50endpointtesthooks.a

sbin_PROGRAMS = icecc-cache-service
check_PROGRAMS = icecc-cache-service-test

icecc_cache_service_SOURCES = p50_cache_service.cpp p50_cache_service.h \
	../client/p50_route_owner.cpp ../client/p50_route_owner.h \
	../client/p50_zstd_sender.cpp ../client/p50_zstd_sender.h
icecc_cache_service_LDADD = libp50endpoint.a libp50adoptedoutcomewriter.a \
	libp50input.a \
	libprotocol50.a \
	libp50localtransport.a ../services/libicecc.la \
	$(LIBZSTD_LIBS) $(XXHASH_LIBS) $(BOOST_LIBS)
icecc_cache_service_CPPFLAGS = $(AM_CPPFLAGS) $(BOOST_CPPFLAGS)
icecc_cache_service_CXXFLAGS = -O2 -Werror -pthread
icecc_cache_service_LDFLAGS = $(BOOST_LDFLAGS) -pthread

# The four-link capacity gate needs deterministic settlement/ARM hooks. Keep
# this separate from the installed production service and compile distinct
# target-prefixed objects so its test macro never contaminates product code.
icecc_cache_service_test_SOURCES = $(icecc_cache_service_SOURCES)
icecc_cache_service_test_LDADD = libp50endpointtesthooks.a \
	$(icecc_cache_service_LDADD)
icecc_cache_service_test_CPPFLAGS = $(AM_CPPFLAGS) $(BOOST_CPPFLAGS) \
	-DICECC_P50_ENDPOINT_TEST_HOOKS
icecc_cache_service_test_CXXFLAGS = -O2 -Werror -pthread
icecc_cache_service_test_LDFLAGS = $(BOOST_LDFLAGS) -pthread

libprotocol50_a_SOURCES = \
	protocol50.cpp \
	protocol50.h \
	s3_persistence.cpp \
	s3_persistence.h \
	p50_actions.cpp \
	p50_actions.h \
	p50_slice0.cpp \
	p50_slice0.h \
	p50_profile.cpp \
	p50_profile.h \
	p50_zstd.cpp \
	p50_zstd.h \
	p50_input_record.cpp \
	p50_input_record.h \
	p50_source_identity.cpp \
	p50_source_identity.h \
	p50_phase_open.cpp \
	p50_phase_open.h \
	p50_input_wait.cpp \
	p50_input_wait.h \
	p50_incarnation_identity.h \
	p50_fsession_control.cpp \
	p50_fsession_control.h \
	p50_fsession_route.cpp \
	p50_fsession_route.h \
	p50_fsession_sidecar_op.cpp \
	p50_fsession_sidecar_op.h \
	p50_fsession_daemon_op.cpp \
	p50_fsession_daemon_op.h \
	p50_fsession_service_owner.cpp \
	p50_fsession_service_owner.h \
	p50_fsession_payloads.cpp \
	p50_fsession_payloads.h

AM_CPPFLAGS = -I$(top_srcdir) $(LIBZSTD_CFLAGS)
libp50endpoint_a_SOURCES = p50_endpoint.cpp p50_endpoint.h \
	p50_endpoint_run_cancel.cpp p50_endpoint_run_cancel.h
libp50endpoint_a_CPPFLAGS = $(AM_CPPFLAGS) $(BOOST_CPPFLAGS)

# Unit-only compatibility object for the pre-existing endpoint behavioral
# tests.  Product libraries remain macro-free; the hook object is linked only
# by the legacy test binary and cannot provide a product cancellation path.
libp50endpointtesthooks_a_SOURCES = p50_endpoint.cpp p50_endpoint.h
libp50endpointtesthooks_a_CPPFLAGS = $(AM_CPPFLAGS) $(BOOST_CPPFLAGS) \
	-DICECC_P50_ENDPOINT_TEST_HOOKS

libp50localtransport_a_SOURCES = p50_local_transport.cpp p50_local_transport.h \
	p50_fd_handoff.cpp p50_fd_handoff.h \
	p50_daemon_control.cpp p50_daemon_control.h \
	p50_control_operation.cpp p50_control_operation.h
libp50input_a_SOURCES = p50_input_fd_attachment.cpp p50_input_fd_attachment.h \
	p50_input_lifecycle.cpp p50_input_lifecycle.h
libp50input_a_CPPFLAGS = $(AM_CPPFLAGS)
libp50sidecar_a_SOURCES = p50_sidecar_identity.cpp p50_sidecar_identity.h \
	p50_sidecar_lifecycle.cpp p50_sidecar_lifecycle.h \
	p50_ready_advertisement.cpp p50_ready_advertisement.h
libp50sidecar_a_CPPFLAGS = $(AM_CPPFLAGS)
libp50daemonsidecaradapter_a_SOURCES = p50_daemon_sidecar_adapter.cpp \
	p50_daemon_sidecar_adapter.h
libp50daemonsidecaradapter_a_CPPFLAGS = $(AM_CPPFLAGS)
libp50adoptedoutcomewriter_a_SOURCES = p50_adopted_outcome_writer.cpp \
	p50_adopted_outcome_writer.h
libp50adoptedoutcomewriter_a_CPPFLAGS = $(AM_CPPFLAGS)

EXTRA_DIST = \
	codec/FORMAT.md \
	codec/P29V1_FORMAT.md \
	codec/ZSTD_FORMATS.md \
	codec/README.md \
	codec/p29_intern.h \
	codec/p29_online_s1.h \
	codec/p29_wire.h \
	codec/provider.h \
	codec/tuples.h \
	p50_control_operation.h \
	p50_daemon_control.h \
	p50_daemon_control.cpp \
	README.md \
	P50_PROTOCOL.md \
	KEY_LAYOUT_V1_CENSUS.md \
	key_layout_census.py \
	formal/Protocol50.tla \
	formal/Protocol50.cfg \
	formal/Protocol50AbortMutant.cfg \
	formal/Protocol50RelSeqMutant.cfg \
	formal/Protocol50OperationDigestMutant.cfg \
	formal/Protocol50JobLifecycle.tla \
	formal/Protocol50JobLifecycle.cfg \
	formal/Protocol50JobRestartProgress.cfg \
	formal/Protocol50JobLeaseMutant.cfg \
	formal/Protocol50JobCancelLeaseMutant.cfg \
	formal/Protocol50JobOwnershipMutant.cfg \
	formal/Protocol50JobLifecycle.md \
	formal/Protocol50Reconnect.tla \
	formal/Protocol50Reconnect.cfg \
	formal/Protocol50ReconnectSameGuidMutant.cfg \
	formal/Protocol50ReconnectActiveResetMutant.cfg \
	formal/Protocol50ReconnectRepeatResetMutant.cfg \
	formal/Protocol50MultiRoute.tla \
	formal/Protocol50MultiRoute.cfg \
	formal/Protocol50MultiRouteMoveMutant.cfg \
	formal/Protocol50MultiRouteResultMutant.cfg \
	formal/Protocol50IncarnationBridge.tla \
	formal/Protocol50IncarnationBridge.cfg \
	formal/Protocol50IncarnationProgress.cfg \
	formal/Protocol50IncarnationMutant.cfg \
	formal/Protocol50IncarnationOwnershipMutant.cfg \
	formal/Protocol50Assignment.tla \
	formal/Protocol50Assignment.cfg \
	formal/Protocol50AssignmentRestart.cfg \
	formal/Protocol50AssignmentMixedCompat.cfg \
	formal/Protocol50AssignmentStrictLegacyMutant.cfg \
	formal/Protocol50AssignmentReadyMutant.cfg \
	formal/Protocol50AssignmentCancelledPublishMutant.cfg \
	formal/Protocol50AssignmentReleaseMutant.cfg \
	formal/Protocol50AssignmentTombstoneMutant.cfg \
	formal/Protocol50AssignmentEpochReuseMutant.cfg \
	formal/Protocol50Assignment.md \
	formal/Protocol50AssignmentOrdering.tla \
	formal/Protocol50AssignmentOrdering.cfg \
	formal/Protocol50AssignmentOrderingEarlyCancelWitness.cfg \
	formal/Protocol50AssignmentOrderingAdvisoryWitness.cfg \
	formal/Protocol50AssignmentOrderingPrepareMutant.cfg \
	formal/Protocol50AssignmentOrderingReadyMutant.cfg \
	formal/Protocol50AssignmentOrderingRevokeMutant.cfg \
	formal/Protocol50AssignmentDelivery.tla \
	formal/Protocol50AssignmentDelivery.cfg \
	formal/Protocol50AssignmentReadyWireMutant.cfg \
	formal/Protocol50AssignmentRevokeRequestWireMutant.cfg \
	formal/Protocol50AssignmentRevokeResultWireMutant.cfg \
	formal/Protocol50AssignmentLatePrepareMutant.cfg \
	formal/Protocol50AssignmentRevokeClaimCrossingWitness.cfg \
	formal/Protocol50AssignmentStaleRevokeResultWitness.cfg \
	formal/Protocol50AssignmentDelivery.md \
	formal/run_assignment_delivery_tlc.sh \
	formal/Protocol50AssignmentIdentity.tla \
	formal/Protocol50AssignmentIdentity.cfg \
	formal/Protocol50AssignmentIdentityCell43_43_43.cfg \
	formal/Protocol50AssignmentIdentityCell43_43_50.cfg \
	formal/Protocol50AssignmentIdentityCell43_50_43.cfg \
	formal/Protocol50AssignmentIdentityCell43_50_50.cfg \
	formal/Protocol50AssignmentIdentityCell50_43_43.cfg \
	formal/Protocol50AssignmentIdentityCell50_43_50.cfg \
	formal/Protocol50AssignmentIdentityCell50_50_43.cfg \
	formal/Protocol50AssignmentIdentityCell50_50_50.cfg \
	formal/Protocol50AssignmentIdentityStrictWitness.cfg \
	formal/Protocol50AssignmentIdentityStrictMixedWitness.cfg \
	formal/Protocol50AssignmentIdentityLocalWitness.cfg \
	formal/Protocol50AssignmentIdentityReconnectWitness.cfg \
	formal/Protocol50AssignmentIdentityRevokedWitness.cfg \
	formal/Protocol50AssignmentIdentitySettlementWitness.cfg \
	formal/Protocol50AssignmentIdentityRejectStaleEpochWitness.cfg \
	formal/Protocol50AssignmentIdentityRejectStaleWireWitness.cfg \
	formal/Protocol50AssignmentIdentityRejectStaleNonceWitness.cfg \
	formal/Protocol50AssignmentIdentityRejectEpochOnlyWitness.cfg \
	formal/Protocol50AssignmentIdentityRejectNonceOnlyWitness.cfg \
	formal/Protocol50AssignmentIdentityStaleEpochMutant.cfg \
	formal/Protocol50AssignmentIdentityStaleWireMutant.cfg \
	formal/Protocol50AssignmentIdentityStaleNonceMutant.cfg \
	formal/Protocol50AssignmentIdentityEpochOnlyMutant.cfg \
	formal/Protocol50AssignmentIdentityNonceOnlyMutant.cfg \
	formal/Protocol50AssignmentIdentityDropMutant.cfg \
	formal/Protocol50AssignmentIdentityStrictAbsentMutant.cfg \
	formal/Protocol50AssignmentIdentityReconnectDropMutant.cfg \
	formal/Protocol50AssignmentIdentityReleaseClaimedMutant.cfg \
	formal/Protocol50AssignmentIdentity.md \
	formal/run_assignment_identity_tlc.sh \
	formal/run_tlc.sh \
	formal/.gitignore \
	formal/check_trace.py \
	formal/Protocol50Global.tla \
	formal/Protocol50Global.cfg \
	formal/Protocol50GlobalAggregateCapMutant.cfg \
	formal/Protocol50GlobalNamespaceCapMutant.cfg \
	formal/Protocol50GlobalSlotMutant.cfg \
	formal/Protocol50GlobalLruMutant.cfg \
	formal/Protocol50GlobalGenerationWrapMutant.cfg \
	formal/Protocol50GlobalAdmissionMutant.cfg \
	formal/Protocol50GlobalConflictMutant.cfg \
	formal/Protocol50GlobalCrashMutant.cfg \
	formal/Protocol50GlobalReverseSlotMutant.cfg \
	formal/Protocol50GlobalContentMutant.cfg \
	formal/Protocol50GlobalStallMutant.cfg \
	formal/Protocol50GlobalLocalLruMutant.cfg \
	formal/Protocol50GlobalLifecycleResetMutant.cfg \
	formal/Protocol50GlobalCrossGuidMutant.cfg \
	formal/Protocol50GlobalStagingCapMutant.cfg \
	formal/Protocol50GlobalTotalCapMutant.cfg \
	formal/Protocol50GlobalTrace.md \
	formal/check_global_trace.py \
	formal/global-trace.jsonl \
	formal/global-fresh-cycle.jsonl \
	formal/global-admit-at-terminal-generation.jsonl \
	formal/run_global_trace_gate.sh \
	formal/run_global_tlc.sh \
	formal/run_global_formal_gate.sh \
	p50_daemon_cache_dispatch.h \
	p50_adopted_outcome_writer.cpp \
	p50_adopted_outcome_writer.h \
	formal/README.md

# Historical GRZ/libbsc generators stay under research/ in Git, not the product
# source distribution. The codec tuple API and frozen test vectors stay above
# and in unittests; none of them builds or links a GRZ/libbsc implementation.

# The historical explicit list above remains compatible, while this reviewed
# manifest closes omissions as new formal lanes and mutant configs are added.
P50_FORMAL_DIST_MANIFEST = $(srcdir)/formal/DISTFILES
P50_FORMAL_DIST_FILES = $(shell sed -e '/^[[:space:]]*$$/d' "$(P50_FORMAL_DIST_MANIFEST)")
EXTRA_DIST += formal/DISTFILES $(P50_FORMAL_DIST_FILES)

check-local:
	sh "$(top_srcdir)/dev/python.sh" "$(srcdir)/key_layout_census.py" \
		--root $(top_srcdir) --check $(srcdir)/KEY_LAYOUT_V1_CENSUS.md
