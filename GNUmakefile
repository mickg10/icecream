.DEFAULT_GOAL := all

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
%:
	@if [ -f Makefile ]; then \
		$(MAKE) -f Makefile $@; \
	else \
		echo "ERROR: Makefile not found. Run ./configure (or ./autogen.sh && ./configure) first." >&2; \
		exit 1; \
	fi
