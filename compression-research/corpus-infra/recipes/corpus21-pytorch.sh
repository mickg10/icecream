#!/bin/bash
# corpus21 — PyTorch (https://github.com/pytorch/pytorch)
# Preprocessed with system g++ 11.4.0.  See recipes/README.md for conventions.
set -euo pipefail
ICT=/tanksmall/scratch/ictmp
SRC3=$ICT/src3
source $SRC3/env.sh

git clone --depth 1 https://github.com/pytorch/pytorch.git $SRC3/pytorch
cd $SRC3/pytorch && git submodule update --init --recursive --depth 1 --jobs 4

# Torch's codegen wants pyyaml + typing_extensions; the mta env already has
# both and we only READ it (never `conda install -n mta`, other agents use it).
PTPY=/tanksmall/MICKG2/mickg/miniconda3/envs/mta/bin/python3
cmake -S $SRC3/pytorch -B $SRC3/pytorch/_bld -G Ninja \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DPython_EXECUTABLE=$PTPY -DPython3_EXECUTABLE=$PTPY \
  -DUSE_CUDA=OFF -DUSE_ROCM=OFF -DUSE_DISTRIBUTED=OFF -DBUILD_PYTHON=OFF \
  -DUSE_NUMPY=OFF -DBUILD_TEST=ON -DUSE_MKLDNN=OFF -DUSE_KINETO=OFF \
  -DUSE_OPENMP=ON -DUSE_XNNPACK=ON -DUSE_FBGEMM=ON

# REQUIRED: ATen is code-generated.  Straight after configure only 712/1913 TUs
# preprocess; 982 of the failures are "ATen/core/TensorBody.h: No such file",
# plus sleef.h and the onnx protobuf headers.  Codegen only, no object files.
cd $SRC3/pytorch/_bld
ninja -j6 -k 0 ATEN_CPU_FILES_GEN_TARGET include/sleef.h
mapfile -t PB < <(ninja -t targets all | awk -F: '{print $1}' | grep -E '\.pb\.h$' | grep -v '^/' | sort -u)
ninja -j6 -k 0 "${PB[@]}" || true
# -> 1742/1913

python3 $ICT/build2/preprocess_corpus.py \
  --cc-json $SRC3/pytorch/_bld/compile_commands.json \
  --outdir $ICT/corpus21 --jobs 10 --log $SRC3/logs/pytorch.pp.faillog
find $ICT/corpus21 -name '*.ii' | sort > $ICT/corpus21/manifest.txt
