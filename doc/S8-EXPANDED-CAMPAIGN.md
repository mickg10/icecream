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
`GRZ_RESIDUAL`, `ZSTD_COHORT`, and `ZSTD_GLOBAL`. Only the four implemented
Root methods can become `READY`, and only after an authenticated producer
capability manifest proves the exact producer source/version/binaries. The
two topology records keep stream capacity separate from execution concurrency.

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
emits the current dimension; its implemented subset is 704 theoretical
descriptors but remains `NOT_READY` unless an authenticated producer capability
manifest is supplied. The base Root planner artifact has no such integrated
capability, so its READY count is zero. The retained Firefox giant/four-block
extension is `NOT_RUN`.

When available, pass `--capability-manifest PATH --capability-manifest-sha256
SHA256`. The manifest must authenticate capability version, producer source
commit/digest, and the five exact executable descriptors; otherwise current
implemented-profile cells remain held.
