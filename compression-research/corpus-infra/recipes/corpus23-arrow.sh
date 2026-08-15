#!/bin/bash
# corpus23 — Apache Arrow C++ (https://github.com/apache/arrow, cpp/ subtree)
# Preprocessed with system g++ 11.4.0.  See recipes/README.md for conventions.
set -euo pipefail
ICT=/tanksmall/scratch/ictmp
SRC3=$ICT/src3
source $SRC3/env.sh

git clone --depth 1 https://github.com/apache/arrow.git $SRC3/arrow

# DEPENDENCY_SOURCE=AUTO, not BUNDLED.  With BUNDLED, arrow points the compile
# lines at include dirs under _bld/_deps that ExternalProject only populates at
# BUILD time, so every TU fails to preprocess.  AUTO uses the conda deps
# (thrift-cpp brotli rapidjson xsimd snappy zstd lz4 zlib re2) and only
# FetchContent-downloads the rest, which does land at configure time.
cmake -S $SRC3/arrow/cpp -B $SRC3/arrow/_bld -G Ninja \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DARROW_DEPENDENCY_SOURCE=AUTO -DARROW_BUILD_TESTS=OFF \
  -DARROW_COMPUTE=ON -DARROW_CSV=ON -DARROW_JSON=ON -DARROW_FILESYSTEM=ON \
  -DARROW_IPC=ON -DARROW_PARQUET=ON -DARROW_WITH_SNAPPY=ON -DARROW_WITH_ZSTD=ON \
  -DARROW_WITH_LZ4=ON -DARROW_WITH_ZLIB=ON -DARROW_WITH_BROTLI=ON \
  -DARROW_WITH_RE2=ON -DARROW_WITH_UTF8PROC=ON

python3 $ICT/build2/preprocess_corpus.py \
  --cc-json $SRC3/arrow/_bld/compile_commands.json \
  --outdir $ICT/corpus23 --jobs 10 --log $SRC3/logs/arrow.pp.faillog
find $ICT/corpus23 -name '*.ii' | sort > $ICT/corpus23/manifest.txt
