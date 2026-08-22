sbin_PROGRAMS = iceccd

iceccd_SOURCES = \
	main.cpp \
	serve.cpp \
	compiler_input.cpp \
	workit.cpp \
	environment.cpp \
	load.cpp \
	file_util.cpp \
	statewriter.cpp

iceccd_LDADD = \
	../services/libicecc.la \
	$(LIB_KINFO) \
	$(LIBCAP_NG_LIBS) \
	$(LIBARCHIVE_LIBS)

AM_CPPFLAGS = \
	-I$(top_srcdir)/services \
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
