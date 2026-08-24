sbin_PROGRAMS = iceccd

iceccd_SOURCES = \
	main.cpp \
	serve.cpp \
	compiler_input.cpp \
	workit.cpp \
	../cache/p50_daemon_cache_dispatch.cpp \
	environment.cpp \
	load.cpp \
	file_util.cpp \
	statewriter.cpp

iceccd_LDADD = \
	../cache/libp50daemonsidecaradapter.a \
	../cache/libp50readyadvertisement.a \
	../cache/libp50sidecarsupervisor.a \
	../cache/libp50localtransport.a \
	../services/libicecc.la \
	$(LIB_KINFO) \
	$(LIBCAP_NG_LIBS) \
	$(LIBARCHIVE_LIBS)

AM_CPPFLAGS = \
	-I$(top_srcdir)/services \
	-I$(top_srcdir)/cache \
	$(LIBARCHIVE_CFLAGS)

AM_LIBTOOLFLAGS = --silent

noinst_HEADERS = \
	compiler_input.h \
	statewriter.h \
	environment.h \
	load.h \
	serve.h \
	workit.h \
	file_util.h
