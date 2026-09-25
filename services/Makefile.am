lib_LTLIBRARIES = libicecc.la
AM_CPPFLAGS = $(XXHASH_CFLAGS)
libicecc_la_SOURCES = job.cpp comm.cpp p50_cache_session_wire.cpp exitcode.cpp getifaddrs.cpp logging.cpp ncpus.c pipes.cpp tempfile.c platform.cpp gcc.cpp util.cpp digest128.cpp
libicecc_la_LIBADD = \
	$(LZO_LDADD) \
	$(LIBZSTD_LIBS) \
	$(LIBCAP_NG_LIBS) \
	$(XXHASH_LIBS) \
	$(DL_LDADD)

libicecc_la_CFLAGS = -fPIC -DPIC
libicecc_la_CXXFLAGS = -fPIC -DPIC

icedir = $(includedir)/icecc
ice_HEADERS = \
	job.h \
	comm.h \
	p50_cache_profile_mask.h \
	p50_store_identity_wire.h \
	logging.h

noinst_HEADERS = \
	p50_cache_session_wire.h \
	exitcode.h \
	getifaddrs.h \
	logging.h \
	digest128.h \
	ncpus.h \
	pipes.h \
	tempfile.h \
	platform.h \
	utf8.h \
	util.h

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = icecc.pc

AM_LIBTOOLFLAGS = --silent
