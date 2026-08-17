// icecream #16 — protocol-50 M4/M5 product capability path.
//
// One actual socket representation is both decoded and counted.  Every semantic
// component is an independent selected RAW/zstd-1/zstd-3 frame; object state persists
// under one SourceGeneration while entropy state ends at every component boundary.
// Persistent F mutations and C's receiver mirror advance only after a TU Ack.
//
// build: g++ -O3 -march=native -std=c++17 -DICE_LINE_CAP_LOG2=23
//        cap_m4_main.cpp cap_codec.cpp -o cap_m4_main -lzstd
#include "cap_codec.h"
#include "cap_protocol.h"
#include "cap_transport.h"
#include "cap_identity.h"
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <chrono>
#include <random>
#include <string>

using namespace capc;
using Clock=std::chrono::steady_clock;
static double seconds_since(Clock::time_point begin){return std::chrono::duration<double>(Clock::now()-begin).count();}

enum Scenario { SC_COLD=0,SC_RESTART=1,SC_LATEJOIN=2 };
enum ComponentKind { CK_ROOT=0,CK_BLOCK=1,CK_NEED=2,CK_PATH=3,CK_CONTROL=4,CK_LITERAL=5,CK_ARRAY_CONTROL=6,CK_ARRAY_VALUES=7,CK_COUNT=8 };
static const char*const COMPONENT_NAMES[CK_COUNT]={"root","block","need","path","region_control","raw_run","array_control","array_values"};

struct Options {
    const char*manifest=nullptr;
    size_t max_files=SIZE_MAX;
    uint32_t repetitions=1;
    capp::CodecPolicy policy=capp::CodecPolicy::Best;
    Scenario scenario=SC_COLD;
    uint32_t scenario_tu=0;
    uint32_t public_budget=UINT32_MAX;
    uint32_t corrupt_tu=UINT32_MAX;
};

struct FrameLedger {
    std::array<uint64_t,7> bytes{};
    std::array<uint64_t,7> count{};
    void note(cap::Frame frame,size_t payload){size_t i=size_t(frame);bytes[i]+=4+payload;++count[i];}
    uint64_t total()const{uint64_t n=0;for(uint64_t value:bytes)n+=value;return n;}
};

struct ComponentLedger {
    uint64_t raw=0,selected=0,z1_candidates=0,z3_candidates=0;
    uint64_t raw_selected=0,z1_selected=0,z3_selected=0,count=0;
    void note(const capp::EncodedComponent&component){
        raw+=component.raw_size;selected+=component.wire.size();++count;
        if(component.z1_size!=SIZE_MAX)z1_candidates+=component.z1_size;
        if(component.z3_size!=SIZE_MAX)z3_candidates+=component.z3_size;
        if(component.codec==capp::ComponentCodec::Raw)++raw_selected;
        else if(component.codec==capp::ComponentCodec::Zstd1)++z1_selected;
        else ++z3_selected;
    }
    void note_received(const std::vector<uint8_t>&wire,size_t rawSize){
        raw+=rawSize;selected+=wire.size();++count;
        if(!wire.empty()&&wire[0]==uint8_t(capp::ComponentCodec::Raw))++raw_selected;
        else if(!wire.empty()&&wire[0]==uint8_t(capp::ComponentCodec::Zstd1))++z1_selected;
        else if(!wire.empty()&&wire[0]==uint8_t(capp::ComponentCodec::Zstd3))++z3_selected;
    }
};

struct CodecContexts {
    ZSTD_CCtx*z1=ZSTD_createCCtx();
    ZSTD_CCtx*z3=ZSTD_createCCtx();
    ZSTD_DCtx*decoder=ZSTD_createDCtx();
    ~CodecContexts(){ZSTD_freeCCtx(z1);ZSTD_freeCCtx(z3);ZSTD_freeDCtx(decoder);}
};

static bool send_counted(int fd,cap::Frame frame,const std::vector<uint8_t>&payload,FrameLedger&ledger){
    if(!cap::send_frame(fd,frame,payload))return false;
    ledger.note(frame,payload.size());return true;
}
static bool recv_counted(int fd,cap::Frame&frame,std::vector<uint8_t>&payload,FrameLedger&ledger){
    if(!cap::recv_frame(fd,frame,payload))return false;
    ledger.note(frame,payload.size());return true;
}

static cap::SourceGeneration make_generation(){
    cap::SourceGeneration generation{};std::random_device random;
    for(uint8_t&byte:generation)byte=uint8_t(random());
    bool any=false;for(uint8_t byte:generation)any|=byte!=0;
    if(!any)generation[0]=1;
    return generation;
}

static const char* policy_name(capp::CodecPolicy policy){
    switch(policy){case capp::CodecPolicy::Raw:return "raw";case capp::CodecPolicy::Zstd1:return "z1";
        case capp::CodecPolicy::Zstd3:return "z3";case capp::CodecPolicy::Best:return "best";}
    return "unknown";
}

static const char* scenario_name(Scenario scenario){
    switch(scenario){case SC_COLD:return "cold";case SC_RESTART:return "restart";case SC_LATEJOIN:return "latejoin";}
    return "unknown";
}

static bool parse_policy(const char*value,capp::CodecPolicy&policy){
    if(!strcmp(value,"raw")){policy=capp::CodecPolicy::Raw;return true;}
    if(!strcmp(value,"z1")){policy=capp::CodecPolicy::Zstd1;return true;}
    if(!strcmp(value,"z3")){policy=capp::CodecPolicy::Zstd3;return true;}
    if(!strcmp(value,"best")){policy=capp::CodecPolicy::Best;return true;}
    return false;
}

