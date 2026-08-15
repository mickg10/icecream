#!/bin/bash
# corpus19 — Qt 6 qtbase (https://github.com/qt/qtbase)
# Preprocessed with system g++ 11.4.0.  See recipes/README.md for conventions.
set -euo pipefail
ICT=/tanksmall/scratch/ictmp
SRC3=$ICT/src3
source $SRC3/env.sh

git clone --depth 1 https://github.com/qt/qtbase.git $SRC3/qtbase

# INPUT_opengl=no: Qt's OpenGL functionality test fails on this box (no system
# GL dev files, no sudo to add them).  Everything else, incl. Gui/Widgets,
# still configures.
cmake -S $SRC3/qtbase -B $SRC3/qtbase/_bld -G Ninja \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DQT_BUILD_TESTS=OFF -DQT_BUILD_EXAMPLES=OFF \
  -DINPUT_opengl=no

# REQUIRED: configure alone is not preprocessable.  Every Qt TU includes
# forwarding headers (<QtCore/qtconfigmacros.h> etc.) that syncqt generates
# into _bld/include during the build, and Qt .cpp files #include their own moc
# output.  Build the codegen targets only -- no object files.
cd $SRC3/qtbase/_bld
ninja -j6 -k 0 sync_headers
mapfile -t AG < <(ninja -t targets all | grep -oE '^[A-Za-z0-9_/.-]+_autogen' | sort -u)
ninja -j6 -k 0 "${AG[@]}" || true   # a few autogen targets fail; harmless

python3 $ICT/build2/preprocess_corpus.py \
  --cc-json $SRC3/qtbase/_bld/compile_commands.json \
  --outdir $ICT/corpus19 --jobs 10 --log $SRC3/logs/qtbase.pp.faillog
find $ICT/corpus19 -name '*.ii' | sort > $ICT/corpus19/manifest.txt
