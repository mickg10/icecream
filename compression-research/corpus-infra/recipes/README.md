# corpus17-26 recipes

One script per corpus, capturing the exact clone + configure + preprocess
commands used to regenerate it. They are written to be re-runnable from an
empty `/tanksmall/scratch/ictmp/src3`.

Shared conventions (see `_common.md` for the rationale):

* **Compiler is system `g++` 11.4.0** (`/usr/lib/gcc/x86_64-linux-gnu/11`),
  the same toolchain corpus1-16 were preprocessed with, so system-header text
  is identical across the whole corpus family. Exceptions are called out in
  the individual recipe and recorded in that corpus's `METADATA.json`.
* **Dependencies come from conda, never from `sudo apt`** — this box has no
  sudo. The env is `/tanksmall/MICKG2/mickg/miniconda3/envs/cppdeps`
  (created here, not shared with `mta`, so the other agents' Python is
  untouched). `env.sh` puts it on `CMAKE_PREFIX_PATH`/`PKG_CONFIG_PATH`.
* **CMake binary dir is `_bld`, not `build`** — folly and others ship an
  in-tree `build/` *source* directory that a cmake binary dir would clobber.
* `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` is passed everywhere because the
  available cmake is 4.2.1, which otherwise rejects projects declaring
  `cmake_minimum_required` below 3.5.
* Configure only — we never link. `preprocess_corpus.py` re-runs each
  compile line with `-E` to produce the mirrored `.ii`.
