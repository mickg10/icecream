# Paired transfer-window benchmark: Firefox32 sample

This is a bounded local paired-workload measurement, not completion of the
performance gate in `p50-transfer-concurrency.md` §9.4. It records a 32-TU
slice of the preserved Firefox turn-A/turn-B corpus. No external farm, remote
host, or network transport was involved.

## Provenance and workload

- Three profiles, three repetitions each, seven modes per profile: R1 and R2
  windows 1, 2, 4, 8, 16, and 30; 63 cells total. Every cell runs A-fresh,
  A-retained, and B-edited in the same order and verifies decoded raw bytes.
- Input order and TU identifiers were matched by the paired manifest runner.
  Of the first 32 ordered entries, 31 raw digests changed and one was
  unchanged. A contains 243,009,841 raw bytes; B contains 243,009,872.
  Each cell reports 729,029,554 decoded bytes across its three passes.
- Full manifests: `turnA.manifest` SHA-256
  `97fbae891cd55646d1f12d5ea6bf3ca13e39a726a47f523d1e0647d74a3bf0d4`;
  `turnB.manifest` SHA-256
  `1afd696d6d421dc8c2a0a6311f9753ae2a1ef679abb2ff9c465eceb2e9ab7e18`.
- The per-input ordered identity/size/digest record is
  `firefox32-paired-inputs.tsv`, SHA-256
  `fd1fe4f75fbf063480c6d8e040816c646608436d76398a2aaa989027687a2296`.
- Production source closure is the current primary production closure at
  `0dc29331` (the intervening primary changes through `bb7dd504` only changed
  benchmark/test-state documentation and this benchmark TU). The benchmark
  TU SHA-256 is
  `364e1bf333f71d385747cb08f21efb4a904a99dc1a926018ba6a81928fa0c537`;
  binary SHA-256 is
  `fbf9c90ddf8a1dfc132b240802fbaaefb2c2b1bbb6d9c69b3b698e11688ee4ec`.
- Runtime: SDK image `icecream-dev:current-da9f52155b23085c`, image ID
  `sha256:f4620c324a32d6324ebc3958db7377fa6a3b2db8b52e46c63e49513414a5f7ab`,
  container limited to 2 CPUs and 8 GiB memory. Filesystem cache state was
  inherited and uncontrolled; this is not a cold-cache experiment. The
  process CPU metric aggregates all benchmark process threads; it does not
  split client, endpoint, and codec-worker CPU and is not a cycle count.
- Runner SHA-256:
  `5bb9007fe6c6b21b86ae4a53089a27e91712f2d97af935aa96dcca1c63eef8aa`.
  Reproduction entry point (with the same read-only corpus mounts and writable
  result/tmp mounts) is:

  ```sh
  bash /tanksmall/scratch/tmp/p51-bench-paired-default/run_firefox32_paired_matrix.sh \
    /tanksmall/scratch/tmp/p51-bench-paired-current-build/build/unittests/p50transferwindowbench \
    /tanksmall/scratch/tmp/p51-bench-paired-current-build/results/firefox32-r1
  ```

## Results

Each triplet is fresh / retained / edited. Wall and CPU entries are medians
across the three repetitions with observed `[min..max]` in brackets. First
commit entries are median microseconds with `[min..max]`. Wire entries are
CacheWire bytes across all three passes, median `[min..max]`; C→F and F→C are
shown separately. R1 has no R2 outstanding-window metric. For R2, observed
fresh / retained / edited peaks are median `[min..max]`; these are observations,
not promises to fill the configured window.

