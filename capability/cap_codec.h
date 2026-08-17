#pragma once
// icecream #16 capability harness — cap_codec.h
// Codec primitives EXTRACTED VERBATIM from codec50-m1.cpp (the verified reference).
// Canonical M1 path only: --mixed-regions --byte-array-lines --direct-ordinals, z3.
// Shared by both the C (encoder/authority) and F (decoder/store) roles.  Nothing here
// is redesigned; the hashing, Interner, byte-array/marker parsing and zstd helpers are
// byte-for-byte the reference so the two-process split reproduces the exact wire ledger.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <immintrin.h>
#include <zstd.h>

namespace capc {

static inline uint64_t mix64(uint64_t x){ x^=x>>30; x*=0xbf58476d1ce4e5b9ULL; x^=x>>27; x*=0x94d049bb133111ebULL; return x^(x>>31); }
static inline uint64_t fold128(uint64_t a,uint64_t b){ __uint128_t p=__uint128_t(a)*b; return uint64_t(p)^uint64_t(p>>64); }
static inline uint64_t read64(const char*p){ uint64_t v; memcpy(&v,p,8); return v; }
static inline uint64_t read_tail(const char*p,uint32_t n){ uint64_t v=0; memcpy(&v,p,n); return v; }
static inline uint64_t sampled_hash(const char*p,uint32_t n){
    constexpr uint64_t A=0xa0761d6478bd642fULL,B=0xe7037ed1a0b428dbULL; uint64_t h=mix64(uint64_t(n)^A);
    if(n<=8) return mix64(h^read_tail(p,n));
    if(n<=16) return fold128(read64(p)^A,read64(p+n-8)^h);
    if(n<=32){ h=fold128(read64(p)^A,read64(p+8)^h); return fold128(read64(p+n-16)^B,read64(p+n-8)^h); }
    uint32_t mid=(n>>1)-4; h=fold128(read64(p)^A,read64(p+8)^h); h=fold128(read64(p+mid)^B,read64(p+n-16)^h); return fold128(read64(p+n-8)^A,h^B);
}
static inline uint64_t line_hash(const char*key,uint32_t len){
    constexpr uint64_t secret[3]={0x2d358dccaa6c78a5ULL,0x8bb84b93962eacc9ULL,0x4b33a62ed433d4a3ULL};
    uint64_t seed=0xbdd89aa982704029ULL^uint64_t(len); const char*p=key; uint32_t n=len;
    if(n<=16){ uint64_t a=0,b=0; if(n>=8){a=read64(p);b=read64(p+n-8);} else if(n){a=read_tail(p,n);b=a;} return fold128(a^secret[0],b^seed^secret[1]); }
    uint64_t a=read64(p)^secret[0], b=read64(p+8)^seed; p+=16; n-=16;
    while(n>=48){ seed=fold128(read64(p)^secret[0],read64(p+8)^seed); a=fold128(read64(p+16)^secret[1],read64(p+24)^a); b=fold128(read64(p+32)^secret[2],read64(p+40)^b); p+=48;n-=48; }
    while(n>=16){ seed=fold128(read64(p)^secret[0],read64(p+8)^seed); p+=16;n-=16; }
    if(n){ uint64_t x=n>=8?read64(p):read_tail(p,n); uint64_t y=n>=8?read64(p+n-8):x; seed=fold128(x^secret[1],y^seed); }
    return fold128(a^secret[0],b^seed^secret[2]);
}
static inline const char* next_region(const char*p,const char*end){ const char*q=p+1;
#if defined(__AVX512BW__)
    const __m512i hh=_mm512_set1_epi8('#'); while(q+64<=end){ __m512i v=_mm512_loadu_si512((const void*)q); uint64_t m=_mm512_cmpeq_epi8_mask(v,hh);
        while(m){ unsigned bit=__builtin_ctzll(m); const char*c=q+bit; if(c[-1]=='\n'&&c+1<end&&c[1]==' ') return c; m&=m-1; } q+=64; }
#elif defined(__AVX2__)
    const __m256i hh=_mm256_set1_epi8('#'); while(q+32<=end){ __m256i v=_mm256_loadu_si256((const __m256i*)q); uint32_t m=_mm256_movemask_epi8(_mm256_cmpeq_epi8(v,hh));
        while(m){ unsigned bit=__builtin_ctz(m); const char*c=q+bit; if(c[-1]=='\n'&&c+1<end&&c[1]==' ') return c; m&=m-1; } q+=32; }
#endif
    while(q+1<end){ if(*q=='#'&&q[-1]=='\n'&&q[1]==' ') return q; ++q; } return end;
}
static inline void* huge_zeroed(size_t bytes){ constexpr size_t H=2u<<20; bytes=(bytes+H-1)&~(H-1); void*p=mmap(nullptr,bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0); if(p==MAP_FAILED){perror("mmap");exit(2);} madvise(p,bytes,MADV_HUGEPAGE); return p; }

struct FileSpan{ uint64_t off; uint32_t len; };
struct LineRef{ uint32_t off; uint32_t len; };
struct TinySlot{ uint64_t bytes; uint32_t id; uint8_t len; uint8_t pad[3]; };
struct ShortSlot{ uint64_t lo; uint64_t hi; uint32_t id; uint8_t len; uint8_t pad[3]; };
struct LineSlot{ uint64_t hash; uint32_t off; uint32_t len; uint32_t id; uint32_t pad; };
struct RegionRecord{ uint64_t hash; uint32_t raw_off; uint32_t raw_len; uint32_t ids_off; uint32_t ids_count; uint32_t next1; uint32_t next2; };

#ifndef ICE_LINE_CAP_LOG2
#define ICE_LINE_CAP_LOG2 21
#endif

class Interner {
public:
    static constexpr uint32_t TINY_CAP=1u<<10, SHORT_CAP=1u<<17,
                              LINE_CAP=1u<<ICE_LINE_CAP_LOG2,
                              REGION_INITIAL_CAP=1u<<17;
    Interner(){ tiny_=(TinySlot*)huge_zeroed(sizeof(TinySlot)*TINY_CAP); short_=(ShortSlot*)huge_zeroed(sizeof(ShortSlot)*SHORT_CAP); lines_=(LineSlot*)huge_zeroed(sizeof(LineSlot)*LINE_CAP);
        region_index_.resize(REGION_INITIAL_CAP); region_mask_=REGION_INITIAL_CAP-1; region_records_.reserve(1u<<20); line_bytes_.reserve(64u<<20); region_bytes_.reserve(80u<<20); region_ids_.reserve(8u<<20); id_refs_.reserve(1000000); id_refs_.push_back({0,0}); }
    uint32_t distinct() const { return next_id_-1; }
    uint64_t region_count() const { return region_records_.size(); }
    const LineRef& ref(uint32_t id) const { return id_refs_[id]; }
    const char* line_data(uint32_t off) const { return line_bytes_.data()+off; }
    const uint32_t* region_ids_ptr(uint32_t rid) const { return region_ids_.data()+region_records_[rid].ids_off; }
    uint32_t region_ids_count(uint32_t rid) const { return region_records_[rid].ids_count; }
    const char* region_data(uint32_t rid) const { return region_bytes_.data()+region_records_[rid].raw_off; }
    uint32_t region_raw_len(uint32_t rid) const { return region_records_[rid].raw_len; }
    uint64_t region_key(uint32_t rid) const { return uint64_t(rid)+1; }
    uint64_t distinct_line_bytes() const { return line_bytes_.size(); }
    void process(const char*begin,const char*end,uint32_t*out,size_t&out_count,uint64_t&hits,bool train,std::vector<uint32_t>*rout){
        const char*p=begin; uint32_t previous=UINT32_MAX;
        while(p<end){ bool found=false; uint32_t region_id=UINT32_MAX,n=0; uint64_t h=0;
            if(previous!=UINT32_MAX){ const RegionRecord&prev=region_records_[previous]; const uint32_t cand[2]={prev.next1,prev.next2};
                for(uint32_t enc:cand){ if(!enc)continue; uint32_t cid=enc-1; const RegionRecord&c=region_records_[cid]; if(c.raw_len>uint64_t(end-p))continue; const char*ce=p+c.raw_len;
                    bool eb= ce==end||(ce+1<end&&ce[-1]=='\n'&&ce[0]=='#'&&ce[1]==' ');
                    if(eb&&memcmp(region_bytes_.data()+c.raw_off,p,c.raw_len)==0){ region_id=cid; n=c.raw_len; memcpy(out+out_count,region_ids_.data()+c.ids_off,size_t(c.ids_count)*4); out_count+=c.ids_count; ++hits; found=true; break; } } }
            if(!found){ const char*q=next_region(p,end); n=uint32_t(q-p); h=sampled_hash(p,n)|1ULL; uint32_t slot=uint32_t(h)&region_mask_,probes=0;
                for(;;){ if(++probes>region_index_.size()){fprintf(stderr,"region tbl\n");exit(2);} uint32_t enc=region_index_[slot]; if(!enc)break; RegionRecord&r=region_records_[enc-1];
                    if(r.hash==h&&r.raw_len==n&&memcmp(region_bytes_.data()+r.raw_off,p,n)==0){ region_id=enc-1; memcpy(out+out_count,region_ids_.data()+r.ids_off,size_t(r.ids_count)*4); out_count+=r.ids_count; ++hits; found=true; break; } slot=(slot+1)&region_mask_; } }
            if(!found){ const char*q=p+n; uint32_t ids_off=uint32_t(region_ids_.size()); const char*lp=p;
                while(lp<q){ const void*hit=memchr(lp,'\n',size_t(q-lp)); const char*le=hit?(const char*)hit+1:q; uint32_t id=intern_line(lp,uint32_t(le-lp)); region_ids_.push_back(id); out[out_count++]=id; lp=le; }
                uint32_t raw_off=uint32_t(region_bytes_.size()); region_bytes_.insert(region_bytes_.end(),p,q); region_id=uint32_t(region_records_.size()); region_records_.push_back({h,raw_off,n,ids_off,uint32_t(region_ids_.size()-ids_off),0,0}); insert_region_index(region_id); }
            if(rout) rout->push_back(region_id);
            if(train&&previous!=UINT32_MAX){ RegionRecord&prev=region_records_[previous]; uint32_t enc=region_id+1; if(!prev.next1)prev.next1=enc; else if(prev.next1!=enc&&!prev.next2)prev.next2=enc; }
            previous=region_id; p+=n; } }
    uint32_t intern_line(const char*p,uint32_t n){ if(n<=4)return intern_tiny(p,n); if(n<=16)return intern_short(p,n);
        uint64_t h=line_hash(p,n)|1ULL; uint32_t slot=uint32_t(h)&(LINE_CAP-1),probes=0;
        for(;;){ if(++probes>LINE_CAP){fprintf(stderr,"line tbl\n");exit(2);} LineSlot&s=lines_[slot]; if(!s.id){ uint32_t id=add_line(p,n); s={h,id_refs_[id].off,n,id,0}; return id; } if(s.hash==h&&s.len==n&&memcmp(line_bytes_.data()+s.off,p,n)==0)return s.id; slot=(slot+1)&(LINE_CAP-1); } }
private:
    void insert_region_index(uint32_t region_id){ if((region_records_.size()*10)>(region_index_.size()*7)){ std::vector<uint32_t> g(region_index_.size()*2); uint32_t nm=uint32_t(g.size()-1);
            for(uint32_t id=0;id<region_records_.size()-1;++id){ uint32_t slot=uint32_t(region_records_[id].hash)&nm; while(g[slot])slot=(slot+1)&nm; g[slot]=id+1; } region_index_.swap(g); region_mask_=nm; }
        uint32_t slot=uint32_t(region_records_[region_id].hash)&region_mask_; while(region_index_[slot])slot=(slot+1)&region_mask_; region_index_[slot]=region_id+1; }
    uint32_t add_line(const char*p,uint32_t n){ uint32_t off=uint32_t(line_bytes_.size()); line_bytes_.insert(line_bytes_.end(),p,p+n); uint32_t id=next_id_++; id_refs_.push_back({off,n}); return id; }
    uint32_t intern_tiny(const char*p,uint32_t n){ uint64_t bytes=read_tail(p,n); uint32_t slot=uint32_t(mix64(bytes^(uint64_t(n)<<56)))&(TINY_CAP-1),probes=0;
        for(;;){ if(++probes>TINY_CAP){fprintf(stderr,"tiny\n");exit(2);} TinySlot&s=tiny_[slot]; if(!s.id){uint32_t id=add_line(p,n);s.bytes=bytes;s.id=id;s.len=uint8_t(n);return id;} if(s.len==n&&s.bytes==bytes)return s.id; slot=(slot+1)&(TINY_CAP-1);} }
    uint32_t intern_short(const char*p,uint32_t n){ uint64_t lo=n>=8?read64(p):read_tail(p,n); uint64_t hi=n>8?read_tail(p+8,n-8):lo; uint64_t h=fold128(lo^0xa0761d6478bd642fULL,hi^uint64_t(n)*0xe7037ed1a0b428dbULL); uint32_t slot=uint32_t(h)&(SHORT_CAP-1),probes=0;
        for(;;){ if(++probes>SHORT_CAP){fprintf(stderr,"short\n");exit(2);} ShortSlot&s=short_[slot]; if(!s.id){uint32_t id=add_line(p,n);s.lo=lo;s.hi=hi;s.id=id;s.len=uint8_t(n);return id;} if(s.len==n&&s.lo==lo&&s.hi==hi)return s.id; slot=(slot+1)&(SHORT_CAP-1);} }
    TinySlot*tiny_=nullptr; ShortSlot*short_=nullptr; LineSlot*lines_=nullptr;
    std::vector<uint32_t> region_index_; std::vector<RegionRecord> region_records_; std::vector<char> line_bytes_,region_bytes_; std::vector<uint32_t> region_ids_; std::vector<LineRef> id_refs_; uint32_t next_id_=1,region_mask_=0;
};

struct Corpus{ std::vector<char> bytes; std::vector<FileSpan> files; uint64_t raw=0; };
Corpus load_corpus(const char*manifest,size_t max_files);

// ---- varint / zigzag / u64 ----
static inline void put_varint(std::vector<uint8_t>&o,uint64_t v){ while(v>=0x80){o.push_back(uint8_t(v)|0x80);v>>=7;} o.push_back(uint8_t(v)); }
static inline uint64_t get_varint(const uint8_t*&p){ uint64_t v=0; int s=0; for(;;){ uint8_t b=*p++; v|=uint64_t(b&0x7f)<<s; if(!(b&0x80))break; s+=7; } return v; }
static inline void put_u64le(std::vector<uint8_t>&o,uint64_t v){for(unsigned i=0;i<8;++i)o.push_back(uint8_t(v>>(8*i)));}
static inline void put_zigzag(std::vector<uint8_t>&o,int64_t v){ put_varint(o,(uint64_t(v)<<1)^uint64_t(v>>63)); }
static inline int64_t get_zigzag(const uint8_t*&p){ uint64_t u=get_varint(p); return int64_t(u>>1)^-int64_t(u&1); }
static inline size_t varint_size(uint64_t v){ size_t n=1; while(v>=0x80){++n;v>>=7;} return n; }

// ---- zstd helpers (stateless message + stateful stream) ----
static inline size_t zstd_size(ZSTD_CCtx*c,const uint8_t*data,size_t n,int level,std::vector<uint8_t>&dst){ ZSTD_CCtx_reset(c,ZSTD_reset_session_and_parameters); ZSTD_CCtx_setParameter(c,ZSTD_c_compressionLevel,level);
    size_t bound=ZSTD_compressBound(n); if(dst.size()<bound)dst.resize(bound); size_t r=ZSTD_compress2(c,dst.data(),dst.size(),data?data:(const uint8_t*)"",n); if(ZSTD_isError(r)){fprintf(stderr,"zstd %s\n",ZSTD_getErrorName(r));exit(2);} return r; }
static inline size_t zstd_message_roundtrip(ZSTD_CCtx*c,ZSTD_DCtx*d,const std::vector<uint8_t>&raw,int level,
                                     std::vector<uint8_t>&encoded,std::vector<uint8_t>&decoded){
    size_t encodedSize=zstd_size(c,raw.data(),raw.size(),level,encoded);decoded.resize(raw.size());uint8_t empty=0;
    void*out=decoded.empty()?static_cast<void*>(&empty):static_cast<void*>(decoded.data());
    size_t got=ZSTD_decompressDCtx(d,out,decoded.size(),encoded.data(),encodedSize);
    if(ZSTD_isError(got)||got!=raw.size()||decoded!=raw){fprintf(stderr,"message roundtrip differs: %s\n",ZSTD_isError(got)?ZSTD_getErrorName(got):"size/content");exit(2);}
    return encodedSize;
}
static inline std::vector<uint8_t> zstd_stream_encode(ZSTD_CCtx*c,const std::vector<uint8_t>&raw,
                                                ZSTD_EndDirective directive){
    ZSTD_inBuffer input{raw.data(),raw.size(),0}; std::vector<uint8_t> encoded;
    std::vector<uint8_t> buffer(ZSTD_CStreamOutSize()); size_t remaining;
    do { ZSTD_outBuffer output{buffer.data(),buffer.size(),0};
        remaining=ZSTD_compressStream2(c,&output,&input,directive);
        if(ZSTD_isError(remaining)){fprintf(stderr,"zstd stream encode %s\n",ZSTD_getErrorName(remaining));exit(2);}
        encoded.insert(encoded.end(),buffer.begin(),buffer.begin()+output.pos);
    } while(input.pos<input.size || remaining!=0);
    return encoded;
}
static inline std::vector<uint8_t> zstd_stream_decode(ZSTD_DCtx*d,const std::vector<uint8_t>&encoded,
                                                size_t&remaining){
    ZSTD_inBuffer input{encoded.data(),encoded.size(),0}; std::vector<uint8_t> raw;
    std::vector<uint8_t> buffer(ZSTD_DStreamOutSize()); remaining=1;
    bool again;
    do { ZSTD_outBuffer output{buffer.data(),buffer.size(),0};
        size_t before=input.pos; remaining=ZSTD_decompressStream(d,&output,&input);
        if(ZSTD_isError(remaining)){fprintf(stderr,"zstd stream decode %s\n",ZSTD_getErrorName(remaining));exit(2);}
        raw.insert(raw.end(),buffer.begin(),buffer.begin()+output.pos);
        if(input.pos==before && output.pos==0){ if(input.pos==input.size)break; fprintf(stderr,"zstd stream made no progress\n");exit(2); }
        again=input.pos<input.size || output.pos==output.size;
    } while(again);
    return raw;
}

// ---- D1 marker factoring ----
struct Marker{ std::string path; uint64_t lineno; std::vector<uint8_t> flags; };
bool parse_marker(const char* s, uint32_t len, Marker& m);
void emit_marker(const Marker& m, std::vector<uint8_t>& out);

// ---- P21 byte-array factoring ----
enum ByteNumberFormat : uint8_t { DECIMAL=0, HEX_LL=1, HEX_LU=2, HEX_UL=3, HEX_UU=4 };
struct GeneratedByteArray {
    std::vector<uint8_t> values;
    std::string prefix, separator, suffix;
    uint8_t format=DECIMAL;
};
struct ByteArrayStyle {
    std::string prefix, separator, suffix;
    uint8_t format=DECIMAL;
    bool operator<(const ByteArrayStyle&o) const {
        if(prefix!=o.prefix)return prefix<o.prefix;
        if(separator!=o.separator)return separator<o.separator;
        if(suffix!=o.suffix)return suffix<o.suffix;
        return format<o.format;
    }
    bool operator==(const ByteArrayStyle&o) const {
        return prefix==o.prefix&&separator==o.separator&&suffix==o.suffix&&format==o.format;
    }
};
bool parse_byte_array(const char*data,uint32_t length,GeneratedByteArray&out);
void append_rendered_byte_array(const ByteArrayStyle&style,const uint8_t*values,size_t count,std::vector<uint8_t>&out);

// ---- P24 mixed-region line/region state ----
struct MixedCLineState { uint32_t source_region=UINT32_MAX; uint32_t source_offset=0; uint32_t public_id=0; };
struct MixedFLineView { uint32_t source_region=0; uint32_t source_offset=0; uint32_t length=0; };
struct MixedFRegionView { size_t offset=0; uint32_t length=0; bool known=false; };

// ---- M3: system-header source reads are REMOVED (the reduced grammar is self-describing).
// This stub exists only to make the disabled read path AUDITABLE and regression-enforced:
// any call increments a global counter and aborts, so a future edit that reintroduces a
// header read fails loudly.  A conformant M3 run never calls it (system_header_reads()==0). ----
uint64_t system_header_reads();
struct SourceTextStore {
    explicit SourceTextStore(bool=false){}
    [[noreturn]] void get(const std::string& path);   // DISABLED — aborts if ever called
};

// =====================================================================================
// C role: mixed-region materializer (encoder/authority).  Extracted from codec50-m1.cpp
// lines 778-888 for the canonical path (useByteArrayLines=true, useProjectSource=false).
// Persistent members survive across TUs; the per-TU output buffers are reset each call.
// =====================================================================================
struct MixedEncoder {
    // ---- persistent authority state ----
    std::vector<MixedCLineState> mixedCLine;                 // sized dict.distinct()+1 in init()
    uint32_t nextMixedPublic=1;
    std::unordered_map<std::string,uint32_t> pathid;
    std::vector<std::string> paths;
    // ---- counters (persistent) ---- mixedOps: [0]RAW_RUN runs [1]publish [2]ref [3]BYTE_ARRAY [4]PP_MARKER
    std::array<uint64_t,7> mixedOps{};
    uint64_t op7_count=0, op8_count=0, op9_count=0;          // M2 public-Line recovery ops
    uint64_t op7_wire=0, op8_wire=0, op9_wire=0;             // M2 "bytes recovered" split (raw)
    uint64_t mixedLiteralRaw=0, mixedArrayValues=0;
    uint64_t n_marker=0, n_literal=0;
    // ---- M2 authority model of F's cache (mirror; kept EXACT by F's declarations) ----
    std::vector<uint8_t> fknownReg;                         // sized NREG in init(); F holds region r
    std::vector<uint8_t> fknownPublic;                      // grows with nextMixedPublic; F holds ordinal
    bool recovering=false;                                  // post-restart/late-join: literal blocks + op7/op8
    // ---- per-TU outputs (reset at each materialize) ----
    std::array<std::vector<uint8_t>,6> mixedRaw;
    std::vector<uint8_t> fill_paths;
    uint32_t np=0;

