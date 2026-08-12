.DEFAULT_GOAL := all

.PHONY: docker_build docker_test formal_trace_tests formal_falsify formal_preflight formal_stage

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

formal_trace_tests:
	@PYTHONDONTWRITEBYTECODE=1 PYTHONPATH=formal python3 -m unittest -v formal/test_check_g4_trace.py

formal_falsify:
	@PYTHONDONTWRITEBYTECODE=1 python3 formal/run_g4_formal.py --falsify-only

formal_preflight:
	@PYTHONDONTWRITEBYTECODE=1 python3 formal/run_g4_formal.py --check-only

formal_stage: formal_trace_tests formal_preflight
	@PYTHONDONTWRITEBYTECODE=1 python3 formal/run_g4_formal.py

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
