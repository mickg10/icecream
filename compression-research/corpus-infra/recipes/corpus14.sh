# corpus14 -- LevelDB key-value store, tests + benchmarks.
# Cloned --recursive: third_party/googletest and third_party/benchmark are needed to
# configure.  Their own TUs are filtered OUT of the corpus (see FILTER) so corpus14
# is LevelDB source only.
PROJECT=LevelDB
CHECKOUT=leveldb
GIT_URL=https://github.com/google/leveldb.git
GIT_REF=
CLONE_ARGS=--recursive
SRC_SUBDIR=.
MODE=ccjson
FILTER=1
PP_LIMIT=0
EXPECTED_TU=72
CMAKE_ARGS=(-DCMAKE_CXX_STANDARD=17 -DLEVELDB_BUILD_TESTS=ON -DLEVELDB_BUILD_BENCHMARKS=ON -DLEVELDB_INSTALL=OFF)
