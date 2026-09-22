# Sorbet 1.5 cleanup implementation

User-approved follow-up to the harness readability/correctness audit.
Keep changes incremental; preserve recorded qualification identities and replay.
Implementation status is separate from S* qualification. The owner subsequently
authorized publishing the verified branch and an isolated fresh-checkout trial
on research6; historical evidence deletion and existing-farm changes remain out
of scope.

| Item | Work | Status |
| --- | --- | --- |
| 1 | Release every client gate after failed disk-fill coordination; preserve cleanup errors | Implemented; focused regression checks pass |
| 2 | Observe event failures while waiting for workload commands; bounded owned-process cleanup | Confirmed; explicit transport cancellation still needed |
| 3 | Remove unsupported Fedora 28 from supported package aggregates | Implemented; local contract checks pass |
| 4 | Clean-checkout Compose bootstrap, dependencies and bounded build jobs | Implemented; local contract checks pass; no Docker build yet |
| 5 | Separate small operator configuration from qualification artifacts; portable role mapping | Pending design and migration tests |
| 6 | Validate only selected corpus artifacts when executing commands | Pending; retain full selected-artifact validation |
| 7 | Explicit execution policy instead of scenario-name/assertion switches | Pending; old records must retain their semantics |
| 8 | Extract focused modules from event/collection/verdict monoliths | Pending; avoid a new framework |
| 9 | Move embedded worker scripts into directly editable packaged files | Main workload script extracted byte-for-byte; event scripts remain pending |
| 10 | Centralize versioned receipt contracts without changing historical replay | Pending; old 128 MiB versus current 512 MiB needs explicit treatment |
| 11 | Define request/start/completion times and clock domains in new event records | Pending; requires a record-version migration |
| 12 | Separate expensive scale tests from the default fast entrypoint | Verified; 1,455 fast tests and the single marked scale test both pass |
| 13 | Shared local test entrypoints and CI coverage | Local entrypoints implemented and tested; CI modernization pending |
| 14 | Recorded resource presets for image/corpus compression | Pending; preserve artifact identity checks |
| 15 | Operator/test documentation and unused-file inventory | Test guide and initial inventory done; full operator guide pending |

## Unused-file policy and initial inventory

- No tracked `.pyc`, `__pycache__`, `.orig`, `.rej`, `.bak`, `.log`, `.tmp`,
  editor-backup, `.DS_Store`, generated `configure` or `Makefile.in` files were
  found by the initial tracked-file filename scan. This is not a dead-code proof.
- `package_builder/fedora28/build_rpm.sh` and `verify_rpm.sh` are deliberate
  unsupported-platform checks, exercised by `test-build-requirements.sh`.
  Keep them, although they must not be in the supported aggregate.
- Historical research and resolver-v1 replay support are not deletion candidates
  merely because the current runner does not import them.
- Moved `doc/S8-EXPANDED-CAMPAIGN.md` to
  `research/farmharness/docs/S8-EXPANDED-CAMPAIGN.md`; updated runnable paths,
  marked the guide historical, and included it in the explicit distribution
  manifest. Existing research tools still serve native tests and simulator replay.
- `.cirrus.yml` and `.travis.yml` describe older build environments and omit
  current dependencies. They need modernization or an explicit retirement
  decision, not silent deletion as allegedly unused files.
- Check CLI entrypoints, imports, distribution manifests, build files and docs
  before marking any remaining candidate removable. No content discarded.

## First extraction

`farmharness/integration/workers/manifest_driver.sh` now contains the former
embedded workload program. Its bytes match the previous Python string exactly
(SHA-256 `9d4c72674c3957109ec383c94d5e384b1846975ef199d0c4a0e887239bf2bf40`).
The public `MANIFEST_DRIVER` constant still exposes those same bytes, preserving
command identity. Bash syntax, all 62 workload tests, and all eight distribution
tests passed. Other embedded event programs still need separate extractions.

## Validation policy

Use focused local tests for each increment, then a complete local harness run.
The first batch's fast selection passed 1,455 tests in 89.52 seconds, with one
marked test deselected. The separate thorough selection passed that test
(1 passed, 1,455 deselected in 445.29 seconds). The retained 1,000-row namespace
test previously took about 451 seconds out of 540 seconds total; the marker
changes only selection, not assertions.
Do not claim a Docker build, remote farm run, or a newly qualified product image
from mocked/local tests. See `PROJECT_STATE.md` for the existing S* evidence.
