# corpus2 -- RocksDB embedded key-value store.
# GOTCHA: WITH_ALL_TESTS/WITH_TESTS only take effect in a Debug build; a Release
# configure silently drops every test TU and the corpus collapses to the library.
# That is why this one corpus is CMAKE_BUILD_TYPE=Debug.
PROJECT=RocksDB
CHECKOUT=rocksdb
GIT_URL=https://github.com/facebook/rocksdb.git
GIT_REF=
CLONE_ARGS=
SRC_SUBDIR=.
MODE=ccjson
FILTER=1
PP_LIMIT=0
EXPECTED_TU=622
CMAKE_BUILD_TYPE=Debug
CMAKE_ARGS=(
  -DWITH_TESTS=ON -DWITH_ALL_TESTS=ON -DWITH_TOOLS=ON -DWITH_CORE_TOOLS=ON
  -DWITH_BENCHMARK_TOOLS=OFF -DWITH_EXAMPLES=OFF -DWITH_GFLAGS=OFF
  -DWITH_SNAPPY=OFF -DWITH_LZ4=OFF -DWITH_ZLIB=OFF -DWITH_BZ2=OFF -DWITH_ZSTD=OFF
  -DFAIL_ON_WARNINGS=OFF -DPORTABLE=0 -DROCKSDB_BUILD_SHARED=OFF
)