    void init(uint32_t distinctLines, uint32_t nreg){ mixedCLine.assign(size_t(distinctLines)+1, MixedCLineState{}); fknownReg.assign(nreg,0); fknownPublic.assign(1,0); }
    void forget_public(uint32_t ord){ if(ord<fknownPublic.size()) fknownPublic[ord]=0; }   // F declared a drop
    void reset_model(){ std::fill(fknownReg.begin(),fknownReg.end(),0); std::fill(fknownPublic.begin(),fknownPublic.end(),0); recovering=true; }
    // Materialize missReg (IN RECEIVED ORDER) -> mixedRaw[0..3] + fill_paths.  Returns nr.
    uint32_t materialize(const Interner& dict, const std::vector<uint32_t>& missReg, size_t t);
};

// =====================================================================================
// F role: decoder/store.  Extracted from codec50-m1.cpp lines 678-686 (block install),
// 982-1038 (mixed fill decode), 1107-1127 (reconstruct).  Harness cross-checks against
// the C authority (dict) are removed; F touches only wire bytes + its own store + (for
// op5/op6) system-header files on disk, and verifies against corpus in the caller.
// =====================================================================================
struct FStore {
    uint32_t NREG=0, NBLK=0;
    std::vector<uint8_t> FmixedRegionData;
    std::vector<MixedFRegionView> FmixedRegions;            // size NREG
    // ---- M2: public Lines as IMMUTABLE materialized bytes (indexed by generation-local ordinal) ----
    std::vector<std::vector<uint8_t>> FpublicBytes{ {} };   // 1-based; index 0 unused
    std::vector<uint8_t> FpublicPresent{ 0 };               // 1 = held, 0 = never had / evicted
    std::vector<uint32_t> FpublicLastUse{ 0 };              // TU index of last use (LRU eviction)
    uint32_t Fpublic_next=1;                                // implicit op1 ordinal cursor (resynced on rejoin)
    uint32_t publicHeld=0;                                  // count of present public Lines
    uint32_t publicBudget=UINT32_MAX;                       // eviction cap (UINT32_MAX = unbounded)
    std::vector<uint32_t> pendingDrops;                     // ordinals evicted since last NEED
    std::vector<std::string> Fpaths;
    std::vector<uint8_t> FknownBlk;                         // size NBLK
    std::vector<std::vector<uint32_t>> FblkChildren;        // id-indexed (SPARSE: restart re-installs subset)
    std::vector<uint32_t> Freg_stream;
    std::vector<uint32_t> FrequiredRegionStamp, FrequiredBlockStamp;
    uint32_t requestStamp=0;

