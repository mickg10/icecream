#!/bin/bash
# corpus24 — Bitcoin Core (https://github.com/bitcoin/bitcoin)
# Preprocessed with system g++ 11.4.0.  See recipes/README.md for conventions.
set -euo pipefail
ICT=/tanksmall/scratch/ictmp
SRC3=$ICT/src3
source $SRC3/env.sh

git clone --depth 1 https://github.com/bitcoin/bitcoin.git $SRC3/bitcoin

# Cap'n Proto is a hard requirement of src/ipc/libmultiprocess (the alternative
# is -DENABLE_IPC=OFF, which drops those TUs).  conda-forge has it:
#   conda install -n cppdeps --override-channels -c conda-forge capnproto sqlite
cmake -S $SRC3/bitcoin -B $SRC3/bitcoin/_bld -G Ninja \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DBUILD_TESTS=ON -DBUILD_BENCH=ON -DBUILD_FUZZ_BINARY=ON \
  -DENABLE_WALLET=ON -DBUILD_GUI=OFF

# NOTE: ~60/681 TUs fail to preprocess from a configure-only tree because they
# include build-generated headers (mp/proxy.capnp.h from capnp codegen, and the
# bin2header outputs bench/data/*.raw.h, node/data/ip_asn.dat.h, test/data/
# *.json.h).  Run those codegen targets first if you want the last 9%.
python3 $ICT/build2/preprocess_corpus.py \
  --cc-json $SRC3/bitcoin/_bld/compile_commands.json \
  --outdir $ICT/corpus24 --jobs 10 --log $SRC3/logs/bitcoin.pp.faillog
find $ICT/corpus24 -name '*.ii' | sort > $ICT/corpus24/manifest.txt
