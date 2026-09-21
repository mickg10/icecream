# Research and historical material

Experimental codecs, statistical research tools, retained measurements and
historical reports belong here, not in the project root or production codec
directories. New experiments should use this tree from the start.

| Directory | Contents | Production use |
|---|---|---|
| `codecs/` | alpha-line, MO-factor and residual-group experiments | None; not compiled or linked by the product |
| `vendor/grouprlz/` | frozen GRZ reference generators | None; retained for reproducibility |
| `vendor/libbsc/` | historical dependency provenance, source manifest and licence | None; not a product dependency |
| `distribution/` | S5 statistical contract, schema and verifier | Historical evidence tooling; imported by its retained runner/tests |
| `farmharness/` | Retained S4–S8 experiments, replay tools, schemas, tests and supervisor image | Historical tooling; a few native test/simulator consumers import it explicitly |
| `measurements/` | runway census and corrected Firefox trace | Read-only inputs to the layout regression check |
| `reports/` | historical performance, deployment and audit reports | Documentation, not current qualification status |

Production remains in `client/`, `daemon/`, `scheduler/`, `services/` and
`cache/`. The live acceptance harness remains in `farmharness/integration/`.
Regression tests and their immutable golden fixtures remain in `unittests/`
and `tests/`; a fixture's research origin does not make it disposable.
The active resolver stays at `farmharness/newgen_farm_env.py`, alongside the
current integration harness. It does not import the archived S4–S8 tools.
The latter move together to `research/farmharness/`, with their internal
imports and retained native test/simulator references updated. They are not
an invitation to restart retired GRZ or calibration campaigns.

One formerly research-located header, `capability/grouprlz/p29_online_s1.h`,
is a real production dependency. It is promoted **byte-for-byte** to
`cache/codec/p29_online_s1.h`, with its include sites and distribution list
updated. No product C++ source may include `research/` or `capability/`.

## Path changes

- Root `AUDIT-ISSUE-1.md`, `BENCH`, `DEPLOYMENT-MATRIX.md`, `PERF-REPORT.md`,
  `WEBGUI-REVIEW.md` and `P50_RESULT_DISPOSITION_WIRE_AUDIT.md` → `reports/`.
- `vendor/` → `research/vendor/` (archived sources, no compatibility symlinks).
- Unused `capability/grouprlz/` codec headers → `codecs/`.
- `capability/distribution/` statistical tools → `distribution/`.
- Both retained capability TSVs → `measurements/`.
- Top-level `farmharness/s[4-8]*` tools, tests and schemas →
  `research/farmharness/`; their Markdown guides → `farmharness/docs/` here.
- `docker/s8-supervisor.Dockerfile` and `docs/S8_SUPERVISOR_IMAGE.md` →
  `farmharness/docker/` and `farmharness/docs/` here.

For retained Python tools, run from the checkout with `PYTHONPATH=.` (or
set `PYTHONPATH` to the checkout's absolute path from another directory).
Prefer module invocation, for example:

```sh
python3 -m research.farmharness.s5_paired_summary --help
```

Historical tools still enforce their historical contracts. In particular,
the held-out GRZ preflight is not a current-product acceptance gate and must
not be weakened to accept the current three-profile product.

Quoted paths in historical reports and provenance describe their original
commits and are intentionally retained. Current imports, includes and build
references use the new paths. Immutable evidence outside this checkout is not
rewritten. For current status see [PROJECT_STATE.md](../PROJECT_STATE.md).

Only explicitly listed regression support and documentation are shipped by
`Makefile.am`; there is no recursive `EXTRA_DIST = research`. Experimental
codec implementations and GRZ/libbsc archives are not product build inputs.
The P40/P43 ordinary wire path and P50 profile IDs remain unchanged.
