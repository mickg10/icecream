EXTRA_DIST = \
	README.md \
	capability/distribution/firefox-corrected.compile-trace.tsv \
	capability/grouprlz/p29_online_s1.h \
	capability/grouprlz/runway-census.tsv \
	docker/farm/README.md \
	docker/farm/.gitignore \
	docker/farm/Dockerfile.node \
	docker/farm/environments/README.md \
	docker/farm/environments/Dockerfile.conan-gcc-p50 \
	docker/farm/environments/Dockerfile.debian-gcc-p50 \
	docker/farm/environments/Dockerfile.fedora-clang-libcxx-p50 \
	docker/farm/environments/Dockerfile.linuxbrew-p50 \
	docker/farm/acceptance.sh \
	docker/farm/acceptance/tiny.cpp \
	docker/farm/farm.py \
	docker/farm/inventory/2026-08-21-local-lan.json \
	docker/farm/inventory/2026-08-22-p50-dependencies.json \
	docker/farm/inventory/2026-08-22-topology-correction.json \
	docker/farm/manifests/local-lan.json \
	docker/farm/run-c1f1 \
	docker/farm/run-c1f2 \
	docker/farm/run-c1f4 \
	docker/farm/run-nas-c1f1 \
	docker/farm/run-nas-c1f2 \
	docker/farm/run-nas-c1f4 \
	docker/farm/run-nas-c1f4-max32 \
	docker/farm/run-q2-c1f1 \
	docker/farm/run-q2-c1f2 \
	docker/farm/run-q2-c1f3 \
	docker/farm/run-q2-c1f3-max32 \
	docker/farm/run-matrix \
	docker/farm/test_farm.py

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
	python3 $(top_srcdir)/docker/farm/test_farm.py
	python3 -m unittest \
		capability/distribution/test_run_scenario.py \
		capability/distribution/test_build_p29_ledger.py \
		capability/distribution/test_build_grz_ledger.py \
		capability/distribution/test_run_suite.py
	python3 -m compileall -q capability/distribution

.PHONY: farm-check
farm-check:
	python3 $(top_srcdir)/docker/farm/test_farm.py
