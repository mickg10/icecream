#!/bin/bash
# Shared definitions: corpus -> project name, source checkout(s), description.
# Sourced by the metrics/snapshot/regenerate scripts.

ICT=/tanksmall/scratch/ictmp
INFRA=$ICT/corpus-infra

CORPORA=(corpus corpus2 corpus3 corpus4 corpus5 corpus6 corpus7 corpus8 corpus9 \
         corpus10 corpus11 corpus12 corpus13 corpus14 corpus15 corpus16)

proj_of () {
  case "$1" in
    corpus)   echo "LLVM" ;;
    corpus2)  echo "RocksDB" ;;
    corpus3)  echo "DuckDB" ;;
    corpus4)  echo "abseil+protobuf" ;;
    corpus5)  echo "OpenCV" ;;
    corpus6)  echo "Godot" ;;
    corpus7)  echo "fmt" ;;
    corpus8)  echo "spdlog" ;;
    corpus9)  echo "Catch2" ;;
    corpus10) echo "nlohmann-json" ;;
    corpus11) echo "range-v3" ;;
    corpus12) echo "Eigen" ;;
    corpus13) echo "re2" ;;
    corpus14) echo "LevelDB" ;;
    corpus15) echo "simdjson" ;;
    corpus16) echo "cereal" ;;
  esac
}

# space-separated absolute source checkout paths
srcs_of () {
  case "$1" in
    corpus)   echo "$ICT/corpus/llvm-project" ;;
    corpus2)  echo "$ICT/build2/rocksdb" ;;
    corpus3)  echo "$ICT/build2/duckdb" ;;
    corpus4)  echo "$ICT/build2/abseil-cpp $ICT/build2/protobuf" ;;
    corpus5)  echo "$ICT/build2/opencv" ;;
    corpus6)  echo "$ICT/build2/godot" ;;
    corpus7)  echo "$ICT/src2/fmt" ;;
    corpus8)  echo "$ICT/src2/spdlog" ;;
    corpus9)  echo "$ICT/src2/catch2" ;;
    corpus10) echo "$ICT/src2/json" ;;
    corpus11) echo "$ICT/src2/range-v3" ;;
    corpus12) echo "$ICT/src2/eigen" ;;
    corpus13) echo "$ICT/src2/re2" ;;
    corpus14) echo "$ICT/src2/leveldb" ;;
    corpus15) echo "$ICT/src2/simdjson" ;;
    corpus16) echo "$ICT/src2/cereal" ;;
  esac
}

desc_of () {
  case "$1" in
    corpus)   echo "LLVM compiler infrastructure (llvm/ only, X86 target, Release)" ;;
    corpus2)  echo "RocksDB embedded persistent key-value store (lib + tests, Debug)" ;;
    corpus3)  echo "DuckDB in-process analytical SQL database (in-tree extensions only)" ;;
    corpus4)  echo "Abseil C++ common libraries plus protobuf runtime/compiler sources" ;;
    corpus5)  echo "OpenCV computer-vision library (all default modules)" ;;
    corpus6)  echo "Godot game engine (all C++ TUs after SCons codegen)" ;;
    corpus7)  echo "fmt header-only/compiled string formatting library (tests enabled)" ;;
    corpus8)  echo "spdlog fast C++ logging library (tests + examples)" ;;
    corpus9)  echo "Catch2 C++ unit-test framework (development build, examples + extra tests)" ;;
    corpus10) echo "nlohmann/json single-header JSON library (test suite)" ;;
    corpus11) echo "range-v3 range algorithms/views library (tests + examples)" ;;
    corpus12) echo "Eigen header-only linear-algebra template library (test suite)" ;;
    corpus13) echo "RE2 regular-expression engine (tests enabled)" ;;
    corpus14) echo "LevelDB key-value storage library (tests + benchmarks)" ;;
    corpus15) echo "simdjson SIMD JSON parser (developer mode)" ;;
    corpus16) echo "cereal header-only C++11 serialization library (tests + sandbox)" ;;
  esac
}

build_recipe_of () {
  case "$1" in
    corpus)   echo "cmake -S llvm-project/llvm -G Ninja -DCMAKE_BUILD_TYPE=Release -DLLVM_TARGETS_TO_BUILD=X86 -DLLVM_ENABLE_PROJECTS='' -DCMAKE_CXX_FLAGS=-save-temps=obj; bounded ninja build harvests *.ii next to each object" ;;
    corpus2)  echo "cmake -DCMAKE_BUILD_TYPE=Debug -DWITH_TESTS=ON (tests need Debug) -DCMAKE_EXPORT_COMPILE_COMMANDS=ON; preprocess_corpus.py replays every compile_commands.json entry through -E" ;;
    corpus3)  echo "cmake -DCMAKE_BUILD_TYPE=Release -DBUILD_UNITTESTS=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON (in-tree extensions only); preprocess_corpus.py -E replay" ;;
    corpus4)  echo "cmake abseil-cpp (-DABSL_BUILD_TESTING=ON) and protobuf (src, non-test subset via cc_pbsrc.json); preprocess_corpus.py -E replay into corpus4/{abseil,protobuf}" ;;
    corpus5)  echo "cmake opencv -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON (default module set); preprocess_corpus.py -E replay" ;;
    corpus6)  echo "scons on godot to run codegen + emit compile_commands.json (compiledb), then preprocess_corpus.py -E replay of every C++ TU" ;;
    *)        echo "src2/build_one.sh: cmake configure only (-DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_STANDARD=17 + per-project test flags), filter out googletest/benchmark/_deps entries, then preprocess_corpus.py -E replay" ;;
  esac
}

# Extensions counted as raw project source.
SRC_EXTS=(c cc cpp cxx 'c++' C h hh hpp hxx 'h++' tcc inl inc ipp ixx)

# Emit NUL-separated source file paths for a corpus to stdout.
src_files_0 () {
  local corpus="$1" d args=()
  for e in "${SRC_EXTS[@]}"; do
    args+=( -o -name "*.$e" )
  done
  unset 'args[0]'   # drop leading -o
  for d in $(srcs_of "$corpus"); do
    [ -d "$d" ] || continue
    find "$d" -type f \( "${args[@]}" \) \
      ! -path '*/.git/*' ! -path '*/build/*' ! -path '*/CMakeFiles/*' ! -name '*.ii' -print0
  done
}