static capp::EncodedComponent encode_named(const std::vector<uint8_t>&raw,capp::CodecPolicy policy,
                                            CodecContexts&codec,ComponentLedger&ledger,bool blob=true){
    auto component=capp::encode_component(raw,policy,codec.z1,codec.z3,blob);ledger.note(component);return component;
}

static bool parse_need(const std::vector<uint8_t>&raw,uint32_t nreg,std::vector<uint32_t>&missing,
                       std::vector<uint32_t>&drops){
    const uint8_t*p=raw.data(),*e=p+raw.size();uint64_t count=0;
    if(!get_varint_bounded(p,e,count)||count>nreg)return false;
    missing.clear();missing.reserve(size_t(count));
    std::vector<uint8_t>seenRegion(nreg,0);
    for(uint64_t i=0;i<count;++i){uint32_t id=0;if(!get_u32_bounded(p,e,id)||id>=nreg||seenRegion[id])return false;seenRegion[id]=1;missing.push_back(id);}
    if(!get_varint_bounded(p,e,count)||count>raw.size())return false;
    drops.clear();drops.reserve(size_t(count));
    for(uint64_t i=0;i<count;++i){uint32_t id=0;if(!get_u32_bounded(p,e,id)||!id)return false;
        if(std::find(drops.begin(),drops.end(),id)!=drops.end())return false;
        drops.push_back(id);}
    return p==e;
}

static std::vector<uint8_t> build_need(const std::vector<uint32_t>&missing,const std::vector<uint32_t>&drops){
    std::vector<uint8_t>raw;put_varint(raw,missing.size());for(uint32_t id:missing)put_varint(raw,id);
    put_varint(raw,drops.size());for(uint32_t id:drops)put_varint(raw,id);return raw;
}

static bool derive_requirements(FStore&store,const std::vector<uint8_t>&root,
                                const FStore::BlockTransaction&blocks,
                                std::vector<uint32_t>&requiredRegions,std::vector<uint32_t>&requiredBlocks){
    if(++store.requestStamp==0){std::fill(store.FrequiredRegionStamp.begin(),store.FrequiredRegionStamp.end(),0);
        std::fill(store.FrequiredBlockStamp.begin(),store.FrequiredBlockStamp.end(),0);store.requestStamp=1;}
    requiredRegions.clear();requiredBlocks.clear();
    auto requireRegion=[&](uint32_t id){if(store.FrequiredRegionStamp[id]!=store.requestStamp){store.FrequiredRegionStamp[id]=store.requestStamp;requiredRegions.push_back(id);}};
    auto requireBlock=[&](uint32_t id){if(store.FrequiredBlockStamp[id]!=store.requestStamp){store.FrequiredBlockStamp[id]=store.requestStamp;requiredBlocks.push_back(id);}};
    const uint8_t*p=root.data(),*e=p+root.size();
    while(p<e){uint32_t token=0;if(!get_u32_bounded(p,e,token))return false;
        if(token<store.NREG)requireRegion(token);
        else{uint32_t block=token-store.NREG;if(block>=store.NBLK)return false;requireBlock(block);}}
    for(uint32_t block:requiredBlocks){const auto*children=store.block_children(block,&blocks);if(!children)return false;
        for(uint32_t child:*children){if(child>=store.NREG)return false;requireRegion(child);}}
    return true;
}

static uint64_t fnv_bytes(uint64_t hash,const void*data,size_t size){
    const uint8_t*p=static_cast<const uint8_t*>(data);for(size_t i=0;i<size;++i){hash^=p[i];hash*=1099511628211ull;}return hash;
}
static uint64_t store_digest(const FStore&store){
    uint64_t hash=1469598103934665603ull;
    hash=fnv_bytes(hash,store.FmixedRegionData.data(),store.FmixedRegionData.size());
    for(const auto&view:store.FmixedRegions){hash=fnv_bytes(hash,&view.offset,sizeof(view.offset));
        hash=fnv_bytes(hash,&view.length,sizeof(view.length));uint8_t known=view.known?1:0;hash=fnv_bytes(hash,&known,1);}
    hash=fnv_bytes(hash,store.FpublicPresent.data(),store.FpublicPresent.size());
    hash=fnv_bytes(hash,store.FpublicLastUse.data(),store.FpublicLastUse.size()*sizeof(uint32_t));
    for(size_t i=0;i<store.FpublicBytes.size();++i)if(store.FpublicPresent[i])hash=fnv_bytes(hash,store.FpublicBytes[i].data(),store.FpublicBytes[i].size());
    hash=fnv_bytes(hash,store.FknownBlk.data(),store.FknownBlk.size());
    for(size_t i=0;i<store.FblkChildren.size();++i)if(store.FknownBlk[i])hash=fnv_bytes(hash,store.FblkChildren[i].data(),store.FblkChildren[i].size()*sizeof(uint32_t));
    hash=fnv_bytes(hash,store.Freg_stream.data(),store.Freg_stream.size()*sizeof(uint32_t));
    for(const auto&path:store.Fpaths)hash=fnv_bytes(hash,path.data(),path.size());
    hash^=uint64_t(store.Fpublic_next)<<32|store.publicHeld;return hash;
}

