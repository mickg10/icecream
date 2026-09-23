.DEFAULT_GOAL := all

# Keep historical integration defaults; the developer commands use root farm.json.
ICEFARM_USER_FARM := $(FARM)
FARM ?= $(CURDIR)/farmharness/integration/farm.local.json
ICEFARM_PYTHON ?= sh "$(CURDIR)/dev/python.sh"
ICEFARM_TMPDIR ?=
export ICEFARM_TMPDIR
ICEFARM_SCRATCH_GUARD = sh "$(CURDIR)/farmharness/integration/check_scratch.sh"
ICEFARM_TMP_ENV = env PYTHONDONTWRITEBYTECODE=1 \
	TMPDIR="$$ICEFARM_TMPDIR" TMP="$$ICEFARM_TMPDIR" \
	TEMP="$$ICEFARM_TMPDIR" TEMPDIR="$$ICEFARM_TMPDIR"
# Exactly the current scenario catalogue, including legacy and fault controls.
# Historical experiments remain available through an explicit LABELS= override.
ICEFARM_SEALED_LABELS = p43-1.4.0,p50s2-5b2e5801,p50s30-f-refusal-57a1e336,p50s4-diag-b269fad9,p50s4-h3-armed-57a1e336,p50s90-f-hidden-skew-f9648cc1,p50s90-f-revision-2-f9648cc1
ICEFARM_SOURCE_LABELS = $(ICEFARM_SEALED_LABELS)
ICEFARM_IMAGE_RECEIPT_DIR ?=
ICEFARM_SOURCE_ARCHIVE_DIR ?=

.PHONY: python-sync dev-bootstrap qa
python-sync:
	@sh "$(CURDIR)/dev/python.sh" --sync

check:
	@sh "$(CURDIR)/dev/python.sh" --exec $(MAKE) -f Makefile check

.PHONY: check

dev-bootstrap qa:
	@$(ICEFARM_SCRATCH_GUARD)
	@$(ICEFARM_TMP_ENV) $(ICEFARM_PYTHON) -B dev/bootstrap.py \
		$(if $(filter qa,$@),qa,bootstrap) \
		--farm "$(if $(ICEFARM_USER_FARM),$(FARM),$(CURDIR)/farm.json)"

.PHONY: integration_source_archives integration_images integration_smoke integration_controls \
	integration_ladder integration_twobuild integration_full test-harness-fast \
	test-harness-thorough protocol50-formal
integration_source_archives:
	@$(ICEFARM_SCRATCH_GUARD)
	@test -n "$(strip $(ICEFARM_SOURCE_ARCHIVE_DIR))" || { \
		echo "set ICEFARM_SOURCE_ARCHIVE_DIR to a new output directory" >&2; exit 2; }
	@$(ICEFARM_TMP_ENV) $(ICEFARM_PYTHON) -m farmharness.integration.farmtest source-archives \
		--farm "$(FARM)" \
		--labels "$(if $(strip $(LABELS)),$(strip $(LABELS)),$(ICEFARM_SOURCE_LABELS))" \
		--repo "$(CURDIR)" --output-dir "$(ICEFARM_SOURCE_ARCHIVE_DIR)"

integration_images:
	@$(ICEFARM_SCRATCH_GUARD)
	@$(ICEFARM_TMP_ENV) $(ICEFARM_PYTHON) -m farmharness.integration.farmtest images \
		--farm "$(FARM)" --foundations --repo "$(CURDIR)" \
		$(if $(strip $(ICEFARM_IMAGE_RECEIPT_DIR)),--output "$(ICEFARM_IMAGE_RECEIPT_DIR)/foundations.json")
	@$(ICEFARM_TMP_ENV) $(ICEFARM_PYTHON) -m farmharness.integration.farmtest images \
		--farm "$(FARM)" --labels "$(if $(strip $(LABELS)),$(strip $(LABELS)),$(ICEFARM_SEALED_LABELS))" \
		--repo "$(CURDIR)" \
		$(if $(strip $(ICEFARM_SOURCE_ARCHIVE_DIR)),--source-archive-dir "$(ICEFARM_SOURCE_ARCHIVE_DIR)") \
		$(if $(strip $(ICEFARM_IMAGE_RECEIPT_DIR)),--output "$(ICEFARM_IMAGE_RECEIPT_DIR)/sealed-products.json")

