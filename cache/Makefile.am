noinst_LIBRARIES = libprotocol50.a libp50endpoint.a

libprotocol50_a_SOURCES = \
	protocol50.cpp \
	protocol50.h \
	p50_actions.cpp \
	p50_actions.h \
	p50_slice0.cpp \
	p50_slice0.h \
	p50_zstd.cpp \
	p50_zstd.h \
	p50_input_record.cpp \
	p50_input_record.h

AM_CPPFLAGS = -I$(top_srcdir) $(LIBZSTD_CFLAGS)
libp50endpoint_a_SOURCES = p50_endpoint.cpp p50_endpoint.h
libp50endpoint_a_CPPFLAGS = $(AM_CPPFLAGS) $(BOOST_CPPFLAGS)

EXTRA_DIST = \
	PROFILE-REGISTRY.md \
	registry-dist-gate.sh \
	README.md \
	P48_P50_CONVERGENCE.md \
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
	formal/README.md

check-local:
	python3 $(srcdir)/key_layout_census.py \
		--root $(top_srcdir) --check $(srcdir)/KEY_LAYOUT_V1_CENSUS.md