static std::vector<uint8_t> pack_final_ack(bool exact,uint64_t verified,uint64_t raw,uint64_t decodeNs,uint64_t failures,uint64_t wireBeforeAck){
    std::vector<uint8_t>out;out.push_back(exact?1:0);put_varint(out,verified);put_varint(out,raw);put_varint(out,decodeNs);put_varint(out,failures);
    put_varint(out,wireBeforeAck);return out;
}
static bool unpack_final_ack(const std::vector<uint8_t>&payload,bool&exact,uint64_t&verified,uint64_t&raw,
                             uint64_t&decodeNs,uint64_t&failures,uint64_t&wireBeforeAck){
    if(payload.empty()||payload[0]>1)return false;
    exact=payload[0]!=0;
    const uint8_t*p=payload.data()+1,*e=payload.data()+payload.size();
    return get_varint_bounded(p,e,verified)&&get_varint_bounded(p,e,raw)&&get_varint_bounded(p,e,decodeNs)&&
           get_varint_bounded(p,e,failures)&&get_varint_bounded(p,e,wireBeforeAck)&&p==e;
}

static bool c_resync(int fd,uint32_t expectedTU,MixedEncoder&encoder,std::vector<uint8_t>&knownBlocks,FrameLedger&ledger){
    cap::Frame frame;std::vector<uint8_t>payload;uint32_t resume=0;
    if(!recv_counted(fd,frame,payload,ledger)||frame!=cap::Frame::Rejoin||!capp::try_unpack_rejoin(payload,resume)||resume!=expectedTU)return false;
    encoder.reset_model();std::fill(knownBlocks.begin(),knownBlocks.end(),0);
    std::vector<uint8_t>resync=capp::pack_resync(encoder.nextMixedPublic,encoder.paths);
    return send_counted(fd,cap::Frame::Ack,resync,ledger);
}

static bool f_resync(int fd,uint32_t expectedTU,FStore&store,FrameLedger&ledger){
    if(!send_counted(fd,cap::Frame::Rejoin,capp::pack_rejoin(expectedTU),ledger))return false;
    cap::Frame frame;std::vector<uint8_t>payload;uint32_t nextPublic=0;std::vector<std::string>paths;
    if(!recv_counted(fd,frame,payload,ledger)||frame!=cap::Frame::Ack||!capp::try_unpack_resync(payload,nextPublic,paths))return false;
    store.reset_store(nextPublic);store.Fpaths=std::move(paths);return true;
}

