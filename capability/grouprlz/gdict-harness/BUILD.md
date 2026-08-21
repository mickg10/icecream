# Rebuilding the P29+BSC codec used by the generic-dict cold-start experiment

Source: `/tmp/issue16-p29-bsc-integration-20260817/src/` (local-oracle's P29+BSC integration),
copied to `~/gdict/src/` on quietbox2. `codec50-bigcap.patch` makes the two fixed interner
capacities overridable; nothing else is changed.

```sh
# MT-enabled zstd 1.4.8 (Ubuntu's system libzstd 1.4.8 is built WITHOUT ZSTD_MULTITHREAD and
# rejects ZSTD_c_nbWorkers, which the P29 blob lane sets)
curl -sL https://github.com/facebook/zstd/archive/refs/tags/v1.4.8.tar.gz | tar xz
make -C zstd-1.4.8/lib -j16 ZSTD_MULTITHREAD=1 libzstd.a CFLAGS="-O3 -fPIC -DZSTD_MULTITHREAD"

g++ -O3 -std=c++23 -march=znver3 \
    -DICE_LINE_CAP_LOG2=25 -DICE_SHORT_CAP_LOG2=23 -DICE_TINY_CAP_LOG2=16 \
    -DWITH_BSC_GROUPS -pthread \
    -I~/libbsc/libbsc -I zstd-1.4.8/lib \
    codec50-bigcap.cpp -o codec50-bigcap-mt \
    ~/grouprlz/libbsc.a zstd-1.4.8/lib/libzstd.a -lz
```

Anchor (must hold, byte for byte):

```sh
./run_alt.sh rocksdb_check /home/ttuser/ictmp/corpus2/manifest.txt   # TOTAL=8537705
```

Both the stock binary, the plain rebuild and the big-capacity MT rebuild all produce
`TOTAL=8537705` on RocksDB and `TOTAL=37391954` on Firefox.
