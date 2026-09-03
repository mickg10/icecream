# Codec templates

This directory is the shared, header-only home for codec algorithms whose
wire bytes are selected by named parameter tuples and whose storage, history,
and failure policy are supplied by compile-time resource providers.

Phase 0 contains only the provider contracts, tuple catalogue, and frozen
format description.  No production source includes this directory yet.

The vectors under `unittests/codec_golden/` are immutable witnesses cut from
the named product and research revisions.  Never regenerate a golden merely
to make a test pass.  A legitimate byte-format change gets a new tuple id and
a new golden set while the old tuple remains checked against its old vectors.

Golden inputs are identity/smoke fixtures only.  Effectiveness gates use a
corpus of at least 1,000 translation units; Phase 0 makes no effectiveness or
G3 claim.
