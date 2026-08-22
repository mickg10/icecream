EXTRA_DIST = README.md
EXTRA_DIST += \
	capability/distribution/DASHBOARD-SPEC.md \
	capability/distribution/FIREFOX-STATIC-ROUTING-SWEEP-RESULTS.md \
	capability/distribution/ICECREAM-REAL-TRANSFER-ARCHITECTURE.md \
	capability/distribution/PHYSICAL-CODEC-DISTRIBUTION-RESULTS.md \
	capability/distribution/R4-MINIMUM-EXPERIMENT-CORE.md \
	capability/distribution/README.md \
	capability/distribution/SHARED-UPLINK-WIDTH-RESULTS.md \
	capability/distribution/SIMULATOR-RULING-AUDIT.md \
	capability/distribution/build_firefox_workload.py \
	capability/distribution/build_grz_ledger.py \
	capability/distribution/build_p29_ledger.py \
	capability/distribution/dashboard.py \
	capability/distribution/dashboard_template.html \
	capability/distribution/dashboard-rs/Cargo.toml \
	capability/distribution/dashboard-rs/Cargo.lock \
	capability/distribution/dashboard-rs/src/main.rs \
	capability/distribution/event.schema.json \
	capability/distribution/execution.schema.json \
	capability/distribution/experiment.schema.json \
	capability/distribution/firefox-1c-20f.json \
	capability/distribution/firefox-5build-topology-suite.json \
	capability/distribution/firefox-c1f1-capacity-bandwidth-suite.json \
	capability/distribution/firefox-c1f20-200b1g.json \
	capability/distribution/firefox-c1f20-physical-suite.json \
	capability/distribution/firefox-corrected.compile-trace.tsv \
	capability/distribution/firefox-corrected.summary.json \
	capability/distribution/firefox-f-width-suite.json \
	capability/distribution/firefox_compile_samples.tsv \
	capability/distribution/fixtures/static-routing/j0.ii \
	capability/distribution/fixtures/static-routing/j1.ii \
	capability/distribution/fixtures/static-routing/j2.ii \
	capability/distribution/fixtures/static-routing/j3.ii \
	capability/distribution/fixtures/static-routing/scenario.json \
	capability/distribution/fixtures/static-routing/trace.tsv \
	capability/distribution/physical-ledger.schema.json \
	capability/distribution/render_dashboard.sh \
	capability/distribution/route-trace.schema.json \
	capability/distribution/run_firefox_static_routing_sweep.py \
	capability/distribution/run_scenario.py \
	capability/distribution/run_static_routing_smoke.py \
	capability/distribution/run_suite.py \
	capability/distribution/samples/r4-minimum/alpha.ii \
	capability/distribution/samples/r4-minimum/beta.ii \
	capability/distribution/samples/r4-minimum/execution.json \
	capability/distribution/samples/r4-minimum/experiment.json \
	capability/distribution/samples/r4-minimum/input-route-trace.jsonl \
	capability/distribution/samples/r4-minimum/route-trace.jsonl \
	capability/distribution/samples/r4-minimum/sample-experiment.jsonl \
	capability/distribution/samples/r4-minimum/trace.tsv \
	capability/distribution/scenario.schema.json \
	capability/distribution/scenarios/firefox-10c-50f-60cores.json \
	capability/distribution/scenarios/firefox-1c-1f-1000000cores.json \
	capability/distribution/scenarios/firefox-1c-20f-50cores.json \
	capability/distribution/scenarios/firefox-1c-50f-50cores.json \
	capability/distribution/scenarios/firefox-c1f1-1000000b-bw10000g.json \
	capability/distribution/scenarios/firefox-c1f1-1000000b-bw1g.json \
	capability/distribution/scenarios/firefox-c1f1-10000b-bw10g.json \
	capability/distribution/scenarios/firefox-c1f1-10000b-bw1g.json \
	capability/distribution/scenarios/firefox-c1f1-200b1g.json \
	capability/distribution/scenarios/firefox-c1f2-200b1g.json \
	capability/distribution/scenarios/firefox-c1f20-200b1g.json \
	capability/distribution/scenarios/firefox-c1f3-200b1g.json \
	capability/distribution/scenarios/firefox-c1f4-200b1g.json \
	capability/distribution/simulator-source.json \
	capability/distribution/suite.schema.json \
	capability/distribution/summary.schema.json \
	capability/distribution/test_build_grz_ledger.py \
	capability/distribution/test_build_p29_ledger.py \
	capability/distribution/test_dashboard.py \
	capability/distribution/test_run_scenario.py \
	capability/distribution/test_run_suite.py \
	capability/distribution/timeline.schema.json \
	capability/distribution/validate_experiment.py \
	capability/distribution/workload-input.schema.json

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
