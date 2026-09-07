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
	tests/webgui \
	capability/grouprlz/p29_online_s1.h \
	capability/grouprlz/alpha_line_codec.h \
	capability/grouprlz/runway-census.tsv \
	capability/distribution/firefox-corrected.compile-trace.tsv \
	distro_installed_identity.sh \
	distro_installed_identity_gates.sh \
	s1b_validate_installed_facts.py \
	distro_probe.sh \
	S1B_EXIT_MANIFEST.md

# Keep the farm tooling distributable without recursively copying private or
# generated files from the working tree.  The reviewed manifest contains only
# tracked paths; its test rejects farm.local.json, results, caches and symlinks.
ICEFARM_DIST_MANIFEST = $(srcdir)/farmharness/DISTFILES
ICEFARM_DIST_FILES = $(shell sed -e '/^[[:space:]]*$$/d' "$(ICEFARM_DIST_MANIFEST)")
EXTRA_DIST += farmharness/DISTFILES $(ICEFARM_DIST_FILES)

ACLOCAL_AMFLAGS = -I m4
AM_LIBTOOLFLAGS = --silent

FARM ?= $(abs_top_srcdir)/farmharness/integration/farm.local.json
ICEFARM_PYTHON ?= python3
ICEFARM_TMPDIR ?= /tmp/i
ICEFARM_TMP_ENV = env ICEFARM_TMPDIR="$(ICEFARM_TMPDIR)" \
	TMPDIR="$(ICEFARM_TMPDIR)" TMP="$(ICEFARM_TMPDIR)" \
	TEMP="$(ICEFARM_TMPDIR)" TEMPDIR="$(ICEFARM_TMPDIR)"
ICEFARM_SEALED_LABELS = p43-1.4.0,p50s2-5b2e5801,p50s4-89917385,p50s4-b42d65e8,p50s4-0c820e79,p50s4-2deb91d6,p50s4-57a1e336,p50s4-h3-tail-57a1e336
ICEFARM_SOURCE_LABELS = $(ICEFARM_SEALED_LABELS),p50s30-f-refusal-mutant-candidate,p50s30-f-refusal-0c820e79,p50s30-f-refusal-2deb91d6,p50s30-f-refusal-57a1e336,p50s90-f-revision-2-candidate,p50s90-f-revision-2-57a1e336
ICEFARM_IMAGE_RECEIPT_DIR ?=
ICEFARM_SOURCE_ARCHIVE_DIR ?=

.PHONY: integration_source_archives integration_images integration_smoke integration_controls \
	integration_ladder integration_twobuild integration_full
integration_source_archives:
	@test -n "$(strip $(ICEFARM_SOURCE_ARCHIVE_DIR))" || { \
		echo "set ICEFARM_SOURCE_ARCHIVE_DIR to a new output directory" >&2; exit 2; }
	$(ICEFARM_TMP_ENV) $(ICEFARM_PYTHON) -m farmharness.integration.farmtest source-archives \
		--farm "$(FARM)" \
		--labels "$(if $(strip $(LABELS)),$(strip $(LABELS)),$(ICEFARM_SOURCE_LABELS))" \
		--repo "$(abs_top_srcdir)" --output-dir "$(ICEFARM_SOURCE_ARCHIVE_DIR)"

integration_images:
	$(ICEFARM_TMP_ENV) $(ICEFARM_PYTHON) -m farmharness.integration.farmtest images \
		--farm "$(FARM)" --foundations --repo "$(abs_top_srcdir)" \
		$(if $(strip $(ICEFARM_IMAGE_RECEIPT_DIR)),--output "$(ICEFARM_IMAGE_RECEIPT_DIR)/foundations.json")
	$(ICEFARM_TMP_ENV) $(ICEFARM_PYTHON) -m farmharness.integration.farmtest images \
		--farm "$(FARM)" \
		--labels "$(if $(strip $(LABELS)),$(strip $(LABELS)),$(ICEFARM_SEALED_LABELS))" \
		--repo "$(abs_top_srcdir)" \
		$(if $(strip $(ICEFARM_SOURCE_ARCHIVE_DIR)),--source-archive-dir "$(ICEFARM_SOURCE_ARCHIVE_DIR)") \
		$(if $(strip $(ICEFARM_IMAGE_RECEIPT_DIR)),--output "$(ICEFARM_IMAGE_RECEIPT_DIR)/sealed-products.json")

integration_smoke:
	$(ICEFARM_TMP_ENV) $(ICEFARM_PYTHON) -m farmharness.integration.farmtest suite \
		--farm "$(FARM)" \
		--suite "$(abs_top_srcdir)/farmharness/integration/suites/smoke.json"

integration_controls:
	$(ICEFARM_TMP_ENV) $(ICEFARM_PYTHON) -m farmharness.integration.farmtest suite \
		--farm "$(FARM)" \
		--suite "$(abs_top_srcdir)/farmharness/integration/suites/controls.json"

integration_ladder:
	$(ICEFARM_TMP_ENV) $(ICEFARM_PYTHON) -m farmharness.integration.farmtest suite \
		--farm "$(FARM)" \
		--suite "$(abs_top_srcdir)/farmharness/integration/suites/ladder.json" \
		--stop-on-fail

integration_twobuild:
	$(ICEFARM_TMP_ENV) $(ICEFARM_PYTHON) -m farmharness.integration.farmtest suite \
		--farm "$(FARM)" \
		--suite "$(abs_top_srcdir)/farmharness/integration/suites/twobuild.json" \
		--stop-on-fail

integration_full:
	$(ICEFARM_TMP_ENV) $(ICEFARM_PYTHON) -m farmharness.integration.farmtest suite \
		--farm "$(FARM)" \
		--suite "$(abs_top_srcdir)/farmharness/integration/suites/full.json" \
		--stop-on-fail

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
.PHONY: protocol50-formal
protocol50-formal:
	@test -n "$$TLA2TOOLS_JAR" || { \
		echo "set TLA2TOOLS_JAR to a pinned tla2tools.jar" >&2; exit 2; }
	$(ICEFARM_TMP_ENV) $(ICEFARM_PYTHON) \
		$(srcdir)/cache/formal/run_formal_aggregate.py
