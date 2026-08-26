sbin_PROGRAMS = iceccd

iceccd_SOURCES = \
	main.cpp \
	connection_provenance.cpp \
	serve.cpp \
	p50_task_count.cpp \
	p50_completion_record.cpp \
	compiler_input.cpp \
	workit.cpp \
	../cache/p50_daemon_cache_dispatch.cpp \
	../cache/p50_source_identity.cpp \
	../cache/p50_phase_open.cpp \
	../cache/p50_input_wait.cpp \
	../cache/p50_reverse_fd_retry.cpp \
	environment.cpp \
	load.cpp \
	file_util.cpp \
	statewriter.cpp

iceccd_LDADD = \
	../cache/libp50daemonsidecaradapter.a \
	../cache/libp50inputfd.a \
	../cache/libp50inputlifecycle.a \
	../cache/libp50readyadvertisement.a \
	../cache/libp50sidecarsupervisor.a \
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

noinst_HEADERS = \
	compiler_input.h \
	p50_task_count.h \
	p50_completion_record.h \
	statewriter.h \
	environment.h \
	load.h \
	serve.h \
	workit.h \
	file_util.h \
	connection_provenance.h
