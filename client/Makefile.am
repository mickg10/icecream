bin_PROGRAMS = icecc
bin_SCRIPTS = icecc-create-env icecc-test-env

# The completion-flow integration gate needs to force one real Accepted-send
# failure after all output bytes arrive.  Keep that fault injector out of the
# shipped wrapper: this check-only binary recompiles remote.cpp under the
# dedicated test macro and otherwise links the ordinary client library.
check_PROGRAMS = icecc-p50-completion-test

noinst_LIBRARIES = libclient.a libp50zstdsender.a
libclient_a_SOURCES = \
        arg.cpp \
        argv.c \
        cpp.cpp \
        invocation_timing.cpp \
	input_pump.cpp \
        local.cpp \
        remote.cpp \
        util.cpp \
        md5.c \
        safeguard.cpp

icecc_SOURCES = \
	main.cpp 
icecc_LDADD = \
	libclient.a \
	../cache/libp50localtransport.a \
	libp50zstdsender.a \
	../cache/libp50endpoint.a \
	../cache/libp50adoptedoutcomewriter.a \
	../cache/libprotocol50.a \
	../services/libicecc.la \
	$(LIBZSTD_LIBS) $(XXHASH_LIBS) $(BOOST_LIBS)
icecc_CPPFLAGS = $(AM_CPPFLAGS) $(BOOST_CPPFLAGS)
icecc_CXXFLAGS = -pthread
icecc_LDFLAGS = $(BOOST_LDFLAGS) -pthread

icecc_p50_completion_test_SOURCES = main.cpp remote.cpp
icecc_p50_completion_test_CPPFLAGS = $(AM_CPPFLAGS) $(BOOST_CPPFLAGS) \
	-DICECC_P50_COMPLETION_TEST_HOOKS
icecc_p50_completion_test_CXXFLAGS = -pthread
icecc_p50_completion_test_LDFLAGS = $(BOOST_LDFLAGS) -pthread
icecc_p50_completion_test_LDADD = \
	libclient.a \
	../cache/libp50localtransport.a \
	libp50zstdsender.a \
	../cache/libp50endpoint.a \
	../cache/libp50adoptedoutcomewriter.a \
	../cache/libprotocol50.a \
	../services/libicecc.la \
	$(LIBZSTD_LIBS) $(XXHASH_LIBS) $(BOOST_LIBS)

noinst_HEADERS = \
	argv.h \
	client.h \
	input_pump.h \
	md5.h \
	p50_remote_diagnostics.h \
	util.h
AM_CPPFLAGS = \
	-DPLIBDIR=\"$(pkglibexecdir)\" \
	-I$(top_srcdir)/services \
	-I$(top_srcdir)/
libclient_a_CPPFLAGS = $(AM_CPPFLAGS) $(BOOST_CPPFLAGS)

libp50zstdsender_a_SOURCES = p50_zstd_sender.cpp p50_zstd_sender.h \
	p50_route_owner.cpp p50_route_owner.h \
	p50_compile_binding.cpp p50_compile_binding.h
libp50zstdsender_a_CPPFLAGS = $(AM_CPPFLAGS) $(BOOST_CPPFLAGS)

EXTRA_DIST = $(bin_SCRIPTS) P50_ZSTD_SENDER.md

AM_LIBTOOLFLAGS = --silent

install-exec-local:
	$(mkinstalldirs) $(DESTDIR)$(bindir)
	rm -f $(DESTDIR)$(bindir)/icerun
	$(LN_S) $(bindir)/icecc $(DESTDIR)$(bindir)/icerun
	$(mkinstalldirs) $(DESTDIR)$(pkglibexecdir)
	rm -f $(DESTDIR)$(pkglibexecdir)/icecc-create-env
	$(LN_S) $(bindir)/icecc-create-env $(DESTDIR)$(pkglibexecdir)/icecc-create-env

	$(mkinstalldirs) $(DESTDIR)$(pkglibexecdir)/bin
	for link in g++ gcc c++ cc $(CLANG_SYMLINK_WRAPPERS); do \
		rm -f $(DESTDIR)$(pkglibexecdir)/bin/$$link ;\
		$(LN_S) $(bindir)/icecc $(DESTDIR)$(pkglibexecdir)/bin/$$link ;\
	done

uninstall-local:
	rm $(DESTDIR)$(bindir)/icerun
	rm $(DESTDIR)$(pkglibexecdir)/icecc-create-env

	for link in g++ gcc c++ cc $(CLANG_SYMLINK_WRAPPERS); do \
		rm $(DESTDIR)$(pkglibexecdir)/bin/$$link ;\
	done