integration_smoke:
	@$(ICEFARM_SCRATCH_GUARD)
	@$(ICEFARM_TMP_ENV) $(ICEFARM_PYTHON) -m farmharness.integration.farmtest suite \
		--farm "$(FARM)" --suite farmharness/integration/suites/smoke.json

integration_controls:
	@$(ICEFARM_SCRATCH_GUARD)
	@$(ICEFARM_TMP_ENV) $(ICEFARM_PYTHON) -m farmharness.integration.farmtest suite \
		--farm "$(FARM)" --suite farmharness/integration/suites/controls.json

integration_ladder:
	@$(ICEFARM_SCRATCH_GUARD)
	@$(ICEFARM_TMP_ENV) $(ICEFARM_PYTHON) -m farmharness.integration.farmtest suite \
		--farm "$(FARM)" --suite farmharness/integration/suites/ladder.json \
		--stop-on-fail

integration_twobuild:
	@$(ICEFARM_SCRATCH_GUARD)
	@$(ICEFARM_TMP_ENV) $(ICEFARM_PYTHON) -m farmharness.integration.farmtest suite \
		--farm "$(FARM)" --suite farmharness/integration/suites/twobuild.json \
		--stop-on-fail

integration_full:
	@$(ICEFARM_SCRATCH_GUARD)
	@$(ICEFARM_TMP_ENV) $(ICEFARM_PYTHON) -m farmharness.integration.farmtest suite \
		--farm "$(FARM)" --suite farmharness/integration/suites/full.json \
		--stop-on-fail

# Local deterministic Python harness coverage; neither target contacts a farm.
test-harness-fast:
	@$(ICEFARM_SCRATCH_GUARD)
	@$(ICEFARM_TMP_ENV) $(ICEFARM_PYTHON) -B -m pytest -q -p no:cacheprovider \
		-m 'not thorough' farmharness/integration/tests

test-harness-thorough:
	@$(ICEFARM_SCRATCH_GUARD)
	@$(ICEFARM_TMP_ENV) $(ICEFARM_PYTHON) -B -m pytest -q -p no:cacheprovider \
		farmharness/integration/tests

# This target intentionally works in an unconfigured source release.  The
# aggregate runner authenticates the TLC jar and gives every lane a unique,
# retained state directory below the caller-selected scratch root.
protocol50-formal:
	@$(ICEFARM_SCRATCH_GUARD)
	@$(ICEFARM_TMP_ENV) $(ICEFARM_PYTHON) cache/formal/run_formal_aggregate.py

.PHONY: docker_build docker_test
docker_build:
	@cd package_builder/ubuntu22.04 && docker compose run --rm --build deb
	@cd package_builder/ubuntu24.04 && docker compose run --rm --build deb
	@cd package_builder/fedora-latest && docker compose run --rm --build rpm

docker_test: docker_build
	@cd package_builder/ubuntu22.04 && docker compose run --rm --build verify
	@cd package_builder/ubuntu24.04 && docker compose run --rm --build verify
	@cd package_builder/fedora-latest && docker compose run --rm --build verify

# Forward any other targets to the existing autotools Makefile.
# Clean targets in an UNCONFIGURED tree are a successful no-op: package
# builds (dh_auto_clean, %autosetup) run `make distclean` before configuring,
# and an error here breaks every packager that sees this wrapper.
%:
	@if [ -f Makefile ]; then \
		$(MAKE) -f Makefile $@; \
	else \
		case "$@" in \
		clean|distclean|maintainer-clean|mostlyclean) \
			echo "nothing to $@: tree is not configured";; \
		*) \
			echo "ERROR: Makefile not found. Run ./configure (or ./autogen.sh && ./configure) first." >&2; \
			exit 1;; \
		esac; \
	fi