static int run_c(int fd,const Interner&dict,const Corpus&corpus,
                 const std::vector<uint32_t>&tokens,const std::vector<size_t>&tokenOffsets,
                 const std::vector<size_t>&blockOffsets,const std::vector<uint32_t>&blockChildren,
                 const std::vector<uint32_t>&blockCopySource,const std::vector<uint8_t>&blockCopyOk,
                 uint32_t nreg,uint32_t nblk,const Options&options){
    const uint32_t physicalTus=uint32_t(corpus.files.size());
    const uint64_t logicalTotal64=uint64_t(physicalTus)*options.repetitions;
    if(logicalTotal64>UINT32_MAX)return 2;
    const uint32_t logicalTotal=uint32_t(logicalTotal64);
    CodecContexts codec;FrameLedger frames;std::array<ComponentLedger,CK_COUNT>components{};
    MixedEncoder encoder;encoder.init(dict.distinct(),nreg);std::vector<uint8_t>knownBlocks(nblk,0);
    std::vector<uint32_t>requiredBlockStamp(nblk,0);uint32_t requestStamp=0;
    uint64_t socketFailures=0,rawSuccessful=0;double encodeSeconds=0;
    std::vector<uint64_t>repetitionWire(options.repetitions,0);

    // A late-joining F begins with an empty store, but C's generation authority has
    // already learned the chronological prefix.  This warm pass creates only C state.
    if(options.scenario==SC_LATEJOIN&&options.scenario_tu){
        std::vector<uint32_t>regionStamp(nreg,0),blockStamp(nblk,0),required,blocks,missing;uint32_t stamp=0;
        for(uint32_t logical=0;logical<options.scenario_tu;++logical){uint32_t physical=logical%physicalTus;
            const uint32_t*tk=&tokens[tokenOffsets[physical]];size_t count=tokenOffsets[physical+1]-tokenOffsets[physical];
            if(++stamp==0){std::fill(regionStamp.begin(),regionStamp.end(),0);std::fill(blockStamp.begin(),blockStamp.end(),0);stamp=1;}
            required.clear();blocks.clear();auto addRegion=[&](uint32_t id){if(regionStamp[id]!=stamp){regionStamp[id]=stamp;required.push_back(id);}};
            for(size_t i=0;i<count;++i){uint32_t token=tk[i];if(token<nreg)addRegion(token);else{uint32_t block=token-nreg;
                if(blockStamp[block]!=stamp){blockStamp[block]=stamp;blocks.push_back(block);}}}
            for(uint32_t block:blocks){knownBlocks[block]=1;for(size_t j=blockOffsets[block];j<blockOffsets[block+1];++j)addRegion(blockChildren[j]);}
            missing.clear();for(uint32_t region:required)if(!encoder.fknownReg[region])missing.push_back(region);
            if(!missing.empty())encoder.materialize(dict,missing,logical);
        }
        fprintf(stderr,"C M4 latejoin authority prefix=%u nextPublic=%u paths=%zu\n",options.scenario_tu,encoder.nextMixedPublic,encoder.paths.size());
    }

    cap::SourceGeneration generation=make_generation();
    std::vector<uint8_t>hello=capp::pack_hello_m4(generation,nreg,nblk,physicalTus,options.repetitions);
    if(!send_counted(fd,cap::Frame::Hello,hello,frames))return 2;
    uint32_t startLogical=options.scenario==SC_LATEJOIN?options.scenario_tu:0;
    auto wallStart=Clock::now();

    for(uint32_t logical=startLogical;logical<logicalTotal;++logical){
        uint64_t tuWireStart=frames.total();
        if((options.scenario==SC_RESTART||options.scenario==SC_LATEJOIN)&&logical==options.scenario_tu){
            if(!c_resync(fd,logical,encoder,knownBlocks,frames)){fprintf(stderr,"C M4: planned resync failed TU %u\n",logical);return 2;}
        }
        uint32_t attempt=0;
        for(;;++attempt){
            uint32_t physical=logical%physicalTus;
            const uint32_t*tk=&tokens[tokenOffsets[physical]];size_t tokenCount=tokenOffsets[physical+1]-tokenOffsets[physical];
            auto encodeBegin=Clock::now();
            std::vector<uint8_t>rootRaw;rootRaw.reserve(tokenCount*2);for(size_t i=0;i<tokenCount;++i)put_varint(rootRaw,tk[i]);
            if(++requestStamp==0){std::fill(requiredBlockStamp.begin(),requiredBlockStamp.end(),0);requestStamp=1;}
            std::vector<uint32_t>requiredBlocks;
            for(size_t i=0;i<tokenCount;++i)if(tk[i]>=nreg){uint32_t block=tk[i]-nreg;
                if(requiredBlockStamp[block]!=requestStamp){requiredBlockStamp[block]=requestStamp;requiredBlocks.push_back(block);}}
            std::vector<uint32_t>manifestBlocks;for(uint32_t block:requiredBlocks)if(!knownBlocks[block])manifestBlocks.push_back(block);
            std::vector<uint8_t>blockRaw;
            if(!manifestBlocks.empty()){
                put_varint(blockRaw,manifestBlocks.size());
                for(uint32_t block:manifestBlocks){put_varint(blockRaw,block);size_t length=blockOffsets[block+1]-blockOffsets[block];
                    if(blockCopyOk[block]&&!encoder.recovering){blockRaw.push_back(1);put_varint(blockRaw,blockCopySource[block]);put_varint(blockRaw,length);}
                    else{blockRaw.push_back(0);put_varint(blockRaw,length);for(size_t j=blockOffsets[block];j<blockOffsets[block+1];++j)put_varint(blockRaw,blockChildren[j]);}}
                for(uint32_t block:manifestBlocks)knownBlocks[block]=1; // tentative until Ack; reset on rejection
            }
            auto rootComponent=encode_named(rootRaw,options.policy,codec,components[CK_ROOT]);
            auto blockComponent=encode_named(blockRaw,options.policy,codec,components[CK_BLOCK]);
            encodeSeconds+=seconds_since(encodeBegin);
            std::vector<uint8_t>rootPayload=capp::pack_root_m4(logical,rootComponent.wire,blockComponent.wire);
            if(!send_counted(fd,cap::Frame::Root,rootPayload,frames)){fprintf(stderr,"C M4: Root send failed\n");return 2;}

            cap::Frame incoming;std::vector<uint8_t>payload;
            if(!recv_counted(fd,incoming,payload,frames))return 2;
            if(incoming==cap::Frame::Ack){
                uint32_t ackTU=0;bool accepted=true;
                if(!capp::try_unpack_tu_ack(payload,ackTU,accepted)||ackTU!=logical||accepted)return 2;
                ++socketFailures;if(!c_resync(fd,logical,encoder,knownBlocks,frames))return 2;continue;
            }
            if(incoming!=cap::Frame::Need){fprintf(stderr,"C M4: expected Need TU %u\n",logical);return 2;}
            uint32_t needTU=0;std::vector<uint8_t>needWire,needRaw;
            if(!capp::try_unpack_need_m4(payload,needTU,needWire)||needTU!=logical||!capp::decode_component(needWire,codec.decoder,needRaw))return 2;
            components[CK_NEED].note_received(needWire,needRaw.size());
            std::vector<uint32_t>missing,drops;
            if(!parse_need(needRaw,nreg,missing,drops)){fprintf(stderr,"C M4: invalid Need TU %u\n",logical);return 2;}
            for(uint32_t ordinal:drops)encoder.forget_public(ordinal);

            encodeBegin=Clock::now();
            MixedEncoder::AuthorityTransaction authorityTx;encoder.begin_authority_transaction(authorityTx);
            if(!missing.empty())encoder.materialize(dict,missing,logical,&authorityTx);
            else{for(auto&part:encoder.mixedRaw)part.clear();encoder.fill_paths.clear();encoder.np=0;
                encoder.fill_path_base=uint32_t(encoder.paths.size());encoder.fill_public_base=encoder.nextMixedPublic;}
            std::array<std::vector<uint8_t>,6>mixedWire;
            auto pathComponent=encode_named(encoder.fill_paths,options.policy,codec,components[CK_PATH]);
            for(size_t part=0;part<4;++part){auto component=encode_named(encoder.mixedRaw[part],options.policy,codec,components[CK_CONTROL+part]);
                mixedWire[part]=std::move(component.wire);}
            bool inject=logical==options.corrupt_tu&&attempt==0;
            if(inject){mixedWire[0].push_back(0xff);++components[CK_CONTROL].selected;fprintf(stderr,"C M4: injected rejected component at TU %u\n",logical);}
            encodeSeconds+=seconds_since(encodeBegin);
            std::vector<uint8_t>fillPayload=capp::pack_fill_m4(logical,encoder.fill_path_base,encoder.fill_public_base,
                                                               pathComponent.wire,mixedWire,4);
            if(!send_counted(fd,cap::Frame::Fill,fillPayload,frames))return 2;
            if(!recv_counted(fd,incoming,payload,frames)||incoming!=cap::Frame::Ack)return 2;
            uint32_t ackTU=0;bool accepted=false;
            if(!capp::try_unpack_tu_ack(payload,ackTU,accepted)||ackTU!=logical)return 2;
            if(!accepted){
                encoder.rollback_authority_transaction(authorityTx);++socketFailures;
                if(!c_resync(fd,logical,encoder,knownBlocks,frames))return 2;
                continue;
            }
            encoder.commit_authority_transaction(authorityTx);
            rawSuccessful+=corpus.files[physical].len;break;
        }
        repetitionWire[logical/physicalTus]+=frames.total()-tuWireStart;
    }

    if(!send_counted(fd,cap::Frame::Done,std::vector<uint8_t>{},frames))return 2;
    uint64_t cWireBeforeFinalAck=frames.total();
    cap::Frame finalFrame;std::vector<uint8_t>finalPayload;
    if(!recv_counted(fd,finalFrame,finalPayload,frames)||finalFrame!=cap::Frame::Ack)return 2;
    bool exact=false;uint64_t verified=0,fRaw=0,decodeNs=0,fFailures=0,fWireBeforeFinalAck=0;
    if(!unpack_final_ack(finalPayload,exact,verified,fRaw,decodeNs,fFailures,fWireBeforeFinalAck))return 2;
    uint64_t expectedVerified=logicalTotal-startLogical;
    if(verified!=expectedVerified||fRaw!=rawSuccessful||fFailures!=socketFailures||fWireBeforeFinalAck!=cWireBeforeFinalAck)exact=false;
    uint64_t frameClosed=0;for(uint64_t bytes:frames.bytes)frameClosed+=bytes;
    if(frameClosed!=frames.total())exact=false;

    static const char*const frameNames[7]={"hello","root","need","fill","done","ack","rejoin"};
    printf("\n==== CAP-M4 protocol-50 independent selected component frames (%s) — %s ====\n",policy_name(options.policy),options.manifest);
    printf("RESULT exact=%s codec=%s scenario=%s physical_tus=%u logical_tus=%u served=%llu raw=%llu actual_socket=%llu ratio=%.3f failures=%llu\n",
           exact?"OK":"FAIL",policy_name(options.policy),scenario_name(options.scenario),physicalTus,logicalTotal,(unsigned long long)verified,(unsigned long long)rawSuccessful,
           (unsigned long long)frames.total(),frames.total()?double(rawSuccessful)/frames.total():0.0,(unsigned long long)socketFailures);
    printf("FRAME_LEDGER closure=%s",frameClosed==frames.total()?"OK":"FAIL");
    for(size_t i=0;i<7;++i)printf(" %s=%llu/%llu",frameNames[i],(unsigned long long)frames.bytes[i],(unsigned long long)frames.count[i]);
    printf(" total=%llu\n",(unsigned long long)frames.total());
    for(size_t i=0;i<CK_COUNT;++i){const auto&part=components[i];
        printf("COMPONENT %-13s raw=%llu z1_candidate=%llu z3_candidate=%llu selected=%llu choices(raw/z1/z3)=%llu/%llu/%llu frames=%llu\n",COMPONENT_NAMES[i],
               (unsigned long long)part.raw,(unsigned long long)part.z1_candidates,(unsigned long long)part.z3_candidates,
               (unsigned long long)part.selected,(unsigned long long)part.raw_selected,
               (unsigned long long)part.z1_selected,(unsigned long long)part.z3_selected,(unsigned long long)part.count);}
    for(size_t i=0;i<repetitionWire.size();++i)printf("REPETITION %zu wire=%llu\n",i+1,(unsigned long long)repetitionWire[i]);
    double fDecode=decodeNs/1e9;
    double relationshipSeconds=seconds_since(wallStart);
    printf("THROUGHPUT C_transform=%.3f GB/s F_decode_expand=%.3f GB/s relationship=%.3fs relationship_rate=%.3f GB/s system_header_reads=%llu\n",
           encodeSeconds?rawSuccessful/encodeSeconds/1e9:0.0,fDecode?rawSuccessful/fDecode/1e9:0.0,relationshipSeconds,
           relationshipSeconds?rawSuccessful/relationshipSeconds/1e9:0.0,
           (unsigned long long)system_header_reads());
    fflush(stdout);return exact?0:1;
}

