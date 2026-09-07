.DEFAULT_GOAL := all

FARM ?= $(CURDIR)/farmharness/integration/farm.local.json
ICEFARM_PYTHON ?= python3
ICEFARM_TMPDIR ?= /tmp/i
ICEFARM_TMP_ENV = env ICEFARM_TMPDIR="$(ICEFARM_TMPDIR)" \
	TMPDIR="$(ICEFARM_TMPDIR)" TMP="$(ICEFARM_TMPDIR)" \
	TEMP="$(ICEFARM_TMPDIR)" TEMPDIR="$(ICEFARM_TMPDIR)"
ICEFARM_SEALED_LABELS = p43-1.4.0,p50s2-5b2e5801,p50s4-89917385,p50s4-b42d65e8,p50s4-0c820e79,p50s4-2deb91d6,p50s4-57a1e336,p50s4-h3-tail-mutant
ICEFARM_SOURCE_LABELS = $(ICEFARM_SEALED_LABELS),p50s30-f-refusal-mutant-candidate,p50s30-f-refusal-0c820e79,p50s30-f-refusal-2deb91d6,p50s30-f-refusal-57a1e336,p50s90-f-revision-2-candidate
ICEFARM_IMAGE_RECEIPT_DIR ?=
ICEFARM_SOURCE_ARCHIVE_DIR ?=

.PHONY: integration_source_archives integration_images integration_smoke integration_controls \
	integration_ladder integration_twobuild integration_full protocol50-formal
integration_source_archives:
	@test -n "$(strip $(ICEFARM_SOURCE_ARCHIVE_DIR))" || { \
		echo "set ICEFARM_SOURCE_ARCHIVE_DIR to a new output directory" >&2; exit 2; }
	@$(ICEFARM_TMP_ENV) $(ICEFARM_PYTHON) -m farmharness.integration.farmtest source-archives \
		--farm "$(FARM)" \
		--labels "$(if $(strip $(LABELS)),$(strip $(LABELS)),$(ICEFARM_SOURCE_LABELS))" \
		--repo "$(CURDIR)" --output-dir "$(ICEFARM_SOURCE_ARCHIVE_DIR)"

integration_images:
	@$(ICEFARM_TMP_ENV) $(ICEFARM_PYTHON) -m farmharness.integration.farmtest images \
		--farm "$(FARM)" --foundations --repo "$(CURDIR)" \
		$(if $(strip $(ICEFARM_IMAGE_RECEIPT_DIR)),--output "$(ICEFARM_IMAGE_RECEIPT_DIR)/foundations.json")
	@$(ICEFARM_TMP_ENV) $(ICEFARM_PYTHON) -m farmharness.integration.farmtest images \
		--farm "$(FARM)" --labels "$(if $(strip $(LABELS)),$(strip $(LABELS)),$(ICEFARM_SEALED_LABELS))" \
		--repo "$(CURDIR)" \
		$(if $(strip $(ICEFARM_SOURCE_ARCHIVE_DIR)),--source-archive-dir "$(ICEFARM_SOURCE_ARCHIVE_DIR)") \
		$(if $(strip $(ICEFARM_IMAGE_RECEIPT_DIR)),--output "$(ICEFARM_IMAGE_RECEIPT_DIR)/sealed-products.json")

integration_smoke:
	@$(ICEFARM_TMP_ENV) $(ICEFARM_PYTHON) -m farmharness.integration.farmtest suite \
		--farm "$(FARM)" --suite farmharness/integration/suites/smoke.json

integration_controls:
	@$(ICEFARM_TMP_ENV) $(ICEFARM_PYTHON) -m farmharness.integration.farmtest suite \
		--farm "$(FARM)" --suite farmharness/integration/suites/controls.json

integration_ladder:
	@$(ICEFARM_TMP_ENV) $(ICEFARM_PYTHON) -m farmharness.integration.farmtest suite \
		--farm "$(FARM)" --suite farmharness/integration/suites/ladder.json \
		--stop-on-fail

integration_twobuild:
	@$(ICEFARM_TMP_ENV) $(ICEFARM_PYTHON) -m farmharness.integration.farmtest suite \
		--farm "$(FARM)" --suite farmharness/integration/suites/twobuild.json \
		--stop-on-fail

integration_full:
	@$(ICEFARM_TMP_ENV) $(ICEFARM_PYTHON) -m farmharness.integration.farmtest suite \
		--farm "$(FARM)" --suite farmharness/integration/suites/full.json \
		--stop-on-fail

# This target intentionally works in an unconfigured source release.  The
# aggregate runner authenticates the TLC jar and gives every lane a unique,
# retained state directory below the caller-selected scratch root.
protocol50-formal:
	@$(ICEFARM_TMP_ENV) $(ICEFARM_PYTHON) cache/formal/run_formal_aggregate.py

.PHONY: docker_build docker_test
docker_build:
	@cd package_builder/ubuntu22.04 && docker compose run --rm --build deb
	@cd package_builder/ubuntu24.04 && docker compose run --rm --build deb
	@cd package_builder/fedora28 && docker compose run --rm --build rpm
	@cd package_builder/fedora-latest && docker compose run --rm --build rpm

docker_test: docker_build
	@cd package_builder/ubuntu22.04 && docker compose run --rm --build verify
	@cd package_builder/ubuntu24.04 && docker compose run --rm --build verify
	@cd package_builder/fedora28 && docker compose run --rm --build verify
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
