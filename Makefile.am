# Ship the branch's documentation, tooling, packaging and integration
# suites: none of these live under a SUBDIRS Makefile.am, so without
# explicit entries `make dist` silently produced a tarball missing the very
# tests and tools the reports describe.
EXTRA_DIST = README.md \
	AUDIT-ISSUE-1.md \
	DEPLOYMENT-MATRIX.md \
	PERF-REPORT.md \
	WEBGUI-REVIEW.md \
	GNUmakefile \
	log_extractor \
	package_builder \
	tests/compose \
	tests/stress \
	tests/webgui

ACLOCAL_AMFLAGS = -I m4
AM_LIBTOOLFLAGS = --silent

SUBDIRS = \
	services \
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
