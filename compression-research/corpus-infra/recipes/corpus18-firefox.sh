#!/bin/bash
# corpus18 — Mozilla Firefox / gecko-dev (https://github.com/mozilla/gecko-dev)
# NOTE: Firefox builds with clang (conda clang21 here), not system g++ 11.4.
set -euo pipefail
ICT=/tanksmall/scratch/ictmp
SRC3=$ICT/src3
CPP=/tanksmall/MICKG2/mickg/miniconda3/envs/cppdeps
CL=/tanksmall/MICKG2/mickg/miniconda3/envs/clang21

git clone --depth 1 https://github.com/mozilla/gecko-dev.git $SRC3/gecko-dev
cd $SRC3/gecko-dev

# `mach bootstrap` does NOT provision a toolchain on this box (it tries
# `sudo apt-get` and leaves ~/.mozbuild/toolchains empty), so point mach at
# conda clang21.  System /usr/bin/clang++ is unusable: clang-14 selects a
# gcc-12 that has no C++ headers installed, so even <cstddef> is not found.
cat > mozconfig.corpus <<CFG
mk_add_options MOZ_OBJDIR=@TOPSRCDIR@/obj-cc
ac_add_options --disable-bootstrap
ac_add_options --without-wasm-sandboxed-libraries
ac_add_options --enable-application=browser
export CC=$CL/bin/clang
export CXX=$CL/bin/clang++
CFG

# deps: conda install -n cppdeps -c conda-forge alsa-lib gtk3 pulseaudio-client pixman nasm
# cbindgen is not on conda-forge; build it with the system rust (1.97):
#   cargo install cbindgen --root $SRC3/cargo-tools
#
# Run mach with the mta python 3.11.  Putting $CPP/bin first on PATH gives mach
# conda's python 3.14, which it rejects (mach supports <=3.12) and then dies in
# command_util.py with AttributeError: 'Constant' object has no attribute 's'.
export PATH=$PATH:$SRC3/cargo-tools/bin:$CPP/bin
export PKG_CONFIG_PATH=$CPP/lib/pkgconfig:$CPP/share/pkgconfig
export MOZCONFIG=$PWD/mozconfig.corpus
MACHPY=/tanksmall/MICKG2/mickg/miniconda3/envs/mta/bin/python3

$MACHPY ./mach --no-interactive configure
$MACHPY ./mach --no-interactive build-backend -b CompileDB   # -> obj-cc/compile_commands.json

# REQUIRED: CompileDB alone is not preprocessable -- every TU is force-included
# with obj-cc/mozilla-config.h, which only the build generates.  The `export`
# tier produces it plus the generated/ headers, without compiling objects.
$MACHPY ./mach --no-interactive build export

python3 $SRC3/preprocess_corpus_v2.py \
  --cc-json $SRC3/gecko-dev/obj-cc/compile_commands.json \
  --outdir $ICT/corpus18 --jobs 10 --log $SRC3/logs/gecko-dev.pp.faillog
find $ICT/corpus18 -name '*.ii' | sort > $ICT/corpus18/manifest.txt
