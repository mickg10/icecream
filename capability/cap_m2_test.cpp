// icecream #16 capability harness — cap_m2_test.cpp
// Unit tests for the two M2 correctness properties best exercised in isolation (the live
// restart/late-join/eviction scenarios are cap_main --restart/--latejoin/--evict):
//   (4) UNEQUAL-REBIND  — an attempt to rebind a held public-Line ordinal to different bytes
//                          is rejected; the same bytes are accepted idempotently.
//   (5) TRANSACTIONAL   — a Fill that fails validation commits NOTHING (store byte-for-byte
//       ROLLBACK          unchanged), then the ordinary path recovers on the next Fill.
// Drives FStore::decode_fill directly with hand-built mixed control/literal streams.
//
// build: g++ -O3 -std=c++17 -DICE_LINE_CAP_LOG2=23 cap_m2_test.cpp cap_codec.cpp -o cap_m2_test -lzstd
#include "cap_codec.h"
#include <cstdio>
#include <string>
using namespace capc;

// Build a single-region control stream that carries one op7 (define ordinal from bytes).
static void one_region_op7(std::array<std::vector<uint8_t>,6>& rec, uint32_t ord, const std::string& bytes){
    for(auto& v:rec) v.clear();
    put_varint(rec[0],1);                       // regionCount
    put_varint(rec[0],bytes.size());            // rawLength
    rec[0].push_back(7); put_varint(rec[0],ord); put_varint(rec[0],bytes.size());
    rec[1].insert(rec[1].end(),bytes.begin(),bytes.end());
}

int main(){
    int fails=0;
    auto check=[&](bool c,const char* m){ if(!c){ fprintf(stderr,"  FAIL: %s\n",m); ++fails; } else fprintf(stderr,"  ok: %s\n",m); };

    // ===== TEST 4: UNEQUAL-REBIND rejection =====
    fprintf(stderr,"[4] unequal-rebind:\n");
    {
        FStore F; F.init(/*NREG*/8,/*NBLK*/0);
        std::vector<uint8_t> nopaths;
        std::array<std::vector<uint8_t>,6> rec;
        std::string h="hello world\n";
        one_region_op7(rec,5,h);
        check(F.decode_fill(rec,std::vector<uint32_t>{0u},nopaths,0),"first op7 defines ordinal 5");
        check(F.FpublicPresent.size()>5 && F.FpublicPresent[5]==1,"ordinal 5 present");
        check(F.FpublicBytes[5].size()==h.size(),"ordinal 5 bytes materialized");
        check(F.FmixedRegions[0].known,"region 0 committed");

        size_t rd_before=F.FmixedRegionData.size();
        std::array<std::vector<uint8_t>,6> rec2; std::string w="DIFFERENT!!\n";
        one_region_op7(rec2,5,w);                 // same ordinal, different bytes
        check(!F.decode_fill(rec2,std::vector<uint32_t>{1u},nopaths,1),"unequal rebind of ordinal 5 REJECTED");
        check(!F.FmixedRegions[1].known,"region 1 NOT committed after reject");
        check(F.FmixedRegionData.size()==rd_before,"region data unchanged after reject");
        check(F.FpublicBytes[5].size()==h.size() && memcmp(F.FpublicBytes[5].data(),h.data(),h.size())==0,"ordinal 5 bytes intact (immutable)");

        std::array<std::vector<uint8_t>,6> rec3; one_region_op7(rec3,5,h);   // same ordinal, SAME bytes
        check(F.decode_fill(rec3,std::vector<uint32_t>{2u},nopaths,2),"idempotent re-provide (same bytes) accepted");
        check(F.FmixedRegions[2].known,"region 2 committed on idempotent re-provide");
    }

    // ===== TEST 5: TRANSACTIONAL ROLLBACK =====
    fprintf(stderr,"[5] transactional-rollback:\n");
    {
        FStore F; F.init(/*NREG*/8,/*NBLK*/0);
        // A Fill that installs a path + partially builds region 2, then hits a bad opcode.
        std::array<std::vector<uint8_t>,6> bad; for(auto&v:bad)v.clear();
        put_varint(bad[0],1);            // regionCount
        put_varint(bad[0],8);            // rawLength 8 (never reached)
        bad[0].push_back(0); put_varint(bad[0],4); std::string a="aaaa"; bad[1].insert(bad[1].end(),a.begin(),a.end());  // op0: 4 bytes
        bad[0].push_back(200);           // BAD opcode -> validation failure mid-region
        std::vector<uint8_t> fpBad; put_varint(fpBad,3); fpBad.push_back('x'); fpBad.push_back('y'); fpBad.push_back('z');  // a new path

        size_t rd0=F.FmixedRegionData.size(), paths0=F.Fpaths.size(), held0=F.publicHeld;
        check(!F.decode_fill(bad,std::vector<uint32_t>{2u},fpBad,0),"corrupt Fill rejected");
        check(F.FmixedRegionData.size()==rd0,"region data fully reverted");
        check(F.Fpaths.size()==paths0,"path install reverted (nothing committed)");
        check(F.publicHeld==held0,"public store unchanged");
        check(!F.FmixedRegions[2].known,"region 2 not committed");

        // Ordinary path recovers: a correct Fill for region 2.
        std::array<std::vector<uint8_t>,6> good; for(auto&v:good)v.clear();
        put_varint(good[0],1); put_varint(good[0],4); good[0].push_back(0); put_varint(good[0],4); good[1].insert(good[1].end(),a.begin(),a.end());
        std::vector<uint8_t> fp2;
        check(F.decode_fill(good,std::vector<uint32_t>{2u},fp2,1),"ordinary path recovers after failed Fill");
        check(F.FmixedRegions[2].known && F.FmixedRegions[2].length==4,"region 2 now committed correctly");
        check(F.FmixedRegionData.size()==rd0+4,"exactly the good region's bytes committed");
    }

    // ===== TEST 6: EMBEDDED_OBJECT (op10) reserved stub rejects cleanly (M3) =====
    fprintf(stderr,"[6] EMBEDDED_OBJECT op10 stub:\n");
    {
        FStore F; F.init(/*NREG*/8,/*NBLK*/0); std::vector<uint8_t> nopaths;
        std::array<std::vector<uint8_t>,6> rec; for(auto&v:rec)v.clear();
        put_varint(rec[0],1); put_varint(rec[0],4);   // region 0, rawLength 4
        rec[0].push_back(10);                          // op10 EMBEDDED_OBJECT (reserved, P26-P29)
        size_t rd0=F.FmixedRegionData.size();
        check(!F.decode_fill(rec,std::vector<uint32_t>{0u},nopaths,0),"op10 EMBEDDED_OBJECT rejected (reserved, not implemented)");
        check(F.FmixedRegionData.size()==rd0 && !F.FmixedRegions[0].known,"op10 reject commits nothing");
    }

    printf("cap_m2_test (rebind + rollback + op10-stub): %s\n", fails==0?"PASS":"FAIL");
    return fails==0?0:1;
}
