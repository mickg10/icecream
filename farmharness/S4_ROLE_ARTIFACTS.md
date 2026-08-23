# S4 role artifacts: P43 vs trunk hash-bound runtime roots

Built on q3 (tt-quietbox3) inside the pinned container
`icecream/farm-node:ubuntu22-gcc11-boost174`. Not committed to git (binaries);
this file is the durable record of exactly what was built, how, and what it
hashes to. Roots and tars live on q3 under `~/role-artifacts/`.

## Source provenance

| Set | Commit | Tag/description | Verification |
|---|---|---|---|
| p43 | `cd74801e0fa4e83e3ae254ca1d7fe98642f36b89` | tag `1.4`, "Update version to 1.4 for release" (2022-03-04) | `git fetch origin +refs/tags/1.4:refs/tags/p43-authority` resolved to this exact SHA; fetched fresh via `git archive` and extracted into `~/p43-build/source` on q3 |
| p50 | `43297d535232d8becb58866d5fb6cc73fa3b033d` | trunk successor, "Add inert protocol-50 cache endpoint advertisement" | reused existing `~/ad-build` on q3 -- verified first via `git archive 43297d53... | ssh q3 tar -x` into a scratch dir, then `diff -rq` against `~/ad-build/source` filtered for the (expected) one-sided autogen-generated extras: **0 differing/missing tracked files** |

## Build outcomes

**p43**: `autogen.sh` -> `configure` -> `make -j24`, all exit 0. No source or
build-recipe tweaks were needed (only benign `docbook2x is missing` warnings
for man-page generation, unrelated to the role executables). autogen.sh
emitted the expected AC_TRY_COMPILE/AC_PROG_LIBTOOL obsolescence warnings for
this AC_PREREQ([2.63])-era configure.ac -- warnings only, no errors.

**p50**: reused `~/ad-build` (built earlier, confirmed BUILD-EXIT=0 in its
own `build.log`) after the tree-diff verification above.

## MANIFEST.tsv -- p43-root (`~/role-artifacts/p43-root/MANIFEST.tsv`)

```
path	sha256	version_probe
obj/scheduler/icecc-scheduler	c205c1347064503b3ab7c56af4584ffb0806f52e2c5786d343883db871c44525	ICECREAM scheduler 1.4.0
obj/daemon/iceccd	62bcc05ad91461212cd9616d960905c97c839b316aac3a9e1e1080608a193441	ICECREAM daemon 1.4.0
obj/client/icecc	c3dc54eadca303f21eaa1f90c265a38a1038378be08f887b24bb3bbea7dabeae	ICECC 1.4.0
obj/client/icecc-create-env	aab94b6ea8f41335de807f814d24a56e827ce5efaf8b06797a365699e83b36cb	n/a (script, no embedded version marker)
```

## MANIFEST.tsv -- p50-root (`~/role-artifacts/p50-root/MANIFEST.tsv`)

```
path	sha256	version_probe
obj/scheduler/icecc-scheduler	6ccb91b0dab7596adf2344cedbd56f5eb606815410e85ad1b32cd56a24e78792	ICECREAM scheduler 1.4.92
obj/daemon/iceccd	c0a1df52caee16e5b10d5101f89c4919d3b5df6e073993ed8b239b256ae9d88a	ICECREAM daemon 1.4.92
obj/client/icecc	2c8691ea61c188b0b3d84808a9b577aa2c854b7c4ea132cd398353578bab6853	ICECC 1.4.92
obj/client/icecc-create-env	ee7d30b240c38bccf66d4afcdd45993f115a01d4a2fb4e9143d38596609d2ba4	n/a (script, no embedded version marker)
```

Version probes: `icecc --version` and (for the scheduler, which rejects
`--version` as an unrecognized option in both eras but still emits its
startup banner on that path) the `ICECREAM scheduler ...` line from that
same invocation. `iceccd` has no `--version` flag in either era; its
identity is read via `strings obj/daemon/iceccd | grep 'ICECREAM daemon'`.
`icecc-create-env` is a shell script with no embedded version marker in
either era -- its sha256 is still the identity anchor.

All three linked-and-versioned executables' `ldd` output resolves entirely
to system libraries under `/lib/x86_64-linux-gnu/` in both sets -- nothing
else from the build tree is dlopened, so the four listed files are the
complete runtime root.

## Tar artifacts (`~/role-artifacts/`)

| File | sha256 | bytes |
|---|---|---|
| p43-root.tar | `6da186b6c4c10f2f15d9e5440156ed890112523f53e905f9dd0d2f314540a3ae` | 12646400 |
| p50-root.tar | `c81e6f1f6f32e29a49b5c5623a5c91ccc3197eba160475993659b46d975450db` | 18442240 |

Each tar is `tar -cf <set>-root.tar -C <set>-root .`, i.e. it unpacks to
`obj/{scheduler,daemon,client}/...` + `MANIFEST.tsv` at its own root -- a
drop-in replacement for farm.py's `TREE` bind-source (mount at `/work`).

## Selection mechanism

`farmharness/farm.py` gained `ROLE_SET_DIR = {"p43": "~/role-artifacts/p43-root",
"p50": "~/role-artifacts/p50-root"}` and `role_tree(binary_set)`
(`None` -> the original hardcoded `TREE`, unchanged). `up()` takes
`binary_set_s`/`binary_set_f` (scheduler / every worker), `run_client()`
takes `binary_set_c`; `main()` exposes `--binary-set-S/-C/-F {p43,p50}`,
each defaulting to `None` so omitting all three reproduces prior behavior
exactly. `role_tree()`/`HOSTS`/`ROLE_SET_DIR` are also now safely importable
without triggering a live deploy (`main()` moved behind
`if __name__ == "__main__":`) -- required for
`farmharness/artifact_selection_test.sh` to exercise the real selection code
instead of a shell reimplementation of it.

Currently only q3 has `~/role-artifacts/` populated (the task's caution
scope excluded touching research6/research7/q2); a cross-host cell using
non-default sets for F on another host needs that host's
`~/role-artifacts/` populated first -- a natural, separate follow-up.

## Selection-mutation gate: both verbatim runs

See the S4 delivery report for the full PASS run, the deliberately-broken
FAIL run (role_tree() temporarily collapsed to always resolve p50), and the
restore-and-reconfirm-PASS run.
