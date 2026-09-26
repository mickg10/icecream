# Reproducible paired preprocessor inputs for P50

`tools/p50_regenerate_paired_corpus.py` regenerates fresh A and B preprocessed
translation units from one compile database and one pinned source tree. It
does not pair an archived A file with a freshly generated B file. It preserves
the TU identity/order, records input and output digests, and reports whether
fresh A reproduces each archived input exactly.

The supported source-edit contract is intentionally narrow: A and B roots must
be the same pinned checkout, and B edits are supplied as a sparse overlay at
the original relative source paths. Clang's VFS overlay maps the edited bytes
to the original virtual filename, preserving `__FILE__`, line markers, and
quoted-include lookup. The generated preprocessor output is never rewritten.
The generator rejects a different A/B source root rather than silently mixing
include trees. Only selected translation-unit source files may appear in the
sparse overlay; header edits and non-selected source edits are unsupported and
rejected, not silently ignored. Its byte cap is checked during selection and
after each output; both manifests are published only after both complete
corpora fit.

Example (paths should be fresh, task-owned paths; an existing output directory
is rejected):

```sh
uv run python tools/p50_regenerate_paired_corpus.py \
  --compile-commands /path/to/build/compile_commands.json \
  --original-source-root /path/to/pinned/source \
  --build-root /path/to/build \
  --archive-root /path/to/archive \
  --archive-manifest /path/to/archive/manifest.txt \
  --source-root-a /path/to/pinned/source \
  --source-root-b /path/to/pinned/source \
  --overlay-b-root /path/to/sparse-b-overlay \
  --output-dir /path/to/new-output-directory \
  --expected-revision FULL_GIT_SHA --count 32 --max-bytes 503316480
```

## Bounded RocksDB sample evidence

The checked sample used ClickHouse's vendored RocksDB at revision
`6d0a17390c451dac5db306436a6033a6be71718f`, with its matching
`_bld/compile_commands.json` and archived corpus manifest. It selected the
largest 32 archived TUs matching `contrib/rocksdb/`: 279,514,742 archived
bytes (below the 480 MiB generator cap and 512 MiB benchmark limit). The
archive has 318 RocksDB-named entries; 317 are source TUs selected by the
source-path filter, while the additional generated `build_version.cc` is not
part of that source subset.

Fresh A matched all 32 archived TUs byte-for-byte. B had one controlled
source-level edit: an appended `static_assert(sizeof(void*) >= 4, ...)` in
`db_impl.cc`. This checks pairing/provenance and preprocessing, not a natural
developer workload or a meaningful product change. B differed from archived A
for exactly that one TU; all other B TUs matched. This is corpus preparation
evidence only, not a transfer timing result or a compiler-speed claim.

The retained generator run is outside the source tree at
`/tanksmall/scratch/tmp/p51-regenerate-probe/rocksdb-top32-v1`. Its metadata
SHA-256 is `82e8e59c60ecd7fdf881512fac34e6a7bc91341ab9ec6d6a95e4adcbdc857f7c`;
the 32-entry A and B manifest SHA-256 values are respectively
`14e6dd2fdbd9148d78da482cb6f690d8f0c55408f3f9b298a34d20fec80f6e77` and
`58960e5a144265637b40531131fadaf252174730d3e3747489e824d9512c7688`.
The generator log SHA-256 is
`0b93f27df1e809c4e16ff72384a08df09ad803c841af6ebf4e97494cc0d1db5b`.

## RocksDB32 paired transfer matrix

This is a bounded local-loopback measurement on the generated 32-TU A/B
corpus, not a cross-host result or a natural edit workload. It ran three
repetitions for each profile and mode: R1/W1 plus R2/W1, W2, W4, W8, W16,
and W30. Each cell performs A-fresh, A-retained, and B-edited passes in that
order and verifies exact decoded bytes. The controlled edit is the single
`db_impl.cc` static assertion described above; 1 raw digest changed and 31
were unchanged. Each cell verified 838,544,291 decoded bytes: A-fresh
279,514,742, A-retained 279,514,742, B-edited 279,514,807. Filesystem cache
state was inherited and uncontrolled. Process CPU includes all benchmark
threads and does not isolate codec workers.