static int run_f(int fd,const Corpus&corpus,const Options&options){
    CodecContexts codec;FrameLedger frames;std::array<ComponentLedger,CK_COUNT>components{};
    cap::Frame frame;std::vector<uint8_t>payload;
    if(!recv_counted(fd,frame,payload,frames)||frame!=cap::Frame::Hello)return 2;
    cap::SourceGeneration generation{};uint32_t nreg=0,nblk=0,physicalTus=0,repetitions=0;
    if(!capp::try_unpack_hello_m4(payload,generation,nreg,nblk,physicalTus,repetitions)||
       physicalTus!=corpus.files.size()||repetitions!=options.repetitions)return 2;
    FStore store;store.init(nreg,nblk);store.publicBudget=options.public_budget;
    uint64_t logicalTotal64=uint64_t(physicalTus)*repetitions;if(logicalTotal64>UINT32_MAX)return 2;
    uint32_t logicalTotal=uint32_t(logicalTotal64),startLogical=options.scenario==SC_LATEJOIN?options.scenario_tu:0;
    uint64_t verified=0,verifiedRaw=0,failures=0,evicted=0;bool exact=true;double decodeSeconds=0;

    for(uint32_t logical=startLogical;logical<logicalTotal;++logical){
        if((options.scenario==SC_RESTART||options.scenario==SC_LATEJOIN)&&logical==options.scenario_tu){
            if(!f_resync(fd,logical,store,frames)){fprintf(stderr,"F M4: planned resync failed TU %u\n",logical);return 2;}
        }
        uint32_t attempt=0;
        for(;;++attempt){
            if(attempt>3){fprintf(stderr,"F M4: too many retries TU %u\n",logical);return 2;}
            bool checkRollback=logical==options.corrupt_tu&&attempt==0;uint64_t beforeDigest=checkRollback?store_digest(store):0;
            auto reject=[&](FStore::FillTransaction*fill,const char*why)->bool{
                if(fill&&fill->active)store.rollback_fill(*fill);
                bool unchanged=!checkRollback||store_digest(store)==beforeDigest;
                fprintf(stderr,"F M4: rejected TU %u attempt %u (%s), state_unchanged=%s\n",logical,attempt,why,unchanged?"yes":"NO");
                if(!unchanged)exact=false;
                if(!send_counted(fd,cap::Frame::Ack,capp::pack_tu_ack(logical,false),frames))return false;
                ++failures;return f_resync(fd,logical,store,frames);
            };

            if(!recv_counted(fd,frame,payload,frames)||frame!=cap::Frame::Root)return 2;
            auto decodeBegin=Clock::now();uint32_t rootTU=0;std::vector<uint8_t>rootWire,blockWire,rootRaw,blockRaw;
            if(!capp::try_unpack_root_m4(payload,rootTU,rootWire,blockWire)||rootTU!=logical||
               !capp::decode_component(rootWire,codec.decoder,rootRaw)||!capp::decode_component(blockWire,codec.decoder,blockRaw)){
                decodeSeconds+=seconds_since(decodeBegin);if(!reject(nullptr,"Root/component decode"))return 2;continue;
            }
            components[CK_ROOT].note_received(rootWire,rootRaw.size());components[CK_BLOCK].note_received(blockWire,blockRaw.size());
            FStore::BlockTransaction blockTx;
            if(!store.stage_blocks(blockRaw,blockTx)){
                decodeSeconds+=seconds_since(decodeBegin);if(!reject(nullptr,"Block stage"))return 2;continue;
            }
            std::vector<uint32_t>requiredRegions,requiredBlocks,missing;
            if(!derive_requirements(store,rootRaw,blockTx,requiredRegions,requiredBlocks)){
                decodeSeconds+=seconds_since(decodeBegin);if(!reject(nullptr,"Root requirements"))return 2;continue;
            }
            for(uint32_t region:requiredRegions)if(!store.FmixedRegions[region].known)missing.push_back(region);
            decodeSeconds+=seconds_since(decodeBegin);

            std::vector<uint8_t>needRaw=build_need(missing,store.pendingDrops);store.pendingDrops.clear();
            auto needComponent=encode_named(needRaw,options.policy,codec,components[CK_NEED]);
            std::vector<uint8_t>needPayload=capp::pack_need_m4(logical,needComponent.wire);
            if(!send_counted(fd,cap::Frame::Need,needPayload,frames))return 2;
            if(!recv_counted(fd,frame,payload,frames)||frame!=cap::Frame::Fill)return 2;

            decodeBegin=Clock::now();uint32_t fillTU=0,pathBase=0,publicBase=0;std::vector<uint8_t>pathWire,pathRaw;
            std::array<std::vector<uint8_t>,6>mixedWire,mixedRaw;
            bool componentsOk=capp::try_unpack_fill_m4(payload,fillTU,pathBase,publicBase,pathWire,mixedWire,4)&&fillTU==logical&&
                              capp::decode_component(pathWire,codec.decoder,pathRaw);
            if(componentsOk)for(size_t part=0;part<4;++part)if(!capp::decode_component(mixedWire[part],codec.decoder,mixedRaw[part])){componentsOk=false;break;}
            if(!componentsOk){decodeSeconds+=seconds_since(decodeBegin);if(!reject(nullptr,"Fill/component decode"))return 2;continue;}
            components[CK_PATH].note_received(pathWire,pathRaw.size());
            for(size_t part=0;part<4;++part)components[CK_CONTROL+part].note_received(mixedWire[part],mixedRaw[part].size());
            FStore::FillTransaction fillTx;
            if(!store.stage_fill(mixedRaw,missing,pathRaw,pathBase,publicBase,logical,fillTx)){
                decodeSeconds+=seconds_since(decodeBegin);if(!reject(nullptr,"Fill stage"))return 2;continue;
            }
            std::vector<uint8_t>reconstructed;std::vector<uint32_t>occurrences;
            bool expanded=store.reconstruct_staged(rootRaw,&blockTx,reconstructed,occurrences);
            uint32_t physical=logical%physicalTus;const auto&file=corpus.files[physical];const char*original=corpus.bytes.data()+file.off;
            bool bytesEqual=expanded&&reconstructed.size()==file.len&&(!file.len||!memcmp(reconstructed.data(),original,file.len));
            if(!bytesEqual){decodeSeconds+=seconds_since(decodeBegin);if(!reject(&fillTx,"exact expansion"))return 2;continue;}
            store.commit_fill(fillTx);store.commit_blocks(blockTx);store.Freg_stream.insert(store.Freg_stream.end(),occurrences.begin(),occurrences.end());
            decodeSeconds+=seconds_since(decodeBegin);
            if(!send_counted(fd,cap::Frame::Ack,capp::pack_tu_ack(logical,true),frames))return 2;
            ++verified;verifiedRaw+=file.len;
            if(options.public_budget!=UINT32_MAX){size_t before=store.publicHeld;store.evict_to_budget(logical);evicted+=before-store.publicHeld;}
            break;
        }
    }

    if(!recv_counted(fd,frame,payload,frames)||frame!=cap::Frame::Done||!payload.empty())return 2;
    uint64_t decodeNs=uint64_t(decodeSeconds*1e9),wireBeforeAck=frames.total();
    std::vector<uint8_t>finalAck=pack_final_ack(exact,verified,verifiedRaw,decodeNs,failures,wireBeforeAck);
    if(!send_counted(fd,cap::Frame::Ack,finalAck,frames))return 2;
    struct rusage usage{};getrusage(RUSAGE_SELF,&usage);
    fprintf(stderr,"F M4: exact=%s verified=%llu raw=%llu failures=%llu held=%u evicted=%llu frame_wire=%llu decode=%.6fs peak_rss=%.1fMiB headers=%llu\n",
            exact?"OK":"FAIL",(unsigned long long)verified,(unsigned long long)verifiedRaw,(unsigned long long)failures,
            store.publicHeld,(unsigned long long)evicted,(unsigned long long)frames.total(),decodeSeconds,usage.ru_maxrss/1024.0,
            (unsigned long long)system_header_reads());
    return exact&&system_header_reads()==0?0:1;
}

