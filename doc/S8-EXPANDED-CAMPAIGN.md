# S8 expanded campaign planner

`farmharness/s8_campaign_planner.py` creates an authenticated declarative
index and `descriptors.jsonl`. It does not emit Docker, simulator, or farm
commands. A later native/live runner binds descriptors whose status is `READY`
to `icecream.s8.native-live-runner-v1`.

The corpus input is the retained `icecream-s8-image-authority-inventory-v1`
authority. The planner rechecks its seal, each manifest's raw byte count and
SHA-256, ordered source-path list, and all 11 TU counts (8,261 total). The
four historical image IDs are accepted only through the immutable recovery
report and remain `MISSING_EXTERNAL_AUTHORITY`; they are never replaced by a
local image. An exact current image content ID may be supplied separately.

Methods are distinct: `RAW_II`, `ZSTD_TU`, `ZSTD_ROUTE`, `P29`,
`GRZ_RESIDUAL`, `ZSTD_COHORT`, and `ZSTD_GLOBAL`. Only the four implemented
Root methods are `READY` on an exact current image. The two topology records
keep stream capacity separate from execution concurrency.

Example declarative invocation (the timestamp is caller-owned and immutable):

```sh
python3 farmharness/s8_campaign_planner.py \
  --corpus-inventory /tanksmall/scratch/ictmp/experiments/icecream/s8-image-authority-inventory/20260829T101006Z/inventory.json \
  --image-recovery-report /tanksmall/scratch/ictmp/s8-image-authority-recovery-luna/20260829T110547Z/recovery.json \
  --image-recovery-sha256 a2eafa4b1c01ca7c21e9f52b72700582a563c52fd2cea075b36260ad7f110bf5 \
  --output-root /tanksmall/scratch/ictmp/experiments/icecream/s8-expanded-campaign/20260829T120000Z \
  --timestamp 20260829T120000Z
```

With no current image ID, only the 4,928 historical descriptors are emitted
and all are held. Supplying an exact current image content ID additionally
emits the current dimension; its implemented subset is 704 descriptors.
The canonical 32-cell one-TU audit is a satisfied prerequisite, not depth
completion. The retained Firefox giant/four-block extension is `NOT_RUN`.
