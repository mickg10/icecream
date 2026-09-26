# Reproducible paired preprocessor inputs for P50

`tools/p50_regenerate_paired_corpus.py` regenerates fresh A and B preprocessed
translation units from one compile database and one pinned source tree. It
does not pair an archived A file with a freshly generated B file. It preserves
the TU identity/order, records input and output digests, and reports whether
fresh A reproduces each archived input exactly.

The supported source-edit contract is intentionally narrow: A and B roots must
be the same pinned checkout, and B edits are supplied as a sparse overlay at
the original relative source paths. Clang's VFS overlay maps the edited bytes
to the original virtual filename, preserving `__FILE__`, line markers, and
quoted-include lookup. The generated preprocessor output is never rewritten.
The generator rejects a different A/B source root rather than silently mixing
include trees. Its byte cap is checked during selection and after each output;
both manifests are published only after both complete corpora fit.

Example (paths should be fresh, task-owned paths; an existing output directory
is rejected):

```sh
uv run python tools/p50_regenerate_paired_corpus.py \
  --compile-commands /path/to/build/compile_commands.json \
  --original-source-root /path/to/pinned/source \
  --build-root /path/to/build \
  --archive-root /path/to/archive \
  --archive-manifest /path/to/archive/manifest.txt \
  --source-root-a /path/to/pinned/source \
  --source-root-b /path/to/pinned/source \
  --overlay-b-root /path/to/sparse-b-overlay \
  --output-dir /path/to/new-output-directory \
  --expected-revision FULL_GIT_SHA --count 32 --max-bytes 503316480
```

## Bounded RocksDB sample evidence

The checked sample used ClickHouse's vendored RocksDB at revision
`6d0a17390c451dac5db306436a6033a6be71718f`, with its matching
`_bld/compile_commands.json` and archived corpus manifest. It selected the
largest 32 archived TUs matching `contrib/rocksdb/`: 279,514,742 archived
bytes (below the 480 MiB generator cap and 512 MiB benchmark limit). The
archive has 318 RocksDB-named entries; 317 are source TUs selected by the
source-path filter, while the additional generated `build_version.cc` is not
part of that source subset.

Fresh A matched all 32 archived TUs byte-for-byte. B had one controlled
source-level edit: an appended `static_assert(sizeof(void*) >= 4, ...)` in
`db_impl.cc`. This checks pairing/provenance and preprocessing, not a natural
developer workload or a meaningful product change. B differed from archived A
for exactly that one TU; all other B TUs matched. This is corpus preparation
evidence only, not a transfer timing result or a compiler-speed claim.

The retained generator run is outside the source tree at
`/tanksmall/scratch/tmp/p51-regenerate-probe/rocksdb-top32-v1`. Its metadata
SHA-256 is `82e8e59c60ecd7fdf881512fac34e6a7bc91341ab9ec6d6a95e4adcbdc857f7c`;
the 32-entry A and B manifest SHA-256 values are respectively
`14e6dd2fdbd9148d78da482cb6f690d8f0c55408f3f9b298a34d20fec80f6e77` and
`58960e5a144265637b40531131fadaf252174730d3e3747489e824d9512c7688`.
The generator log SHA-256 is
`0b93f27df1e809c4e16ff72384a08df09ad803c841af6ebf4e97494cc0d1db5b`.
