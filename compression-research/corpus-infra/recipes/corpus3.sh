# corpus3 -- DuckDB in-process analytical SQL database.
# GOTCHA: only the IN-TREE extensions are built.  Anything that pulls extensions over
# the network at configure time (autoload/autoinstall) is turned OFF -- otherwise the
# corpus depends on what a remote extension repo served that day.
PROJECT=DuckDB
CHECKOUT=duckdb
GIT_URL=https://github.com/duckdb/duckdb.git
GIT_REF=
CLONE_ARGS=
SRC_SUBDIR=.
MODE=ccjson
FILTER=1
PP_LIMIT=0
EXPECTED_TU=689
CMAKE_ARGS=(
  -DCMAKE_CXX_STANDARD=17
  -DBUILD_UNITTESTS=ON -DBUILD_TESTING=ON -DENABLE_UNITTEST_CPP_TESTS=ON
  -DBUILD_COMPLETE_EXTENSION_SET=ON -DBUILD_EXTENSIONS_ONLY=OFF
  -DENABLE_EXTENSION_AUTOLOADING=OFF -DENABLE_EXTENSION_AUTOINSTALL=OFF
  -DBUILD_BENCHMARKS=OFF -DBUILD_SHELL=ON
)
