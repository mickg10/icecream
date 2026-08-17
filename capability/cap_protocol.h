#pragma once
// icecream #16 capability harness — cap_protocol.h
// (De)serialization of the per-TU C<->F dialogue payloads carried inside cap_transport
// frames.  Sub-framing only; the codec semantics live in cap_codec.  Every blob is
// length-prefixed with a varint so the reader needs no side-channel counts.
//
//   Root (C->F) = blob(rootb) ++ blob(blockRaw)          blockRaw empty => no new blocks
//   Need (F->C) = missingRaw                              (varint cnt ++ ids ++ varint 0)
//   Fill (C->F) = blob(fill_paths) ++ blob(m0)..blob(m3) mixed parts, empty when absent
#include <array>
#include <cstdint>
#include <vector>
#include "cap_codec.h"
#include "cap_identity.h"

namespace capp {

static constexpr uint32_t CAP_PROTOCOL_VERSION=50;
enum class ComponentCodec : uint8_t { Raw=0, Zstd1=1, Zstd3=2 };
enum class CodecPolicy : uint8_t { Raw=0, Zstd1=1, Zstd3=2, Best=3 };

struct EncodedComponent {
    std::vector<uint8_t> wire;       // selector byte followed by raw bytes or one complete zstd frame
    ComponentCodec codec=ComponentCodec::Raw;
    size_t raw_size=0,z1_size=SIZE_MAX,z3_size=SIZE_MAX;
};

static inline size_t component_cost(size_t payloadBytes,bool blobLengthPrefix){
    size_t envelope=payloadBytes+1;  // explicit codec selector
    return envelope+(blobLengthPrefix?capc::varint_size(envelope):0);
}

static inline EncodedComponent encode_component(const std::vector<uint8_t>&raw,CodecPolicy policy,
                                                ZSTD_CCtx*z1,ZSTD_CCtx*z3,bool blobLengthPrefix){
    EncodedComponent result;result.raw_size=raw.size();
    const std::vector<uint8_t>*best=&raw;size_t bestCost=component_cost(raw.size(),blobLengthPrefix);
    std::vector<uint8_t>one,three;
    auto consider=[&](int level,ZSTD_CCtx*ctx,std::vector<uint8_t>&candidate,ComponentCodec codec){
        size_t n=capc::zstd_size(ctx,raw.data(),raw.size(),level,candidate);candidate.resize(n);
        if(level==1)result.z1_size=n;else result.z3_size=n;
        size_t cost=component_cost(n,blobLengthPrefix);
        if(cost<bestCost){bestCost=cost;best=&candidate;result.codec=codec;}
    };
    if(policy==CodecPolicy::Zstd1||policy==CodecPolicy::Best)consider(1,z1,one,ComponentCodec::Zstd1);
    if(policy==CodecPolicy::Zstd3||policy==CodecPolicy::Best)consider(3,z3,three,ComponentCodec::Zstd3);
    result.wire.reserve(best->size()+1);result.wire.push_back(uint8_t(result.codec));result.wire.insert(result.wire.end(),best->begin(),best->end());
    return result;
}

static inline bool decode_component(const std::vector<uint8_t>&wire,ZSTD_DCtx*d,std::vector<uint8_t>&raw){
    if(wire.empty())return false;
    uint8_t selector=wire[0];
    if(selector==uint8_t(ComponentCodec::Raw)){raw.assign(wire.begin()+1,wire.end());return true;}
    if(selector!=uint8_t(ComponentCodec::Zstd1)&&selector!=uint8_t(ComponentCodec::Zstd3))return false;
    std::vector<uint8_t>frame(wire.begin()+1,wire.end());return capc::zstd_frame_try_decode(d,frame,raw);
}

static inline void put_blob(std::vector<uint8_t>& o, const std::vector<uint8_t>& b){
    capc::put_varint(o, b.size());
    o.insert(o.end(), b.begin(), b.end());
}
static inline std::vector<uint8_t> get_blob(const uint8_t*& p, const uint8_t* e){
    uint64_t n = capc::get_varint(p);
    if(uint64_t(e-p) < n){ fprintf(stderr,"protocol: truncated blob\n"); exit(2); }
    std::vector<uint8_t> b(p, p+n); p += n; return b;
}
static inline bool try_get_blob(const uint8_t*&p,const uint8_t*e,std::vector<uint8_t>&out){
    uint64_t n=0;if(!capc::get_varint_bounded(p,e,n)||n>size_t(e-p))return false;
    out.assign(p,p+size_t(n));p+=size_t(n);return true;
}

// ---- M4 Hello: one relationship-scoped cache generation, protocol 50 ----
static inline std::vector<uint8_t> pack_hello_m4(const cap::SourceGeneration&generation,uint32_t nreg,
                                                 uint32_t nblk,uint32_t physicalTus,uint32_t repetitions){
    std::vector<uint8_t>out(generation.begin(),generation.end());
    capc::put_varint(out,CAP_PROTOCOL_VERSION);capc::put_varint(out,nreg);capc::put_varint(out,nblk);
    capc::put_varint(out,physicalTus);capc::put_varint(out,repetitions);return out;
}
static inline bool try_unpack_hello_m4(const std::vector<uint8_t>&payload,cap::SourceGeneration&generation,
                                       uint32_t&nreg,uint32_t&nblk,uint32_t&physicalTus,uint32_t&repetitions){
    if(payload.size()<16)return false;
    memcpy(generation.data(),payload.data(),16);
    const uint8_t*p=payload.data()+16,*e=payload.data()+payload.size();uint64_t version=0;
    return capc::get_varint_bounded(p,e,version)&&version==CAP_PROTOCOL_VERSION&&
           capc::get_u32_bounded(p,e,nreg)&&capc::get_u32_bounded(p,e,nblk)&&
           capc::get_u32_bounded(p,e,physicalTus)&&capc::get_u32_bounded(p,e,repetitions)&&p==e&&
           physicalTus>0&&repetitions>0;
}

// ---- M4 Root: TU index + independently selected Root and Block components ----
static inline std::vector<uint8_t> pack_root_m4(uint32_t tu,const std::vector<uint8_t>&root,
                                                const std::vector<uint8_t>&blocks){
    std::vector<uint8_t>out;capc::put_varint(out,tu);put_blob(out,root);put_blob(out,blocks);return out;
}
static inline bool try_unpack_root_m4(const std::vector<uint8_t>&payload,uint32_t&tu,
                                      std::vector<uint8_t>&root,std::vector<uint8_t>&blocks){
    const uint8_t*p=payload.data(),*e=p+payload.size();
    return capc::get_u32_bounded(p,e,tu)&&try_get_blob(p,e,root)&&try_get_blob(p,e,blocks)&&p==e;
}

// ---- M4 Need: TU index + one selected component ----
static inline std::vector<uint8_t> pack_need_m4(uint32_t tu,const std::vector<uint8_t>&need){
    std::vector<uint8_t>out;capc::put_varint(out,tu);put_blob(out,need);return out;
}
static inline bool try_unpack_need_m4(const std::vector<uint8_t>&payload,uint32_t&tu,std::vector<uint8_t>&need){
    const uint8_t*p=payload.data(),*e=p+payload.size();return capc::get_u32_bounded(p,e,tu)&&try_get_blob(p,e,need)&&p==e;
}

// ---- M4 Fill: TU index + replay-stable ordinal bases + five selected components ----
static inline std::vector<uint8_t> pack_fill_m4(uint32_t tu,uint32_t pathBase,uint32_t publicBase,
                                                const std::vector<uint8_t>&paths,
                                                const std::array<std::vector<uint8_t>,6>&mixed,size_t partCount){
    std::vector<uint8_t>out;capc::put_varint(out,tu);capc::put_varint(out,pathBase);capc::put_varint(out,publicBase);
    put_blob(out,paths);for(size_t i=0;i<partCount;++i)put_blob(out,mixed[i]);return out;
}
static inline bool try_unpack_fill_m4(const std::vector<uint8_t>&payload,uint32_t&tu,uint32_t&pathBase,
                                      uint32_t&publicBase,std::vector<uint8_t>&paths,
                                      std::array<std::vector<uint8_t>,6>&mixed,size_t partCount){
    const uint8_t*p=payload.data(),*e=p+payload.size();
    if(!capc::get_u32_bounded(p,e,tu)||!capc::get_u32_bounded(p,e,pathBase)||!capc::get_u32_bounded(p,e,publicBase)||!try_get_blob(p,e,paths))return false;
    for(size_t i=0;i<partCount;++i)if(!try_get_blob(p,e,mixed[i]))return false;
    return p==e;
}

static inline std::vector<uint8_t> pack_tu_ack(uint32_t tu,bool accepted){
    std::vector<uint8_t>out;capc::put_varint(out,tu);out.push_back(accepted?1:0);return out;
}
static inline bool try_unpack_tu_ack(const std::vector<uint8_t>&payload,uint32_t&tu,bool&accepted){
    const uint8_t*p=payload.data(),*e=p+payload.size();
    if(!capc::get_u32_bounded(p,e,tu)||e-p!=1||*p>1)return false;
    accepted=*p!=0;return true;
}

// M5 Ack carries the receiver's post-TU removals. C applies these to that
// receiver's mirror before assigning its next TU. Generation-local ordinals
// remain immutable and are never reused.
struct CacheDropsM5 {
    std::vector<uint32_t>regions,public_lines,blocks;
};
static inline void put_u32_list(std::vector<uint8_t>&out,const std::vector<uint32_t>&values){
    capc::put_varint(out,values.size());for(uint32_t value:values)capc::put_varint(out,value);
}
static inline bool try_get_u32_list(const uint8_t*&p,const uint8_t*e,std::vector<uint32_t>&values,uint32_t limit,bool allowZero){
    uint64_t count=0;if(!capc::get_varint_bounded(p,e,count)||count>size_t(e-p))return false;
    values.clear();values.reserve(size_t(count));
    for(uint64_t i=0;i<count;++i){uint32_t value=0;if(!capc::get_u32_bounded(p,e,value)||value>=limit||(!allowZero&&!value))return false;
        values.push_back(value);}
    return true;
}
static inline std::vector<uint8_t> pack_tu_ack_m5(uint32_t tu,bool accepted,const CacheDropsM5&drops){
    std::vector<uint8_t>out;capc::put_varint(out,tu);out.push_back(accepted?1:0);
    put_u32_list(out,drops.regions);put_u32_list(out,drops.public_lines);put_u32_list(out,drops.blocks);return out;
}
static inline bool try_unpack_tu_ack_m5(const std::vector<uint8_t>&payload,uint32_t&tu,bool&accepted,CacheDropsM5&drops,
                                        uint32_t nreg,uint32_t nblk,uint32_t publicLimit=UINT32_MAX){
    const uint8_t*p=payload.data(),*e=p+payload.size();if(!capc::get_u32_bounded(p,e,tu)||p==e||*p>1)return false;
    accepted=*p++!=0;return try_get_u32_list(p,e,drops.regions,nreg,true)&&
        try_get_u32_list(p,e,drops.public_lines,publicLimit,false)&&try_get_u32_list(p,e,drops.blocks,nblk,true)&&p==e;
}

// ---- Root ----
static inline std::vector<uint8_t> pack_root(const std::vector<uint8_t>& rootb,
                                             const std::vector<uint8_t>& blockRaw){
    std::vector<uint8_t> o; put_blob(o, rootb); put_blob(o, blockRaw); return o;
}
static inline void unpack_root(const std::vector<uint8_t>& payload,
                               std::vector<uint8_t>& rootb, std::vector<uint8_t>& blockRaw){
    const uint8_t* p = payload.data(); const uint8_t* e = p + payload.size();
    rootb = get_blob(p, e); blockRaw = get_blob(p, e);
    if(p != e){ fprintf(stderr,"protocol: root trailing bytes\n"); exit(2); }
}

// ---- Fill ----
static inline std::vector<uint8_t> pack_fill(const std::vector<uint8_t>& fill_paths,
                                             const std::array<std::vector<uint8_t>,6>& mixedEncoded,
                                             size_t mixedPartCount){
    std::vector<uint8_t> o; put_blob(o, fill_paths);
    for(size_t i=0;i<mixedPartCount;++i) put_blob(o, mixedEncoded[i]);
    return o;
}
static inline void unpack_fill(const std::vector<uint8_t>& payload,
                               std::vector<uint8_t>& fill_paths,
                               std::array<std::vector<uint8_t>,6>& mixedEncoded,
                               size_t mixedPartCount){
    const uint8_t* p = payload.data(); const uint8_t* e = p + payload.size();
    fill_paths = get_blob(p, e);
    for(size_t i=0;i<mixedPartCount;++i) mixedEncoded[i] = get_blob(p, e);
    if(p != e){ fprintf(stderr,"protocol: fill trailing bytes\n"); exit(2); }
}

// ---- M2 Rejoin (F->C, Frame::Rejoin) / resync reply (C->F, Frame::Ack) ----
// Rejoin  = varint(resumeTU).  Resync = varint(nextPublicOrdinal) ++ the full path table
// (generation-lived, bulk-restored so op4/op5 pathIds resolve; public Lines stay on-demand).
static inline std::vector<uint8_t> pack_rejoin(uint32_t resumeTU){
    std::vector<uint8_t> o; capc::put_varint(o,resumeTU); return o;
}
static inline uint32_t unpack_rejoin(const std::vector<uint8_t>& p){
    const uint8_t* q=p.data(); return uint32_t(capc::get_varint(q));
}
static inline bool try_unpack_rejoin(const std::vector<uint8_t>&payload,uint32_t&resumeTU){
    const uint8_t*p=payload.data(),*e=p+payload.size();return capc::get_u32_bounded(p,e,resumeTU)&&p==e;
}
static inline std::vector<uint8_t> pack_resync(uint32_t nextPublic, const std::vector<std::string>& paths){
    std::vector<uint8_t> o; capc::put_varint(o,nextPublic); capc::put_varint(o,paths.size());
    for(const auto& s:paths){ capc::put_varint(o,s.size()); o.insert(o.end(),s.begin(),s.end()); }
    return o;
}
static inline void unpack_resync(const std::vector<uint8_t>& p, uint32_t& nextPublic, std::vector<std::string>& paths){
    const uint8_t* q=p.data(),*e=q+p.size(); nextPublic=uint32_t(capc::get_varint(q));
    uint64_t n=capc::get_varint(q); paths.clear(); paths.reserve(n);
    for(uint64_t i=0;i<n;++i){ uint64_t L=capc::get_varint(q); if(uint64_t(e-q)<L){fprintf(stderr,"protocol: resync trunc\n");exit(2);} paths.emplace_back((const char*)q,(size_t)L); q+=L; }
}
static inline bool try_unpack_resync(const std::vector<uint8_t>&payload,uint32_t&nextPublic,std::vector<std::string>&paths){
    const uint8_t*p=payload.data(),*e=p+payload.size();uint64_t count=0;
    if(!capc::get_u32_bounded(p,e,nextPublic)||!nextPublic||!capc::get_varint_bounded(p,e,count)||count>payload.size())return false;
    paths.clear();paths.reserve(size_t(count));
    for(uint64_t i=0;i<count;++i){uint64_t length=0;if(!capc::get_varint_bounded(p,e,length)||length>size_t(e-p))return false;
        paths.emplace_back((const char*)p,size_t(length));p+=size_t(length);}
    return p==e;
}

}  // namespace capp
