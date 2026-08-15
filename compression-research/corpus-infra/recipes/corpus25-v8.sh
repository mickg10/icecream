#!/bin/bash
# corpus25 — V8 (https://chromium.googlesource.com/v8/v8)
# NOTE: V8 builds with the chromium-bundled clang against the bundled debian
# bullseye sysroot, NOT system g++ 11.4 -- gn does not offer a supported way to
# swap that out.  So this corpus's system-header text differs from the others.
set -euo pipefail
ICT=/tanksmall/scratch/ictmp
SRC3=$ICT/src3

git clone --depth 1 https://chromium.googlesource.com/chromium/tools/depot_tools $SRC3/depot_tools
mkdir -p $SRC3/v8src && cd $SRC3/v8src
PATH=$SRC3/depot_tools:$PATH fetch --no-history v8

cd $SRC3/v8src/v8
# Do NOT pass use_custom_libcxx=false: it trips
#   assert(!v8_enable_sandbox || use_safe_libcxx) "sandbox requires libc++ hardening"
PATH=$SRC3/depot_tools:$PATH ./buildtools/linux64/gn gen out/x64 \
  --args='is_debug=false target_cpu="x64" v8_enable_i18n_support=false treat_warnings_as_errors=false use_sysroot=true' \
  --export-compile-commands

# REQUIRED: without codegen only ~107/3801 TUs preprocess -- 182 failures are
# torque-generated/instance-types.h and 109 are
# builtins-generated/bytecodes-builtins-list.h.  Asking ninja for every
# generated .h/.inc builds the torque binary and runs it.
mapfile -t H < <(ninja -C out/x64 -t targets all | awk -F: '{print $1}' | grep -E '\.(h|inc)$' | sort -u)
ninja -C out/x64 -j6 -k 0 "${H[@]}"

# preprocess_corpus_v2.py, not the shared driver: gn compile lines carry
# `-MD -MF obj/<target>/<x>.o.d`, and with -c/-o swapped for -E the object
# directory does not exist, so clang dies with "error opening obj/.../x.o.d"
# before preprocessing.  v2 strips -MD/-MMD/-MF/-MT/-MQ.
python3 $SRC3/preprocess_corpus_v2.py \
  --cc-json $SRC3/v8src/v8/out/x64/compile_commands.json \
  --outdir $ICT/corpus25 --jobs 10 --log $SRC3/logs/v8src.pp.faillog
find $ICT/corpus25 -name '*.ii' | sort > $ICT/corpus25/manifest.txt
