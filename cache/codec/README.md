# Codec templates

This directory is the shared, header-only home for codec algorithms whose
wire bytes are selected by named parameter tuples and whose storage, history,
and failure policy are supplied by compile-time resource providers.

`p29_intern.h` supplies the P29 Line/Region interner. It reproduces the retained
research identities while obtaining owned tables and byte arenas from a bounded
provider; its product test adapter publishes ordinals through `CObjectArena`.
The revision-1 `P29V1` production profile uses this interner.

The provider-owned `P29Serializer`/`P29Deserializer` in `p29_wire.h` reproduce
the retained P29-v1 core streams, stage one TU transactionally, materialize
every TU exactly, and publish decoded Regions and Blocks only on commit.
CacheWire revision 1 uses these templates through the
`P29V1` profile; endpoint, route, publication, and failure policy remain outside
the template.

The vectors under `unittests/codec_golden/` are immutable witnesses cut from
the named product and research revisions.  Never regenerate a golden merely
to make a test pass.  A legitimate byte-format change gets a new tuple id and
a new golden set while the old tuple remains checked against its old vectors.
The `unittests/codec_golden/product/p29` and
`unittests/codec_golden/product/grz` directories retain historical v0 bytes;
they do not name CacheWire revision-1 product profiles.

GRZ and libbsc are not product dependencies. The sources under
`research/vendor/grouprlz/` and their libbsc provenance are archived generators,
excluded from the product source distribution. Keep the frozen tuple catalogue
and golden fixtures: they are used by the codec tests, not by profile selection.
The supported product profiles remain `P29V1=1`, `ZSTD_TU=2`, and
`ZSTD_ROUTE=3`. Ordinary legacy-wire LZO/zstd support is separate and retained.

The production OnlineS1 implementation is `p29_online_s1.h` in this
directory. It is a production dependency, not archived research. Experimental
alpha-line, MO-factor and residual-group implementations live in `research/codecs/`.

Golden inputs are identity/smoke fixtures only.  Effectiveness gates use a
corpus of at least 1,000 translation units.  The interner micro-gate therefore
uses the retained 2,498-TU Firefox manifest in addition to the small identity
vectors.

See [P29V1_FORMAT.md](P29V1_FORMAT.md) for the current P29 inner grammar,
[ZSTD_FORMATS.md](ZSTD_FORMATS.md) for both current ZSTD profiles, and
[FORMAT.md](FORMAT.md) for historical layouts. Cross-zstd-version golden tests
compare exact decoded per-frame contents, order and kind; same-build/provider
tests compare bytes.
