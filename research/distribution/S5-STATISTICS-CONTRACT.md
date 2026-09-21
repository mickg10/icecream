# S5 ZSTD_TU statistical contract

This is verifier/tooling only. It does not run a benchmark and makes no
deployment or performance claim. The verifier is deliberately fail-closed;
the absence of a valid decision leaves the cache opt-in and legacy-default.

## V6 authenticated invocation

V6 takes two immutable inputs: an independently retained canonical
preregistration and an evidence document.  Invoke it as:

```text
python verify_s5_statistics.py --preregistration preregistration.json \
  --preregistration-sha256 <suite-supplied-digest> \
  --evidence-root evidence-root evidence.json -o result.json
```

The supplied preregistration digest is SHA-256 of the complete canonical
preregistration file bytes, including its final LF. Both JSON inputs must be
canonical UTF-8 JSON terminated by exactly one LF;
duplicate keys, non-finite constants, unknown fields, CRLF, blank/trailing
records and oversized inputs are rejected before verification.  The artifact
root inventory is descriptor-relative and every entry is an object containing
exactly `path`, `sha256`, and `bytes`.  Symlinks, hard links, aliases,
non-regular files and unreferenced files are rejected.  The root is checked
against the preregistration's expected inventory, never against an evidence
producer's replacement list.

Every measured build supplies integer monotonic `start_ns`/`end_ns`; wall time
is derived from that interval.  The independent contract binds the executing
verifier source hash, exact block plan, clock, seed, replication limit and
failure policy.  Raw TU rows and preparation/reset/prewarm event ledgers are
required for correctness but are descriptive only: inference remains on whole
independent block ratios.

## Immutable input

An evidence JSON document has `schema`, `contract`, `contract_digest`,
`builds`, and `blocks`. `contract_digest` is SHA-256 of canonical JSON for
the `contract` object after removing `contract_digest` and
`preregistration_digest`. The contract is immutable and binds
`source_sha256`, `binary_sha256`, `compiler_env_digest`,
`workload_manifest_digest`, `wire_definition_sha256`, and
`block_plan_sha256`, plus a wire definition with direction, layer,
raw-preprocessed-byte denominator, and environment transfer excluded.
It also freezes `modes: ["cache", "legacy"]`, `regimes: ["cold", "warm"]`,
`bootstrap_unit: "whole_block"`, `ratio_definition: "cache_over_legacy"`,
`estimator: "geometric_mean_log_ratio"`, `bound_method:
"whole_block_bootstrap"`, the bootstrap seed/replicate count,
linear-quantile convention, and verifier implementation identity.

V6 does not accept the historical self-listed `evidence_refs` manifest.
Every join, TU, event, reset, and product artifact must be a structured
descriptor in the external preregistration inventory.  The preregistration
root and every nested object are closed schemas: unknown or omitted fields,
duplicate keys, noncanonical encodings, and nonfinite numbers are rejected
before digest acceptance.  The contract contains an exact per-build ordered
`tu_plan` with `tu_count`, corpus/TU identities, workload/compiler/input/
reference digests, and four independently bound ordered identity arrays:
`assignment_ids`, `input_record_ids`, `result_record_ids`, and
`ledger_record_ids`. Evidence must join each planned TU exactly once across
its block, build, assignment, input, result, and directional ledgers. Each raw
row carries the corresponding `assignment_id`, `input_record_id`,
`result_record_id`, and `ledger_record_id`. Those identities are globally
unique and are checked against the frozen plan; content digests may repeat
where the paired workload explicitly requires the same content. Omitted
upstream S4 raw schemas produce an explicit typed HOLD; they cannot be
replaced by empty synthetic arrays or booleans.

The fixed prewarm manifest, content, and initial-state digests are in
`contract.prewarm`.
The required block count is even and at least four per regime.

## Raw observation shape

Every build remains in `builds`, including `failed` and `censored` builds.
A build binds a mode, regime, positive `wall_seconds`, private namespace
(`namespace_id`, `c_guid`, and `f_store`), measurement exclusion flags, a
prewarm manifest/content/initial-state digest and mode-private snapshot
identity, and assignment-strength join evidence. The join row key is exactly
the set `{run_id, scheduler_epoch, wire_id, wire_nonce, attempt_id,
selected_f_identity, f_store_generation, c_guid, tu_seq}` with zero orphan,
duplicate, or dropped rows. `join.evidence_ref` and each block's
`join_evidence_ref` are retained immutable references.

Cold builds additionally require evidence of fresh C_GUID, F-store wipe,
and the reset being outside the interval. Warm builds must not perform a
reset. Both modes in a block share the fixed prewarm manifest, content, and
initial-state digest, while snapshot and cache/F namespaces remain private.
Preparation, reset, and prewarm are explicitly outside the measured full-build
interval. Raw interval timestamps, when present, must be ordered.

A block has exactly two build IDs, one cache and one legacy, with canonical
mapping `A=cache`, `B=legacy`. `AB` means cache then legacy; `BA` means legacy
then cache. `sequence_build_ids` proves this order. Blocks are independent
units. Within each regime there must be an even number (at least four), equal
numbers of AB and BA, and strict alternation. Thus at least eight complete
builds are required per regime.

## Canonical recomputation

Only complete full-build pairs enter the statistic. For each complete block:

```text
R_b = T_cache,b / T_legacy,b
G = exp(mean(log(R_b)))
```

Cold and warm are computed separately. The deterministic bootstrap samples
whole `R_b` blocks only. It uses the preregistered seed, replicate count,
SHA-256 counter PRNG, and linear quantiles. `lower_95` is the 5th percentile
and `upper_95` the 95th percentile of bootstrap geometric means. TU rows are
never resampled or treated as observations; descriptive per-TU percentiles,
if any, are not an inferential input.

The decision is exactly:

* **RED** iff `lower_95 > 1.10` in either cold or warm;
* **GREEN** iff `upper_95 <= 1.10` in both cold and warm;
* otherwise **INCONCLUSIVE**.

Any contract, join, state, order, timing, failure/censor, or completeness
issue is invalid and yields INCONCLUSIVE, never GREEN. Existing derived
statistics are not trusted; optional `recomputed.statistics_sha256` must
match the verifier's exact recomputation.

JSONL input uses one `{"kind":"contract",...}` record (optionally preceded by
a header), followed in order by block, build, TU, event, artifact, and optional
recomputed records. Parser and CLI use the same assembly path. `compute_statistics` is a fail-closed public compatibility
entry point. The private arithmetic routine accepts only immutable primitive
ratio tuples plus the preregistered seed/replicate parameters and returns
arithmetic values only; it accepts no document or text and has no decision
field. Only `verify_preregistered`, after every independent gate passes,
constructs a GREEN/RED result. Output is compact, sorted JSON containing raw-count retention,
issues, separately recomputed cold/warm ratios, estimator and bounds.
