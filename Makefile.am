EXTRA_DIST = \
	README.md \
	capability/distribution/firefox-corrected.compile-trace.tsv \
	capability/grouprlz/p29_online_s1.h \
	capability/grouprlz/runway-census.tsv

ACLOCAL_AMFLAGS = -I m4
AM_LIBTOOLFLAGS = --silent

SUBDIRS = \
	services \
	cache \
	daemon \
	client \
	doc \
	suse \
	compilerwrapper \
	scheduler \
	tests \
	unittests

dist-hook:
	( cd $(top_srcdir) && git log --date=short --pretty="format:@%cd  %an  <%ae>  [%H]%n%n%s%n%n%e%b" | sed -e "s|^\([^@]\)|\t\1|" -e "s|^@||" ) >$(distdir)/ChangeLog

test: install
# 'make test' also depends on 'make check', but using plain 'test: check' dependency breaks with recursive make
	$(MAKE) check
	$(MAKE) -C tests $@
test-strict: install
	$(MAKE) check
	$(MAKE) -C tests $@

.PHONY: integration_tests
integration_tests:
	python3 -m unittest \
		capability/distribution/test_run_scenario.py \
		capability/distribution/test_build_p29_ledger.py \
		capability/distribution/test_build_grz_ledger.py \
		capability/distribution/test_run_suite.py
	python3 -m compileall -q capability/distribution
