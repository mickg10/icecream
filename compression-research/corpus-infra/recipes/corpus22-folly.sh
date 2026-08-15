#!/bin/bash
# corpus22 — Meta folly (https://github.com/facebook/folly)
# Preprocessed with system g++ 11.4.0.  See recipes/README.md for conventions.
set -euo pipefail
ICT=/tanksmall/scratch/ictmp
SRC3=$ICT/src3
source $SRC3/env.sh

git clone --depth 1 https://github.com/facebook/folly.git $SRC3/folly

# deps: conda install -n cppdeps --override-channels -c conda-forge \
#   boost-cpp gflags glog fmt double-conversion libevent lz4-c zstd snappy \
#   libsodium openssl fast_float
# fast_float is easy to miss -- CMake/folly-deps.cmake find_package(FastFloat)
# is fatal without it.
#
# The cmake binary dir MUST NOT be $SRC3/folly/build: folly ships an in-tree
# build/ SOURCE directory (FBBuildOptions.cmake etc.) that cmake would clobber,
# and the failure surfaces later as a confusing "include could not find
# requested file: FBBuildOptions".
cmake -S $SRC3/folly -B $SRC3/folly/_bld -G Ninja \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DCMAKE_CXX_STANDARD=20 -DBUILD_TESTS=OFF

python3 $ICT/build2/preprocess_corpus.py \
  --cc-json $SRC3/folly/_bld/compile_commands.json \
  --outdir $ICT/corpus22 --jobs 10 --log $SRC3/logs/folly.pp.faillog
find $ICT/corpus22 -name '*.ii' | sort > $ICT/corpus22/manifest.txt