The run reused the exact Firefox32 benchmark binary SHA-256
`fbf9c90ddf8a1dfc132b240802fbaaefb2c2b1bbb6d9c69b3b698e11688ee4ec` and
runtime image `icecream-dev:current-da9f52155b23085c`, image ID
`sha256:f4620c324a32d6324ebc3958db7377fa6a3b2db8b52e46c63e49513414a5f7ab`,
under 2 CPU/8 GiB limits. The matrix runner SHA-256 is
`dc9f93eaaa17d7bdec9cede348502804a41317d78c6d35285918170484c11144`.
The retained output directory is
`/tanksmall/scratch/tmp/p51-rocksdb32-paired.AsJH6C`; its log SHA-256 is
`fa6f803fd96b53b89e701e4bb1c475267641634c811553ddec861a8dee3bc386` and
ordered input TSV SHA-256 is
`49274d63bcce5da30308b9c9d1ec4162f4d8678b1c2f12e1dcca1b09e42d0f5f`.
The log records all 63 cell exits as `rc=0` and `MATRIX_EXIT=0`. It also
records the exact A/B manifest hashes and metadata hash above.

All timing and first-commit cells below are medians over the three
repetitions, with observed `[min,max]`. Values within each triplet are
fresh/retained/edited. Peak outstanding is the maximum seen across the three
repetitions for each pass (not the median); R1 does not expose the R2 window
metric. CacheWire is the per-cell C→F/F→C total, with median `[min,max]`.
Profile CLI indexes are 0=ZSTD_TU, 1=P29V1, and 2=ZSTD_ROUTE. Emitted
profile IDs differ and are the names in the first column.