    void init(uint32_t nreg, uint32_t nblk){
        NREG=nreg; NBLK=nblk;
        FmixedRegions.assign(nreg, MixedFRegionView{});
        FknownBlk.assign(nblk, 0);
        FblkChildren.assign(nblk, {});
        FrequiredRegionStamp.assign(nreg, 0);
        FrequiredBlockStamp.assign(nblk, 0);
        Freg_stream.reserve(1u<<20);
        FmixedRegionData.reserve(64u<<20);
    }
    void install_blocks(const std::vector<uint8_t>& blockRaw);
    // Transactional: on any validation failure the whole Fill commits NOTHING (returns false,
    // store byte-for-byte unchanged).  Installs Fpaths, decodes the mixed streams into the region
    // store, materializes public-Line bytes (op1), re-establishes them under recovery (op7/op8),
    // and rejects an unequal rebind of an existing ordinal.
    bool decode_fill(const std::array<std::vector<uint8_t>,6>& recovered,
                     const std::vector<uint32_t>& missReg,
                     const std::vector<uint8_t>& fill_paths, uint32_t t);
    void reconstruct(const std::vector<uint8_t>& Frootb, std::vector<uint8_t>& recon);

    // ---- M2 restart / eviction ----
    void reset_store(uint32_t resyncPublicNext){           // worker restart: drop the whole store
        FmixedRegionData.clear(); std::vector<uint8_t>().swap(FmixedRegionData); FmixedRegionData.reserve(64u<<20);
        FmixedRegions.assign(NREG, MixedFRegionView{});
        FpublicBytes.assign(1,{}); FpublicPresent.assign(1,0); FpublicLastUse.assign(1,0); publicHeld=0; pendingDrops.clear();
        Fpublic_next=resyncPublicNext;
        FknownBlk.assign(NBLK,0); FblkChildren.assign(NBLK,{});
        Freg_stream.clear(); Fpaths.clear();
        std::fill(FrequiredRegionStamp.begin(),FrequiredRegionStamp.end(),0);
        std::fill(FrequiredBlockStamp.begin(),FrequiredBlockStamp.end(),0);
        requestStamp=0;
    }
    void evict_to_budget(uint32_t t){                      // drop LRU public Lines beyond the budget
        (void)t;
        while(publicHeld>publicBudget){
            uint32_t victim=0,best=UINT32_MAX;
            for(uint32_t o=1;o<FpublicPresent.size();++o) if(FpublicPresent[o]&&FpublicLastUse[o]<best){best=FpublicLastUse[o];victim=o;}
            if(!victim) break;
            FpublicPresent[victim]=0; FpublicBytes[victim].clear(); std::vector<uint8_t>().swap(FpublicBytes[victim]); --publicHeld;
            pendingDrops.push_back(victim);
        }
    }
};

}  // namespace capc