int main(int argc,char**argv){
    Options options;
    for(int i=1;i<argc;++i){
        if(!strcmp(argv[i],"--manifest")&&i+1<argc)options.manifest=argv[++i];
        else if(!strcmp(argv[i],"--max-files")&&i+1<argc){char*end=nullptr;unsigned long long value=strtoull(argv[++i],&end,10);
            if(!end||*end||!value)return 2;
            options.max_files=size_t(value);}
        else if(!strcmp(argv[i],"--repetitions")&&i+1<argc){char*end=nullptr;unsigned long value=strtoul(argv[++i],&end,10);
            if(!end||*end||!value||value>UINT32_MAX)return 2;
            options.repetitions=uint32_t(value);}
        else if(!strcmp(argv[i],"--codec")&&i+1<argc){if(!parse_policy(argv[++i],options.policy))return 2;}
        else if(!strcmp(argv[i],"--z")&&i+1<argc){const char*value=argv[++i];
            if(!strcmp(value,"1"))options.policy=capp::CodecPolicy::Zstd1;else if(!strcmp(value,"3"))options.policy=capp::CodecPolicy::Zstd3;else return 2;}
        else if(!strcmp(argv[i],"--restart")&&i+1<argc){options.scenario=SC_RESTART;options.scenario_tu=uint32_t(strtoul(argv[++i],nullptr,10));}
        else if(!strcmp(argv[i],"--latejoin")&&i+1<argc){options.scenario=SC_LATEJOIN;options.scenario_tu=uint32_t(strtoul(argv[++i],nullptr,10));}
        else if(!strcmp(argv[i],"--evict")&&i+1<argc)options.public_budget=uint32_t(strtoul(argv[++i],nullptr,10));
        else if(!strcmp(argv[i],"--corrupt-tu")&&i+1<argc)options.corrupt_tu=uint32_t(strtoul(argv[++i],nullptr,10));
        else{fprintf(stderr,"unknown option: %s\n",argv[i]);return 2;}
    }
    if(!options.manifest){
        fprintf(stderr,"usage: %s --manifest F [--codec raw|z1|z3|best] [--repetitions N] [--max-files N] [--restart T|--latejoin T] [--evict N] [--corrupt-tu T]\n",argv[0]);return 2;
    }

    auto start=Clock::now();Corpus corpus=load_corpus(options.manifest,options.max_files);Interner dictionary;
    if(corpus.files.empty()||corpus.files.size()>UINT32_MAX)return 2;
    uint64_t logicalTotal=uint64_t(corpus.files.size())*options.repetitions;
    if(logicalTotal>UINT32_MAX)return 2;
    if(options.scenario!=SC_COLD&&options.scenario_tu>=logicalTotal){fprintf(stderr,"scenario TU out of range\n");return 2;}
    if(options.corrupt_tu!=UINT32_MAX&&options.corrupt_tu>=logicalTotal){fprintf(stderr,"corrupt TU out of range\n");return 2;}

    std::vector<uint32_t>allRegions;std::vector<size_t>regionOffsets{0};
    {uint32_t maxLength=0;for(const auto&file:corpus.files)maxLength=std::max(maxLength,file.len);
     std::vector<uint32_t>output(size_t(maxLength)+1),regions;uint64_t hits=0;
     for(const auto&file:corpus.files){size_t outputCount=0;regions.clear();const char*begin=corpus.bytes.data()+file.off;
        dictionary.process(begin,begin+file.len,output.data(),outputCount,hits,true,&regions);
        allRegions.insert(allRegions.end(),regions.begin(),regions.end());regionOffsets.push_back(allRegions.size());}}
    uint32_t nreg=uint32_t(dictionary.region_count()),physicalTus=uint32_t(corpus.files.size());
    fprintf(stderr,"M4 loaded+interned %.3fs TUs=%u raw=%llu regions=%u occurrences=%zu lines=%u\n",seconds_since(start),physicalTus,
            (unsigned long long)corpus.raw,nreg,allRegions.size(),dictionary.distinct());

    // S1 online previous-factor blocks, identical to the accepted M3 semantic checkpoint.
    std::vector<uint32_t>blockChildren;std::vector<size_t>blockOffsets{0};std::unordered_map<uint64_t,uint32_t>blockDictionary;
    std::vector<uint32_t>tokens;std::vector<size_t>tokenOffsets{0};std::vector<uint32_t>blockCopySource;std::vector<uint8_t>blockCopyOk;
    {size_t streamSize=allRegions.size();constexpr uint32_t MIN_MATCH=3,MAX_CHAIN=64,HASH_BITS=22;
     std::vector<uint32_t>head(size_t(1)<<HASH_BITS,UINT32_MAX),previous(streamSize,UINT32_MAX);
     auto kgram=[&](size_t position){uint64_t hash=1469598103934665603ull;for(uint32_t j=0;j<MIN_MATCH;++j){hash^=allRegions[position+j];hash*=1099511628211ull;}
        return (hash*0x9e3779b97f4a7c15ull)>>(64-HASH_BITS);};
     auto blockGet=[&](const uint32_t*values,size_t length,uint32_t source,uint8_t copyOk){
        uint64_t hash=1469598103934665603ull^(length*0x100000001b3ull);for(size_t j=0;j<length;++j){hash^=values[j];hash*=1099511628211ull;}
        auto found=blockDictionary.find(hash);if(found!=blockDictionary.end()){uint32_t block=found->second;
            if(blockOffsets[block+1]-blockOffsets[block]==length&&!memcmp(&blockChildren[blockOffsets[block]],values,length*sizeof(uint32_t)))return nreg+block;}
        uint32_t block=uint32_t(blockOffsets.size()-1);blockChildren.insert(blockChildren.end(),values,values+length);blockOffsets.push_back(blockChildren.size());
        blockCopySource.push_back(source);blockCopyOk.push_back(copyOk);if(found==blockDictionary.end())blockDictionary.emplace(hash,block);return nreg+block;};
     for(uint32_t tu=0;tu<physicalTus;++tu){size_t begin=regionOffsets[tu],end=regionOffsets[tu+1],position=begin;
        while(position<end){size_t bestLength=0,bestPosition=0;
            if(position+MIN_MATCH<=end&&position+MIN_MATCH<=streamSize){uint32_t candidate=head[kgram(position)],chain=0;
                while(candidate!=UINT32_MAX&&chain<MAX_CHAIN){if(candidate<position){size_t length=0,maximum=end-position;
                    while(length<maximum&&allRegions[candidate+length]==allRegions[position+length])++length;
                    if(length>=MIN_MATCH&&length>bestLength){bestLength=length;bestPosition=candidate;if(length==maximum)break;}}
                    candidate=previous[candidate];++chain;}}
            size_t step=1;if(bestLength>=MIN_MATCH){tokens.push_back(blockGet(&allRegions[position],bestLength,uint32_t(bestPosition),bestPosition+bestLength<=regionOffsets[tu]));step=bestLength;}
            else tokens.push_back(allRegions[position]);
            for(size_t j=position;j<position+step;++j)if(j+MIN_MATCH<=streamSize){uint64_t gram=kgram(j);previous[j]=head[gram];head[gram]=uint32_t(j);}
            position+=step;}
        tokenOffsets.push_back(tokens.size());}}
    uint32_t nblk=uint32_t(blockOffsets.size()-1);
    fprintf(stderr,"M4 S1 blocks=%u tokens=%zu token/region=%.4f\n",nblk,tokens.size(),double(tokens.size())/allRegions.size());

    int sockets[2];if(socketpair(AF_UNIX,SOCK_STREAM,0,sockets)!=0){perror("socketpair");return 2;}
    pid_t child=fork();if(child<0){perror("fork");return 2;}
    if(child==0){close(sockets[0]);int result=run_f(sockets[1],corpus,options);close(sockets[1]);_exit(result);}
    close(sockets[1]);
    int result=run_c(sockets[0],dictionary,corpus,tokens,tokenOffsets,blockOffsets,blockChildren,blockCopySource,blockCopyOk,nreg,nblk,options);
    close(sockets[0]);int status=0;waitpid(child,&status,0);bool childOk=WIFEXITED(status)&&WEXITSTATUS(status)==0;
    struct rusage self{},children{};getrusage(RUSAGE_SELF,&self);getrusage(RUSAGE_CHILDREN,&children);
    fprintf(stderr,"M4 complete total=%.3fs C_peak=%.1fMiB F_peak=%.1fMiB child_ok=%s\n",seconds_since(start),self.ru_maxrss/1024.0,
            children.ru_maxrss/1024.0,childOk?"yes":"no");
    return result==0&&childOk?0:1;
}