| Mode | Pass wall ms F/R/E | Process CPU ms F/R/E | First commit µs F/R/E | Peak outstanding max F/R/E | CacheWire C→F / F→C bytes |
|---|---:|---:|---:|---:|---:|
| ZSTD_TU R1/W1 | 1927[1789,1964]/1941[1888,2116]/2272[2151,2318] | 1924[1795,1967]/1948[1892,2125]/2280[2154,2315] | 77178[69596,84756]/60683[57978,62381]/68459[67529,73190] | n/a | 100651575[100651575,100651575]/22575[22575,22575] |
| ZSTD_TU R2/W1 | 1918[1898,1996]/1593[1587,1662]/1673[1458,1724] | 1919[1900,2018]/1620[1586,1680]/1696[1463,1723] | 111076[99419,111610]/101998[78248,105281]/222750[198570,343290] | 1/1/1 | 100666708[100666708,100666708]/11352[11352,11352] |
| ZSTD_TU R2/W2 | 837[800,876]/850[798,976]/885[879,976] | 1458[1421,1537]/1473[1406,1564]/1572[1508,1625] | 102993[100550,116298]/90678[83364,97395]/81208[79004,84032] | 2/2/2 | 100666708[100666708,100666708]/11352[11352,11352] |
| ZSTD_TU R2/W4 | 831[826,881]/875[836,917]/970[956,1081] | 1421[1386,1518]/1484[1476,1579]/1660[1603,1685] | 107281[94416,108709]/93166[92892,99862]/81397[77268,97497] | 2/2/2 | 100666180[100666048,100666312]/11352[11352,11352] |
| ZSTD_TU R2/W8 | 803[794,879]/805[778,966]/1051[933,1120] | 1464[1333,1505]/1490[1423,1590]/1732[1538,1789] | 101904[100222,106534]/95304[80265,99768]/87844[79910,89597] | 2/2/2 | 100666136[100666092,100666620]/11352[11352,11352] |
| ZSTD_TU R2/W16 | 870[777,873]/874[869,927]/986[981,1046] | 1471[1371,1551]/1493[1439,1559]/1662[1586,1678] | 95455[91251,113174]/90607[89654,91949]/84780[84068,85098] | 4/2/2 | 100666356[100666268,100666532]/11352[11352,11352] |
| ZSTD_TU R2/W30 | 799[783,880]/766[760,802]/942[803,949] | 1398[1332,1413]/1320[1316,1401]/1526[1385,1552] | 101322[94668,106625]/84313[83378,89694]/75472[75102,86402] | 2/2/2 | 100666356[100666048,100666400]/11352[11352,11352] |
| P29V1 R1/W1 | 1076[956,1312]/858[563,919]/793[660,882] | 1076[954,1309]/854[556,924]/786[660,880] | 303469[292826,336419]/26656[19969,30751]/24109[23550,27899] | n/a | 1592571[1592571,1592571]/44869[44869,44869] |
| P29V1 R2/W1 | 1154[1116,1212]/810[729,819]/779[702,816] | 1156[1119,1225]/810[737,820]/783[707,820] | 356390[331335,370386]/66588[64032,71893]/68909[58801,74209] | 1/1/1 | 1607704[1607704,1607704]/11352[11352,11352] |
| P29V1 R2/W2 | 790[759,819]/435[406,456]/444[403,456] | 1114[1046,1122]/662[645,670]/700[647,704] | 358061[337946,366183]/60401[60098,63596]/62085[60520,66495] | 2/2/2 | 1607704[1607704,1607738]/11352[11352,11352] |
| P29V1 R2/W4 | 766[763,840]/449[376,519]/489[351,508] | 1107[1083,1175]/683[658,806]/771[575,795] | 338671[337653,377990]/70306[63632,71121]/67083[58874,70975] | 4/4/4 | 1607660[1607528,1607704]/11352[11352,11352] |
| P29V1 R2/W8 | 801[751,831]/438[402,547]/501[476,524] | 1140[1128,1171]/697[608,765]/745[708,806] | 357057[347223,360663]/61692[57835,62822]/64431[56198,70477] | 8/8/8 | 1607660[1607396,1607660]/11352[11352,11352] |
| P29V1 R2/W16 | 752[729,807]/404[360,484]/401[349,475] | 1102[1083,1157]/627[601,710]/616[607,717] | 340723[338004,351548]/61096[58890,63534]/66601[59966,80282] | 12/16/16 | 1607660[1607484,1607704]/11352[11352,11352] |
| P29V1 R2/W30 | 709[706,809]/409[398,469]/418[392,447] | 1057[1054,1200]/636[623,727]/668[668,686] | 336862[333540,345103]/63534[59624,68691]/56345[55737,65703] | 10/14/15 | 1607616[1607572,1607660]/11352[11352,11352] |
| ZSTD_ROUTE R1/W1 | 12250[12093,12258]/12524[12430,12567]/12092[11964,13138] | 12207[12067,12211]/12516[12409,12533]/12085[11932,13020] | 99091[88258,99865]/193633[193167,194611]/198497[190230,204078] | n/a | 79859485[79859485,79859485]/22575[22575,22575] |
| ZSTD_ROUTE R2/W1 | 11591[11429,11795]/12348[12202,12554]/12022[11825,12188] | 11582[11366,11797]/12348[12211,12547]/12033[11837,12191] | 121144[119259,131709]/342839[332750,419931]/340361[339261,516963] | 1/1/1 | 79874618[79874618,79874618]/11352[11352,11352] |
| ZSTD_ROUTE R2/W2 | 9517[9492,9584]/10059[9797,10066]/9910[9793,10281] | 11077[10788,11139]/11361[11246,11626]/11242[11194,12035] | 145379[131994,151593]/337390[322459,368113]/332144[329659,394151] | 2/1/2 | 79874618[79874618,79874618]/11352[11352,11352] |
| ZSTD_ROUTE R2/W4 | 9487[9400,9587]/10117[9731,10440]/10004[9804,10735] | 11058[10887,11242]/11733[11208,11826]/11469[11322,12562] | 133907[131101,133907]/343441[325812,352083]/335590[325125,526997] | 1/1/2 | 79874618[79874574,79874618]/11352[11352,11352] |
| ZSTD_ROUTE R2/W8 | 9455[9356,9561]/10111[9818,10329]/10145[10016,10250] | 10884[10875,11139]/11679[11247,12040]/11716[11648,11800] | 142077[129804,142633]/347302[342770,420542]/384111[338754,537051] | 1/1/2 | 79874618[79874574,79874618]/11352[11352,11352] |
| ZSTD_ROUTE R2/W16 | 9522[9034,9849]/10249[9758,10364]/9865[9684,10644] | 11075[10513,11276]/11577[11299,12109]/11151[11097,12469] | 142114[129090,145786]/329604[322753,404900]/346341[336520,506520] | 1/2/2 | 79874618[79874530,79874618]/11352[11352,11352] |
| ZSTD_ROUTE R2/W30 | 9690[9519,10087]/10229[10109,10440]/10460[10069,11030] | 11272[10959,11762]/11798[11732,12077]/12127[11659,12804] | 137969[121812,143746]/416853[330201,423629]/505627[456596,547111] | 1/1/1 | 79874618[79874618,79874618]/11352[11352,11352] |