| Mode | Pass wall ms F/R/E | Process CPU ms F/R/E | First commit µs F/R/E | Observed peak F/R/E | CacheWire C→F / F→C bytes |
|---|---:|---:|---:|---:|---:|
| R1 P1 | 1303[1236..1410] / 687[645..771] / 890[727..954] | 1306[1236..1406] / 691[644..758] / 893[726..948] | 418294[387955..425518] / 36471[29261..39480] / 34186[30165..37457] | n/a | 3609887[3609887..3609887] / 66003[66003..66003] |
| R1 P2 | 1467[1449..1824] / 1548[1466..1921] / 1718[1578..1994] | 1474[1447..1845] / 1541[1461..1937] / 1686[1589..2016] | 84302[80892..100789] / 91906[80403..93545] / 85035[70192..93644] | n/a | 109592724[109592724..109592724] / 22575[22575..22575] |
| R1 P3 | 12018[11749..12237] / 12548[12391..12975] / 12155[12137..12565] | 11980[11725..12210] / 12541[12348..12965] / 12165[12142..12525] | 118177[109364..126825] / 227890[224858..276904] / 227754[227013..235512] | n/a | 97416199[97416199..97416199] / 22575[22575..22575] |
| R2 P1 W1 | 1276[1215..1361] / 655[625..667] / 634[620..741] | 1267[1233..1388] / 659[629..677] / 645[625..754] | 438805[438163..440164] / 65016[63722..65074] / 66684[60327..67178] | 1[1..1] / 1[1..1] / 1[1..1] | 3625020[3625020..3625020] / 11352[11352..11352] |
| R2 P1 W2 | 1033[1028..1105] / 467[443..473] / 419[391..477] | 1316[1304..1390] / 647[642..661] / 617[575..685] | 446120[436572..464062] / 66507[54294..71855] / 60914[55358..61328] | 2[2..2] / 2[2..2] / 2[2..2] | 3625020[3625020..3625020] / 11352[11352..11352] |
| R2 P1 W4 | 860[850..966] / 392[377..498] / 429[423..442] | 1311[1252..1439] / 646[564..768] / 667[632..699] | 451570[400344..493948] / 63240[62623..70537] / 61829[53452..72426] | 4[4..4] / 4[4..4] / 4[4..4] | 3624492[3624487..3624756] / 11352[11352..11352] |
| R2 P1 W8 | 899[857..914] / 341[331..446] / 373[314..388] | 1335[1287..1371] / 562[527..694] / 621[487..626] | 473478[414922..477454] / 55143[51593..60184] / 55692[55430..56423] | 7[6..8] / 8[7..8] / 8[8..8] | 3624492[3624360..3624624] / 11352[11352..11352] |
| R2 P1 W16 | 845[824..908] / 430[377..461] / 379[362..435] | 1258[1246..1327] / 637[623..663] / 616[550..633] | 428308[412407..450767] / 63305[57251..72561] / 62423[55381..69220] | 7[6..12] / 13[10..16] / 13[8..14] | 3624404[3624184..3624536] / 11352[11352..11352] |
| R2 P1 W30 | 791[783..934] / 374[324..457] / 350[332..430] | 1188[1186..1396] / 634[520..671] / 540[530..665] | 444266[423435..445862] / 62600[55182..66336] / 51107[47805..57519] | 6[6..7] / 10[10..16] / 12[10..13] | 3624272[3624140..3624492] / 11352[11352..11352] |
| R2 P2 W1 | 1324[1269..1700] / 1235[1232..1418] / 1365[1275..1564] | 1274[1262..1702] / 1250[1248..1452] / 1354[1281..1594] | 113620[106846..127291] / 103310[91021..105917] / 88166[81542..327034] | 1[1..1] / 1[1..1] / 1[1..1] | 109607857[109607857..109607857] / 11352[11352..11352] |
| R2 P2 W2 | 999[941..1015] / 927[901..976] / 1018[976..1102] | 1355[1299..1411] / 1291[1259..1389] / 1386[1338..1598] | 106549[105216..119559] / 89329[88228..89973] / 86331[85898..94044] | 2[2..2] / 2[2..2] / 2[2..2] | 109607857[109607857..109607857] / 11352[11352..11352] |
| R2 P2 W4 | 837[817..939] / 858[827..871] / 1040[1024..1047] | 1308[1258..1437] / 1293[1274..1424] / 1486[1472..1676] | 104034[98351..124519] / 94133[82064..100223] / 95083[92994..100772] | 3[3..4] / 3[2..4] / 3[2..3] | 109606713[109606713..109606757] / 11352[11352..11352] |
| R2 P2 W8 | 887[813..968] / 854[798..1027] / 981[920..1080] | 1300[1278..1570] / 1281[1277..1630] / 1478[1341..1719] | 110119[99990..124106] / 97895[91230..125552] / 93964[83614..97808] | 3[3..5] / 4[3..4] / 2[2..4] | 109606493[109606493..109606757] / 11352[11352..11352] |
| R2 P2 W16 | 858[814..891] / 856[794..891] / 1050[1018..1106] | 1409[1301..1443] / 1378[1374..1457] / 1622[1550..1761] | 110511[108304..127479] / 103983[101424..104312] / 92002[84458..98994] | 4[4..4] / 4[4..5] / 3[2..3] | 109606581[109606537..109606713] / 11352[11352..11352] |
| R2 P2 W30 | 882[877..938] / 948[909..972] / 999[972..1054] | 1411[1347..1531] / 1459[1430..1478] / 1557[1525..1610] | 129065[110489..130317] / 94056[90617..104589] / 101026[90184..118730] | 4[4..5] / 3[2..3] / 3[3..5] | 109606625[109606581..109606845] / 11352[11352..11352] |
| R2 P3 W1 | 11925[11746..12147] / 12544[12440..12703] / 11983[11983..12442] | 11863[11737..12156] / 12551[12405..12697] / 11994[11979..12454] | 163007[154066..172452] / 400727[339246..402415] / 374662[350361..492917] | 1[1..1] / 1[1..1] / 1[1..1] | 97431332[97431332..97431332] / 11352[11352..11352] |
| R2 P3 W2 | 10154[9788..10241] / 10039[9962..10351] / 9891[9832..10422] | 11519[11217..11750] / 11483[11418..11838] / 11312[11237..11789] | 169422[162920..178681] / 413123[412797..419888] / 504247[486935..511823] | 1[1..1] / 1[1..1] / 1[1..1] | 97431332[97431332..97431332] / 11352[11352..11352] |
| R2 P3 W4 | 9915[9416..9974] / 10336[10053..11001] / 10209[10024..10714] | 11262[10822..11462] / 11928[11316..12653] / 11711[11350..12106] | 174923[164647..175035] / 410270[341062..430000] / 390509[344370..525627] | 1[1..2] / 1[1..1] / 1[1..1] | 97431332[97431288..97431332] / 11352[11352..11352] |
| R2 P3 W8 | 9893[9844..9981] / 10197[10154..10510] / 10104[10103..10148] | 11260[11243..11399] / 11746[11537..11995] / 11681[11548..11761] | 162394[136353..173970] / 398831[346849..412950] / 497022[355622..527970] | 1[1..2] / 1[1..1] / 1[1..1] | 97431332[97431288..97431332] / 11352[11352..11352] |
| R2 P3 W16 | 9673[9308..9919] / 10075[10056..10088] / 10094[9959..10356] | 11068[10903..11369] / 11435[11273..11571] / 11601[11225..11767] | 150989[147442..176490] / 351051[349973..357220] / 363614[360484..365883] | 1[1..2] / 1[1..1] / 1[1..1] | 97431332[97431288..97431332] / 11352[11352..11352] |
| R2 P3 W30 | 9729[9706..10190] / 10312[10123..10566] / 10438[9779..10656] | 11090[11026..11376] / 11837[11571..11843] / 12170[11107..12269] | 147530[142571..167090] / 402189[349424..424031] / 383533[339101..562585] | 1[1..1] / 1[1..1] / 1[1..2] | 97431332[97431288..97431332] / 11352[11352..11352] |

The observed R2 peak ranged from 1 to 16. Configured windows are admission
bounds, not saturation targets; in particular, the ZSTD_ROUTE profile remained
at peak 1–2 even at larger windows. The results therefore do not demonstrate
that every workload reaches W30, nor establish a universal throughput win.
R1 is serial transfer latency while R2 reports concurrent submission/queueing;
compare their pass metrics with that offered-load distinction in mind.

## Durable artifacts

The full run log is
`/tanksmall/scratch/tmp/p51-bench-paired-current-build/results/firefox32-r1/firefox32-paired-matrix.log`,
SHA-256
`0fc22c199c0b6cbab30570cacd948a5583056f4cddae14d3e100da60e83e1a62`.
It contains `MATRIX_EXIT=0`, all 63 individual `rc=0` exits, and the raw per-cell
metrics. The accompanying input identity TSV is at the same directory.

This is only one 32-TU Firefox sample. The §9.4 gate still needs larger
corpora, an available genuine edited-input RocksDB pair, and any required
cross-host measurements. Existing RocksDB `.ii` files and cold/warm history
are not themselves an edited A/B corpus.
