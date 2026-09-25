sbin_PROGRAMS = iceccd

iceccd_SOURCES = \
	main.cpp \
	connection_provenance.cpp \
	serve.cpp \
	p50_fork_fd_hygiene.cpp \
	p50_task_count.cpp \
	p50_completion_record.cpp \
	compiler_input.cpp \
	workit.cpp \
	../cache/p50_daemon_cache_dispatch.cpp \
	environment.cpp \
	load.cpp \
	file_util.cpp \
	statewriter.cpp

iceccd_LDADD = \
	../cache/libp50daemonsidecaradapter.a \
	../cache/libp50sidecar.a \
	../cache/libp50input.a \
	../cache/libprotocol50.a \
	../cache/libp50localtransport.a \
	../services/libicecc.la \
	$(LIBZSTD_LIBS) \
	$(XXHASH_LIBS) \
	$(LIB_KINFO) \
	$(LIBCAP_NG_LIBS) \
	$(LIBARCHIVE_LIBS)

AM_CPPFLAGS = \
	-I$(top_srcdir) \
	-I$(top_srcdir)/services \
	-I$(top_srcdir)/cache \
	$(LIBARCHIVE_CFLAGS)

AM_LIBTOOLFLAGS = --silent
EXTRA_DIST = P50_FORK_FD_HYGIENE.md

noinst_HEADERS = \
	client_queue_order.h \
	poll_readiness.h \
	compiler_group_signal.h \
	compiler_input.h \
	p50_task_count.h \
	p50_fork_fd_hygiene.h \
	p50_completion_record.h \
	p50_cache_recovery_policy.h \
	statewriter.h \
	environment.h \
	load.h \
	serve.h \
	workit.h \
	file_util.h \
	connection_provenance.h \
	p50_source_arm_wait_lease.h