R1 submits serially, whereas R2 submits concurrently. Per-job latency includes
different queueing and is not a like-for-like comparison; whole-pass wall time
does compare completion of the same ordered input workload. In R2, the
maximum observed peak at configured W30 was 2/2/2 for ZSTD_TU, 10/14/15 for
P29V1, and 1/1/1 for ZSTD_ROUTE. Thus the ROUTE workload did not approach W30;
this matrix does not demonstrate saturation or a universal window speedup.

## Three-corpus P29V1 paired window comparison

The table compares the same frozen benchmark binary across the Firefox32,
RocksDB32, and ClickHouse programs32 preprocessed-input matrices. Values are
median pass wall time in milliseconds over three repetitions, each triplet
fresh/retained/edited; bracketed values are observed min/max. For the complete
ClickHouse raw 63-cell table and provenance, see
[`p50-window-clickhouse-programs32-20260926.json`](../measurements/p50-window-clickhouse-programs32-20260926.json).

| Corpus | R1/W1 fresh/retained/edited ms | R2/W1 fresh/retained/edited ms | R2/W30 fresh/retained/edited ms |
|---|---|---|---|
| Firefox32 | 1303[1236..1410] / 687[645..771] / 890[727..954] | 1276[1215..1361] / 655[625..667] / 634[620..741] | 791[783..934] / 374[324..457] / 350[332..430] |
| RocksDB32 | 1076[956..1312] / 858[563..919] / 793[660..882] | 1154[1116..1212] / 810[729..819] / 779[702..816] | 709[706..809] / 409[398..469] / 418[392..447] |
| ClickHouse programs32 | 1452[1323..1524] / 1009[965..1051] / 1111[988..1165] | 1376[1326..1413] / 859[845..917] / 961[874..988] | 961[940..983] / 490[490..524] / 490[428..542] |

The P29V1 comparison uses emitted profile ID 1 (CLI profile index 1), not
profile ID 2 (ZSTD_TU). These are descriptive paired measurements, not an
isolated causal estimate: R1 and R2 have different submission behavior, OS
cache state is inherited/uncontrolled, and W30 is a cap rather than guaranteed
occupancy. In particular, observed P29V1 peaks at W30 were below 30 in all
three corpora. Do not generalize to cross-host throughput or full project
build performance.
