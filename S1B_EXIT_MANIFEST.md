# S1b 1.5.0 exit contract

This file is the committed S1b contract.  It deliberately contains no
historical hashes from an older source tree.  Exact run evidence is emitted by
`distro_installed_identity_gates.sh` from the successor SHA supplied on its
command line.

The distribution tarball contains this file, so embedding that tarball's own
SHA-256 here would be a recursive self-hash.  Instead, the gate makes two
isolated no-Git exports of the exact successor, produces both tarballs with a
fixed source-date epoch and normalized tar metadata, requires them byte-equal,
and prints one `REPRODUCIBLE-DIST-SHA256`.  That digest, the successor Git
SHA/tree, and the per-distro JSON-manifest hashes are the immutable publication
record.

## Release authority

- Source version: exactly `1.5.0` in `configure.ac`.
- Client identity: exactly `ICECC 1.5.0`.
- Daemon identity set: exactly one line, exactly
  `ICECREAM daemon 1.5.0`.
- Scheduler identity set: exactly one line, exactly
  `ICECREAM scheduler 1.5.0`.
- pkg-config identity set: exactly one `Version:` field, exactly
  `Version: 1.5.0`.

The committed image authorities are:

| Distro | Immutable launch reference | Required image ID |
|---|---|---|
| Ubuntu 22 | `sha256:fe001a6138f017608b8846b43bf268a76a9d7a5b66c3364ba3f881da2ff0c54b` | `sha256:fe001a6138f017608b8846b43bf268a76a9d7a5b66c3364ba3f881da2ff0c54b` |
| Ubuntu 24 | `ubuntu@sha256:33ceb71981b602c1a7443a53469e4dba065f7503eab3078a2d7a57a2ab987517` | `sha256:a6f81fb630d51837271b89f8193810a5fc493fa4f30a55d7ebcdb3a66f3cc63a` |
| Fedora 40 | `fedora@sha256:3c86d25fef9d2001712bc3d9b091fc40cf04be4767e48f1aa3b785bf58d300ed` | `sha256:b368d29df3b50e2acc0d6622493a29dafedbbc5a58ad03cab73bddca16c23858` |

`distro_installed_identity.sh` requires both the recorded registry digest
and image ID on the registry-backed rows.  Ubuntu 22 has no RepoDigest and is
therefore launched by its immutable image ID.  Every launch uses
`--pull=never`.  A valid alternate registry image is a required red mutant;
a merely nonempty, internally consistent RepoDigest is not authority.

## Self-contained distribution

The exact `make dist-gzip` archive must contain exactly one byte-identical
copy of each of:

- `distro_installed_identity.sh`
- `distro_installed_identity_gates.sh`
- `s1b_validate_installed_facts.py`
- `distro_probe.sh`
- `S1B_EXIT_MANIFEST.md`

All shipped shell scripts are syntax-checked from the extracted archive.  Every
subsequent installed-identity row executes the extracted producer against that
same no-Git extracted source tree.  Removing any one `EXTRA_DIST` line is a
required red mutant before a distro row starts.

`distro_probe.sh` is retained only as the earlier build-tree compatibility
probe; it now uses the same immutable image references and
`--pull=never`.  S1b EXIT authority comes from the installed-identity
producer and its JSON manifests.

## Required installed manifest

Each distro row emits a sorted JSON array.  The producer parses it back before
PASS and requires this exact, duplicate-free roster:

- `destdir/usr/local/bin/icecc`
- `destdir/usr/local/bin/icecc-create-env`
- `destdir/usr/local/sbin/iceccd`
- `destdir/usr/local/sbin/icecc-scheduler`
- `destdir/usr/local/lib/libicecc.a`
- `destdir/usr/local/lib/pkgconfig/icecc.pc`
- `build/configure.log`
- `build/services.log`
- `build/cache.log`
- `build/daemon.log`
- `build/scheduler.log`
- `build/client.log`
- `build/install.log`
- `build/destdir-listing.txt`
- `build/package-inventory.txt`

Every row has exactly `path,type,mode,size,sha256,identity`; type is
`file`; size is positive; SHA-256 is 64 lowercase hexadecimal characters;
installed executables are mode 755; libraries, metadata, inventories and logs
are mode 644.  Package inventory must have exactly one result for every
declared distro dependency.  The companion facts validator independently
requires exactly one `{mode,size,sha256}` fact for each of those 15 artifacts
(45 keys total); a helper-variable-clobber mutant that mangles one key must be
rejected.

## Freshness and known-caught controls

Normal execution plants hidden sentinels in both build and DESTDIR roots,
removes every visible and hidden top-level entry with `find ... -exec rm`,
and records `POST-CLEAN-EMPTY=YES` before configure starts.

The clean-deletion mutant disables only that removal command.  Both production
call sites, the emptiness assertion and the later pipeline remain present; the
row must record `post_clean_empty=NO` and fail at that assertion.  The
extracted producer is restored byte-exact after every source mutant.

The artifact matrix independently corrupts or removes:

- client and create-env;
- daemon absent/wrong and daemon competing-version identity;
- scheduler absent/wrong and scheduler competing-version identity;
- static library;
- pkg-config wrong and pkg-config competing `Version:` field;
- image authority;
- package inventory;
- build log.

Each row must fail naming its own predicate, and the extracted source content
hash must remain unchanged after every row.

## Reproduction

```sh
./distro_installed_identity_gates.sh \
  /path/to/icecream <exact-successor-sha> /dedicated/scratch ubuntu24
```

The unabridged publication record consists of:

1. exact successor SHA, tree, parent and no-Git archive hash;
2. `REPRODUCIBLE-DIST-SHA256`;
3. Ubuntu 22/24/Fedora normal facts and installed JSON manifests;
4. SHA-256 of every installed JSON manifest;
5. clean-deletion, alternate-registry, actual-launch substitution, and all
   artifact/competing-identity red rows;
6. final byte-equality of the extracted producer and immutable source tree.

S1b establishes release and installed identity only.  Final S4 P50 role hashes
are rebuilt later from the converged S1+S1b+S2 product on the designated
pinned build provenance.
