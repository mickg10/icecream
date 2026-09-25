# Ship the branch's documentation, tooling, packaging and integration
# suites: none of these live under a SUBDIRS Makefile.am, so without
# explicit entries `make dist` silently produced a tarball missing the very
# tests and tools the reports describe.
EXTRA_DIST = README.md \
	pyproject.toml \
	uv.lock \
	.python-version \
	dev/python.sh \
	farm.json \
	dev/bootstrap.py \
	dev/Dockerfile \
	dev/Dockerfile.runtime \
	dev/run-qa.sh \
	dev/run-gate.sh \
	dev/gate-result.py \
	dev/mixed.py \
	dev/README.md \
	PROJECT_STATE.md \
	research/README.md \
	pytest.ini \
	research/reports/BENCH \
	research/reports/P50_RESULT_DISPOSITION_WIRE_AUDIT.md \
	GNUmakefile \
	log_extractor \
	package_builder \
	tests/compose \
	tests/stress \
	tests/webgui \
	research/measurements/runway-census.tsv \
	research/measurements/firefox-corrected.compile-trace.tsv \
	research/distribution/S5-STATISTICS-CONTRACT.md \
	research/distribution/s5-statistics.schema.json \
	research/distribution/s5_statistics.py \
	research/distribution/verify_s5_statistics.py \
	distro_installed_identity.sh \
	distro_installed_identity_gates.sh \
	s1b_validate_installed_facts.py \
	distro_probe.sh \
	S1B_EXIT_MANIFEST.md

# Keep the active farm tooling and relocated research replay tooling
# distributable without recursively copying private or generated files.
# The explicit manifest rejects farm.local.json, results, caches and symlinks.
ICEFARM_DIST_MANIFEST = $(srcdir)/farmharness/DISTFILES
ICEFARM_DIST_FILES = $(shell sed -e '/^[[:space:]]*$$/d' "$(ICEFARM_DIST_MANIFEST)")
EXTRA_DIST += farmharness/DISTFILES $(ICEFARM_DIST_FILES)

ACLOCAL_AMFLAGS = -I m4
AM_LIBTOOLFLAGS = --silent

ICEFARM_USER_FARM := $(FARM)
FARM ?= $(abs_top_srcdir)/farmharness/integration/farm.local.json
ICEFARM_PYTHON ?= sh "$(abs_top_srcdir)/dev/python.sh"
ICEFARM_TMPDIR ?=
export ICEFARM_TMPDIR
ICEFARM_SCRATCH_GUARD = sh "$(abs_top_srcdir)/farmharness/integration/check_scratch.sh"
ICEFARM_TMP_ENV = env PYTHONDONTWRITEBYTECODE=1 \
	TMPDIR="$$ICEFARM_TMPDIR" TMP="$$ICEFARM_TMPDIR" \
	TEMP="$$ICEFARM_TMPDIR" TEMPDIR="$$ICEFARM_TMPDIR"
# Exactly the current scenario catalogue, including legacy and fault controls.
# Historical experiments remain available through an explicit LABELS= override.
ICEFARM_SEALED_LABELS = p43-1.4.0,p50s2-5b2e5801,p50s30-f-refusal-57a1e336,p50s4-diag-b269fad9,p50s4-h3-armed-57a1e336,p50s90-f-hidden-skew-f9648cc1,p50s90-f-revision-2-f9648cc1
ICEFARM_SOURCE_LABELS = $(ICEFARM_SEALED_LABELS)
ICEFARM_IMAGE_RECEIPT_DIR ?=
ICEFARM_SOURCE_ARCHIVE_DIR ?=

.PHONY: python-sync dev-bootstrap dev-gate qa
python-sync:
	sh "$(abs_top_srcdir)/dev/python.sh" --sync

dev-bootstrap qa:
	$(ICEFARM_SCRATCH_GUARD)
	$(ICEFARM_TMP_ENV) $(ICEFARM_PYTHON) -B "$(abs_top_srcdir)/dev/bootstrap.py" \
		$(if $(filter qa,$@),qa,bootstrap) \
		--farm "$(if $(ICEFARM_USER_FARM),$(FARM),$(abs_top_srcdir)/farm.json)"

