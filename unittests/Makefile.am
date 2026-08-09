TESTS = testargs lockexec backpressure schedpolicy connectivityprobe connectivityprobe-negative.sh relisten-quick.sh relogin relogin-negative.sh legacyannouncement legacyannouncement-negative.sh siblingpin siblingpin-negative.sh statsapply statsapply-negative.sh utf8test statewritertest schedbp-quick.sh schedstress-quick.sh remoteice-quick.sh

AM_CPPFLAGS = -I$(top_srcdir)/client -I$(top_srcdir)/services -I$(top_srcdir)/
testargs_LDADD = ../client/libclient.a ../services/libicecc.la

check_PROGRAMS = testargs lockexec backpressure schedpolicy connectivityprobe relisten relogin relogin_mutant legacyannouncement legacyannouncement_mutant siblingpin siblingpin_mutant statsapply statsapply_mutant utf8test statewritertest schedbp
testargs_SOURCES = args.cpp
lockexec_SOURCES = lockexec.cpp
lockexec_LDADD = ../client/libclient.a ../services/libicecc.la

schedpolicy_SOURCES = schedpolicy.cpp

connectivityprobe_SOURCES = connectivityprobetest.cpp ../scheduler/connectivityprobe.cpp

relisten_SOURCES = relisten.cpp
relisten_LDADD = ../services/libicecc.la

relogin_SOURCES = relogintest.cpp \
    ../scheduler/relogin.cpp \
    ../scheduler/compileserver.cpp \
    ../scheduler/connectivityprobe.cpp \
    ../scheduler/job.cpp \
    ../scheduler/jobstat.cpp
relogin_LDADD = ../services/libicecc.la

relogin_mutant_SOURCES = $(relogin_SOURCES)
relogin_mutant_CPPFLAGS = $(AM_CPPFLAGS) -DICECC_TEST_RELOGIN_MUTANT_FALSE_SUCCESS
relogin_mutant_LDADD = ../services/libicecc.la

legacyannouncement_SOURCES = legacyannouncementtest.cpp \
    ../services/legacyannouncement.cpp

legacyannouncement_mutant_SOURCES = $(legacyannouncement_SOURCES)
legacyannouncement_mutant_CPPFLAGS = $(AM_CPPFLAGS) \
    -DICECC_TEST_LEGACY_ANNOUNCEMENT_MUTANT_OFF_BY_ONE

siblingpin_SOURCES = siblingpintest.cpp \
    ../scheduler/siblingpin.cpp \
    ../scheduler/job.cpp \
    ../scheduler/compileserver.cpp \
    ../scheduler/connectivityprobe.cpp \
    ../scheduler/jobstat.cpp
siblingpin_LDADD = ../services/libicecc.la

siblingpin_mutant_SOURCES = $(siblingpin_SOURCES)
siblingpin_mutant_CPPFLAGS = $(AM_CPPFLAGS) -DICECC_TEST_SIBLING_PIN_MUTANT_EXACT_WORKER
siblingpin_mutant_LDADD = ../services/libicecc.la

statsapply_SOURCES = statsapplytest.cpp \
    ../scheduler/compileserver.cpp \
    ../scheduler/connectivityprobe.cpp \
    ../scheduler/job.cpp \
    ../scheduler/jobstat.cpp
statsapply_LDADD = ../services/libicecc.la

statsapply_mutant_SOURCES = $(statsapply_SOURCES)
statsapply_mutant_CPPFLAGS = $(AM_CPPFLAGS) -DICECC_TEST_STATS_MUTANT_CLIENT_COUNT
statsapply_mutant_LDADD = ../services/libicecc.la

backpressure_SOURCES = backpressure.cpp

utf8test_SOURCES = utf8test.cpp

statewritertest_SOURCES = statewritertest.cpp ../daemon/statewriter.cpp
statewritertest_CPPFLAGS = $(AM_CPPFLAGS) -I$(top_srcdir)/daemon
# statewriter.cpp now logs its one-shot death notice via the shared logger
statewritertest_LDADD = ../services/libicecc.la
backpressure_CXXFLAGS = -pthread
backpressure_LDFLAGS = -pthread
backpressure_LDADD = ../services/libicecc.la

# Integration harness for scheduler backpressure (issue #1).  Built by
# `make check` so it cannot bit-rot; execution is manual because it starts a
# real scheduler and takes minutes -- see the header of schedbp.cpp.
schedbp_SOURCES = schedbp.cpp
schedbp_CXXFLAGS = -pthread
schedbp_LDFLAGS = -pthread
schedbp_LDADD = ../services/libicecc.la

check-local: sndbuf_shim.so
sndbuf_shim.so: $(srcdir)/sndbuf_shim.c
	$(CC) -shared -fPIC $(srcdir)/sndbuf_shim.c -ldl -o $@

CLEANFILES = sndbuf_shim.so
EXTRA_DIST = sndbuf_shim.c connectivityprobe-negative.sh relisten-quick.sh relogin-negative.sh legacyannouncement-negative.sh siblingpin-negative.sh statsapply-negative.sh schedbp-quick.sh schedstress-quick.sh remoteice-quick.sh

# Make the tests also print the test log if they fail.
check: export VERBOSE=1
