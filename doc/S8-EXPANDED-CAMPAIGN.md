# S8 expanded campaign planner

`farmharness/s8_campaign_planner.py` creates an authenticated declarative
index and `descriptors.jsonl`. It does not emit Docker, simulator, or farm
commands. A later native/live runner binds descriptors whose status is `READY`
to `icecream.s8.native-live-runner-v1`.

The corpus input is the retained `icecream-s8-image-authority-inventory-v1`
authority. The planner rechecks its seal, each manifest's raw byte count and
SHA-256, and streams every listed source snapshot through a no-follow private
file descriptor. `corpus-snapshots.jsonl` records ordinal, literal path,
resolved path, bytes, and SHA-256 for all 8,261 TUs, plus per-corpus ordered
snapshot digests. The planner preserves the ordered source-path list and all
11 TU counts (8,261 total). The
four historical image IDs are accepted only through the immutable recovery
report and remain `MISSING_EXTERNAL_AUTHORITY`; they are never replaced by a
local image. An exact current image content ID may be supplied separately.

The `s8-matrix-audit-v1` prerequisite is independently rehashed and must be
PASS with 32/32 completed cells, 16/16 calibration cells, 16/16 held-out
cells, and empty missing/invalid lists. The canonical 32-cell one-TU audit is
a prerequisite, not a completed depth campaign.

Methods are distinct: `RAW_II`, `ZSTD_TU`, `ZSTD_ROUTE`, `P29`,
`GRZ_RESIDUAL`, `ZSTD_COHORT`, and `ZSTD_GLOBAL`. `RAW_II` is an implemented,
explicit whole-legacy control arm, not a fifth compressed profile and not an
alias for `P29`. Its descriptor retains the required
`raw_ii_legacy_wire_witness` and `raw_ii_engine_template` inputs. The dedicated
`farmharness/s8_raw_ii_predictive_producer.py` consumes those inputs: the
witness binds C-to-F as `CompileFile + FileChunk + End`, while the separately
scoped `raw_ii_control_engine` binds F-to-C and one elapsed service duration
per occurrence. It never reads P29 predictions. The four compressed methods
use the generic producer capability; RAW_II uses its separate control
authority. The two topology records keep stream capacity separate from
execution concurrency.

The campaign driver keeps its default four-profile grid unchanged. Select the
control arm explicitly with `--profiles RAW_II` (or alongside compressed
profiles), and provide both `--raw-ii-witness PATH` and
`--raw-ii-engine-manifest-template PATH`. Missing or non-private inputs fail
closed. Predictive execution invokes the dedicated producer and emits the
same curve-manifest shape consumed by the comparison boundary; external live
execution still requires the regular authenticated farm authority and live
finalizer. A normalizer record for RAW_II must carry the exact
`icecream-s8-raw-ii-control-baseline-v1` declaration on both manifests.

Each witness and control-engine row is keyed by the complete occurrence
identity `(ordinal, source_relative, source_sha256, source_bytes)`. This keeps
distinct translation units with identical content distinct, and the producer
re-snapshots the plan's source manifest and inputs immediately before use. The
v2 control-engine schema supplies one authenticated `elapsed_ns` service
duration per occurrence; the producer applies the authenticated plan's
global-slot schedule and reports C1F20 makespan rather than a serial sum.
Predictive RAW_II commands receive an explicit Git product root
(`--product-root`); emitted source commit/tree identities are the actual
product `HEAD` and `HEAD^{tree}`, with a bounded digest of generated files
outside `HEAD`. `P29` may appear only as the mature shell selector; all plan,
curve, and comparison identities remain `RAW_II`.

Example declarative invocation (the timestamp is caller-owned and immutable):

```sh
python3 farmharness/s8_campaign_planner.py \
  --corpus-inventory /tanksmall/scratch/ictmp/experiments/icecream/s8-image-authority-inventory/20260829T101006Z/inventory.json \
  --image-recovery-report /tanksmall/scratch/ictmp/s8-image-authority-recovery-luna/20260829T110547Z/recovery.json \
  --image-recovery-sha256 a2eafa4b1c01ca7c21e9f52b72700582a563c52fd2cea075b36260ad7f110bf5 \
  --matrix-audit /tanksmall/scratch/ictmp/experiments/icecream/s8-matrix-audit/20260829T083511Z/summary.json \
  --matrix-audit-sha256 f8c8e308256ece6da7d0f0d7cdba235bd55e083d2ce02895cd127c23b7137e62 \
  --output-root /tanksmall/scratch/ictmp/experiments/icecream/s8-expanded-campaign/20260829T120000Z \
  --timestamp 20260829T120000Z
```

With no current image ID, only the 4,928 historical descriptors are emitted
and all are held. Supplying an exact current image content ID additionally
emits the current dimension; its compressed implemented subset is 704
theoretical descriptors and RAW_II adds 176, for 880 implemented descriptors.
RAW_II descriptors remain `NOT_READY` unless their exact authority cell is
supplied. The retained Firefox giant/four-block extension is `NOT_RUN`.

When available, pass `--capability-manifest PATH --capability-manifest-sha256
SHA256`. The manifest must include source bytes/SHA-256 and the five exact
binary bytes/SHA-256 descriptors. The planner reopens every declared path with
a no-follow descriptor, checks stable file identity while streaming, and
requires byte-for-byte descriptor equality. The source must additionally be a
tracked file in a clean Git worktree whose independently observed HEAD, tree,
and committed blob match the manifest; otherwise current compressed cells
remain held (or the explicit capability input is rejected).

For RAW_II, pass `--raw-ii-authority-manifest PATH
--raw-ii-authority-manifest-sha256 SHA256`. The
`icecream-s8-raw-ii-planner-authority-v1` manifest binds the dedicated
producer source and distinct witness/control-engine files for each supplied
`(corpus, regime)` cell. Each file is private, digest-stable, scoped to
RAW_II, declares the calibration/held-out split required by the producer, and
must cover every ordered corpus occurrence exactly once. The control-engine
manifest also requires a valid `[A-Za-z0-9_.-]+` model ID. Missing or
uncovered cells remain `NOT_READY`; P29 and compressed capability inputs cannot
satisfy this boundary.
