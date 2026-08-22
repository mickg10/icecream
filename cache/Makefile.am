noinst_LIBRARIES = libprotocol50.a

libprotocol50_a_SOURCES = \
	protocol50.cpp \
	protocol50.h \
	p50_actions.cpp \
	p50_actions.h \
	p50_slice0.cpp \
	p50_slice0.h \
	p50_zstd.cpp \
	p50_zstd.h \
	p50_loopback.cpp \
	p50_loopback.h

AM_CPPFLAGS = -I$(top_srcdir) $(BOOST_CPPFLAGS) $(LIBZSTD_CFLAGS)

EXTRA_DIST = \
	README.md \
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
	formal/run_tlc.sh \
	formal/.gitignore \
	formal/check_trace.py \
	formal/README.md

check-local:
	python3 $(srcdir)/key_layout_census.py \
		--root $(top_srcdir) --check $(srcdir)/KEY_LAYOUT_V1_CENSUS.md
