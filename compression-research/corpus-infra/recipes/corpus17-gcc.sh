#!/bin/bash
# corpus17 — GNU GCC (https://github.com/gcc-mirror/gcc)
# Preprocessed with system g++ 11.4.0.  See recipes/README.md for conventions.
set -euo pipefail
ICT=/tanksmall/scratch/ictmp
SRC3=$ICT/src3

git clone --depth 1 https://github.com/gcc-mirror/gcc.git $SRC3/gcc
cd $SRC3/gcc && ./contrib/download_prerequisites      # gmp/mpfr/mpc/isl

mkdir -p $SRC3/gcc-build && cd $SRC3/gcc-build
$SRC3/gcc/configure --disable-bootstrap --enable-languages=c,c++ \
  --disable-multilib --disable-libsanitizer --disable-werror

# GCC is autotools, so there is no -DCMAKE_EXPORT_COMPILE_COMMANDS.  A REAL
# build is required (not `make -n`): gcc/ compiles generated sources
# (insn-*.cc, generic-match-*.cc) that only exist after genattrtab et al. run.
# -k so a late link failure still leaves a complete set of gcc/ compile lines.
make -j5 -k 2>&1 | tee $SRC3/logs/gcc.make.log

# `compiledb -p` dies on this log with IndexError: GCC's recursive make emits
# more "Leaving directory" than "Entering directory" lines and compiledb pops
# an empty dir stack.  Use the local parser instead.  It also skips TUs built
# by the freshly-built xgcc (libstdc++ etc.) so the corpus stays on one
# toolchain's system headers.
python3 $SRC3/make_log_to_ccjson.py \
  --log $SRC3/logs/gcc.make.log \
  --out $SRC3/gcc-build/compile_commands.json \
  --root $SRC3/gcc-build

python3 $ICT/build2/preprocess_corpus.py \
  --cc-json $SRC3/gcc-build/compile_commands.json \
  --outdir $ICT/corpus17 --jobs 10 --log $SRC3/logs/gcc.pp.faillog
find $ICT/corpus17 -name '*.ii' | sort > $ICT/corpus17/manifest.txt
