sbin_PROGRAMS = iceccd

iceccd_SOURCES = \
	main.cpp \
	serve.cpp \
	workit.cpp \
	environment.cpp \
	load.cpp \
	file_util.cpp

# -pthread at compile AND link: the state-JSONL writer runs on a
# std::thread, and glibc before 2.34 keeps pthread_create in libpthread --
# without the flag the daemon links on modern glibc but not on older build
# roots (Fedora 28).
iceccd_CXXFLAGS = $(AM_CXXFLAGS) -pthread
iceccd_LDFLAGS = -pthread

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
	environment.h \
	load.h \
	serve.h \
	workit.h \
	file_util.h
