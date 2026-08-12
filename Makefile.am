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

# Top-level integration launcher (local-oracle 5258904367 #7 / 21:30): build the
# whole tree, then run the deterministic scenario suites through the hardened serial
# runner in unittests.  Real-farm, formal (TLA/TLAPS), and performance gates are
# SEPARATE stages with distinct environments -- deliberately not run here.
integration_tests:
	$(MAKE)
	$(MAKE) -C unittests integration_tests
.PHONY: integration_tests

# Farm / formal / performance are SEPARATE stages with distinct environments and
# evidence (real cluster; TLA/TLAPS toolchain; perf harness).  They are named here so
# the tester knows they exist, but are intentionally not wired into integration_tests.
farm_stage formal_stage performance_stage:
	@echo "$@: separate stage with its own environment/evidence; run explicitly (not part of integration_tests)."
.PHONY: farm_stage formal_stage performance_stage

test: install
# 'make test' also depends on 'make check', but using plain 'test: check' dependency breaks with recursive make
	$(MAKE) check
	$(MAKE) -C tests $@
test-strict: install
	$(MAKE) check
	$(MAKE) -C tests $@
