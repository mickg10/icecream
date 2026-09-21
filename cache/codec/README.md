# Codec templates

This directory is the shared, header-only home for codec algorithms whose
wire bytes are selected by named parameter tuples and whose storage, history,
and failure policy are supplied by compile-time resource providers.

Phase 0 contains the provider contracts, tuple catalogue, and frozen format
description.  Phase 3a adds `p29_intern.h`, the wire-neutral P29 Line/Region
interner.  It reproduces the retained research identities while obtaining all
owned tables and byte arenas from a bounded provider; its product test adapter
publishes the resulting ordinals through `CObjectArena`.  The P29-v1 landing
is complete: the revision-1 `P29V1` production profile uses this interner.

Phase 3b adds the provider-owned `P29Serializer`/`P29Deserializer` in
`p29_wire.h`.  It reproduces the retained P29-v1 core streams, stages one TU
transactionally, materializes every TU exactly, and publishes decoded Regions
and Blocks only on commit.  CacheWire revision 1 uses this template through the
`P29V1` profile; endpoint, route, publication, and failure policy remain outside
the template.

The vectors under `unittests/codec_golden/` are immutable witnesses cut from
the named product and research revisions.  Never regenerate a golden merely
to make a test pass.  A legitimate byte-format change gets a new tuple id and
a new golden set while the old tuple remains checked against its old vectors.
The `product/p29` and `product/grz` directories retain historical v0 bytes;
they do not name CacheWire revision-1 product profiles.

GRZ and libbsc are not product dependencies. The sources under
`research/vendor/grouprlz/` and their libbsc provenance are archived generators,
excluded from the product source distribution. Keep the frozen tuple catalogue
and golden fixtures: they are used by the codec tests, not by profile selection.
The supported product profiles remain `P29V1=1`, `ZSTD_TU=2`, and
`ZSTD_ROUTE=3`. Ordinary legacy-wire LZO/zstd support is separate and retained.

The production OnlineS1 implementation is `p29_online_s1.h` in this
directory, promoted unchanged from its original `capability/grouprlz/` path.
It is a production dependency, not archived research. Experimental alpha-line,
MO-factor and residual-group implementations live in `research/codecs/`.

Golden inputs are identity/smoke fixtures only.  Effectiveness gates use a
corpus of at least 1,000 translation units.  The interner micro-gate therefore
uses the retained 2,498-TU Firefox manifest in addition to the small identity
vectors.
