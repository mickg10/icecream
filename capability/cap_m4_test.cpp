// Focused protocol-50 M4 tests: selected independent components, bounded payloads,
// staged Blocks/Fills, equal replay, and rollback after later validation fails.
#include "cap_codec.h"
#include "cap_protocol.h"
#include <cstdio>
#include <string>
using namespace capc;

static int failures=0;
static void check(bool condition,const char*message){
    fprintf(stderr,"  %s: %s\n",condition?"ok":"FAIL",message);if(!condition)++failures;
}

static std::array<std::vector<uint8_t>,6> one_region_op9(const std::string&bytes){
    std::array<std::vector<uint8_t>,6>parts;
    put_varint(parts[0],1);put_varint(parts[0],bytes.size());parts[0].push_back(9);put_varint(parts[0],bytes.size());
    parts[1].insert(parts[1].end(),bytes.begin(),bytes.end());return parts;
}

int main(){
    fprintf(stderr,"[1] independent selected components:\n");
    ZSTD_CCtx*z1=ZSTD_createCCtx(),*z3=ZSTD_createCCtx();ZSTD_DCtx*decoder=ZSTD_createDCtx();
    std::vector<uint8_t>raw(65536,'x'),decoded;
    for(capp::CodecPolicy policy:{capp::CodecPolicy::Raw,capp::CodecPolicy::Zstd1,capp::CodecPolicy::Zstd3,capp::CodecPolicy::Best}){
        auto component=capp::encode_component(raw,policy,z1,z3,true);
        check(capp::decode_component(component.wire,decoder,decoded)&&decoded==raw,"selected component exact");
        check(component.wire.size()<=raw.size()+1,"min-with-RAW never grows component envelope");
    }
    auto compressed=capp::encode_component(raw,capp::CodecPolicy::Zstd3,z1,z3,true);
    check(compressed.codec==capp::ComponentCodec::Zstd3,"compressible z3 candidate selected");
    compressed.wire.push_back(0xff);
    check(!capp::decode_component(compressed.wire,decoder,decoded),"trailing byte rejects complete zstd component");
    std::vector<uint8_t>empty;auto emptyComponent=capp::encode_component(empty,capp::CodecPolicy::Best,z1,z3,true);
    check(emptyComponent.codec==capp::ComponentCodec::Raw&&capp::decode_component(emptyComponent.wire,decoder,decoded)&&decoded.empty(),"empty component uses exact RAW envelope");
    ZSTD_freeCCtx(z1);ZSTD_freeCCtx(z3);ZSTD_freeDCtx(decoder);

    fprintf(stderr,"[2] bounded outer protocol:\n");
    std::vector<uint8_t>rootWire{0,1,2},blockWire{0,3},payload=capp::pack_root_m4(7,rootWire,blockWire),rootOut,blockOut;uint32_t tu=0;
    check(capp::try_unpack_root_m4(payload,tu,rootOut,blockOut)&&tu==7&&rootOut==rootWire&&blockOut==blockWire,"Root components consume exactly");
    payload.push_back(0);
    check(!capp::try_unpack_root_m4(payload,tu,rootOut,blockOut),"Root trailing bytes rejected");
    std::vector<uint8_t>overlong{0x80,0x00};const uint8_t*p=overlong.data(),*e=p+overlong.size();uint64_t value=0;
    check(!get_varint_bounded(p,e,value),"non-canonical overlong varint rejected");
    check(!capp::try_unpack_rejoin(std::vector<uint8_t>{1,0},tu),"Rejoin trailing bytes rejected");

    fprintf(stderr,"[3] staged and idempotent Block definitions:\n");
    FStore blocks;blocks.init(16,2);std::vector<uint8_t>blockRaw;
    put_varint(blockRaw,1);put_varint(blockRaw,0);blockRaw.push_back(0);put_varint(blockRaw,3);put_varint(blockRaw,1);put_varint(blockRaw,2);put_varint(blockRaw,3);
    FStore::BlockTransaction blockTx;
    check(blocks.stage_blocks(blockRaw,blockTx)&&!blocks.FknownBlk[0],"Block parsed without early commit");
    blocks.commit_blocks(blockTx);check(blocks.FknownBlk[0]&&blocks.FblkChildren[0]==std::vector<uint32_t>({1,2,3}),"Block commits after TU acceptance");
    check(blocks.stage_blocks(blockRaw,blockTx),"equal Block replay accepted");blocks.commit_blocks(blockTx);
    std::vector<uint8_t>unequal;put_varint(unequal,1);put_varint(unequal,0);unequal.push_back(0);put_varint(unequal,1);put_varint(unequal,4);
    check(!blocks.stage_blocks(unequal,blockTx),"unequal Block replay rejected");

    fprintf(stderr,"[4] complete Fill replay and post-decode rollback:\n");
    FStore store;store.init(4,0);std::string text="hello replay\n";auto parts=one_region_op9(text);
    std::vector<uint8_t>paths;std::string path="unit/header.hpp";put_varint(paths,path.size());paths.insert(paths.end(),path.begin(),path.end());
    FStore::FillTransaction fill;
    check(store.stage_fill(parts,std::vector<uint32_t>{0},paths,0,1,3,fill),"first Fill staged");
    std::vector<uint8_t>root;put_varint(root,0);std::vector<uint8_t>reconstructed;std::vector<uint32_t>occurrences;
    check(store.reconstruct_staged(root,nullptr,reconstructed,occurrences)&&std::string(reconstructed.begin(),reconstructed.end())==text,"staged Fill expands exactly before commit");
    store.commit_fill(fill);store.Freg_stream.insert(store.Freg_stream.end(),occurrences.begin(),occurrences.end());
    size_t dataSize=store.FmixedRegionData.size(),pathSize=store.Fpaths.size(),publicSize=store.FpublicBytes.size();
    check(store.Fpublic_next==2&&store.publicHeld==1&&store.FpublicLastUse[1]==3,"first implicit public binding committed");
    check(store.stage_fill(parts,std::vector<uint32_t>{0},paths,0,1,9,fill),"equal complete Fill replay staged");
    check(store.FmixedRegionData.size()==dataSize&&store.Fpaths.size()==pathSize,"equal Region/path replay adds no retained bytes");
    store.commit_fill(fill);
    check(store.FpublicLastUse[1]==9&&store.FpublicBytes.size()==publicSize,"equal replay is idempotent and ages only on commit");

    auto later=one_region_op9("later\n");
    check(store.stage_fill(later,std::vector<uint32_t>{1},{},uint32_t(store.Fpaths.size()),store.Fpublic_next,10,fill),"later Fill staged");
    check(store.FmixedRegions[1].known,"staged Region visible for expansion");
    store.rollback_fill(fill);
    check(!store.FmixedRegions[1].known&&store.FmixedRegionData.size()==dataSize&&store.Fpublic_next==2,"later validation failure restores complete Fill state");

    auto changed=one_region_op9("hello changed\n");
    check(!store.stage_fill(changed,std::vector<uint32_t>{0},paths,0,1,11,fill),"unequal complete Fill replay rejected");
    check(store.FmixedRegionData.size()==dataSize&&store.Fpaths.size()==pathSize&&store.FpublicLastUse[1]==9,"unequal replay leaves retained state unchanged");
    auto sparse=one_region_op9("sparse ordinal\n");
    check(store.stage_fill(sparse,std::vector<uint32_t>{2},{},uint32_t(store.Fpaths.size()),17,12,fill),"sparse global public ordinal staged");
    store.commit_fill(fill);
    check(store.Fpublic_next==18&&store.FpublicPresent.size()>17&&store.FpublicPresent[17],"receiver accepts sparse C-authoritative ordinal");

    fprintf(stderr,"[5] rejected C attempt rolls back authority discoveries:\n");
    const std::string source="# 1 \"a.hpp\"\nshared line\n# 2 \"b.hpp\"\nshared line\n";
    Interner dictionary;std::vector<uint32_t>lineIds(source.size()+1),regions;size_t lineCount=0;uint64_t hits=0;
    dictionary.process(source.data(),source.data()+source.size(),lineIds.data(),lineCount,hits,true,&regions);
    check(regions.size()==2,"authority fixture has two Regions");
    MixedEncoder encoder;encoder.init(dictionary.distinct(),uint32_t(dictionary.region_count()));
    MixedEncoder::AuthorityTransaction authority;
    encoder.begin_authority_transaction(authority);encoder.materialize(dictionary,regions,1,&authority);
    check(encoder.nextMixedPublic==2&&encoder.paths.size()==2,"attempt creates one public Line and two paths");
    encoder.rollback_authority_transaction(authority);
    bool pristine=encoder.nextMixedPublic==1&&encoder.paths.empty()&&encoder.pathid.empty();
    for(const auto&state:encoder.mixedCLine)pristine=pristine&&state.source_region==UINT32_MAX&&state.public_id==0;
    check(pristine&&encoder.mixedOps==std::array<uint64_t,7>{},"rejected attempt restores C authority byte-for-byte fields");
    encoder.begin_authority_transaction(authority);encoder.materialize(dictionary,regions,1,&authority);encoder.commit_authority_transaction(authority);
    check(encoder.nextMixedPublic==2&&encoder.paths.size()==2,"subsequent valid attempt commits the same discoveries");

    fprintf(stderr,"[6] Region identity is independent of observation order:\n");
    MixedEncoder reverseEncoder;reverseEncoder.init(dictionary.distinct(),uint32_t(dictionary.region_count()));
    MixedEncoder::AuthorityTransaction reverseAuthority;std::vector<uint32_t>reversed{regions[1],regions[0]};
    reverseEncoder.begin_authority_transaction(reverseAuthority);reverseEncoder.materialize(dictionary,reversed,2,&reverseAuthority);reverseEncoder.commit_authority_transaction(reverseAuthority);
    check(reverseEncoder.nextMixedPublic==2,"reverse observation can publish a Line from a larger Region ordinal");

    printf("cap_m4_test (components + bounded protocol + staged C/F replay): %s\n",failures?"FAIL":"PASS");
    return failures?1:0;
}