# Build this checkout in a unique scratch run, then execute one allowlisted
# process gate in a separate root container on a private bridge network.
dev-gate:
	$(ICEFARM_SCRATCH_GUARD)
	@test -n "$(strip $(GATE))" || { echo "set GATE to a supported opt-in gate" >&2; exit 2; }
	$(ICEFARM_TMP_ENV) $(ICEFARM_PYTHON) -B "$(abs_top_srcdir)/dev/bootstrap.py" \
		gate --gate "$(GATE)" \
		--farm "$(if $(ICEFARM_USER_FARM),$(FARM),$(abs_top_srcdir)/farm.json)"

.PHONY: integration_source_archives integration_images integration_smoke integration_controls \
	integration_ladder integration_twobuild integration_full
integration_source_archives:
	$(ICEFARM_SCRATCH_GUARD)
	@test -n "$(strip $(ICEFARM_SOURCE_ARCHIVE_DIR))" || { \
		echo "set ICEFARM_SOURCE_ARCHIVE_DIR to a new output directory" >&2; exit 2; }
	$(ICEFARM_TMP_ENV) $(ICEFARM_PYTHON) -m farmharness.integration.farmtest source-archives \
		--farm "$(FARM)" \
		--labels "$(if $(strip $(LABELS)),$(strip $(LABELS)),$(ICEFARM_SOURCE_LABELS))" \
		--repo "$(abs_top_srcdir)" --output-dir "$(ICEFARM_SOURCE_ARCHIVE_DIR)"

integration_images:
	$(ICEFARM_SCRATCH_GUARD)
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
	$(ICEFARM_SCRATCH_GUARD)
	$(ICEFARM_TMP_ENV) $(ICEFARM_PYTHON) -m farmharness.integration.farmtest suite \
		--farm "$(FARM)" \
		--suite "$(abs_top_srcdir)/farmharness/integration/suites/smoke.json"

integration_controls:
	$(ICEFARM_SCRATCH_GUARD)
	$(ICEFARM_TMP_ENV) $(ICEFARM_PYTHON) -m farmharness.integration.farmtest suite \
		--farm "$(FARM)" \
		--suite "$(abs_top_srcdir)/farmharness/integration/suites/controls.json"

integration_ladder:
	$(ICEFARM_SCRATCH_GUARD)
	$(ICEFARM_TMP_ENV) $(ICEFARM_PYTHON) -m farmharness.integration.farmtest suite \
		--farm "$(FARM)" \
		--suite "$(abs_top_srcdir)/farmharness/integration/suites/ladder.json" \
		--stop-on-fail

integration_twobuild:
	$(ICEFARM_SCRATCH_GUARD)
	$(ICEFARM_TMP_ENV) $(ICEFARM_PYTHON) -m farmharness.integration.farmtest suite \
		--farm "$(FARM)" \
		--suite "$(abs_top_srcdir)/farmharness/integration/suites/twobuild.json" \
		--stop-on-fail

integration_full:
	$(ICEFARM_SCRATCH_GUARD)
	$(ICEFARM_TMP_ENV) $(ICEFARM_PYTHON) -m farmharness.integration.farmtest suite \
		--farm "$(FARM)" \
		--suite "$(abs_top_srcdir)/farmharness/integration/suites/full.json" \
		--stop-on-fail

# Local deterministic Python harness coverage; neither target contacts a farm.
.PHONY: test-harness-fast test-harness-thorough
test-harness-fast:
	$(ICEFARM_SCRATCH_GUARD)
	cd "$(abs_top_srcdir)" && $(ICEFARM_TMP_ENV) $(ICEFARM_PYTHON) -B -m pytest -q -p no:cacheprovider \
		-m 'not thorough' farmharness/integration/tests

test-harness-thorough:
	$(ICEFARM_SCRATCH_GUARD)
	cd "$(abs_top_srcdir)" && $(ICEFARM_TMP_ENV) $(ICEFARM_PYTHON) -B -m pytest -q -p no:cacheprovider \
		farmharness/integration/tests

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
	$(ICEFARM_SCRATCH_GUARD)
	@test -n "$$TLA2TOOLS_JAR" || { \
		echo "set TLA2TOOLS_JAR to a pinned tla2tools.jar" >&2; exit 2; }
	$(ICEFARM_TMP_ENV) $(ICEFARM_PYTHON) \
		$(srcdir)/cache/formal/run_formal_aggregate.py
