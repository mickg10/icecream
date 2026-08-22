EXTRA_DIST = README.md
EXTRA_DIST += \
	capability/distribution/DASHBOARD-SPEC.md \
	capability/distribution/dashboard.py \
	capability/distribution/dashboard_template.html \
	capability/distribution/render_dashboard.sh \
	capability/distribution/test_dashboard.py \
	capability/distribution/dashboard-rs/Cargo.toml \
	capability/distribution/dashboard-rs/Cargo.lock \
	capability/distribution/dashboard-rs/src/main.rs

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

.PHONY: integration_tests
integration_tests:
	python3 -m unittest \
		capability/distribution/test_run_scenario.py \
		capability/distribution/test_build_p29_ledger.py \
		capability/distribution/test_build_grz_ledger.py \
		capability/distribution/test_run_suite.py \
		capability/distribution/test_dashboard.py
	python3 -m compileall -q capability/distribution
	cargo test --manifest-path capability/distribution/dashboard-rs/Cargo.toml
