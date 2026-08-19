// codec50.cpp — real byte-exact "Protocol-50" codec for the icecream line-dedup bake-off (issue #16).
//
// This is the honest measurement harness the FINAL spec requires: a real encoder (C) that turns
// interned .ii structure into a self-describing, length-framed, zstd-compressed wire byte stream, and a
// real decoder (F) that consumes ONLY those bytes, installs immutable objects, expands the root, and
// reconstructs the exact .ii bytes. Every wire byte is charged by category. FinalRatio = raw / wire,
// ONE cold chronological pass, reported per corpus at f = 0.10/0.25/0.50/0.75/1.00 with the H200
// trailing-window ratio.  Each material lane records its selected zstd level; no corpus-name branches
// or uncharged dictionaries are permitted.
//
// Milestone-1 variants (this file): V1 = stable Lines + marker Regions; D1 = preprocessor-marker/path
// factoring of "# N \"path\" flags" lines into (path-object, lineno, flags). D2 (relative-LZ line
// codec, definition_codec.h) plugs into the line-definition leg when available. The real two-process
// socketpair + throughput and the S0/S1/S3 structure planes build on this same serializer/decoder.
//
// build: g++ -O3 -march=native -std=c++17 codec50.cpp -o codec50 -lzstd -lz

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_map>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <immintrin.h>
#include <zstd.h>
#include <zlib.h>
#include "alpha_line_codec.h"
#include "p29_online_s1.h"
#include "p29_sparse_blocks.h"
#include "mo_factor_codec.h"
#if defined(WITH_BSC_GROUPS)
  #include "residual_group_codec.h"
#endif
#if defined(WITH_D2) && __has_include("definition_codec.h")
  #include "definition_codec.h"
  #define HAVE_DEFCODEC 1
#endif

using Clock = std::chrono::steady_clock;
static double secs(Clock::time_point b){ return std::chrono::duration<double>(Clock::now()-b).count(); }
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
static void* huge_zeroed(size_t bytes){ constexpr size_t H=2u<<20; bytes=(bytes+H-1)&~(H-1); void*p=mmap(nullptr,bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0); if(p==MAP_FAILED){perror("mmap");exit(2);} madvise(p,bytes,MADV_HUGEPAGE); return p; }

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
    // Faithful stand-in for the persistent C-cache key: the store assigns one nonzero monotonic
    // u64 when a Region is first installed.  It is deliberately distinct from the sampled lookup
    // hash above, which may repeat and is always followed by an exact byte comparison.
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
static Corpus load_corpus(const char*manifest,size_t max_files){
    FILE*mf=fopen(manifest,"r"); if(!mf){perror(manifest);exit(2);} std::vector<std::string> paths; char path[8192]; uint64_t total=0;
    while(fgets(path,sizeof path,mf)){ size_t n=strlen(path); while(n&&(path[n-1]=='\n'||path[n-1]=='\r'))path[--n]=0; if(!n)continue; struct stat st{}; if(stat(path,&st)!=0){perror(path);exit(2);} paths.emplace_back(path); total+=uint64_t(st.st_size); if(paths.size()==max_files)break; }
    fclose(mf); Corpus c; c.bytes.resize(size_t(total)+64); c.files.reserve(paths.size()); uint64_t off=0;
    for(auto&p:paths){ FILE*f=fopen(p.c_str(),"rb"); if(!f){perror(p.c_str());exit(2);} struct stat st{}; fstat(fileno(f),&st); size_t n=size_t(st.st_size); if(n&&fread(c.bytes.data()+off,1,n,f)!=n){fprintf(stderr,"short read\n");exit(2);} fclose(f); c.files.push_back({off,uint32_t(n)}); off+=n; }
    c.raw=off; return c;
}

#if defined(WITH_BSC_GROUPS)
struct LiteralGroupRecord {
    size_t first_tu=0,last_tu=0,wire_offset=0,wire_size=0;
    size_t decoded_offset=0,decoded_size=0;
    residual_group::Kind kind=residual_group::Kind::Zstd3;
};

struct LiteralGroupPlan {
    std::vector<uint8_t>wire,decoded;
    std::vector<uint32_t>per_tu_raw;
    std::vector<LiteralGroupRecord>groups;
    std::array<uint64_t,3>selected{};
    double encode_seconds=0,decode_seconds=0;
    size_t workers=1;
    bool evaluated_zstd10=true;
};

static std::vector<uint8_t>read_binary_file(const std::string&path){
    FILE*file=fopen(path.c_str(),"rb");if(!file){perror(path.c_str());throw std::runtime_error("open failed");}
    if(fseek(file,0,SEEK_END)!=0){fclose(file);throw std::runtime_error("seek failed: "+path);}
    long end=ftell(file);if(end<0||fseek(file,0,SEEK_SET)!=0){fclose(file);throw std::runtime_error("size failed: "+path);}
    std::vector<uint8_t>bytes(static_cast<size_t>(end));
    if(!bytes.empty()&&fread(bytes.data(),1,bytes.size(),file)!=bytes.size()){fclose(file);throw std::runtime_error("short read: "+path);}
    if(fclose(file)!=0)throw std::runtime_error("close failed: "+path);
    return bytes;
}

static LiteralGroupPlan build_literal_group_plan(const std::string&prefix,size_t tus,
                                                  size_t stride,size_t group_tus,
                                                  size_t workers,bool evaluate_zstd10,
                                                  const char*wire_path){
    if(stride<2||!group_tus||!workers)throw std::runtime_error("invalid literal group dimensions");
    const std::string raw_path=prefix+".literal.raw",length_path=prefix+".lengths.raw";
    std::vector<uint8_t>raw=read_binary_file(raw_path),length_bytes=read_binary_file(length_path);
    if(length_bytes.size()%4)throw std::runtime_error("mixed length table is not a u32 array");
    const size_t values=length_bytes.size()/4;
    if(values!=tus*stride)throw std::runtime_error("mixed length table must contain one complete row per TU");
    LiteralGroupPlan plan;plan.per_tu_raw.resize(tus);plan.workers=workers;
    plan.evaluated_zstd10=evaluate_zstd10;
    std::vector<size_t>offsets(tus+1);
    for(size_t t=0;t<tus;++t){
        const uint8_t*p=length_bytes.data()+4*(t*stride+1);
        const uint32_t length=uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);
        plan.per_tu_raw[t]=length;
        if(offsets[t]>SIZE_MAX-length)throw std::runtime_error("literal group raw size overflows");
        offsets[t+1]=offsets[t]+length;
    }
    if(offsets.back()!=raw.size())throw std::runtime_error("literal group lengths do not span the raw input");
    const size_t group_count=(tus+group_tus-1)/group_tus;
    plan.groups.resize(group_count);std::vector<std::vector<uint8_t>>frames(group_count),decoded_groups(group_count);
    for(size_t index=0;index<group_count;++index){
        const size_t first=index*group_tus;
        const size_t last=std::min(tus,first+group_tus),raw_size=offsets[last]-offsets[first];
        LiteralGroupRecord&group=plan.groups[index];group.first_tu=first;group.last_tu=last;
        group.decoded_offset=offsets[first];group.decoded_size=raw_size;
    }
    const size_t active_workers=std::min(workers,std::max(size_t(1),group_count));
    auto parallel=[&](auto task){
        std::atomic<size_t>next{0};std::vector<std::exception_ptr>errors(active_workers);std::vector<std::thread>threads;threads.reserve(active_workers);
        for(size_t worker=0;worker<active_workers;++worker)threads.emplace_back([&,worker]{try{
            residual_group::Codec codec;for(;;){size_t index=next.fetch_add(1);if(index>=group_count)break;task(codec,index);}
          }catch(...){errors[worker]=std::current_exception();}});
        for(auto&thread:threads)thread.join();
        for(const auto&error:errors)if(error)std::rethrow_exception(error);
    };
    auto started=Clock::now();
    parallel([&](residual_group::Codec&codec,size_t index){
        LiteralGroupRecord&group=plan.groups[index];if(!group.decoded_size)return;
        frames[index]=codec.encode(raw.data()+group.decoded_offset,group.decoded_size,
                                   &group.kind,evaluate_zstd10);
    });
    plan.encode_seconds=std::chrono::duration<double>(Clock::now()-started).count();
    started=Clock::now();
    parallel([&](residual_group::Codec&codec,size_t index){
        LiteralGroupRecord&group=plan.groups[index];if(!group.decoded_size)return;
        residual_group::DecodedFrame decoded=codec.decode(frames[index].data(),frames[index].size());
        if(decoded.wire_bytes!=frames[index].size()||decoded.kind!=group.kind||decoded.raw.size()!=group.decoded_size||
           memcmp(decoded.raw.data(),raw.data()+group.decoded_offset,group.decoded_size))throw std::runtime_error("literal group frame roundtrip differs");
        decoded_groups[index]=std::move(decoded.raw);
    });
    plan.decode_seconds=std::chrono::duration<double>(Clock::now()-started).count();
    for(size_t index=0;index<group_count;++index){
        LiteralGroupRecord&group=plan.groups[index];group.wire_offset=plan.wire.size();group.wire_size=frames[index].size();
        plan.wire.insert(plan.wire.end(),frames[index].begin(),frames[index].end());
        if(plan.decoded.size()!=group.decoded_offset)throw std::runtime_error("literal group decoded offset differs");
        plan.decoded.insert(plan.decoded.end(),decoded_groups[index].begin(),decoded_groups[index].end());
        if(group.wire_size)++plan.selected[size_t(group.kind)];
    }
    if(plan.decoded.size()!=raw.size())throw std::runtime_error("literal group decoded extent differs");
    if(wire_path){FILE*file=fopen(wire_path,"wb");if(!file){perror(wire_path);throw std::runtime_error("wire output open failed");}
        if(!plan.wire.empty()&&fwrite(plan.wire.data(),1,plan.wire.size(),file)!=plan.wire.size()){fclose(file);throw std::runtime_error("wire output short write");}
        if(fclose(file)!=0)throw std::runtime_error("wire output close failed");}
    return plan;
}
#endif
// ---- physical two-direction wire sinks ---------------------------------------------
// The "wire by category" totals below are an ACCOUNTING SUM over categories; they are not
// a byte stream, so they cannot show that a build closed at a physical offset or that a
// completed prefix never changes.  These sinks are the real thing: one ordered append-only
// file per direction, every message typed and length-delimited, every TU explicitly closed.
// C->F is the primary score.  F->C is a separate sink and is NEVER added into it.
enum : uint8_t {
    WT_ROOT=1, WT_BLOCKDEF=2, WT_NEED=3, WT_ASSOC=4, WT_PATHDEF=5, WT_LINEDEF=6,
    WT_REGIONDEF=7, WT_FILL0=8, WT_SELECTOR=20, WT_BLOB=21, WT_BLOBPATCH=22,
    WT_LITGROUP=23, WT_FBREQ=30, WT_FBREPLY=31, WT_TU_END=0xFE, WT_BUILD_CLOSE=0xFF };
// In REPLAY mode the identical call sites read the stream back instead of writing it: each
// message must be present, in order, with the declared type and a byte-identical payload, and
// the stream must be fully consumed at the end.  That turns "I believe I emitted everything"
// into a checkable property -- a message the encoder needs but never wrote desynchronises the
// type sequence and fails the run.
struct WireSink {
    FILE* f=nullptr; bool replay=false; uint64_t off=0, frames=0, payload=0;
    std::vector<uint8_t> back;
    void open(const char*p,bool forReplay){ replay=forReplay; f=fopen(p,forReplay?"rb":"wb"); if(!f){perror(p);exit(2);} }
    void emit(uint8_t type,const uint8_t*data,size_t n){
        if(!f) return;
        if(n>0xffffffffull){fprintf(stderr,"sink frame too large\n");exit(2);}
        const uint8_t h[5]={type,uint8_t(n),uint8_t(n>>8),uint8_t(n>>16),uint8_t(n>>24)};
        if(!replay){
            if(fwrite(h,1,sizeof h,f)!=sizeof h){fprintf(stderr,"sink short write\n");exit(2);}
            if(n&&fwrite(data,1,n,f)!=n){fprintf(stderr,"sink short write\n");exit(2);}
        }else{
            uint8_t g[5];
            if(fread(g,1,sizeof g,f)!=sizeof g){fprintf(stderr,"replay: stream ended before a type-%u frame at offset %llu\n",type,(unsigned long long)off);exit(2);}
            if(memcmp(g,h,sizeof h)!=0){fprintf(stderr,"replay: expected type=%u len=%zu at offset %llu, stream has type=%u len=%u\n",type,n,(unsigned long long)off,g[0],unsigned(g[1])|unsigned(g[2])<<8|unsigned(g[3])<<16|unsigned(g[4])<<24);exit(2);}
            back.resize(n);
            if(n&&fread(back.data(),1,n,f)!=n){fprintf(stderr,"replay: truncated type-%u payload at offset %llu\n",type,(unsigned long long)off);exit(2);}
            if(n&&memcmp(back.data(),data,n)!=0){fprintf(stderr,"replay: type-%u payload differs at offset %llu\n",type,(unsigned long long)off);exit(2);}
        }
        off+=sizeof h+n; ++frames;
        if(type!=WT_TU_END&&type!=WT_BUILD_CLOSE)payload+=n;
    }
    void emit(uint8_t type,const std::vector<uint8_t>&v){ emit(type,v.data(),v.size()); }
    void close(const char*name){
        if(!f) return;
        if(replay){ uint8_t x; if(fread(&x,1,1,f)==1){fprintf(stderr,"replay: %s has trailing bytes past the last frame the run consumed\n",name);exit(2);} }
        if(fclose(f)!=0){perror("sink close");exit(2);} f=nullptr;
    }
};
static inline void put_varint(std::vector<uint8_t>&o,uint64_t v){ while(v>=0x80){o.push_back(uint8_t(v)|0x80);v>>=7;} o.push_back(uint8_t(v)); }
static inline uint64_t get_varint(const uint8_t*&p){ uint64_t v=0; int s=0; for(;;){ uint8_t b=*p++; v|=uint64_t(b&0x7f)<<s; if(!(b&0x80))break; s+=7; } return v; }
// The legacy Root namespace places Blocks after the final Region count (NREG+k).  That is
// compact, but the same chronological prefix receives different token values when later TUs add
// Regions.  Stable-Root mode instead uses an explicit low-bit kind tag so every Root token is a
// function of state available at that TU: Region r -> 2*r, Block k -> 2*k+1.
// T_current step 2: the typed tag IS the in-memory Root token, not a late translation of a
// flat index.  A flat index needs the final Region count to say where Blocks begin, so
// building it at TU t means knowing something about TUs that have not been dispatched.  The
// tag is self-describing: low bit = kind, rest = the id within that kind.
static inline bool     tag_is_block(uint32_t tag){ return tag&1u; }
static inline uint32_t tag_id      (uint32_t tag){ return tag>>1; }
// CHECKED creation.  The id arrives as a 64-bit value and is validated BEFORE it is narrowed
// or shifted, so the bound is a property of the operation rather than of whatever the caller
// happened to compute.  A source-level "is the guard present" check cannot tell a live guard
// from a dead one, or from one placed after the shift; these return false instead.
static constexpr uint64_t kTagIdLimit = uint64_t(1)<<31;   // one bit of the u32 is the kind
static inline bool make_region_tag(uint64_t id,uint32_t&tag){
    if(id>=kTagIdLimit){ return false; }
    tag=uint32_t(id)<<1;
    return true;
}
static inline bool make_block_tag(uint64_t id,uint32_t&tag){
    if(id>=kTagIdLimit){ return false; }
    tag=(uint32_t(id)<<1)|1u;
    return true;
}
// Unchecked convenience for ids already validated at admission; both hard-fail rather than
// silently truncating, so neither can become the quiet path.
static inline uint32_t region_tag(uint32_t r){ uint32_t t; if(!make_region_tag(r,t)){fprintf(stderr,"Region id %u exceeds the typed Root tag space\n",r);exit(2);} return t; }
static inline uint32_t block_tag (uint32_t k){ uint32_t t; if(!make_block_tag(k,t)) {fprintf(stderr,"Block id %u exceeds the typed Root tag space\n",k);exit(2);} return t; }
// The legacy Root namespace still wants NREG+k, so that -- and only that -- converts late.
static inline uint32_t legacy_flat_token(uint32_t tag,uint32_t regionCount){
    return tag_is_block(tag) ? regionCount+tag_id(tag) : tag_id(tag);
}
// Wire -> in-memory tag.  In stable mode the wire value already IS the tag, so regionCount is
// only a BOUND and the per-TU count is the right one.  In the legacy flat namespace regionCount
// is the PIVOT that says where Blocks begin, and it has to be the same total the encoder used --
// which is the final one.  That asymmetry is not an accident of this code: a flat namespace
// cannot be resolved without a whole-corpus quantity, and that is precisely why the stable tag
// exists.  Callers pass the per-TU count in stable mode and the final count in legacy mode.
static inline bool wire_to_tag(uint64_t wire,bool stable,uint32_t regionCount,
                               size_t blockBound,uint32_t&tag){
    if(wire>UINT32_MAX) return false;
    const uint32_t value=uint32_t(wire);
    if(stable){
        if(tag_is_block(value) ? tag_id(value)>=blockBound : tag_id(value)>=regionCount) return false;
        tag=value; return true;
    }
    if(value<regionCount){ tag=region_tag(value); return true; }
    const uint32_t k=value-regionCount;
    if(k>=blockBound) return false;
    tag=block_tag(k); return true;
}
static inline void put_u64le(std::vector<uint8_t>&o,uint64_t v){for(unsigned i=0;i<8;++i)o.push_back(uint8_t(v>>(8*i)));}
static inline uint64_t get_u64le(const uint8_t*&p,const uint8_t*end){
    if(end-p<8){fprintf(stderr,"truncated u64\n");exit(2);}uint64_t v=0;for(unsigned i=0;i<8;++i)v|=uint64_t(*p++)<<(8*i);return v;
}
static inline void put_zigzag(std::vector<uint8_t>&o,int64_t v){ put_varint(o,(uint64_t(v)<<1)^uint64_t(v>>63)); }
static inline int64_t get_zigzag(const uint8_t*&p){ uint64_t u=get_varint(p); return int64_t(u>>1)^-int64_t(u&1); }
static inline size_t varint_size(uint64_t v){ size_t n=1; while(v>=0x80){++n;v>>=7;} return n; }
static size_t zstd_size(ZSTD_CCtx*c,const uint8_t*data,size_t n,int level,std::vector<uint8_t>&dst){ ZSTD_CCtx_reset(c,ZSTD_reset_session_and_parameters); ZSTD_CCtx_setParameter(c,ZSTD_c_compressionLevel,level);
    size_t bound=ZSTD_compressBound(n); if(dst.size()<bound)dst.resize(bound); size_t r=ZSTD_compress2(c,dst.data(),dst.size(),data?data:(const uint8_t*)"",n); if(ZSTD_isError(r)){fprintf(stderr,"zstd %s\n",ZSTD_getErrorName(r));exit(2);} return r; }
static size_t zstd_frame_encode(ZSTD_CCtx*c,const std::vector<uint8_t>&raw,int level,std::vector<uint8_t>&encoded){
    size_t size=zstd_size(c,raw.data(),raw.size(),level,encoded);encoded.resize(size);return size;
}
static size_t zstd_message_roundtrip(ZSTD_CCtx*c,ZSTD_DCtx*d,const std::vector<uint8_t>&raw,int level,
                                     std::vector<uint8_t>&encoded,std::vector<uint8_t>&decoded){
    size_t encodedSize=zstd_size(c,raw.data(),raw.size(),level,encoded);decoded.resize(raw.size());uint8_t empty=0;
    void*out=decoded.empty()?static_cast<void*>(&empty):static_cast<void*>(decoded.data());
    size_t got=ZSTD_decompressDCtx(d,out,decoded.size(),encoded.data(),encodedSize);
    if(ZSTD_isError(got)||got!=raw.size()||decoded!=raw){fprintf(stderr,"message roundtrip differs: %s\n",ZSTD_isError(got)?ZSTD_getErrorName(got):"size/content");exit(2);}
    return encodedSize;
}

static size_t zstd_ldm_frame_encode(ZSTD_CCtx*c,const std::vector<uint8_t>&raw,int level,
                                    std::vector<uint8_t>&encoded,int workers=0,
                                    int jobSize=0,int overlapLog=0){
    auto require_zstd=[](size_t result,const char*operation){
        if(ZSTD_isError(result)){fprintf(stderr,"blob zstd %s: %s\n",operation,ZSTD_getErrorName(result));exit(2);}
    };
    require_zstd(ZSTD_CCtx_reset(c,ZSTD_reset_session_and_parameters),"reset");
    require_zstd(ZSTD_CCtx_setParameter(c,ZSTD_c_compressionLevel,level),"compression level");
    require_zstd(ZSTD_CCtx_setParameter(c,ZSTD_c_enableLongDistanceMatching,1),"long-distance matching");
    require_zstd(ZSTD_CCtx_setParameter(c,ZSTD_c_windowLog,27),"window log");
    if(workers){
        require_zstd(ZSTD_CCtx_setParameter(c,ZSTD_c_nbWorkers,workers),"worker count");
        require_zstd(ZSTD_CCtx_setParameter(c,ZSTD_c_jobSize,jobSize),"job size");
        require_zstd(ZSTD_CCtx_setParameter(c,ZSTD_c_overlapLog,overlapLog),"overlap log");
    }
    require_zstd(ZSTD_CCtx_setParameter(c,ZSTD_c_checksumFlag,0),"checksum flag");
    require_zstd(ZSTD_CCtx_setParameter(c,ZSTD_c_contentSizeFlag,0),"content-size flag");
    size_t bound=ZSTD_compressBound(raw.size());encoded.resize(bound);
    uint8_t empty=0;const void*source=raw.empty()?static_cast<const void*>(&empty):static_cast<const void*>(raw.data());
    size_t size=ZSTD_compress2(c,encoded.data(),encoded.size(),source,raw.size());
    if(ZSTD_isError(size)){fprintf(stderr,"blob zstd encode %s\n",ZSTD_getErrorName(size));exit(2);}
    encoded.resize(size);return size;
}
static std::vector<uint8_t> zstd_frame_decode_exact(ZSTD_DCtx*d,const std::vector<uint8_t>&encoded,
                                                     size_t expected){
    std::vector<uint8_t> raw(expected);uint8_t empty=0;
    void*out=raw.empty()?static_cast<void*>(&empty):static_cast<void*>(raw.data());
    size_t size=ZSTD_decompressDCtx(d,out,raw.size(),encoded.data(),encoded.size());
    if(ZSTD_isError(size)||size!=expected){fprintf(stderr,"blob zstd decode %s\n",ZSTD_isError(size)?ZSTD_getErrorName(size):"size differs");exit(2);}
    return raw;
}
static std::vector<uint8_t> zstd_frame_decode_sized(ZSTD_DCtx*d,const std::vector<uint8_t>&encoded){
    if(encoded.empty()){fprintf(stderr,"missing zstd frame\n");exit(2);}
    unsigned long long expected=ZSTD_getFrameContentSize(encoded.data(),encoded.size());
    if(expected==ZSTD_CONTENTSIZE_ERROR||expected==ZSTD_CONTENTSIZE_UNKNOWN||expected>SIZE_MAX){fprintf(stderr,"bad zstd frame content size\n");exit(2);}
    return zstd_frame_decode_exact(d,encoded,size_t(expected));
}

static std::vector<uint8_t> zstd_stream_encode(ZSTD_CCtx*c,const std::vector<uint8_t>&raw,
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
static std::vector<uint8_t> zstd_stream_decode(ZSTD_DCtx*d,const std::vector<uint8_t>&encoded,
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

// P22 ROOT_SLICE capability. Each Root may name an exact contiguous range from a completed
// earlier Root. The sparse C index proposes at most four recent exact candidates; equality is
// checked over Region IDs before a copy is emitted. F expands the program before committing it.
struct RootLocation { uint32_t root=0, start=0; };
struct RootBucket {
    std::array<RootLocation,4> recent{};
    uint32_t count=0;
    void add(RootLocation value){
        if(count<recent.size()) recent[count++]=value;
        else { for(size_t i=1;i<recent.size();++i) recent[i-1]=recent[i]; recent.back()=value; }
    }
};
struct RootSliceBuild {
    std::vector<std::vector<uint8_t>> programs;
    uint64_t copies=0, copied_regions=0, indexed_windows=0, index_entries=0;
};
static inline uint64_t root_seed_hash(const uint32_t*p,uint32_t n){
    uint64_t h=0x9e3779b97f4a7c15ULL^n;
    for(uint32_t i=0;i<n;++i) h=mix64(h^mix64(uint64_t(p[i])+0x9e3779b97f4a7c15ULL+i));
    return h;
}
static RootSliceBuild build_root_slices(const std::vector<uint32_t>&regions,
                                        const std::vector<size_t>&offsets,
                                        uint32_t seed=4,uint32_t stride=8){
    RootSliceBuild out; out.programs.reserve(offsets.size()-1);
    std::unordered_map<uint64_t,RootBucket> index;
    index.reserve(regions.size()/stride+1);
    for(uint32_t root=0;root+1<offsets.size();++root){
        const size_t begin=offsets[root], end=offsets[root+1];
        std::vector<uint8_t> program; put_varint(program,end-begin);
        size_t position=begin;
        while(position<end){
            bool have=false; RootLocation best{}; size_t best_len=0, best_saving=0;
            if(position+seed<=end){
                auto it=index.find(root_seed_hash(&regions[position],seed));
                if(it!=index.end()){
                    const RootBucket& bucket=it->second;
                    for(uint32_t ci=bucket.count;ci-->0;){
                        RootLocation candidate=bucket.recent[ci];
                        size_t source_begin=offsets[candidate.root]+candidate.start;
                        size_t source_end=offsets[candidate.root+1];
                        if(source_begin+seed>source_end ||
                           memcmp(&regions[source_begin],&regions[position],seed*sizeof(uint32_t))) continue;
                        size_t length=seed, maximum=std::min(source_end-source_begin,end-position);
                        while(length<maximum && regions[source_begin+length]==regions[position+length]) ++length;
                        size_t literal=0;
                        for(size_t j=0;j<length;++j) literal+=1+varint_size(regions[position+j]);
                        size_t copy=1+varint_size(candidate.root)+varint_size(candidate.start)+varint_size(length);
                        size_t saving=literal>copy?literal-copy:0;
                        if(saving>best_saving || (saving==best_saving && saving && length>best_len)){
                            have=true; best=candidate; best_len=length; best_saving=saving;
                        }
                    }
                }
            }
            if(have){
                program.push_back(1); put_varint(program,best.root); put_varint(program,best.start); put_varint(program,best_len);
                position+=best_len; ++out.copies; out.copied_regions+=best_len;
            } else {
                program.push_back(0); put_varint(program,regions[position]); ++position;
            }
        }
        out.programs.push_back(std::move(program));
        if(end-begin>=seed){
            for(size_t p=begin;p+seed<=end;p+=stride){
                auto& bucket=index[root_seed_hash(&regions[p],seed)];
                if(bucket.count<bucket.recent.size()) ++out.index_entries;
                bucket.add({root,uint32_t(p-begin)}); ++out.indexed_windows;
            }
        }
    }
    return out;
}

// ---- D1: preprocessor marker factoring. Parse "# <n> \"<path>\"<flags>\n" -> (path,n,flags), and
// reconstruct EXACT bytes; fall back to literal if reconstruction != original. ----
struct Marker{ std::string path; uint64_t lineno; std::vector<uint8_t> flags; };
static bool parse_marker(const char* s, uint32_t len, Marker& m){
    if(len<4 || s[0]!='#' || s[1]!=' ') return false;
    const char* e=s+len; const char* p=s+2;
    if(p>=e || *p<'0'||*p>'9') return false;
    uint64_t n=0; while(p<e && *p>='0'&&*p<='9'){ n=n*10+(*p-'0'); ++p; } m.lineno=n;
    if(p+2>e || p[0]!=' '||p[1]!='"') return false;
    p+=2; const char* q=p; while(q<e && *q!='"') ++q; if(q>=e) return false; m.path.assign(p,q); p=q+1;
    m.flags.clear(); while(p<e && *p==' '){ ++p; if(p>=e||*p<'0'||*p>'9') return false; uint8_t fl=0; while(p<e&&*p>='0'&&*p<='9'){ fl=fl*10+(*p-'0'); ++p; } m.flags.push_back(fl); }
    if(p>=e || *p!='\n' || p+1!=e) return false;   // must end exactly with newline
    return true;
}
static void emit_marker(const Marker& m, std::vector<uint8_t>& out){ char buf[32]; int l=snprintf(buf,sizeof buf,"# %llu \"",(unsigned long long)m.lineno); out.insert(out.end(),buf,buf+l); out.insert(out.end(),m.path.begin(),m.path.end()); out.push_back('"'); for(uint8_t f:m.flags){ out.push_back(' '); l=snprintf(buf,sizeof buf,"%u",f); out.insert(out.end(),buf,buf+l);} out.push_back('\n'); }

enum ByteNumberFormat : uint8_t { DECIMAL=0, HEX_LL=1, HEX_LU=2, HEX_UL=3, HEX_UU=4 };
struct GeneratedByteArray {
    std::vector<uint8_t> values;
    std::string prefix, separator, suffix;
    uint8_t format=DECIMAL;
};
struct CompressedBlob {
    uint32_t first_entry=0;
    uint32_t entry_count=0;
    uint32_t deflated_size=0;
    uint32_t inflated_offset=0;
    uint32_t inflated_size=0;
    uint64_t digest_lo=0;
    uint64_t digest_hi=0;
};
struct BlobPatch {
    uint32_t prefix=0;
    uint32_t suffix=0;
    uint32_t data_offset=0;
    uint32_t data_size=0;
    uint8_t kind=0; // 0 canonical output, 1 prefix/middle/suffix correction, 2 full replacement
};
struct ArrayWireRecord {
    uint32_t style=0;
    uint32_t count=0;
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
struct PackedLines {
    std::vector<uint8_t> bytes;
    std::vector<size_t> offsets{0};

    size_t size() const { return offsets.size()-1; }
    size_t line_size(size_t index) const { return offsets[index+1]-offsets[index]; }
    const uint8_t* line_data(size_t index) const { return bytes.data()+offsets[index]; }
    void append(const uint8_t* data,size_t size){
        bytes.insert(bytes.end(),data,data+size);
        offsets.push_back(bytes.size());
    }
};
struct MixedCLineState {
    uint32_t source_region=UINT32_MAX;
    uint32_t source_offset=0;
    uint32_t public_id=0;
};
struct MixedFLineView {
    uint32_t source_region=0;
    uint32_t source_offset=0;
    uint32_t length=0;
};
struct MixedFRegionView {
    size_t offset=0;
    uint32_t length=0;
    bool known=false;
};
static bool system_source_path(const std::string&path){
    return path.rfind("/usr/include/",0)==0||path.rfind("/usr/lib/gcc/",0)==0||path.rfind("/usr/local/include/",0)==0;
}
struct SourceText {
    bool attempted=false,available=false;
    std::vector<uint8_t> bytes;
    std::vector<uint32_t> offsets;
};
class SourceTextStore {
public:
    explicit SourceTextStore(bool allowProject=false):allowProject_(allowProject){}
    const SourceText& get(const std::string&path){
        SourceText&source=files_[path];if(source.attempted)return source;source.attempted=true;source.offsets.push_back(0);
        struct stat st{};if((!allowProject_&&!system_source_path(path))||stat(path.c_str(),&st)||st.st_size<0||uint64_t(st.st_size)>UINT32_MAX)return source;
        FILE*file=fopen(path.c_str(),"rb");if(!file)return source;source.bytes.resize(size_t(st.st_size));
        if(!source.bytes.empty()&&fread(source.bytes.data(),1,source.bytes.size(),file)!=source.bytes.size()){fclose(file);source.bytes.clear();return source;}fclose(file);
        finish(source);return source;
    }
    bool install(const std::string&path,const uint8_t*data,size_t size){
        if(size>UINT32_MAX)return false;
        SourceText&source=files_[path];
        if(source.available)return source.bytes.size()==size&&(!size||!memcmp(source.bytes.data(),data,size));
        source={};source.attempted=true;source.offsets.push_back(0);
        if(size)source.bytes.assign(data,data+size);
        finish(source);return true;
    }
private:
    static void finish(SourceText&source){
        for(uint32_t i=0;i<source.bytes.size();++i)if(source.bytes[i]=='\n')source.offsets.push_back(i+1);
        if(source.offsets.back()!=source.bytes.size())source.offsets.push_back(source.bytes.size());
        source.available=true;
    }
    bool allowProject_=false;
    std::unordered_map<std::string,SourceText> files_;
};
struct SourceAdmission {
    uint64_t observed_benefit=0;
    uint64_t package_cost=0;
    size_t last_observed_tu=SIZE_MAX;
    bool cost_known=false;
    bool admitted=false;
};
static uint32_t common_prefix(const uint8_t*a,uint32_t an,const uint8_t*b,uint32_t bn){
    uint32_t n=std::min(an,bn),i=0;while(i<n&&a[i]==b[i])++i;return i;
}
static uint32_t common_suffix(const uint8_t*a,uint32_t an,const uint8_t*b,uint32_t bn,uint32_t prefix){
    uint32_t n=std::min(an-std::min(an,prefix),bn-std::min(bn,prefix)),i=0;while(i<n&&a[an-1-i]==b[bn-1-i])++i;return i;
}
static bool packed_line_less(const PackedLines&a,size_t ai,const PackedLines&b,size_t bi){
    size_t an=a.line_size(ai),bn=b.line_size(bi),common=std::min(an,bn);
    int order=memcmp(a.line_data(ai),b.line_data(bi),common);
    return order<0 || (order==0 && an<bn);
}
static bool equal_tail(const char*p,const char*end,const char*value,size_t n){ return size_t(end-p)==n&&!memcmp(p,value,n); }
static int hex_value(uint8_t c){ if(c>='0'&&c<='9')return c-'0'; if(c>='a'&&c<='f')return c-'a'+10; if(c>='A'&&c<='F')return c-'A'+10; return -1; }
static bool parse_byte_array(const char*data,uint32_t length,GeneratedByteArray&out,size_t minimumValues=4){
    const char*begin=data,*end=data+length; if(length<2||end[-1]!='\n')return false;
    const char*p=begin; while(p<end&&(*p==' '||*p=='\t'))++p; const char*token=p;
    if(p<end&&*p==','){ ++p;while(p<end&&(*p==' '||*p=='\t'))++p;token=p; }
    else if(p>=end || !( (*p>='0'&&*p<='9') || (p+2<=end&&p[0]=='0'&&(p[1]=='x'||p[1]=='X')) )){
        const char*brace=(const char*)memchr(p,'{',size_t(end-p)); if(!brace)return false;
        std::string declaration(p,brace);
        if(declaration.find("uint8_t")==std::string::npos&&declaration.find("unsigned char")==std::string::npos)return false;
        p=brace+1;while(p<end&&(*p==' '||*p=='\t'))++p;token=p;
    }
    out={};out.prefix.assign(begin,token); bool haveSeparator=false,haveFormat=false,prefixUpper=false,havePrefix=false,sawLower=false,sawUpper=false; int numberFormat=-2;
    while(p<end){
        int value=0,currentFormat;
        if(p+2<=end&&p[0]=='0'&&(p[1]=='x'||p[1]=='X')){
            bool pu=p[1]=='X'; if(p+4>end)return false; int a=hex_value(uint8_t(p[2])),b=hex_value(uint8_t(p[3])); if(a<0||b<0)return false;
            if(!havePrefix){prefixUpper=pu;havePrefix=true;}else if(prefixUpper!=pu)return false;
            for(int i=2;i<4;++i){sawLower|=p[i]>='a'&&p[i]<='f';sawUpper|=p[i]>='A'&&p[i]<='F';}
            if(sawLower&&sawUpper)return false;
            value=(a<<4)|b;currentFormat=-1;p+=4;
        } else {
            const char*d=p;while(p<end&&*p>='0'&&*p<='9')++p;if(d==p)return false;
            if(p-d>1&&*d=='0')return false;
            for(const char*q=d;q<p;++q){value=value*10+(*q-'0');if(value>255)return false;}
            currentFormat=DECIMAL;
        }
        if(!haveFormat){numberFormat=currentFormat;haveFormat=true;}else if(numberFormat!=currentFormat)return false;
        out.values.push_back(uint8_t(value));
        if(equal_tail(p,end,"\n",1)||equal_tail(p,end,"}\n",2)||equal_tail(p,end,"};\n",3)){out.suffix.assign(p,end);break;}
        if(p>=end||*p!=',')return false;
        const char*separatorBegin=p++;
        if(equal_tail(p,end,"\n",1)||equal_tail(p,end,"}\n",2)||equal_tail(p,end,"};\n",3)){out.suffix.assign(separatorBegin,end);break;}
        while(p<end&&(*p==' '||*p=='\t'))++p;
        std::string current(separatorBegin,p);
        if(!haveSeparator){out.separator=current;haveSeparator=true;}else if(out.separator!=current)return false;
    }
    if(out.suffix.empty()||out.values.size()<minimumValues||!haveFormat)return false;
    if(!haveSeparator)out.separator=",";
    if(numberFormat==DECIMAL)out.format=DECIMAL;
    else if(prefixUpper)out.format=sawLower?HEX_UL:HEX_UU;
    else out.format=sawLower?HEX_LL:HEX_LU;
    return true;
}

static std::pair<uint64_t,uint64_t> blob_digest(const uint8_t*data,size_t size){
    uint64_t a=1469598103934665603ULL,b=0x9e3779b97f4a7c15ULL^size;
    for(size_t i=0;i<size;++i){a=(a^data[i])*1099511628211ULL;b=mix64(b^(uint64_t(data[i])+i*0x100000001b3ULL));}
    return {mix64(a^size),mix64(b^a)};
}
static bool inflate_blob_at(const std::vector<GeneratedByteArray>&entries,size_t first,
                            CompressedBlob&blob,std::vector<uint8_t>&inflated){
    if(first>=entries.size()||entries[first].values.size()<2)return false;
    const auto&head=entries[first].values;uint16_t header=uint16_t(head[0])<<8|head[1];
    if((head[0]&15)!=8||header%31)return false;
    z_stream stream{};if(inflateInit(&stream)!=Z_OK)return false;
    size_t outputBegin=inflated.size(),deflated=0;std::array<uint8_t,256*1024> buffer{};
    bool success=false;
    for(size_t i=first;i<entries.size()&&i-first<UINT32_MAX;++i){
        const auto&values=entries[i].values;
        if(values.size()>UINT_MAX)break;
        stream.next_in=const_cast<Bytef*>(reinterpret_cast<const Bytef*>(values.data()));stream.avail_in=uInt(values.size());
        deflated+=values.size();int result=Z_OK;
        do {
            stream.next_out=buffer.data();stream.avail_out=uInt(buffer.size());
            result=inflate(&stream,Z_NO_FLUSH);size_t made=buffer.size()-stream.avail_out;
            inflated.insert(inflated.end(),buffer.data(),buffer.data()+made);
            if(result==Z_STREAM_END){
                if(stream.avail_in==0&&deflated<=UINT32_MAX&&inflated.size()-outputBegin<=UINT32_MAX){
                    blob.first_entry=uint32_t(first);blob.entry_count=uint32_t(i-first+1);blob.deflated_size=uint32_t(deflated);
                    blob.inflated_offset=uint32_t(outputBegin);blob.inflated_size=uint32_t(inflated.size()-outputBegin);
                    std::vector<uint8_t> original;original.reserve(deflated);
                    for(size_t j=first;j<=i;++j)original.insert(original.end(),entries[j].values.begin(),entries[j].values.end());
                    auto digest=blob_digest(original.data(),original.size());blob.digest_lo=digest.first;blob.digest_hi=digest.second;success=true;
                }
                break;
            }
            if(result!=Z_OK)break;
        } while(stream.avail_in||stream.avail_out==0);
        if(success||result!=Z_OK)break;
    }
    inflateEnd(&stream);
    if(!success)inflated.resize(outputBegin);
    return success;
}
static std::vector<CompressedBlob> find_compressed_blobs(const std::vector<GeneratedByteArray>&entries,
                                                          std::vector<uint8_t>&inflated){
    std::vector<CompressedBlob> blobs;
    for(size_t i=0;i<entries.size();){CompressedBlob blob;
        if(inflate_blob_at(entries,i,blob,inflated)){blobs.push_back(blob);i+=blob.entry_count;}else ++i;}
    return blobs;
}
static bool deflate_blob(const uint8_t*data,size_t size,int level,std::vector<uint8_t>&encoded){
    if(size>ULONG_MAX)return false;
    uLong source=uLong(size);encoded.resize(compressBound(source));uLongf output=uLongf(encoded.size());
    int result=compress2(encoded.data(),&output,data,source,level);if(result!=Z_OK)return false;encoded.resize(size_t(output));return true;
}
static std::vector<uint32_t> generate_canonical_blobs(const std::vector<CompressedBlob>&blobs,const std::vector<uint8_t>&inflated,
                                                       uint32_t requestedThreads,int canonicalLevel,
                                                       std::vector<std::vector<uint8_t>>&encoded){
    encoded.resize(blobs.size());if(blobs.empty())return {};
    uint64_t total=0;for(const auto&blob:blobs)total+=blob.inflated_size;
    uint32_t workers=total>=(1u<<20)?std::min<uint32_t>(requestedThreads,uint32_t(blobs.size())):1;
    std::atomic<size_t>next{0};std::vector<uint8_t>generated(blobs.size());std::vector<std::thread>threads;threads.reserve(workers);
    for(uint32_t worker=0;worker<workers;++worker)threads.emplace_back([&]{for(;;){size_t i=next.fetch_add(1,std::memory_order_relaxed);if(i>=blobs.size())break;
        const auto&blob=blobs[i];if(uint64_t(blob.inflated_offset)+blob.inflated_size>inflated.size()||!deflate_blob(inflated.data()+blob.inflated_offset,blob.inflated_size,canonicalLevel,encoded[i]))continue;
        generated[i]=1;
    }});
    for(auto&thread:threads)thread.join();
    std::vector<uint32_t>failed;for(size_t i=0;i<blobs.size();++i)if(!generated[i])failed.push_back(uint32_t(i));return failed;
}
static BlobPatch make_blob_patch(const std::vector<uint8_t>&original,const std::vector<uint8_t>*canonical,
                                 std::vector<uint8_t>&patchData){
    BlobPatch patch;if(canonical&&*canonical==original)return patch;
    if(!canonical){patch.kind=2;patch.data_offset=uint32_t(patchData.size());patch.data_size=uint32_t(original.size());patchData.insert(patchData.end(),original.begin(),original.end());return patch;}
    size_t prefix=0,limit=std::min(original.size(),canonical->size());while(prefix<limit&&original[prefix]==(*canonical)[prefix])++prefix;
    size_t suffix=0;while(suffix<limit-prefix&&original[original.size()-1-suffix]==(*canonical)[canonical->size()-1-suffix])++suffix;
    patch.kind=1;patch.prefix=uint32_t(prefix);patch.suffix=uint32_t(suffix);patch.data_offset=uint32_t(patchData.size());patch.data_size=uint32_t(original.size()-prefix-suffix);
    patchData.insert(patchData.end(),original.begin()+prefix,original.end()-suffix);return patch;
}
static bool apply_blob_patch(const BlobPatch&patch,const std::vector<uint8_t>&canonical,
                             const std::vector<uint8_t>&patchData,std::vector<uint8_t>&output){
    if(uint64_t(patch.data_offset)+patch.data_size>patchData.size())return false;
    if(patch.kind==0){output=canonical;return true;}
    if(patch.kind==2){output.assign(patchData.begin()+patch.data_offset,patchData.begin()+patch.data_offset+patch.data_size);return true;}
    if(patch.kind!=1||uint64_t(patch.prefix)+patch.suffix>canonical.size())return false;
    output.assign(canonical.begin(),canonical.begin()+patch.prefix);
    output.insert(output.end(),patchData.begin()+patch.data_offset,patchData.begin()+patch.data_offset+patch.data_size);
    output.insert(output.end(),canonical.end()-patch.suffix,canonical.end());return true;
}
static void append_rendered_byte_array(const ByteArrayStyle&style,const uint8_t*values,
                                       size_t count,std::vector<uint8_t>&out){
    out.insert(out.end(),style.prefix.begin(),style.prefix.end());
    static const char*lo="0123456789abcdef",*up="0123456789ABCDEF";
    for(size_t i=0;i<count;++i){ if(i)out.insert(out.end(),style.separator.begin(),style.separator.end()); uint8_t v=values[i];
        if(style.format==DECIMAL){
            if(v>=100){out.push_back(uint8_t('0'+v/100));v%=100;out.push_back(uint8_t('0'+v/10));out.push_back(uint8_t('0'+v%10));}
            else if(v>=10){out.push_back(uint8_t('0'+v/10));out.push_back(uint8_t('0'+v%10));}
            else out.push_back(uint8_t('0'+v));
        }
        else { bool prefixUpper=style.format>=HEX_UL, lower=style.format==HEX_LL||style.format==HEX_UL;const char*digits=lower?lo:up;out.push_back('0');out.push_back(prefixUpper?'X':'x');out.push_back(digits[v>>4]);out.push_back(digits[v&15]); }
    }
    out.insert(out.end(),style.suffix.begin(),style.suffix.end());
}

// ---- inline relative-LZ definition codec (D2): LZ77 over a growing byte store of ALL prior line
// bytes; a new line is COPY(dist,len)/LITERAL segments against that store. Deterministic + byte-exact
// (encoder and decoder feed the identical line sequence, so their stores stay identical). This is my
// own version to unblock the D2 measurement; the helper's definition_codec is a drop-in replacement. ----
struct RelLZ {
    std::vector<uint8_t> store; std::vector<uint32_t> head, chain; static const uint32_t HB=23, MINM=4, MAXC=64;
    void reset(){ store.clear(); store.reserve(64u<<20); head.assign(size_t(1)<<HB,UINT32_MAX); chain.clear(); }
    inline uint32_t h4(uint32_t pos) const { uint32_t v; memcpy(&v,&store[pos],4); return (v*2654435761u)>>(32-HB); }
    inline void ins(uint32_t pos){ chain[pos]=head[h4(pos)]; head[h4(pos)]=pos; }
    // encode line[0..len) as segments; append to store. seg = repeated: varint(litlen) lit-bytes varint(matchlen) [varint(dist) if matchlen>0].
    void encode(const uint8_t* line, uint32_t len, std::vector<uint8_t>& seg){
        uint32_t base=uint32_t(store.size()); store.insert(store.end(),line,line+len); uint32_t end=uint32_t(store.size());
        chain.resize(store.size(),UINT32_MAX);
        uint32_t i=base;
        while(i<end){ uint32_t litstart=i, ml=0, md=0;
            while(i<end){ uint32_t bl=0,bp=0; if(i+MINM<=end){ uint32_t c=head[h4(i)],ch=0; while(c!=UINT32_MAX&&ch<MAXC){ if(c<i){ uint32_t L=0,mx=end-i; while(L<mx&&store[c+L]==store[i+L])++L; if(L>=MINM&&L>bl){bl=L;bp=c;} } c=chain[c]; ++ch; } }
                if(bl>=MINM){ ml=bl; md=i-bp; break; } ins(i); ++i; }
            put_varint(seg, i-litstart); seg.insert(seg.end(), &store[litstart], &store[litstart]+(i-litstart));
            put_varint(seg, ml); if(ml){ put_varint(seg, md); for(uint32_t j=0;j<ml;++j){ ins(i); ++i; } }
        }
    }
    bool decode(const uint8_t* seg, size_t n, std::vector<uint8_t>& out){
        uint32_t base=uint32_t(store.size()); const uint8_t* p=seg; const uint8_t* e=seg+n;
        while(p<e){ uint64_t litlen=get_varint(p); for(uint64_t j=0;j<litlen;++j) store.push_back(*p++);
            uint64_t ml=get_varint(p); if(ml){ uint64_t md=get_varint(p); uint32_t src=uint32_t(store.size()-md); for(uint64_t j=0;j<ml;++j) store.push_back(store[src+j]); } }
        out.assign(store.begin()+base, store.end()); return true;
    }
    uint64_t store_bytes() const { return store.size(); }
};

enum ComponentWirePart : size_t {
    CW_ROOT,
    CW_BLOCK,
    CW_PATH,
    CW_FRAMING,
    CW_REGION_CONTROL,
    CW_REGION_OTHER,
    CW_LITERAL,
    CW_ARRAY_CONTROL,
    CW_ARRAY_VALUES,
    CW_SOURCE_CONTROL,
    CW_SOURCE_FILES,
    CW_SELECTOR,
    CW_BLOB,
    CW_BLOB_PATCH,
    CW_LINE_OTHER,
    CW_ASSOCIATION,
    CW_MISSING_REQUEST,
    CW_BLOB_FALLBACK_REQUEST,
    CW_BLOB_FALLBACK_REPLY,
    CW_MISSING_OTHER,
    CW_COUNT,
};

static constexpr std::array<const char*,CW_COUNT> componentWireNames={
    "root_wire_bytes","block_wire_bytes","path_wire_bytes","framing_wire_bytes",
    "region_control_wire_bytes","region_other_wire_bytes","literal_wire_bytes",
    "array_control_wire_bytes","array_values_wire_bytes","source_control_wire_bytes",
    "source_files_wire_bytes","selector_wire_bytes","blob_wire_bytes",
    "blob_patch_wire_bytes","line_other_wire_bytes","association_wire_bytes",
    "missing_request_wire_bytes","blob_fallback_request_wire_bytes",
    "blob_fallback_reply_wire_bytes","missing_other_wire_bytes",
};

static constexpr std::array<const char*,8> componentRawNames={
    "region_control_raw_bytes","literal_raw_bytes","array_control_raw_bytes",
    "array_values_raw_bytes","source_control_raw_bytes","source_files_raw_bytes",
    "blob_raw_bytes","blob_patch_raw_bytes",
};

int main(int argc,char**argv){
    const char* manifest=nullptr;const char*residualDumpPath=nullptr;const char*mixedDumpPrefix=nullptr;const char*curveTsvPath=nullptr;const char*literalGroupPrefix=nullptr;const char*literalGroupWirePath=nullptr;const char*cfSinkPath=nullptr;const char*fcSinkPath=nullptr;const char*sinkCurvePath=nullptr; size_t sinkBuildTus=0; bool sinkReplay=false,literalOnDemand=false,selftestTags=false,selftestBadRoot=false; size_t routeCount=0; const char*selectorTsvPath=nullptr; size_t max_files=SIZE_MAX,entropyRestartTus=0,replayRepetitions=1,literalGroupTus=0,literalGroupWorkers=1; int zlevel=3,literalZLevel=-1,arrayZLevel=-1,blobZLevel=-1,halfColdBit=-1,blobCanonicalLevel=9; uint32_t sourceAdmitRatio=6,blobThreads=4,blobZstdWorkers=0,blobZstdJobMiB=0,blobZstdOverlapLog=0,blobFallbackEvery=0,s1MinMatch=3,s1MaxChain=1024; bool useD1=true, useD2=false, useS1=true, useD2mine=false, deep=false, warm=false, usePriorRoot=false,useSortedLines=false,useByteArrayLines=false,useMixedRegions=false,useProjectSource=false,useKeyMap=false,useDirectOrdinals=false,useCompressedBlobs=false,useBlobEagerPatches=true,useMoFactor=false,traceMo=false,useAlphaLines=false,useResidualLdm=false,splitControlCeiling=false,structureCeiling=false,literalGroupEvaluateZstd10=true,stableRootTags=false,openFinalEntropy=false;
    const char*blobDumpPath=nullptr;const char*componentCurveTsvPath=nullptr;
    for(int i=1;i<argc;++i){ if(!strcmp(argv[i],"--manifest")&&i+1<argc)manifest=argv[++i];
        else if(!strcmp(argv[i],"--z")&&i+1<argc)zlevel=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--literal-z")&&i+1<argc){char*end=nullptr;long value=strtol(argv[++i],&end,10);if(!end||*end||value<1||value>9){fprintf(stderr,"bad literal zstd level\n");return 2;}literalZLevel=int(value);}
        else if(!strcmp(argv[i],"--array-z")&&i+1<argc){char*end=nullptr;long value=strtol(argv[++i],&end,10);if(!end||*end||value<1||value>9){fprintf(stderr,"bad array zstd level\n");return 2;}arrayZLevel=int(value);}
        else if(!strcmp(argv[i],"--blob-z")&&i+1<argc){char*end=nullptr;long value=strtol(argv[++i],&end,10);if(!end||*end||value<1||value>9){fprintf(stderr,"bad blob zstd level\n");return 2;}blobZLevel=int(value);}
        else if(!strcmp(argv[i],"--no-d1"))useD1=false;
        else if(!strcmp(argv[i],"--v1"))useS1=false;   // V1 baseline: raw region-id root, no S1 blocks
        else if(!strcmp(argv[i],"--d2"))useD2mine=true;      // inline relative-LZ line codec
        else if(!strcmp(argv[i],"--d2helper"))useD2=true;    // helper's definition_codec (needs -DWITH_D2)
        else if(!strcmp(argv[i],"--prior-root"))usePriorRoot=true;
        else if(!strcmp(argv[i],"--sorted-lines"))useSortedLines=true;
        else if(!strcmp(argv[i],"--byte-array-lines")){useSortedLines=true;useByteArrayLines=true;}
        else if(!strcmp(argv[i],"--mixed-regions"))useMixedRegions=true;
        else if(!strcmp(argv[i],"--compressed-blobs"))useCompressedBlobs=true;
        else if(!strcmp(argv[i],"--mo-factor"))useMoFactor=true;
        else if(!strcmp(argv[i],"--mo-trace"))traceMo=true;
        else if(!strcmp(argv[i],"--alpha-lines"))useAlphaLines=true;
        else if(!strcmp(argv[i],"--residual-ldm"))useResidualLdm=true;
        else if(!strcmp(argv[i],"--residual-dump")&&i+1<argc)residualDumpPath=argv[++i];
        else if(!strcmp(argv[i],"--blob-dump")&&i+1<argc)blobDumpPath=argv[++i];
        else if(!strcmp(argv[i],"--mixed-dump-prefix")&&i+1<argc)mixedDumpPrefix=argv[++i];
        else if(!strcmp(argv[i],"--literal-group-prefix")&&i+1<argc)literalGroupPrefix=argv[++i];
        else if(!strcmp(argv[i],"--literal-group-wire")&&i+1<argc)literalGroupWirePath=argv[++i];
        else if(!strcmp(argv[i],"--literal-group-tus")&&i+1<argc){char*end=nullptr;unsigned long long value=strtoull(argv[++i],&end,10);if(!end||*end||!value||value>SIZE_MAX){fprintf(stderr,"bad literal group TU count\n");return 2;}literalGroupTus=size_t(value);}
        else if(!strcmp(argv[i],"--literal-group-workers")&&i+1<argc){char*end=nullptr;unsigned long long value=strtoull(argv[++i],&end,10);if(!end||*end||!value||value>64){fprintf(stderr,"bad literal group worker count\n");return 2;}literalGroupWorkers=size_t(value);}
        else if(!strcmp(argv[i],"--literal-group-skip-zstd10"))literalGroupEvaluateZstd10=false;
        else if(!strcmp(argv[i],"--cf-sink")&&i+1<argc)cfSinkPath=argv[++i];
        else if(!strcmp(argv[i],"--fc-sink")&&i+1<argc)fcSinkPath=argv[++i];
        else if(!strcmp(argv[i],"--sink-replay"))sinkReplay=true;
        else if(!strcmp(argv[i],"--literal-ondemand"))literalOnDemand=true;
        else if(!strcmp(argv[i],"--route-s1")&&i+1<argc){char*e=nullptr;unsigned long long v=strtoull(argv[++i],&e,10);
            if(!e||*e||!v){fprintf(stderr,"bad route count\n");return 2;}
            // Only the 1F slice exists.  A second route mints canonical Block ids this route
            // never receives, and F installs Blocks by dense arrival order (id+1 must equal
            // Fblk_off.size()), so the gap breaks the NEXT Block's install -- measured as
            // "bad direct Block identity" at W>=4.  Multi-route needs the catalogue
            // materializer and a canonical-id-keyed F store; refuse rather than pretend.
            if(v!=1){fprintf(stderr,"--route-s1 %llu: multi-route is not yet materialized; only 1 is supported\n",v);return 2;}
            routeCount=size_t(v);}
        else if(!strcmp(argv[i],"--selector-tsv")&&i+1<argc)selectorTsvPath=argv[++i];
        else if(!strcmp(argv[i],"--selftest-tags"))selftestTags=true;
        else if(!strcmp(argv[i],"--selftest-bad-root"))selftestBadRoot=true;
        else if(!strcmp(argv[i],"--sink-curve")&&i+1<argc)sinkCurvePath=argv[++i];
        else if(!strcmp(argv[i],"--sink-build-tus")&&i+1<argc){char*end=nullptr;unsigned long long value=strtoull(argv[++i],&end,10);if(!end||*end||!value||value>SIZE_MAX){fprintf(stderr,"bad sink build TU count\n");return 2;}sinkBuildTus=size_t(value);}
        else if(!strcmp(argv[i],"--curve-tsv")&&i+1<argc)curveTsvPath=argv[++i];
        else if(!strcmp(argv[i],"--component-curve-tsv")&&i+1<argc)componentCurveTsvPath=argv[++i];
        else if(!strcmp(argv[i],"--split-control-ceiling"))splitControlCeiling=true;
        else if(!strcmp(argv[i],"--structure-ceiling"))structureCeiling=true;
        else if(!strcmp(argv[i],"--s1-min-match")&&i+1<argc){char*end=nullptr;unsigned long value=strtoul(argv[++i],&end,10);if(!end||*end||value<2||value>16){fprintf(stderr,"bad S1 minimum match\n");return 2;}s1MinMatch=uint32_t(value);}
        else if(!strcmp(argv[i],"--s1-max-chain")&&i+1<argc){char*end=nullptr;unsigned long value=strtoul(argv[++i],&end,10);if(!end||*end||!value||value>4096){fprintf(stderr,"bad S1 maximum chain\n");return 2;}s1MaxChain=uint32_t(value);}
        else if(!strcmp(argv[i],"--blob-threads")&&i+1<argc){char*end=nullptr;unsigned long value=strtoul(argv[++i],&end,10);if(!end||*end||!value||value>32){fprintf(stderr,"bad blob thread count\n");return 2;}blobThreads=uint32_t(value);}
        else if(!strcmp(argv[i],"--blob-zstd-workers")&&i+1<argc){char*end=nullptr;unsigned long value=strtoul(argv[++i],&end,10);if(!end||*end||!value||value>8){fprintf(stderr,"bad blob zstd worker count\n");return 2;}blobZstdWorkers=uint32_t(value);}
        else if(!strcmp(argv[i],"--blob-zstd-job-mib")&&i+1<argc){char*end=nullptr;unsigned long value=strtoul(argv[++i],&end,10);if(!end||*end||!value||value>64){fprintf(stderr,"bad blob zstd job size\n");return 2;}blobZstdJobMiB=uint32_t(value);}
        else if(!strcmp(argv[i],"--blob-zstd-overlap-log")&&i+1<argc){char*end=nullptr;unsigned long value=strtoul(argv[++i],&end,10);if(!end||*end||!value||value>9){fprintf(stderr,"bad blob zstd overlap log\n");return 2;}blobZstdOverlapLog=uint32_t(value);}
        else if(!strcmp(argv[i],"--blob-fallback-every")&&i+1<argc){char*end=nullptr;unsigned long value=strtoul(argv[++i],&end,10);if(!end||*end||!value||value>UINT32_MAX){fprintf(stderr,"bad blob fallback interval\n");return 2;}blobFallbackEvery=uint32_t(value);}
        else if(!strcmp(argv[i],"--blob-lazy-fallback"))useBlobEagerPatches=false;
        else if(!strcmp(argv[i],"--blob-canonical-level")&&i+1<argc){char*end=nullptr;long value=strtol(argv[++i],&end,10);if(!end||*end||value<1||value>9){fprintf(stderr,"bad blob canonical level\n");return 2;}blobCanonicalLevel=int(value);}
        else if(!strcmp(argv[i],"--source-package"))useProjectSource=true;
        else if(!strcmp(argv[i],"--source-admit-ratio")&&i+1<argc){char*end=nullptr;unsigned long value=strtoul(argv[++i],&end,10);if(!end||*end||!value||value>1000){fprintf(stderr,"bad source admission ratio\n");return 2;}sourceAdmitRatio=uint32_t(value);}
        else if(!strcmp(argv[i],"--key-map"))useKeyMap=true;
        else if(!strcmp(argv[i],"--direct-ordinals")){useKeyMap=true;useDirectOrdinals=true;}
        else if(!strcmp(argv[i],"--half-cold-bit")&&i+1<argc){char*end=nullptr;long value=strtol(argv[++i],&end,10);if(!end||*end||(value!=0&&value!=1)){fprintf(stderr,"bad half-cold bit\n");return 2;}halfColdBit=int(value);useKeyMap=true;}
        else if(!strcmp(argv[i],"--deep"))deep=true;         // run slow z19/z22 entropy ladder + reorder test
        else if(!strcmp(argv[i],"--warm"))warm=true;         // Basis C: 2nd pass with dict retained -> warm steady-state wire
        else if(!strcmp(argv[i],"--max-files")&&i+1<argc){ char*t=nullptr; unsigned long long v=strtoull(argv[++i],&t,10); if(!t||*t||!v){fprintf(stderr,"bad max-files\n");return 2;} max_files=size_t(v); }
        else if((!strcmp(argv[i],"--entropy-restart-tus")||!strcmp(argv[i],"--build-tus"))&&i+1<argc){ char*t=nullptr; unsigned long long v=strtoull(argv[++i],&t,10); if(!t||*t||!v||v>SIZE_MAX){fprintf(stderr,"bad entropy restart TU count\n");return 2;} entropyRestartTus=size_t(v); }
        else if(!strcmp(argv[i],"--stable-root-tags"))stableRootTags=true;
        else if(!strcmp(argv[i],"--open-final-entropy"))openFinalEntropy=true;
        else if((!strcmp(argv[i],"--replay-repetitions")||!strcmp(argv[i],"--build-repetitions"))&&i+1<argc){ char*t=nullptr; unsigned long long v=strtoull(argv[++i],&t,10); if(!t||*t||!v||v>SIZE_MAX){fprintf(stderr,"bad replay repetition count\n");return 2;} replayRepetitions=size_t(v); }
        else { fprintf(stderr,"unknown %s\n",argv[i]); return 2; } }
#if !defined(WITH_BSC_GROUPS)
    (void)literalGroupEvaluateZstd10;
#endif
    if(selftestTags){
        // Direct test of the tag/bound semantics, including the case a well-formed encoder
        // never produces: the sentinel slot just past the last real Block.  Driven from the
        // regression script; every branch below is one local-oracle named.
        uint32_t tag=0; int bad=0;
        const uint32_t R=7,B=3;   // Regions 0..6, Blocks 0..2
        auto note=[&](const char*what){ fprintf(stderr,"selftest-tags: %s\n",what); ++bad; };
        for(uint32_t r=0;r<R;++r)            // both parities of Region id must survive
            if(!wire_to_tag(region_tag(r),true,R,B,tag)||tag_is_block(tag)||tag_id(tag)!=r) note("stable Region round trip");
        for(uint32_t k=0;k<B;++k)
            if(!wire_to_tag(block_tag(k),true,R,B,tag)||!tag_is_block(tag)||tag_id(tag)!=k) note("stable Block round trip");
        if(wire_to_tag(region_tag(R),true,R,B,tag)) note("stable accepted an out-of-range Region");
        if(wire_to_tag(block_tag(B),true,R,B,tag))  note("stable accepted an out-of-range Block");
        // legacy flat: NREG+count names the sentinel slot and must be refused, while
        // NREG+count-1 is the last real Block and must be accepted
        if(wire_to_tag(uint64_t(R)+B,false,R,B,tag)) note("legacy accepted the sentinel Block slot");
        if(!wire_to_tag(uint64_t(R)+B-1,false,R,B,tag)||!tag_is_block(tag)||tag_id(tag)!=B-1) note("legacy rejected the last real Block");
        for(uint32_t r=0;r<R;++r)
            if(!wire_to_tag(r,false,R,B,tag)||tag_is_block(tag)||tag_id(tag)!=r) note("legacy Region round trip");
        // checked creation at the boundary, both halves of the id space
        if(!make_region_tag(kTagIdLimit-1,tag)||tag_is_block(tag)||tag_id(tag)!=uint32_t(kTagIdLimit-1)) note("checked creation rejected Region 2^31-1");
        if(!make_block_tag (kTagIdLimit-1,tag)||!tag_is_block(tag)||tag_id(tag)!=uint32_t(kTagIdLimit-1)) note("checked creation rejected Block 2^31-1");
        if(make_region_tag(kTagIdLimit,tag)) note("checked creation accepted Region 2^31");
        if(make_block_tag (kTagIdLimit,tag)) note("checked creation accepted Block 2^31");
        if(make_region_tag(~uint64_t(0),tag)) note("checked creation accepted a 64-bit Region id");
        // UINT32_MAX+1 truncates to 0, which is a perfectly valid id -- so this case fails
        // only if the check happens BEFORE the narrowing, which is the property under test.
        if(make_region_tag(uint64_t(UINT32_MAX)+1,tag)) note("checked creation accepted Region UINT32_MAX+1");
        if(make_block_tag (uint64_t(UINT32_MAX)+1,tag)) note("checked creation accepted Block UINT32_MAX+1");
        printf("selftest-tags: %s\n",bad?"FAIL":"PASS");
        return bad?1:0;
    }
    if(!manifest){ fprintf(stderr,"usage: %s --manifest F [--z LEVEL] [--literal-z 1..9] [--array-z 1..9] [--blob-z 1..9] [--no-d1] [--d2] [--prior-root] [--structure-ceiling] [--s1-min-match N] [--s1-max-chain N] [--sorted-lines|--byte-array-lines|--mixed-regions [--alpha-lines|--residual-ldm] [--residual-dump PATH] [--mixed-dump-prefix PATH] [--literal-group-prefix PREFIX --literal-group-tus N [--literal-group-workers N] [--literal-group-skip-zstd10] [--literal-group-wire PATH]] [--split-control-ceiling] [--compressed-blobs [--mo-factor [--mo-trace]] [--blob-threads N] [--blob-zstd-workers N --blob-zstd-job-mib N --blob-zstd-overlap-log N] [--blob-fallback-every N] [--blob-lazy-fallback] [--blob-canonical-level 1..9] [--blob-dump PATH]] [--key-map|--direct-ordinals|--half-cold-bit 0|1] [--source-package [--source-admit-ratio N]]] [--max-files N] [--replay-repetitions N] [--entropy-restart-tus N] [--stable-root-tags] [--open-final-entropy] [--curve-tsv PATH] [--component-curve-tsv PATH]\n",argv[0]); return 2; }
    if(useProjectSource&&!useMixedRegions){fprintf(stderr,"--source-package requires --mixed-regions\n");return 2;}
    if(useKeyMap&&!useMixedRegions){fprintf(stderr,"--key-map and --half-cold-bit require --mixed-regions\n");return 2;}
    if(useCompressedBlobs&&(!useMixedRegions||!useByteArrayLines)){fprintf(stderr,"--compressed-blobs requires --mixed-regions --byte-array-lines\n");return 2;}
    if(useMoFactor&&!useCompressedBlobs){fprintf(stderr,"--mo-factor requires --compressed-blobs\n");return 2;}
    if(useMoFactor&&useBlobEagerPatches){fprintf(stderr,"--mo-factor requires --blob-lazy-fallback\n");return 2;}
    if(traceMo&&!useMoFactor){fprintf(stderr,"--mo-trace requires --mo-factor\n");return 2;}
    if(useAlphaLines&&!useMixedRegions){fprintf(stderr,"--alpha-lines requires --mixed-regions\n");return 2;}
    if(useResidualLdm&&!useMixedRegions){fprintf(stderr,"--residual-ldm requires --mixed-regions\n");return 2;}
    if(residualDumpPath&&!useMixedRegions){fprintf(stderr,"--residual-dump requires --mixed-regions\n");return 2;}
    if(blobDumpPath&&!useCompressedBlobs){fprintf(stderr,"--blob-dump requires --compressed-blobs\n");return 2;}
    if(mixedDumpPrefix&&!useMixedRegions){fprintf(stderr,"--mixed-dump-prefix requires --mixed-regions\n");return 2;}
    // --literal-ondemand is the PRODUCT path: one TU's literal frame, encoded at that TU
    // from that TU's own bytes.  It needs no dump file and no planning pass, so it is
    // mutually exclusive with the offline plan rather than a mode of it.
    if(literalOnDemand&&literalGroupPrefix){fprintf(stderr,"--literal-ondemand replaces --literal-group-prefix; they are alternatives\n");return 2;}
    if(literalOnDemand&&literalGroupTus&&literalGroupTus!=1){fprintf(stderr,"--literal-ondemand encodes exactly one TU per frame\n");return 2;}
    if((literalGroupPrefix!=nullptr)!=(literalGroupTus!=0)&&!literalOnDemand){fprintf(stderr,"literal groups require both --literal-group-prefix and --literal-group-tus\n");return 2;}
    if(literalGroupWirePath&&!literalGroupPrefix){fprintf(stderr,"--literal-group-wire requires --literal-group-prefix\n");return 2;}
    // Sinks are only meaningful for the message-based path this binding uses; refusing the
    // other paths is better than silently emitting a stream that omits their traffic.
    if((cfSinkPath||fcSinkPath)&&!(useMixedRegions&&useKeyMap&&useDirectOrdinals)){fprintf(stderr,"the wire sinks require --mixed-regions with --direct-ordinals\n");return 2;}
    if(sinkCurvePath&&!cfSinkPath){fprintf(stderr,"--sink-curve requires --cf-sink\n");return 2;}
    if(sinkBuildTus&&!cfSinkPath){fprintf(stderr,"--sink-build-tus requires --cf-sink\n");return 2;}
    if(cfSinkPath&&!fcSinkPath){fprintf(stderr,"--cf-sink requires --fc-sink: the reverse direction is reported, never dropped\n");return 2;}
    // Without S1 there are no Blocks and no route matcher, so --route-s1 would exit 0 having
    // gated nothing -- a silent no-op is worse than a refusal.
    if(routeCount&&!useS1){fprintf(stderr,"--route-s1 requires S1; --v1 has no Blocks to share\n");return 2;}
    if(cfSinkPath&&(warm||replayRepetitions!=1)){fprintf(stderr,"the wire sinks require a single measured pass (no --warm/--replay-repetitions)\n");return 2;}
    if(literalGroupWorkers!=1&&!literalGroupPrefix){fprintf(stderr,"--literal-group-workers requires --literal-group-prefix\n");return 2;}
    if(literalGroupPrefix&&(!useMixedRegions||useAlphaLines||useResidualLdm)){fprintf(stderr,"literal groups require ordinary --mixed-regions literal coding\n");return 2;}
    if(stableRootTags&&!useDirectOrdinals){fprintf(stderr,"--stable-root-tags requires --direct-ordinals\n");return 2;}
    if(stableRootTags&&usePriorRoot){fprintf(stderr,"--stable-root-tags does not support --prior-root\n");return 2;}
    if(openFinalEntropy&&!stableRootTags){fprintf(stderr,"--open-final-entropy requires --stable-root-tags\n");return 2;}
    if(openFinalEntropy&&entropyRestartTus){fprintf(stderr,"--open-final-entropy cannot be combined with entropy restarts\n");return 2;}
#if !defined(WITH_BSC_GROUPS)
    if(literalGroupPrefix){fprintf(stderr,"literal groups require a WITH_BSC_GROUPS build\n");return 2;}
#endif
    if(splitControlCeiling&&!useMixedRegions){fprintf(stderr,"--split-control-ceiling requires --mixed-regions\n");return 2;}
    if(structureCeiling&&!useDirectOrdinals){fprintf(stderr,"--structure-ceiling requires --direct-ordinals\n");return 2;}
    if(blobFallbackEvery&&!useCompressedBlobs){fprintf(stderr,"--blob-fallback-every requires --compressed-blobs\n");return 2;}
    if(blobZstdWorkers&&!useCompressedBlobs){fprintf(stderr,"--blob-zstd-workers requires --compressed-blobs\n");return 2;}
    if((blobZstdJobMiB||blobZstdOverlapLog)&&!blobZstdWorkers){fprintf(stderr,"blob zstd job controls require --blob-zstd-workers\n");return 2;}
    if(blobZstdWorkers&&(!blobZstdJobMiB||!blobZstdOverlapLog)){fprintf(stderr,"blob zstd workers require job size and overlap log\n");return 2;}
    if(!useBlobEagerPatches&&!useCompressedBlobs){fprintf(stderr,"--blob-lazy-fallback requires --compressed-blobs\n");return 2;}
    if(blobCanonicalLevel!=9&&!useCompressedBlobs){fprintf(stderr,"--blob-canonical-level requires --compressed-blobs\n");return 2;}
    if(useCompressedBlobs&&strcmp(zlibVersion(),ZLIB_VERSION)){fprintf(stderr,"zlib header/runtime version differs\n");return 2;}
    if(literalZLevel<0)literalZLevel=zlevel;
    if(arrayZLevel<0)arrayZLevel=zlevel;
    if(blobZLevel<0)blobZLevel=zlevel;
    if((literalZLevel!=zlevel||arrayZLevel!=zlevel)&&!useMixedRegions){fprintf(stderr,"material zstd overrides require --mixed-regions\n");return 2;}
    if(blobZLevel!=zlevel&&!useCompressedBlobs){fprintf(stderr,"--blob-z requires --compressed-blobs\n");return 2;}
    if(usePriorRoot) useS1=false;
    if(useSortedLines){ useD1=false; useD2=false; useD2mine=false; if(warm){fprintf(stderr,"--sorted-lines warm pass not implemented\n");return 2;} }
    if(useMixedRegions){ useSortedLines=false; useD1=false; useD2=false; useD2mine=false;
        if(usePriorRoot||warm){fprintf(stderr,"--mixed-regions supports the cold S1 path only\n");return 2;} }
#ifndef HAVE_DEFCODEC
    if(useD2){ fprintf(stderr,"note: --d2 requested but definition_codec.h not present; ignoring.\n"); useD2=false; }
#endif

    auto t0=Clock::now(); Corpus corpus=load_corpus(manifest,max_files); Interner dict;
    std::vector<uint32_t> allreg; std::vector<size_t> roff; roff.push_back(0);
    { uint32_t maxlen=0; for(auto&f:corpus.files) maxlen=std::max(maxlen,f.len); std::vector<uint32_t> out(size_t(maxlen)+1); uint64_t hits=0; std::vector<uint32_t> rs;
      for(auto&f:corpus.files){ size_t oc=0; rs.clear(); const char*p=corpus.bytes.data()+f.off; dict.process(p,p+f.len,out.data(),oc,hits,true,&rs); allreg.insert(allreg.end(),rs.begin(),rs.end()); roff.push_back(allreg.size()); } }
    const size_t physicalTUs=corpus.files.size();const uint64_t physicalRaw=corpus.raw;
    if(!physicalTUs){fprintf(stderr,"manifest contains no TUs\n");return 2;}
    if(physicalTUs>SIZE_MAX/replayRepetitions||allreg.size()>SIZE_MAX/replayRepetitions||physicalRaw>UINT64_MAX/replayRepetitions){fprintf(stderr,"logical replay size overflow\n");return 2;}
    if(replayRepetitions>1){
      std::vector<uint32_t>physicalRegions;physicalRegions.swap(allreg);std::vector<size_t>physicalOffsets;physicalOffsets.swap(roff);
      const size_t physicalOccurrences=physicalRegions.size();allreg.reserve(physicalOccurrences*replayRepetitions);roff.reserve(physicalTUs*replayRepetitions+1);roff.push_back(0);
      for(size_t repetition=0;repetition<replayRepetitions;++repetition){const size_t base=allreg.size();allreg.insert(allreg.end(),physicalRegions.begin(),physicalRegions.end());for(size_t t=0;t<physicalTUs;++t)roff.push_back(base+physicalOffsets[t+1]);}
      corpus.raw=physicalRaw*replayRepetitions;
    }
    // Validate the Region count at its FULL width and narrow only after it passes.  Narrowing
    // first would make a count of 2^32+n indistinguishable from n, and no amount of checking
    // inside make_region_tag can recover information the cast already discarded.
    const uint64_t regionCountWide=dict.region_count();
    { uint32_t probe; if(!make_region_tag(regionCountWide?regionCountWide-1:0,probe)){
        fprintf(stderr,"too many Regions for a typed Root tag: %llu\n",(unsigned long long)regionCountWide);return 2;} }
    uint32_t NREG=uint32_t(regionCountWide);const size_t TUs=physicalTUs*replayRepetitions;
    if(useS1&&allreg.size()>UINT32_MAX){fprintf(stderr,"S1 logical Region occurrence space exceeds u32\n");return 2;}
    if(entropyRestartTus&&TUs%entropyRestartTus){fprintf(stderr,"TU count %zu is not a multiple of experimental entropy restart interval %zu\n",TUs,entropyRestartTus);return 2;}
    fprintf(stderr,"loaded+interned %.1fs TUs=%zu raw=%llu physical_tus=%zu physical_raw=%llu replay_repetitions=%zu regions=%u region_occ=%zu distinct_lines=%u\n",secs(t0),TUs,(unsigned long long)corpus.raw,physicalTUs,(unsigned long long)physicalRaw,replayRepetitions,NREG,allreg.size(),dict.distinct());
    if(entropyRestartTus)fprintf(stderr,"experimental entropy restarts: TUs/segment=%zu segments=%zu (not a product build signal)\n",entropyRestartTus,TUs/entropyRestartTus);
    if(stableRootTags)fprintf(stderr,"stable Root tags: region=2*r block=2*k+1\n");
    if(openFinalEntropy)fprintf(stderr,"open final entropy streams: diagnostic prefix mode (no END bytes)\n");

    RootSliceBuild root_slices;
    if(usePriorRoot){
        auto tr=Clock::now(); root_slices=build_root_slices(allreg,roff,4,8);
        fprintf(stderr,"P22 ROOT_SLICE: %.1fs copies=%llu copied_regions=%llu index_entries=%llu\n",
                secs(tr),(unsigned long long)root_slices.copies,
                (unsigned long long)root_slices.copied_regions,
                (unsigned long long)root_slices.index_entries);
    }

    // ===== S1: LZ longest-previous-factor over region-id stream -> per-TU token streams + flat Blocks =====
    // token < NREG = region id ; token >= NREG = block id (flat span of region ids). Causal/prequential.
    std::vector<uint32_t> bchild; std::vector<size_t> boff2; boff2.push_back(0);
    std::vector<uint32_t> tokstream; std::vector<size_t> tokoff; tokoff.push_back(0);
    bool s1Ready=false; uint64_t s1Tokens=0; std::vector<uint32_t> curTok, tuRegions;
    std::vector<uint32_t> bcopy_src; std::vector<uint8_t> bcopy_ok;   // block k: def as COPY(src,len) if ok (source in prior TUs)
    // Per-TU sizes of the Region and Block id spaces: what has been DISCOVERED by TU t, never
    // the final totals.  Every bound and every array length below is taken from these.
    std::vector<uint32_t> blocksAfterTu, regionsAfterTu;
    { uint32_t seen=0; regionsAfterTu.reserve(TUs);
      for(size_t t=0;t<TUs;++t){ for(size_t i=roff[t];i<roff[t+1];++i) if(allreg[i]+1>seen) seen=allreg[i]+1;
        regionsAfterTu.push_back(seen); } }
    // T_current step 3: S1 is p29::OnlineS1, admitting ONE complete TU at a time.  It
    // reproduces the old full-route matcher exactly -- same hash, chain, longest match, tie
    // break, canonical children and first source -- so this must be BYTE-IDENTICAL; a delta
    // would be a bug to locate, not a cost of going online.  The old loop's anchors could
    // read the first Regions of the next TU (j+MINMATCH<=NS); the online form retains the
    // incomplete tail and installs those anchors when the next TU becomes current.
    p29::BlockCatalogue blockCatalogue;
    std::unique_ptr<p29::OnlineS1> globalS1, routeS1;   // 1F slice: exactly one route matcher
    if(useS1){
        size_t NS=allreg.size(); uint32_t MINMATCH=s1MinMatch, MAXCHAIN=s1MaxChain, hbits=22;
        p29::OnlineS1::Config s1cfg; s1cfg.min_match=MINMATCH; s1cfg.max_chain=MAXCHAIN; s1cfg.hash_bits=hbits;
        globalS1.reset(new p29::OnlineS1(s1cfg,blockCatalogue));
        if(routeCount) routeS1.reset(new p29::OnlineS1(s1cfg,blockCatalogue));
        // T_current prereq 2: NOTHING is admitted here.  Admission moved into the
        // chronological transaction loop, so at TU t the matcher and catalogue contain only
        // TUs 0..t -- previously the whole corpus had been admitted before TU 0 was even
        // sent, which a live selector cannot rely on (it would query state holding later TUs).
        (void)NS; s1Ready=true;
    } else { // V1: root = raw region-id sequence
        for(size_t t=0;t<TUs;++t){ for(size_t i=roff[t];i<roff[t+1];++i) tokstream.push_back(region_tag(allreg[i])); tokoff.push_back(tokstream.size()); }
        blocksAfterTu.assign(TUs,0);   // no S1 means no Blocks at any TU
    }
    if(useS1) blocksAfterTu.assign(TUs,0);   // filled live, one TU at a time, in the loop
    if(blocksAfterTu.size()!=TUs||regionsAfterTu.size()!=TUs){fprintf(stderr,"per-TU id-space sizes are incomplete\n");return 2;}

    // ===== ENCODER (C) + DECODER (F): one cold chronological pass, PULL protocol (root -> MISSING -> FILL) =====
    // C tracks F's known sets (single F) so it computes exactly the missing closure. Every wire byte is
    // charged by category, compressed at z<=zlevel per message. F reconstructs .ii bytes byte-exact.
    ZSTD_CCtx* z=ZSTD_createCCtx();ZSTD_DCtx* messageD=ZSTD_createDCtx();ZSTD_CCtx* sourceCostZ=useProjectSource?ZSTD_createCCtx():nullptr;
    std::vector<uint8_t> dst,sourceCostDst,messageEncoded,messageDecoded;
    // Line-indexed state grows as Lines are admitted, exactly like the Region/Block arrays.
    // at_line() is used for reads as well as writes: growing on a read is harmless (the slot
    // reads as the same value-initialised zero a pre-sized vector would have held) and it
    // removes the last way an index could run past the end.
    std::vector<uint8_t> fknownLine; std::vector<uint32_t> ClineToF; uint32_t nextFline=1;
    std::vector<uint8_t> fknownReg;   // fknownReg grows per TU
    auto at_line=[](auto&v,uint32_t ln)->auto&{ if(size_t(ln)>=v.size()) v.resize(size_t(ln)+1); return v[ln]; };
    std::array<ZSTD_CCtx*,5> lineZC{}; std::array<ZSTD_DCtx*,5> lineZD{};
    std::array<uint8_t,5> lineZActive{}; size_t linePartCount=useByteArrayLines?5:3;
    if(useSortedLines) for(size_t i=0;i<linePartCount;++i){
        lineZC[i]=ZSTD_createCCtx(); lineZD[i]=ZSTD_createDCtx();
        ZSTD_CCtx_setParameter(lineZC[i],ZSTD_c_compressionLevel,zlevel);
        ZSTD_CCtx_setParameter(lineZC[i],ZSTD_c_contentSizeFlag,0);
    }
    std::array<ZSTD_CCtx*,6> mixedZC{}; std::array<ZSTD_DCtx*,6> mixedZD{};
    std::array<uint8_t,6> mixedZActive{};size_t mixedPartCount=useProjectSource?6:(useByteArrayLines?4:2);
    // useLiteralGroups selects the literal-frame CHANNEL; usePlannedGroups selects the
    // offline whole-route planner behind it.  The product path has the channel without
    // the planner.
    const bool useLiteralGroups=literalGroupPrefix!=nullptr||literalOnDemand;
    const bool usePlannedGroups=literalGroupPrefix!=nullptr;
#if defined(WITH_BSC_GROUPS)
    LiteralGroupPlan literalGroups;
    // The on-demand encoder.  residual_group::Codec carries no state between frames --
    // verified by encoding re2 with --literal-group-workers 1/4/8 and getting a
    // byte-identical stream -- so one instance reused across TUs is safe and matches what
    // the planner's per-worker instances do.
    residual_group::Codec onDemandCodec;
    std::vector<uint8_t> onDemandFrame, onDemandRaw;
    std::array<uint64_t,3> onDemandSelected{};
    if(usePlannedGroups){try{
        literalGroups=build_literal_group_plan(literalGroupPrefix,TUs,mixedPartCount,
            literalGroupTus,literalGroupWorkers,literalGroupEvaluateZstd10,
            literalGroupWirePath);
      }catch(const std::exception&error){fprintf(stderr,"literal group plan: %s\n",error.what());return 2;}
      fprintf(stderr,"literal groups: TUs/group=%zu groups=%zu workers=%zu raw=%zu wire=%zu candidates=%s selector=[z3=%llu bsc=%llu z10=%llu] encode=%.3fs decode=%.3fs\n",
          literalGroupTus,literalGroups.groups.size(),literalGroups.workers,literalGroups.decoded.size(),literalGroups.wire.size(),
          literalGroups.evaluated_zstd10?"zstd3,bsc,zstd10":"zstd3,bsc",
          (unsigned long long)literalGroups.selected[0],(unsigned long long)literalGroups.selected[1],
          (unsigned long long)literalGroups.selected[2],literalGroups.encode_seconds,literalGroups.decode_seconds);
    }
#endif
    const std::array<int,6> mixedZLevel{{zlevel,literalZLevel,arrayZLevel,arrayZLevel,zlevel,zlevel}};
    if(useMixedRegions) for(size_t i=0;i<mixedPartCount;++i){
        mixedZC[i]=ZSTD_createCCtx(); mixedZD[i]=ZSTD_createDCtx();
        ZSTD_CCtx_setParameter(mixedZC[i],ZSTD_c_compressionLevel,mixedZLevel[i]);
        ZSTD_CCtx_setParameter(mixedZC[i],ZSTD_c_contentSizeFlag,0);
        if(useResidualLdm&&i==1){
            ZSTD_CCtx_setParameter(mixedZC[i],ZSTD_c_enableLongDistanceMatching,1);
            ZSTD_CCtx_setParameter(mixedZC[i],ZSTD_c_windowLog,27);
        }
    }
    ZSTD_CCtx*blobZC=useCompressedBlobs?ZSTD_createCCtx():nullptr;
    ZSTD_DCtx*blobZD=useCompressedBlobs?ZSTD_createDCtx():nullptr;
    ZSTD_CCtx*blobPatchZC=useCompressedBlobs?ZSTD_createCCtx():nullptr;
    ZSTD_DCtx*blobPatchZD=useCompressedBlobs?ZSTD_createDCtx():nullptr;
    mo_factor::EncoderState Cmo;mo_factor::DecoderState Fmo;
    ZSTD_CCtx*alphaZC=useAlphaLines?ZSTD_createCCtx():nullptr;
    ZSTD_DCtx*alphaZD=useAlphaLines?ZSTD_createDCtx():nullptr;
    FILE*residualDump=residualDumpPath?fopen(residualDumpPath,"wb"):nullptr;
    if(residualDumpPath&&!residualDump){perror(residualDumpPath);return 2;}
    FILE*blobDump=blobDumpPath?fopen(blobDumpPath,"wb"):nullptr;FILE*blobDumpLengths=nullptr;
    if(blobDumpPath&&!blobDump){perror(blobDumpPath);return 2;}
    if(blobDumpPath){std::string path=std::string(blobDumpPath)+".lengths";blobDumpLengths=fopen(path.c_str(),"wb");if(!blobDumpLengths){perror(path.c_str());return 2;}}
    std::array<FILE*,6>mixedDumps{};FILE*mixedDumpLengths=nullptr;
    if(mixedDumpPrefix){
        static constexpr const char*names[]={"control","literal","array-control","array-values","source-control","source-files"};
        for(size_t i=0;i<mixedPartCount;++i){
            std::string path=std::string(mixedDumpPrefix)+"."+names[i]+".raw";
            mixedDumps[i]=fopen(path.c_str(),"wb");
            if(!mixedDumps[i]){perror(path.c_str());return 2;}
        }
        std::string path=std::string(mixedDumpPrefix)+".lengths.raw";
        mixedDumpLengths=fopen(path.c_str(),"wb");
        if(!mixedDumpLengths){perror(path.c_str());return 2;}
    }
    std::vector<MixedCLineState> mixedCLine;   // grows with the Lines actually admitted
    uint32_t nextMixedPublic=1;SourceTextStore mixedCSource(useProjectSource),mixedFSource;
    std::vector<uint8_t> mixedSourceSent;
    std::vector<SourceAdmission> mixedSourceAdmission;
    std::unordered_map<std::string,uint32_t> pathid; std::vector<std::string> paths;   // D1 path objects (both sides derive same order)
    // ---- F's OWN independent store, built ONLY from decoded wire bytes (proves self-describing) ----
    std::vector<uint8_t> Fline_data; std::vector<size_t> Fline_off; Fline_off.push_back(0);   // line id k (1-based) -> [off[k-1],off[k])
    Fline_data.reserve(64u<<20);
    std::vector<uint32_t> Freg_child; std::vector<size_t> Freg_off; Freg_off.push_back(0);     // region id k (0-based) -> [off[k],off[k+1])
    std::vector<uint8_t> FmixedRegionData;std::vector<MixedFRegionView> FmixedRegions;   // grows per TU
    std::unordered_map<uint64_t,MixedFRegionView> FpreloadedRegions;
    std::vector<MixedFLineView> FmixedPublic(1);
    std::vector<std::string> Fpaths;
    std::vector<uint8_t> fknownBlk;   // C's model of F's known blocks; grows per TU
    // The shared store (p29_sparse_blocks.h) -- the same definition the test links against,
    // so a decoder regression here cannot leave the gate green.  Fblocks.known() is now THE
    // F-side truth for "does F hold this Block"; the C-side mirror fknownBlk stays separate.
    p29::SparseBlockStore Fblocks;
    std::vector<uint32_t> Freg_stream; Freg_stream.reserve(allreg.size());   // F's reconstructed region occurrence stream (for block COPY defs)
    std::vector<uint32_t> Froot_child; std::vector<size_t> Froot_off; Froot_off.push_back(0);   // completed exact Roots for P22 slices
    double w_blockdef=0;
    // wire byte accumulators (post-z, per category) + f-checkpoint tracking
    double w_root=0, w_linedef=0, w_regiondef=0, w_pathdef=0, w_missing=0, w_framing=0;
    std::array<double,6> mixedPartWire{};std::array<uint64_t,7> mixedOps{};uint64_t mixedLiteralRaw=0,mixedArrayValues=0,mixedSourceBytes=0;
    double mixedBlobWire=0,mixedBlobPatchWire=0,mixedBlobTransformCandidateWire=0,mixedBlobOrdinaryCandidateWire=0,mixedBlobMoCandidateWire=0,mixedBlobFallbackRequestWire=0,mixedBlobFallbackReplyWire=0;
    uint64_t mixedBlobCount=0,mixedBlobDeflated=0,mixedBlobInflated=0,mixedBlobPatchRaw=0,mixedBlobCanonicalExact=0,mixedBlobCorrected=0,mixedBlobReplaced=0,mixedBlobFallbacks=0;
    uint64_t mixedBlobTransformTus=0,mixedBlobOrdinaryTus=0,mixedBlobMoPolicyTus=0,mixedBlobMoTus=0,mixedBlobMoMembers=0,mixedBlobMoBytes=0,mixedBlobMoDefinitions=0;
    uint64_t mixedSourcePackageRaw=0,mixedSourcePackageFiles=0,mixedSourcePotentialRaw=0,mixedSourceConsidered=0,mixedSourceAdmitted=0,mixedSourceEstimatedCost=0;double mixedSelectorWire=0;
    double alphaOrdinaryCandidateWire=0,alphaLiteralKeywordCandidateWire=0,alphaParameterizedKeywordCandidateWire=0,alphaBestCandidateWire=0,alphaSelectedWire=0,alphaControlWire=0,alphaDataWire=0,alphaSelectorWire=0;
    double splitControlCeilingWire=0;std::array<uint64_t,8>splitControlRawBytes{},splitControlWireBytes{};
    double structureBatchRoot=0,structureBatchBlock=0,structureBatchJoint=0,structureLdmJoint=0;
    uint64_t alphaInputRaw=0,alphaEligibleLines=0,alphaGapRaw=0,alphaSelectedTus=0,alphaOrdinaryTus=0,alphaLiteralKeywordTus=0,alphaParameterizedKeywordTus=0;
    alpha_line::Stats alphaStats;
    uint64_t preloadedRegionBytes=0,preloadedRegionCount=0,associatedRegionCount=0;double mixedAssociationWire=0,mixedMissingRequestWire=0;
    const double FRAME=4;   // 4-byte length prefix per framed message (the ACCOUNTING charge)
    struct SelRow{size_t tu;uint64_t reserved,rawRootRaw,rawRootZ,globalRootRaw,globalRootZ,newBlocks,defRaw,defZ;};
    std::vector<SelRow> selRows;
    WireSink cfSink,fcSink;   // the PHYSICAL streams: 5-byte typed header per frame
    std::vector<uint64_t>sinkCfOff(TUs),sinkFcOff(TUs),sinkCfFrames(TUs),sinkFcFrames(TUs);
    if(cfSinkPath){cfSink.open(cfSinkPath,sinkReplay);fcSink.open(fcSinkPath,sinkReplay);}
    uint64_t cum_raw=0; double cum_wire=0;
    // f-checkpoints + H200 trailing window
    std::vector<double> ck_f={0.10,0.25,0.50,0.75,1.00}; std::vector<std::pair<double,double>> ck; // (cum_raw_frac target hit -> ratio) recorded
    size_t ckidx=0; std::vector<double> perTU_raw(TUs), perTU_wire(TUs);
    std::vector<std::array<double,CW_COUNT>>perTUComponentWire(TUs);
    std::vector<std::array<uint64_t,8>>perTUComponentRaw(TUs);
    auto componentWireSnapshot=[&](){
      std::array<double,CW_COUNT>parts{};
      parts[CW_ROOT]=w_root;parts[CW_BLOCK]=w_blockdef;parts[CW_PATH]=w_pathdef;parts[CW_FRAMING]=w_framing;
      parts[CW_REGION_CONTROL]=mixedPartWire[0];parts[CW_REGION_OTHER]=w_regiondef-mixedPartWire[0];
      parts[CW_LITERAL]=mixedPartWire[1];parts[CW_ARRAY_CONTROL]=mixedPartWire[2];parts[CW_ARRAY_VALUES]=mixedPartWire[3];
      parts[CW_SOURCE_CONTROL]=mixedPartWire[4];parts[CW_SOURCE_FILES]=mixedPartWire[5];parts[CW_SELECTOR]=mixedSelectorWire;
      parts[CW_BLOB]=mixedBlobWire;parts[CW_BLOB_PATCH]=mixedBlobPatchWire;
      parts[CW_LINE_OTHER]=w_linedef-mixedPartWire[1]-mixedPartWire[2]-mixedPartWire[3]-mixedPartWire[4]-mixedPartWire[5]
          -mixedSelectorWire-mixedBlobWire-mixedBlobPatchWire;
      parts[CW_ASSOCIATION]=mixedAssociationWire;parts[CW_MISSING_REQUEST]=mixedMissingRequestWire;
      parts[CW_BLOB_FALLBACK_REQUEST]=mixedBlobFallbackRequestWire;parts[CW_BLOB_FALLBACK_REPLY]=mixedBlobFallbackReplyWire;
      parts[CW_MISSING_OTHER]=w_missing-mixedAssociationWire-mixedMissingRequestWire-mixedBlobFallbackRequestWire-mixedBlobFallbackReplyWire;
      return parts;
    };
    // decoder-side reconstruction store: line bytes (F), region->line composition (F)
    // F re-derives line bytes from the defs it receives; we verify against the interner's truth.
    Marker mk; std::vector<uint8_t> segbuf, msg, recon, tmp, expectbuf;
#ifdef HAVE_DEFCODEC
    DefCodec encC, decF;  // helper's relative-LZ line codecs (C + F, identical growing stores)
#endif
    RelLZ relC, relF; if(useD2mine){ relC.reset(); relF.reset(); }   // inline relative-LZ line codec
    bool byteexact=true; uint64_t n_marker=0,n_literal=0;
    std::vector<uint8_t> allLineDefs, allRoots, allRegions, allBlocks, allPaths, allMiss;   // diagnostic: batched-z3 floor (cross-message headroom)
    std::vector<uint8_t> allRegionsRaw;   // diagnostic: region-defs as RAW line-ids (no per-region delta) -> preserves cross-region subsequence matches for z3-LDM
    std::array<std::vector<uint8_t>,8> splitControlAll;

    std::vector<uint8_t> associatedReg,FassociatedReg;
    std::vector<uint32_t> requiredRegionStamp,requiredBlockStamp;
    std::vector<uint32_t> FrequiredRegionStamp,FrequiredBlockStamp;uint32_t requestStamp=0;
    if(useKeyMap){
      // --half-cold-bit preloads F from the WHOLE corpus before any TU is sent, so it is not
      // a T_current path and walking the final Region count is what it actually is.  The key
      // COLLISION check is not preload -- it now runs per TU over newly admitted Regions
      // only (see admitRegionKeys below), so the ordinary path never touches a final count.
      if(halfColdBit>=0){
        FpreloadedRegions.reserve(size_t(NREG));
        if(useDirectOrdinals&&FmixedRegions.size()<NREG)FmixedRegions.resize(NREG);
        for(uint32_t r=0;r<NREG;++r){
          const uint64_t key=dict.region_key(r);
          if(int(key&1)!=halfColdBit) continue;
          MixedFRegionView view{FmixedRegionData.size(),dict.region_raw_len(r),true};
          FmixedRegionData.insert(FmixedRegionData.end(),dict.region_data(r),dict.region_data(r)+dict.region_raw_len(r));
          if(useDirectOrdinals)FmixedRegions[r]=view;else FpreloadedRegions.emplace(key,view);
          preloadedRegionBytes+=view.length;++preloadedRegionCount;
        }
      }
    }
    // Region-key uniqueness is only MEANINGFUL where the 64-bit key is what identifies an
    // object on the wire -- the key-map association path.  With --direct-ordinals the wire
    // carries ordinals, the key is never transmitted or looked up, and the table is pure
    // overhead on the product hot path, so it is not built there.
    //
    // Worth stating plainly rather than leaving implied: Interner::region_key(rid) is
    // rid+1, a bijection, so this check cannot fire under ANY mode as the Interner stands
    // today.  It is kept for the key-consuming path because that is the path that breaks
    // first if the key ever becomes a content hash -- which is exactly when a collision
    // stops being impossible.
    const bool keysIdentifyObjects = useKeyMap && !useDirectOrdinals;
    std::unordered_map<uint64_t,uint32_t> uniqueKeys; uint32_t keyedRegions=0;
    auto admitRegionKeys=[&](uint32_t upto)->bool{
      if(!keysIdentifyObjects) return true;
      for(;keyedRegions<upto;++keyedRegions){
        auto inserted=uniqueKeys.emplace(dict.region_key(keyedRegions),keyedRegions);
        if(!inserted.second){fprintf(stderr,"Region key collision: %u and %u\n",inserted.first->second,keyedRegions);return false;}
      }
      return true;
    };

    int npass = warm?2:1;   // --warm: pass 0 primes dict+F-stores (uncounted); final pass measures warm steady-state.
    for(int pass=0; pass<npass; ++pass){
      if(pass+1==npass && npass>1){   // reset all measurement state before the warm pass; keep fknown* flags + F-stores
        w_root=w_linedef=w_regiondef=w_pathdef=w_blockdef=w_missing=w_framing=0; cum_raw=0; cum_wire=0; n_marker=n_literal=0; byteexact=true;
        ck.clear(); ckidx=0; allLineDefs.clear(); allRoots.clear(); allRegions.clear(); allRegionsRaw.clear(); allBlocks.clear(); allPaths.clear(); allMiss.clear();
      }
      std::array<double,CW_COUNT>previousComponentWire=componentWireSnapshot();
      auto tpass=Clock::now(); double enc_s=0, dec_s=0,fallback_c_s=0;   // split C-encode vs F-decode wall (2-proc per-stream proxy)
#if defined(WITH_BSC_GROUPS)
      size_t literalGroupDecodedCursor=0;
#endif
      for(size_t t=0; t<TUs; ++t){
        auto _te=Clock::now();
        // --- T_current: admit ONLY this TU, here, then use the plan it just produced ------
        if(s1Ready){
            curTok.clear();
            tuRegions.assign(allreg.begin()+roff[t],allreg.begin()+roff[t+1]);

            tuRegions.assign(allreg.begin()+roff[t],allreg.begin()+roff[t+1]);
            // GLOBAL admits FIRST, in the defined C admission order, so canonical ids are
            // assigned by the global chronology and never by route scheduling.
            const p29::TuPlan plan=globalS1->admit(tuRegions);
            // 1F SEAM GATE.  With one route the route matcher sees exactly GLOBAL's sequence,
            // so it must mint NOTHING and must agree with GLOBAL on every field -- including
            // the source coordinates, which are only equal because the two histories coincide
            // at 1F and will NOT be equal once routes diverge.  Checking it here is what makes
            // the shared-catalogue seam a proven property rather than an assumption.
            if(routeS1){
                const size_t before=blockCatalogue.size();
                const p29::TuPlan rp=routeS1->admit(tuRegions);
                if(blockCatalogue.size()!=before){fprintf(stderr,"1F seam: route admission grew the catalogue %zu -> %zu at TU=%zu\n",before,blockCatalogue.size(),t);return 2;}
                if(rp.root.size()!=plan.root.size()||rp.block_uses.size()!=plan.block_uses.size()){fprintf(stderr,"1F seam: Root/BlockUse counts differ at TU=%zu\n",t);return 2;}
                // Identical 1F histories must give identical occurrence windows -- asserted
                // rather than inferred from the plans agreeing.
                if(rp.occurrence_begin!=plan.occurrence_begin||rp.occurrence_end!=plan.occurrence_end){fprintf(stderr,"1F seam: occurrence window differs at TU=%zu\n",t);return 2;}
                // The direct invariant: GLOBAL admitted this exact plan first, so the route
                // must find everything and mint nothing.
                if(!rp.new_blocks.empty()){fprintf(stderr,"1F seam: route minted %zu Block(s) at TU=%zu\n",rp.new_blocks.size(),t);return 2;}
                for(size_t i=0;i<rp.root.size();++i)
                    if(rp.root[i].kind!=plan.root[i].kind||rp.root[i].id!=plan.root[i].id){fprintf(stderr,"1F seam: Root ref %zu differs at TU=%zu\n",i,t);return 2;}
                for(size_t i=0;i<rp.block_uses.size();++i){
                    const p29::BlockUse&a=plan.block_uses[i],&b=rp.block_uses[i];
                    if(a.root_index!=b.root_index||a.block_id!=b.block_id||a.source_position!=b.source_position||
                       a.length!=b.length||a.source_precedes_current_tu!=b.source_precedes_current_tu){fprintf(stderr,"1F seam: BlockUse %zu differs at TU=%zu\n",i,t);return 2;}
                    // canonical_was_new is deliberately NOT required to match: GLOBAL admits
                    // first and MINTS, so the route then FINDS the same id.  That asymmetry is
                    // positive evidence the catalogue is shared -- with separate catalogues
                    // both would report a fresh mint -- so it is asserted rather than ignored.
                    if(b.canonical_was_new){fprintf(stderr,"1F seam: the route minted Block %u at TU=%zu after GLOBAL admitted the identical plan\n",b.block_id,t);return 2;}
                    if(a.canonical_was_new&&b.canonical_was_new){fprintf(stderr,"1F seam: both matchers minted Block %u at TU=%zu, so the catalogue is not shared\n",a.block_id,t);return 2;}
                }
            }
            for(const p29::Ref&ref:plan.root)
                curTok.push_back(ref.kind==p29::RefKind::Block?block_tag(ref.id):region_tag(ref.id));
            // Canonical children come from the shared catalogue; the COPY-legality flag is
            // this matcher's admission order and is NOT a claim about any F's residency.
            for(const p29::NewBlock&nb:plan.new_blocks){
                const std::vector<uint32_t>&kids=blockCatalogue.block(nb.id).regions;
                bchild.insert(bchild.end(),kids.begin(),kids.end());
                boff2.push_back(bchild.size());
                bcopy_src.push_back(nb.first_source_position);
                bcopy_ok.push_back(nb.source_precedes_current_tu?1:0);
            }

                        blocksAfterTu[t]=uint32_t(blockCatalogue.size());
            s1Tokens+=curTok.size();
        } else {
            curTok.assign(tokstream.begin()+tokoff[t],tokstream.begin()+tokoff[t+1]);
        }
        // Growth must follow admission now: blocksAfterTu[t] is produced BY this TU's admit.
        // Grow every id-indexed array to what EXISTS at this TU.  Nothing here is ever
        // sized from a final count, and size() therefore means "discovered so far" -- which
        // is also the right bound to validate an incoming id against.
        { const size_t nreg=regionsAfterTu[t], nblk=size_t(blocksAfterTu[t])+1;
          if(fknownReg.size()<nreg){ fknownReg.resize(nreg,0); FmixedRegions.resize(nreg);
            associatedReg.resize(nreg,0); FassociatedReg.resize(nreg,0);
            requiredRegionStamp.resize(nreg,0); FrequiredRegionStamp.resize(nreg,0); }
          if(fknownBlk.size()<nblk){ fknownBlk.resize(nblk,0);
            requiredBlockStamp.resize(nblk,0); FrequiredBlockStamp.resize(nblk,0); }
          if(!admitRegionKeys(uint32_t(nreg))) return 2; }
        const bool endOfEntropyStream=(!openFinalEntropy&&t+1==TUs)||(entropyRestartTus&&(t+1)%entropyRestartTus==0);
        const uint32_t* tk=curTok.data(); size_t tn=curTok.size();
        // ROOT is available to F before its MISSING reply.  With a key map, C first associates every
        // newly-mentioned conversation-dense Region id with its stable key; F binds cache hits and
        // independently returns the exact missing closure.  The legacy cold path is left byte-identical.
        std::vector<uint8_t> rootb,Frootb;
        // The tag goes out as-is; only the legacy namespace needs the final Region count.
        if(usePriorRoot)rootb=root_slices.programs[t];else for(size_t i=0;i<tn;++i)put_varint(rootb,stableRootTags?uint64_t(tk[i]):uint64_t(legacy_flat_token(tk[i],NREG)));
        // --- candidate COSTING (not yet selection) --------------------------------------
        // Every candidate is built from the SAME pre-TU state and expands to the IDENTICAL
        // Region sequence, so the missing-Region and Line material is common to all three
        // and is deliberately NOT counted here: this measures only what the candidates
        // actually differ in, the Root and the Block definitions it implies.
        if(selectorTsvPath&&!usePriorRoot){
            std::vector<uint8_t> rawRoot;
            for(size_t i=roff[t];i<roff[t+1];++i)
                put_varint(rawRoot,stableRootTags?uint64_t(region_tag(allreg[i])):uint64_t(allreg[i]));
            std::vector<uint8_t> costDst;
            const size_t rawBytes=zstd_size(z,rawRoot.data(),rawRoot.size(),zlevel,costDst);
            const size_t globalBytes=zstd_size(z,rootb.data(),rootb.size(),zlevel,costDst);
            // Blocks this Root names that F does not already hold have to be defined too;
            // RAW names none by construction.
            uint64_t newBlocks=0,defRaw=0;
            for(size_t i=0;i<tn;++i) if(tag_is_block(tk[i])&&!Fblocks.known(tag_id(tk[i]))){
                ++newBlocks; defRaw+=blockCatalogue.block(tag_id(tk[i])).regions.size();
            }
            std::vector<uint8_t> defBytes;
            for(size_t i=0;i<tn;++i) if(tag_is_block(tk[i])&&!Fblocks.known(tag_id(tk[i])))
                for(uint32_t r:blockCatalogue.block(tag_id(tk[i])).regions) put_varint(defBytes,r);
            const size_t globalDef=defBytes.empty()?0:zstd_size(z,defBytes.data(),defBytes.size(),zlevel,costDst);
            selRows.push_back({t,uint64_t(perTU_raw.size()>t?0:0),uint64_t(rawRoot.size()),uint64_t(rawBytes),
                               uint64_t(rootb.size()),uint64_t(globalBytes),newBlocks,uint64_t(defRaw),uint64_t(globalDef)});
        }
        std::vector<uint32_t> missReg,missBlk,associationRegs,requiredRegions,requiredBlocks;
        if(useKeyMap){
          if(++requestStamp==0){std::fill(requiredRegionStamp.begin(),requiredRegionStamp.end(),0);std::fill(requiredBlockStamp.begin(),requiredBlockStamp.end(),0);requestStamp=1;}
          auto requireRegion=[&](uint32_t r){
            if(requiredRegionStamp[r]!=requestStamp){requiredRegionStamp[r]=requestStamp;requiredRegions.push_back(r);}
            if(!useDirectOrdinals&&!associatedReg[r]){associatedReg[r]=1;associationRegs.push_back(r);}
          };
          auto requireBlock=[&](uint32_t k){if(requiredBlockStamp[k]!=requestStamp){requiredBlockStamp[k]=requestStamp;requiredBlocks.push_back(k);}};
          if(usePriorRoot){for(size_t i=roff[t];i<roff[t+1];++i)requireRegion(allreg[i]);}
          else for(size_t i=0;i<tn;++i){uint32_t tok=tk[i];if(!tag_is_block(tok))requireRegion(tag_id(tok));else{
            uint32_t k=tag_id(tok);requireBlock(k);
            if(useDirectOrdinals||!fknownBlk[k])for(size_t j=boff2[k];j<boff2[k+1];++j)requireRegion(bchild[j]);
          }}

          // C -> F ROOT.  F derives direct Region/Block requirements from these decoded bytes;
          // child Regions of a first-use Block are carried by the association batch below.
          // messageEncoded is a reused scratch buffer sized to ZSTD_compressBound, so the
          // RETURNED length is the frame -- messageEncoded.size() is not.
          {const size_t n=zstd_message_roundtrip(z,messageD,rootb,zlevel,messageEncoded,messageDecoded);
           w_root+=n;w_framing+=FRAME;cfSink.emit(WT_ROOT,messageEncoded.data(),n);}
          allRoots.insert(allRoots.end(),rootb.begin(),rootb.end());Frootb=messageDecoded;
          // Test-only: append a Root token naming the sentinel slot one past the last real
          // Block.  A well-formed encoder never emits this, so the CALL-SITE bound is
          // otherwise never exercised and a sentinel-inclusive bound would go unnoticed.
          // The decode below must refuse it.
          if(selftestBadRoot&&t==0){
            const uint32_t nblk=blocksAfterTu[0];
            put_varint(Frootb,stableRootTags?uint64_t(block_tag(nblk)):uint64_t(NREG)+nblk);
            fprintf(stderr,"selftest-bad-root: injected sentinel Block %u at TU 0\n",nblk);
          }

          if(useDirectOrdinals){
            // Blocks precede Region NEED in the direct-ordinal form, so F can derive their child
            // Region closure from its own decoded manifest without a separate id/key association.
            std::vector<uint32_t>FrequiredRegions,FrequiredBlocks;
            auto FrequireRegion=[&](uint32_t r){if(FrequiredRegionStamp[r]!=requestStamp){FrequiredRegionStamp[r]=requestStamp;FrequiredRegions.push_back(r);}};
            auto FrequireBlock=[&](uint32_t k){if(FrequiredBlockStamp[k]!=requestStamp){FrequiredBlockStamp[k]=requestStamp;FrequiredBlocks.push_back(k);}};
            const uint8_t*rp=Frootb.data(),*re=rp+Frootb.size();
            while(rp<re){uint64_t wire=get_varint(rp);uint32_t tok;
              if(!wire_to_tag(wire,stableRootTags,stableRootTags?regionsAfterTu[t]:NREG,blocksAfterTu[t],tok)){fprintf(stderr,"bad direct Root token\n");return 2;}
              if(!tag_is_block(tok))FrequireRegion(tag_id(tok));else FrequireBlock(tag_id(tok));}
            if(FrequiredBlocks.size()!=requiredBlocks.size()){fprintf(stderr,"direct Root Block closure differs\n");return 2;}
            for(uint32_t k:FrequiredBlocks)if(requiredBlockStamp[k]!=requestStamp){fprintf(stderr,"direct Root Block identity differs\n");return 2;}

            std::vector<uint32_t>manifestBlocks;for(uint32_t k:FrequiredBlocks)if(!fknownBlk[k])manifestBlocks.push_back(k);
            if(!manifestBlocks.empty()){
              std::vector<uint8_t>blockRaw;put_varint(blockRaw,manifestBlocks.size());
              for(uint32_t k:manifestBlocks){put_varint(blockRaw,k);size_t length=boff2[k+1]-boff2[k];
                if(bcopy_ok[k]){blockRaw.push_back(1);put_varint(blockRaw,bcopy_src[k]);put_varint(blockRaw,length);}
                else{blockRaw.push_back(0);put_varint(blockRaw,length);for(size_t j=boff2[k];j<boff2[k+1];++j)put_varint(blockRaw,bchild[j]);}
              }
              if(structureCeiling)allBlocks.insert(allBlocks.end(),blockRaw.begin(),blockRaw.end());
              size_t bytes=zstd_message_roundtrip(z,messageD,blockRaw,zlevel,messageEncoded,messageDecoded)+FRAME;w_blockdef+=bytes;
              cfSink.emit(WT_BLOCKDEF,messageEncoded.data(),bytes-size_t(FRAME));
              const uint8_t*bp=messageDecoded.data(),*be=bp+messageDecoded.size();uint64_t count=get_varint(bp);
              if(count!=manifestBlocks.size()){fprintf(stderr,"direct Block manifest count differs\n");return 2;}
              for(uint64_t i=0;i<count;++i){uint64_t id=get_varint(bp);if(Fblocks.known(id)){fprintf(stderr,"bad direct Block identity\n");return 2;}uint8_t kind=*bp++;
                if(kind==1){uint64_t source=get_varint(bp),length=get_varint(bp);if(!Fblocks.install_copy(id,Freg_stream,source,length)){fprintf(stderr,"bad direct Block copy\n");return 2;}}
                else if(kind==0){uint64_t length=get_varint(bp);std::vector<uint32_t>kids;kids.reserve(size_t(length<4096?length:4096));for(uint64_t j=0;j<length;++j){uint64_t child=get_varint(bp);if(child>=regionsAfterTu[t]){fprintf(stderr,"bad direct Block child\n");return 2;}kids.push_back(uint32_t(child));}
                  if(!Fblocks.install_children(id,kids.data(),kids.size())){fprintf(stderr,"bad direct Block identity\n");return 2;}}
                else{fprintf(stderr,"bad direct Block kind\n");return 2;}
                fknownBlk[id]=1;
              }
              if(bp!=be){fprintf(stderr,"direct Block manifest has trailing bytes\n");return 2;}
            }
            for(uint32_t k:FrequiredBlocks){if(!Fblocks.known(k)){fprintf(stderr,"missing direct Block definition\n");return 2;}const uint32_t*kb=Fblocks.begin(k);for(uint32_t j=0,n=Fblocks.length(k);j<n;++j)FrequireRegion(kb[j]);}
            if(FrequiredRegions.size()!=requiredRegions.size()){fprintf(stderr,"direct Region closure differs\n");return 2;}
            for(uint32_t r:FrequiredRegions){if(requiredRegionStamp[r]!=requestStamp){fprintf(stderr,"direct Region identity differs\n");return 2;}if(!FmixedRegions[r].known)missReg.push_back(r);}
            if(!manifestBlocks.empty()||!missReg.empty()){
              std::vector<uint8_t>missingRaw;put_varint(missingRaw,missReg.size());for(uint32_t r:missReg)put_varint(missingRaw,r);put_varint(missingRaw,0);
              size_t bytes=zstd_message_roundtrip(z,messageD,missingRaw,zlevel,messageEncoded,messageDecoded)+FRAME;w_missing+=bytes;mixedMissingRequestWire+=bytes;allMiss.insert(allMiss.end(),missingRaw.begin(),missingRaw.end());
              fcSink.emit(WT_NEED,messageEncoded.data(),bytes-size_t(FRAME));   // F -> C: reverse, never in the primary score
              const uint8_t*mp=messageDecoded.data(),*me=mp+messageDecoded.size();uint64_t count=get_varint(mp);std::vector<uint32_t>decoded;decoded.reserve(count);
              for(uint64_t i=0;i<count;++i){uint64_t id=get_varint(mp);if(id>=regionsAfterTu[t]){fprintf(stderr,"bad direct missing Region\n");return 2;}decoded.push_back(uint32_t(id));}
              if(get_varint(mp)!=0||mp!=me||decoded!=missReg){fprintf(stderr,"direct missing reply differs\n");return 2;}
            }
          } else {
          // C -> F: first-use dense-id/key64 associations.  F owns the key-indexed preload and is
          // the only side that decides whether an object is present.
          std::vector<uint32_t> FnewAssociationRegs;
          if(!associationRegs.empty()){
            std::vector<uint8_t> associationRaw;put_varint(associationRaw,associationRegs.size());
            for(uint32_t r:associationRegs){put_varint(associationRaw,r);put_u64le(associationRaw,dict.region_key(r));}
            size_t bytes=zstd_message_roundtrip(z,messageD,associationRaw,zlevel,messageEncoded,messageDecoded)+FRAME;
            w_missing+=bytes;mixedAssociationWire+=bytes;
            const uint8_t*ap=messageDecoded.data(),*ae=ap+messageDecoded.size();uint64_t count=get_varint(ap);
            if(count!=associationRegs.size()){fprintf(stderr,"association count differs\n");return 2;}
            for(uint64_t i=0;i<count;++i){
              if(ap>=ae){fprintf(stderr,"truncated association\n");return 2;}uint64_t dense=get_varint(ap);uint64_t key=get_u64le(ap,ae);
              if(dense>=regionsAfterTu[t]||FassociatedReg[dense]||key!=dict.region_key(uint32_t(dense))){fprintf(stderr,"bad Region association\n");return 2;}
              FassociatedReg[dense]=1;FnewAssociationRegs.push_back(uint32_t(dense));++associatedRegionCount;auto hit=FpreloadedRegions.find(key);
              if(hit!=FpreloadedRegions.end())FmixedRegions[dense]=hit->second;
            }
            if(ap!=ae){fprintf(stderr,"association has trailing bytes\n");return 2;}
          }

          // F -> C: derive the actual requested ids from F-decoded ROOT+association bytes, then
          // test only F's own stores.  This is also the acknowledgement for an all-hit batch.
          std::vector<uint32_t> FrequiredRegions,FrequiredBlocks;
          auto FrequireRegion=[&](uint32_t r){if(FrequiredRegionStamp[r]!=requestStamp){FrequiredRegionStamp[r]=requestStamp;FrequiredRegions.push_back(r);}};
          auto FrequireBlock=[&](uint32_t k){if(FrequiredBlockStamp[k]!=requestStamp){FrequiredBlockStamp[k]=requestStamp;FrequiredBlocks.push_back(k);}};
          for(uint32_t r:FnewAssociationRegs)FrequireRegion(r);
          if(usePriorRoot){fprintf(stderr,"key-map prior Root is not implemented\n");return 2;}
          const uint8_t*rp=Frootb.data(),*re=rp+Frootb.size();
          while(rp<re){uint64_t wire=get_varint(rp);uint32_t tok;
            if(!wire_to_tag(wire,stableRootTags,stableRootTags?regionsAfterTu[t]:NREG,blocksAfterTu[t],tok)){fprintf(stderr,"bad F Root token\n");return 2;}
            if(!tag_is_block(tok))FrequireRegion(tag_id(tok));else FrequireBlock(tag_id(tok));}
          if(FrequiredBlocks.size()!=requiredBlocks.size()) {fprintf(stderr,"F Root Block closure differs\n");return 2;}
          for(uint32_t k:FrequiredBlocks)if(requiredBlockStamp[k]!=requestStamp){fprintf(stderr,"F Root Block identity differs\n");return 2;}
          for(uint32_t r:FrequiredRegions)if(!FmixedRegions[r].known)missReg.push_back(r);
          for(uint32_t k:FrequiredBlocks)if(!Fblocks.known(k))missBlk.push_back(k);
          if(!associationRegs.empty()||!missReg.empty()||!missBlk.empty()){
            std::vector<uint8_t> missingRaw;put_varint(missingRaw,missReg.size());for(uint32_t r:missReg)put_varint(missingRaw,r);
            put_varint(missingRaw,missBlk.size());for(uint32_t k:missBlk)put_varint(missingRaw,NREG+k);
            size_t bytes=zstd_message_roundtrip(z,messageD,missingRaw,zlevel,messageEncoded,messageDecoded)+FRAME;
            w_missing+=bytes;mixedMissingRequestWire+=bytes;allMiss.insert(allMiss.end(),missingRaw.begin(),missingRaw.end());
            std::vector<uint32_t> decodedReg,decodedBlk;const uint8_t*mp=messageDecoded.data(),*me=mp+messageDecoded.size();
            uint64_t regionCount=get_varint(mp);decodedReg.reserve(regionCount);
            for(uint64_t i=0;i<regionCount;++i){if(mp>=me){fprintf(stderr,"truncated missing Region list\n");return 2;}uint64_t r=get_varint(mp);if(r>=NREG) {fprintf(stderr,"bad missing Region id\n");return 2;}decodedReg.push_back(uint32_t(r));}
            if(mp>=me){fprintf(stderr,"truncated missing Block count\n");return 2;}uint64_t blockCount=get_varint(mp);decodedBlk.reserve(blockCount);
            for(uint64_t i=0;i<blockCount;++i){if(mp>=me){fprintf(stderr,"truncated missing Block list\n");return 2;}uint64_t id=get_varint(mp);if(id<NREG||id-NREG>=fknownBlk.size()){fprintf(stderr,"bad missing Block id\n");return 2;}decodedBlk.push_back(uint32_t(id-NREG));}
            if(mp!=me||decodedReg!=missReg||decodedBlk!=missBlk){fprintf(stderr,"missing reply differs\n");return 2;}
          }
          }
          for(uint32_t r:requiredRegions)fknownReg[r]=FmixedRegions[r].known;
        } else {
          // Original single-empty-F model: C's mirror computes the missing closure.
          auto addRegion=[&](uint32_t r){if(fknownReg[r])return;for(uint32_t x:missReg)if(x==r)return;missReg.push_back(r);};
          if(usePriorRoot){for(size_t i=roff[t];i<roff[t+1];++i)addRegion(allreg[i]);}
          else for(size_t i=0;i<tn;++i){uint32_t tok=tk[i];if(!tag_is_block(tok))addRegion(tag_id(tok));else{uint32_t k=tag_id(tok);if(!fknownBlk[k]){
            bool duplicate=false;for(uint32_t x:missBlk)if(x==k){duplicate=true;break;}if(!duplicate){for(size_t j=boff2[k];j<boff2[k+1];++j)addRegion(bchild[j]);missBlk.push_back(k);}
          }}}
          std::vector<uint8_t> missingRaw;put_varint(missingRaw,missReg.size());for(uint32_t r:missReg)put_varint(missingRaw,r);
          put_varint(missingRaw,missBlk.size());for(uint32_t k:missBlk)put_varint(missingRaw,NREG+k);
          if(!missReg.empty()||!missBlk.empty()){w_missing+=zstd_size(z,missingRaw.data(),missingRaw.size(),zlevel,dst)+FRAME;allMiss.insert(allMiss.end(),missingRaw.begin(),missingRaw.end());}
        }
        // --- FILL: new paths, new lines, new region defs, new block defs (topological) ---
        std::vector<uint8_t> fill_paths, fill_lines, fill_regions, fill_regions_raw, fill_blocks; uint32_t np=0,nl=0,nr=0,nb=0;
        std::vector<uint32_t> newLineIds;
        std::array<std::vector<uint8_t>,5> lineRaw, lineEncoded;
        std::array<std::vector<uint8_t>,6> mixedRaw, mixedEncoded;
        std::array<std::vector<uint8_t>,8>splitControl;
        std::vector<alpha_line::Slice>alphaLines;std::vector<uint32_t>alphaGaps;std::vector<uint8_t>alphaGapBytes;
        uint32_t alphaPendingGap=0;uint8_t alphaWireMode=0;std::vector<uint8_t>alphaSelector,alphaOrdinaryEncoded,alphaControlEncoded,alphaDataEncoded;
        std::vector<GeneratedByteArray> mixedArrayEntries;
        std::vector<CompressedBlob> compressedBlobs;
        std::vector<BlobPatch>blobPatches;std::vector<std::vector<uint8_t>>CcanonicalBlobs;
        std::vector<uint8_t> blobRaw,blobEncoded,blobPatchRaw,blobPatchEncoded,blobOriginalRaw;uint8_t blobWireMode=0;
        mo_factor::Encoded moEncoded;std::array<uint32_t,4>moRawSizes{};std::vector<uint8_t>moFactorRaw,moFactorEncoded;
        std::vector<uint32_t>compressedBlobEntriesSeen;
        std::vector<std::pair<uint32_t,const SourceText*>> mixedSourceDefinitions;
        if(!useMixedRegions) for(uint32_t r:missReg){ const uint32_t* lids=dict.region_ids_ptr(r); uint32_t c=dict.region_ids_count(r);
            for(uint32_t j=0;j<c;++j){ uint32_t ln=lids[j]; if(at_line(fknownLine,ln)) continue; at_line(fknownLine,ln)=1; ++nl;
                const LineRef& lr=dict.ref(ln); const char* txt=dict.line_data(lr.off);
                if(useSortedLines) newLineIds.push_back(ln);
                else if(useD1 && parse_marker(txt,lr.len,mk)){ uint32_t pid; auto it=pathid.find(mk.path); if(it==pathid.end()){ pid=uint32_t(paths.size()); pathid.emplace(mk.path,pid); paths.push_back(mk.path);
                        put_varint(fill_paths,mk.path.size()); fill_paths.insert(fill_paths.end(),mk.path.begin(),mk.path.end()); ++np; } else pid=it->second;
                    fill_lines.push_back(1); put_varint(fill_lines,pid); put_varint(fill_lines,mk.lineno); fill_lines.push_back(uint8_t(mk.flags.size())); for(uint8_t f:mk.flags) fill_lines.push_back(f); ++n_marker;
                } else { fill_lines.push_back(0);
                    if(useD2mine){ segbuf.clear(); relC.encode((const uint8_t*)txt,lr.len,segbuf); put_varint(fill_lines,segbuf.size()); fill_lines.insert(fill_lines.end(),segbuf.begin(),segbuf.end()); }
#ifdef HAVE_DEFCODEC
                    else if(useD2){ segbuf.clear(); encC.encode((const uint8_t*)txt,lr.len,segbuf); put_varint(fill_lines,lr.len); put_varint(fill_lines,segbuf.size()); fill_lines.insert(fill_lines.end(),segbuf.begin(),segbuf.end()); }
#endif
                    else { put_varint(fill_lines,lr.len); fill_lines.insert(fill_lines.end(),txt,txt+lr.len); }
                    ++n_literal; }
            }
        }
        if(useMixedRegions && !missReg.empty()){
            put_varint(mixedRaw[0],missReg.size());
            if(splitControlCeiling)put_varint(splitControl[0],missReg.size());
            for(uint32_t r:missReg){
                const uint32_t* lids=dict.region_ids_ptr(r); uint32_t count=dict.region_ids_count(r),offset=0,literalLength=0;
                uint64_t splitRegionOps=0;
                auto splitOpcode=[&](uint8_t opcode){if(splitControlCeiling){splitControl[1].push_back(opcode);++splitRegionOps;}};
                auto splitVarint=[&](size_t stream,uint64_t value){if(splitControlCeiling)put_varint(splitControl[stream],value);};
                std::vector<GeneratedByteArray>regionProbeEntries;std::vector<int32_t>regionLineToProbe(count,-1),regionProbeToBlob;
                if(useCompressedBlobs){
                    for(uint32_t j=0;j<count;++j){const LineRef&line=dict.ref(lids[j]);GeneratedByteArray parsed;
                        if(parse_byte_array(dict.line_data(line.off),line.len,parsed,1)){regionLineToProbe[j]=int32_t(regionProbeEntries.size());regionProbeEntries.push_back(std::move(parsed));}
                    }
                    std::vector<uint8_t>regionInflated;std::vector<CompressedBlob>regionBlobs=find_compressed_blobs(regionProbeEntries,regionInflated);
                    regionProbeToBlob.assign(regionProbeEntries.size(),-1);size_t inflatedBase=blobRaw.size();blobRaw.insert(blobRaw.end(),regionInflated.begin(),regionInflated.end());
                    for(auto blob:regionBlobs){size_t global=compressedBlobs.size();
                        for(size_t entry=blob.first_entry;entry<size_t(blob.first_entry)+blob.entry_count;++entry)regionProbeToBlob[entry]=int32_t(global);
                        blob.first_entry=UINT32_MAX;blob.inflated_offset+=uint32_t(inflatedBase);compressedBlobs.push_back(blob);compressedBlobEntriesSeen.push_back(0);
                    }
                }
                Marker regionMarker;bool regionMarkerOk=false;
                if(count){const LineRef&first=dict.ref(lids[0]);regionMarkerOk=parse_marker(dict.line_data(first.off),first.len,regionMarker);}
                const SourceText*regionSource=regionMarkerOk?&mixedCSource.get(regionMarker.path):nullptr;
                auto ensurePathId=[&](const std::string&path){auto found=pathid.find(path);if(found!=pathid.end())return found->second;
                    uint32_t id=uint32_t(paths.size());pathid.emplace(path,id);paths.push_back(path);put_varint(fill_paths,path.size());fill_paths.insert(fill_paths.end(),path.begin(),path.end());++np;return id;};
                auto ensureSourceDefinition=[&](const std::string&path,uint32_t pathId,const SourceText&source){
                    if(!useProjectSource||system_source_path(path))return;
                    if(mixedSourceSent.size()<=pathId)mixedSourceSent.resize(size_t(pathId)+1);
                    if(mixedSourceSent[pathId])return;
                    mixedSourceSent[pathId]=1;mixedSourceDefinitions.emplace_back(pathId,&source);
                };
                put_varint(mixedRaw[0],dict.region_raw_len(r));
                if(splitControlCeiling)put_varint(splitControl[0],dict.region_raw_len(r));
                auto flushLiteral=[&](){ if(!literalLength)return; mixedRaw[0].push_back(0);put_varint(mixedRaw[0],literalLength);splitOpcode(0);splitVarint(2,literalLength);++mixedOps[0];literalLength=0; };
                for(uint32_t j=0;j<count;++j){
                    uint32_t lineId=lids[j];const LineRef&line=dict.ref(lineId);const char*text=dict.line_data(line.off);MixedCLineState&state=at_line(mixedCLine,lineId);
                    GeneratedByteArray compressedParsed;int32_t probe=useCompressedBlobs?regionLineToProbe[j]:-1;
                    int32_t compressedBlob=probe>=0?regionProbeToBlob[size_t(probe)]:-1;bool forceCompressedArray=compressedBlob>=0;
                    if(forceCompressedArray){compressedParsed=std::move(regionProbeEntries[size_t(probe)]);size_t entry=mixedArrayEntries.size();auto&blob=compressedBlobs[size_t(compressedBlob)];uint32_t&seen=compressedBlobEntriesSeen[size_t(compressedBlob)];
                        if(!seen)blob.first_entry=uint32_t(entry);else if(entry!=size_t(blob.first_entry)+seen){fprintf(stderr,"non-contiguous compressed blob entries\n");return 2;}++seen;}
                    if(forceCompressedArray){
                        mixedArrayValues+=compressedParsed.values.size();flushLiteral();mixedRaw[0].push_back(3);splitOpcode(3);mixedArrayEntries.push_back(std::move(compressedParsed));++mixedOps[3];++n_literal;
                    } else if(state.public_id){
                        flushLiteral();mixedRaw[0].push_back(2);put_varint(mixedRaw[0],state.public_id);splitOpcode(2);splitVarint(3,state.public_id);++mixedOps[2];
                    } else if(state.source_region!=UINT32_MAX&&state.source_region!=r){
                        if(state.source_region>=r||state.source_offset+line.len>dict.region_raw_len(state.source_region)||
                           memcmp(dict.region_data(state.source_region)+state.source_offset,text,line.len)){
                            fprintf(stderr,"bad mixed source Line\n");return 2;
                        }
                        flushLiteral();mixedRaw[0].push_back(1);put_zigzag(mixedRaw[0],int64_t(state.source_region)-int64_t(r));
                        put_varint(mixedRaw[0],state.source_offset);put_varint(mixedRaw[0],line.len);splitOpcode(1);
                        {const int64_t delta=int64_t(state.source_region)-int64_t(r);splitVarint(4,(uint64_t(delta)<<1)^uint64_t(delta>>63));}splitVarint(4,state.source_offset);splitVarint(4,line.len);
                        state.public_id=nextMixedPublic++;++mixedOps[1];
                    } else {
                        if(state.source_region==UINT32_MAX){state.source_region=r;state.source_offset=offset;}
                        GeneratedByteArray parsed;Marker lineMarker;
                        bool arrayLine=useByteArrayLines&&parse_byte_array(text,line.len,parsed),markerLine=parse_marker(text,line.len,lineMarker);
                        size_t arrayValueCount=parsed.values.size();
                        bool sourceCopy=false,sourcePatch=false;uint32_t sourceLine=0,patchPrefix=0,patchSuffix=0,patchMiddle=0;
                        if(regionSource&&j>0){uint64_t candidate=uint64_t(regionMarker.lineno)+j-1;
                            if(candidate&&candidate<regionSource->offsets.size()){uint32_t index=uint32_t(candidate-1),begin=regionSource->offsets[index],end=regionSource->offsets[index+1];
                                if(regionSource->available){sourceLine=index;const uint8_t*base=regionSource->bytes.data()+begin;uint32_t baseLength=end-begin;
                                    if(baseLength==line.len&&!memcmp(base,text,line.len))sourceCopy=true;
                                    else {patchPrefix=common_prefix((const uint8_t*)text,line.len,base,baseLength);patchSuffix=common_suffix((const uint8_t*)text,line.len,base,baseLength,patchPrefix);patchMiddle=line.len-patchPrefix-patchSuffix;
                                        size_t cost=1+varint_size(paths.size())+varint_size(sourceLine)+varint_size(patchPrefix)+varint_size(patchSuffix)+varint_size(patchMiddle)+patchMiddle;
                                        sourcePatch=cost<line.len;}}
                            }
                        }
                        bool systemSource=regionSource&&system_source_path(regionMarker.path),sourceAdmitted=systemSource;
                        uint32_t sourcePath=UINT32_MAX;SourceAdmission*admission=nullptr;
                        if(regionSource&&(sourceCopy||sourcePatch)&&!systemSource&&useProjectSource){
                            sourcePath=ensurePathId(regionMarker.path);
                            if(mixedSourceAdmission.size()<=sourcePath)mixedSourceAdmission.resize(size_t(sourcePath)+1);
                            admission=&mixedSourceAdmission[sourcePath];
                            if(!admission->cost_known){
                                admission->package_cost=zstd_size(sourceCostZ,regionSource->bytes.data(),regionSource->bytes.size(),zlevel,sourceCostDst)
                                    +varint_size(sourcePath)+varint_size(regionSource->bytes.size())+16;
                                mixedSourceEstimatedCost+=admission->package_cost;++mixedSourceConsidered;admission->cost_known=true;
                            }
                            if(!admission->admitted&&admission->last_observed_tu!=SIZE_MAX&&admission->last_observed_tu<t&&
                               admission->observed_benefit>=uint64_t(sourceAdmitRatio)*admission->package_cost){
                                admission->admitted=true;++mixedSourceAdmitted;
                            }
                            sourceAdmitted=admission->admitted;
                        }
                        if(sourceCopy&&sourceAdmitted){
                            if(sourcePath==UINT32_MAX)sourcePath=ensurePathId(regionMarker.path);
                            ensureSourceDefinition(regionMarker.path,sourcePath,*regionSource);
                            flushLiteral();mixedRaw[0].push_back(5);put_varint(mixedRaw[0],sourcePath);put_varint(mixedRaw[0],sourceLine);splitOpcode(5);splitVarint(6,sourcePath);splitVarint(6,sourceLine);++mixedOps[5];mixedSourceBytes+=line.len;
                        } else if(arrayLine){
                            mixedArrayValues+=parsed.values.size();flushLiteral();mixedRaw[0].push_back(3);splitOpcode(3);mixedArrayEntries.push_back(std::move(parsed));++mixedOps[3];++n_literal;
                        } else if(markerLine){
                            uint32_t pathId=ensurePathId(lineMarker.path);
                            flushLiteral();mixedRaw[0].push_back(4);put_varint(mixedRaw[0],pathId);put_varint(mixedRaw[0],lineMarker.lineno);
                            put_varint(mixedRaw[0],lineMarker.flags.size());for(uint8_t flag:lineMarker.flags)mixedRaw[0].push_back(flag);splitOpcode(4);splitVarint(5,pathId);splitVarint(5,lineMarker.lineno);splitVarint(5,lineMarker.flags.size());if(splitControlCeiling)for(uint8_t flag:lineMarker.flags)splitControl[5].push_back(flag);++mixedOps[4];++n_marker;
                        } else if(sourcePatch&&sourceAdmitted){
                            if(sourcePath==UINT32_MAX)sourcePath=ensurePathId(regionMarker.path);
                            ensureSourceDefinition(regionMarker.path,sourcePath,*regionSource);
                            flushLiteral();mixedRaw[0].push_back(6);put_varint(mixedRaw[0],sourcePath);put_varint(mixedRaw[0],sourceLine);
                            put_varint(mixedRaw[0],patchPrefix);put_varint(mixedRaw[0],patchSuffix);put_varint(mixedRaw[0],patchMiddle);splitOpcode(6);splitVarint(7,sourcePath);splitVarint(7,sourceLine);splitVarint(7,patchPrefix);splitVarint(7,patchSuffix);splitVarint(7,patchMiddle);mixedRaw[1].insert(mixedRaw[1].end(),text+patchPrefix,text+patchPrefix+patchMiddle);
                            if(useAlphaLines){if(alphaPendingGap>UINT32_MAX-patchMiddle){fprintf(stderr,"alpha gap overflow\n");return 2;}alphaGapBytes.insert(alphaGapBytes.end(),text+patchPrefix,text+patchPrefix+patchMiddle);alphaPendingGap+=patchMiddle;}
                            ++mixedOps[6];mixedLiteralRaw+=patchMiddle;mixedSourceBytes+=patchPrefix+patchSuffix;++n_literal;
                        } else {mixedRaw[1].insert(mixedRaw[1].end(),text,text+line.len);literalLength+=line.len;mixedLiteralRaw+=line.len;++n_literal;
                            if(useAlphaLines){alphaGaps.push_back(alphaPendingGap);alphaPendingGap=0;alphaLines.push_back({reinterpret_cast<const uint8_t*>(text),line.len});}}
                        if(admission&&!admission->admitted){
                            uint64_t benefit=sourceCopy?(arrayLine?arrayValueCount:(markerLine?0:line.len)):
                                ((!arrayLine&&!markerLine&&sourcePatch)?uint64_t(patchPrefix)+patchSuffix:0);
                            admission->observed_benefit+=benefit;admission->last_observed_tu=t;mixedSourcePotentialRaw+=benefit;
                        }
                    }
                    offset+=line.len;
                }
                flushLiteral();
                if(splitControlCeiling)put_varint(splitControl[0],splitRegionOps);
                if(offset!=dict.region_raw_len(r)){fprintf(stderr,"mixed Region length differs\n");return 2;}
                fknownReg[r]=1;++nr;
            }
            if(useAlphaLines)alphaGaps.push_back(alphaPendingGap);
            if(!mixedArrayEntries.empty()){
                std::vector<uint8_t> blobEntry(mixedArrayEntries.size());
                if(useCompressedBlobs){
                    if(compressedBlobs.size()!=compressedBlobEntriesSeen.size()){fprintf(stderr,"compressed blob state differs\n");return 2;}
                    std::vector<uint8_t>canonicalFailed(compressedBlobs.size());
                    if(useBlobEagerPatches){
                        for(uint32_t index:generate_canonical_blobs(compressedBlobs,blobRaw,blobThreads,blobCanonicalLevel,CcanonicalBlobs))canonicalFailed[index]=1;
                        blobPatches.reserve(compressedBlobs.size());
                    }
                    for(size_t index=0;index<compressedBlobs.size();++index){const auto&blob=compressedBlobs[index];
                        if(compressedBlobEntriesSeen[index]!=blob.entry_count||blob.first_entry==UINT32_MAX){fprintf(stderr,"compressed blob entry count differs\n");return 2;}
                        if(uint64_t(blob.first_entry)+blob.entry_count>blobEntry.size()){fprintf(stderr,"blob entry extent differs\n");return 2;}
                        for(uint32_t i=0;i<blob.entry_count;++i){if(blobEntry[blob.first_entry+i]){fprintf(stderr,"overlapping blobs\n");return 2;}blobEntry[blob.first_entry+i]=1;}
                        std::vector<uint8_t>original;original.reserve(blob.deflated_size);
                        for(size_t entry=blob.first_entry;entry<size_t(blob.first_entry)+blob.entry_count;++entry)original.insert(original.end(),mixedArrayEntries[entry].values.begin(),mixedArrayEntries[entry].values.end());
                        if(original.size()!=blob.deflated_size||original.size()>UINT32_MAX||blobOriginalRaw.size()>UINT32_MAX-original.size()){fprintf(stderr,"blob original source extent differs\n");return 2;}
                        blobOriginalRaw.insert(blobOriginalRaw.end(),original.begin(),original.end());
                        if(useBlobEagerPatches){
                            if(blobPatchRaw.size()>UINT32_MAX-original.size()){fprintf(stderr,"blob patch source extent differs\n");return 2;}
                            const std::vector<uint8_t>*canonical=canonicalFailed[index]?nullptr:&CcanonicalBlobs[index];BlobPatch patch=make_blob_patch(original,canonical,blobPatchRaw);blobPatches.push_back(patch);
                            if(!patch.kind)++mixedBlobCanonicalExact;else if(patch.kind==1)++mixedBlobCorrected;else ++mixedBlobReplaced;
                        }
                        ++mixedBlobCount;mixedBlobDeflated+=blob.deflated_size;mixedBlobInflated+=blob.inflated_size;
                        if(blobDumpLengths){const uint32_t n=blob.inflated_size;const uint8_t le[4]={uint8_t(n),uint8_t(n>>8),uint8_t(n>>16),uint8_t(n>>24)};if(fwrite(le,1,sizeof le,blobDumpLengths)!=sizeof le){fprintf(stderr,"short blob length dump write\n");return 2;}}
                    }
                    mixedBlobPatchRaw+=blobPatchRaw.size();CcanonicalBlobs.clear();
                    if(blobDump&&!blobRaw.empty()&&fwrite(blobRaw.data(),1,blobRaw.size(),blobDump)!=blobRaw.size()){fprintf(stderr,"short blob dump write\n");return 2;}
                    if(!compressedBlobs.empty()){
                        double ordinaryWire=blobOriginalRaw.size()+FRAME;
                        mixedBlobOrdinaryCandidateWire+=ordinaryWire;double transformWire=-1,moWire=-1;bool moPolicy=false;
                        if(useMoFactor){
                            std::vector<mo_factor::Slice>members;members.reserve(compressedBlobs.size());size_t expectedOffset=0;
                            for(const auto&blob:compressedBlobs){
                                if(blob.inflated_offset!=expectedOffset||uint64_t(blob.inflated_offset)+blob.inflated_size>blobRaw.size()){fprintf(stderr,"non-contiguous blob factor input\n");return 2;}
                                members.push_back({blobRaw.data()+blob.inflated_offset,blob.inflated_size});expectedOffset+=blob.inflated_size;
                            }
                            if(expectedOffset!=blobRaw.size()||!Cmo.encode(members,moEncoded)){fprintf(stderr,"MO factor encode failed\n");return 2;}
                            const std::vector<uint8_t>*parts[4]={&moEncoded.control,&moEncoded.definitions,&moEncoded.translations,&moEncoded.ordinary};
                            size_t moMetadata=0;
                            for(size_t part=0;part<4;++part){if(parts[part]->size()>UINT32_MAX){fprintf(stderr,"MO factor part too large\n");return 2;}moRawSizes[part]=uint32_t(parts[part]->size());moMetadata+=varint_size(moRawSizes[part]);moFactorRaw.insert(moFactorRaw.end(),parts[part]->begin(),parts[part]->end());}
                            // A canonical catalog is only routed through the factor when removing its
                            // repeated original-string/table material shrinks the uncompressed member
                            // batch by at least 25%.  This cheap structural decision avoids compressing
                            // both candidates in the live path.  The all-candidate trace established
                            // that every qualifying Godot TU wins on the final zstd-3 wire as well.
                            moPolicy=moEncoded.mo_members&&uint64_t(moFactorRaw.size())*4<=uint64_t(blobRaw.size())*3;
                            if(moPolicy){
                                ++mixedBlobMoPolicyTus;
                                zstd_ldm_frame_encode(blobZC,moFactorRaw,blobZLevel,moFactorEncoded,blobZstdWorkers,int(blobZstdJobMiB<<20),int(blobZstdOverlapLog));
                                moWire=moFactorEncoded.size()+FRAME+moMetadata;mixedBlobMoCandidateWire+=moWire;
                                if(moWire<ordinaryWire){
                                    blobWireMode=4;blobEncoded=std::move(moFactorEncoded);
                                    if(!Cmo.commit(moEncoded)){fprintf(stderr,"MO factor C commit failed\n");return 2;}
                                    ++mixedBlobMoTus;mixedBlobMoMembers+=moEncoded.mo_members;mixedBlobMoBytes+=moEncoded.mo_bytes;mixedBlobMoDefinitions+=moEncoded.pending_definitions.size();
                                }else{blobWireMode=3;blobEncoded=std::move(blobOriginalRaw);++mixedBlobOrdinaryTus;}
                            }
                        }
                        if(!moPolicy){
                            zstd_ldm_frame_encode(blobZC,blobRaw,blobZLevel,blobEncoded,blobZstdWorkers,int(blobZstdJobMiB<<20),int(blobZstdOverlapLog));
                            if(!blobPatchRaw.empty())zstd_ldm_frame_encode(blobPatchZC,blobPatchRaw,blobZLevel,blobPatchEncoded,blobZstdWorkers,int(blobZstdJobMiB<<20),int(blobZstdOverlapLog));
                            size_t patchMetadata=useBlobEagerPatches?compressedBlobs.size():0;
                            if(useBlobEagerPatches)for(const auto&patch:blobPatches)if(patch.kind)patchMetadata+=varint_size(patch.prefix)+varint_size(patch.suffix)+varint_size(patch.data_size);
                            transformWire=blobEncoded.size()+FRAME+(blobPatchEncoded.empty()?0:blobPatchEncoded.size()+FRAME)+patchMetadata;
                            mixedBlobTransformCandidateWire+=transformWire;
                            if(transformWire<ordinaryWire){blobWireMode=useBlobEagerPatches?1:2;++mixedBlobTransformTus;}
                            else{blobWireMode=3;blobEncoded=std::move(blobOriginalRaw);blobPatchEncoded.clear();++mixedBlobOrdinaryTus;}
                        }
                        if(useMoFactor&&traceMo)fprintf(stderr,"MO TU=%zu members=%zu mo_members=%u blob_raw=%zu mo_raw=%zu policy=%u baseline=%.0f ordinary=%.0f mo=%.0f selected=%u new_originals=%zu\n",
                            t,compressedBlobs.size(),moEncoded.mo_members,blobRaw.size(),moFactorRaw.size(),unsigned(moPolicy),transformWire,ordinaryWire,moWire,unsigned(blobWireMode),moEncoded.pending_definitions.size());
                    }
                }
                std::vector<ByteArrayStyle> styles;styles.reserve(mixedArrayEntries.size());
                for(const auto&value:mixedArrayEntries)styles.push_back({value.prefix,value.separator,value.suffix,value.format});
                std::sort(styles.begin(),styles.end());styles.erase(std::unique(styles.begin(),styles.end()),styles.end());
                put_varint(mixedRaw[2],styles.size());
                for(const auto&style:styles){put_varint(mixedRaw[2],style.format);
                    for(const std::string*field:{&style.prefix,&style.separator,&style.suffix}){put_varint(mixedRaw[2],field->size());mixedRaw[2].insert(mixedRaw[2].end(),field->begin(),field->end());}}
                put_varint(mixedRaw[2],mixedArrayEntries.size());
                for(size_t entry=0;entry<mixedArrayEntries.size();++entry){const auto&value=mixedArrayEntries[entry];ByteArrayStyle style{value.prefix,value.separator,value.suffix,value.format};
                    size_t styleId=std::lower_bound(styles.begin(),styles.end(),style)-styles.begin();put_varint(mixedRaw[2],styleId);put_varint(mixedRaw[2],value.values.size());
                    if(!blobEntry[entry])mixedRaw[3].insert(mixedRaw[3].end(),value.values.begin(),value.values.end());}
                if(useCompressedBlobs&&!compressedBlobs.empty()){
                    put_varint(mixedRaw[2],blobWireMode);put_varint(mixedRaw[2],ZLIB_VERNUM);put_varint(mixedRaw[2],blobCanonicalLevel);put_varint(mixedRaw[2],compressedBlobs.size());
                    if(blobWireMode==4)for(uint32_t size:moRawSizes)put_varint(mixedRaw[2],size);
                    for(size_t index=0;index<compressedBlobs.size();++index){const auto&blob=compressedBlobs[index];put_varint(mixedRaw[2],blob.first_entry);put_varint(mixedRaw[2],blob.entry_count);put_varint(mixedRaw[2],blob.deflated_size);put_varint(mixedRaw[2],blob.inflated_size);put_u64le(mixedRaw[2],blob.digest_lo);put_u64le(mixedRaw[2],blob.digest_hi);
                        if(blobWireMode==1){const auto&patch=blobPatches[index];put_varint(mixedRaw[2],patch.kind);if(patch.kind){put_varint(mixedRaw[2],patch.prefix);put_varint(mixedRaw[2],patch.suffix);put_varint(mixedRaw[2],patch.data_size);}}
                    }
                }
            }
            if(!mixedSourceDefinitions.empty()){
                put_varint(mixedRaw[4],mixedSourceDefinitions.size());
                for(const auto&definition:mixedSourceDefinitions){
                    put_varint(mixedRaw[4],definition.first);put_varint(mixedRaw[4],definition.second->bytes.size());
                    mixedRaw[5].insert(mixedRaw[5].end(),definition.second->bytes.begin(),definition.second->bytes.end());
                    mixedSourcePackageRaw+=definition.second->bytes.size();++mixedSourcePackageFiles;
                }
            }
        }
        if(useSortedLines && nl){
            std::sort(newLineIds.begin(),newLineIds.end(),[&](uint32_t a,uint32_t b){
                const LineRef& ar=dict.ref(a); const LineRef& br=dict.ref(b); uint32_t m=std::min(ar.len,br.len);
                int c=memcmp(dict.line_data(ar.off),dict.line_data(br.off),m); return c?c<0:ar.len<br.len;
            });
            for(uint32_t ln:newLineIds) at_line(ClineToF,ln)=nextFline++;
            std::vector<uint32_t> restIds; std::vector<std::pair<uint32_t,GeneratedByteArray>> arrayEntries;
            for(uint32_t ln:newLineIds){
                if(useByteArrayLines){ GeneratedByteArray parsed; const LineRef& lr=dict.ref(ln);
                    if(parse_byte_array(dict.line_data(lr.off),lr.len,parsed)){arrayEntries.emplace_back(ln,std::move(parsed));continue;} }
                restIds.push_back(ln);
            }
            const char* previous=nullptr; uint32_t previousLength=0;
            if(!restIds.empty()) put_varint(lineRaw[0],restIds.size());
            for(uint32_t ln:restIds){
                const LineRef& lr=dict.ref(ln); const char* txt=dict.line_data(lr.off); uint32_t lcp=0, m=std::min(previousLength,lr.len);
                while(lcp<m && previous[lcp]==txt[lcp]) ++lcp;
                put_varint(lineRaw[0],lcp); put_varint(lineRaw[1],lr.len-lcp); lineRaw[2].insert(lineRaw[2].end(),txt+lcp,txt+lr.len);
                previous=txt; previousLength=lr.len;
            }
            if(!arrayEntries.empty()){
                std::vector<ByteArrayStyle> styles; styles.reserve(arrayEntries.size());
                for(const auto&entry:arrayEntries){ const auto&v=entry.second; styles.push_back({v.prefix,v.separator,v.suffix,v.format}); }
                std::sort(styles.begin(),styles.end()); styles.erase(std::unique(styles.begin(),styles.end()),styles.end());
                put_varint(lineRaw[3],styles.size());
                for(const auto&style:styles){ put_varint(lineRaw[3],style.format);
                    for(const std::string*field:{&style.prefix,&style.separator,&style.suffix}){ put_varint(lineRaw[3],field->size()); lineRaw[3].insert(lineRaw[3].end(),field->begin(),field->end()); } }
                put_varint(lineRaw[3],arrayEntries.size());
                for(const auto&entry:arrayEntries){ const auto&v=entry.second; ByteArrayStyle style{v.prefix,v.separator,v.suffix,v.format};
                    size_t id=std::lower_bound(styles.begin(),styles.end(),style)-styles.begin(); put_varint(lineRaw[3],id); put_varint(lineRaw[3],v.values.size());
                    lineRaw[4].insert(lineRaw[4].end(),v.values.begin(),v.values.end()); }
            }
            for(size_t i=0;i<linePartCount;++i) if(!lineRaw[i].empty()){
                lineEncoded[i]=zstd_stream_encode(lineZC[i],lineRaw[i],ZSTD_e_flush); lineZActive[i]=1;
                w_linedef+=lineEncoded[i].size()+FRAME;
            }
            if(useByteArrayLines) w_linedef+=1; // selector/presence mask
        }
        if(useSortedLines && endOfEntropyStream){
            const std::vector<uint8_t> empty;
            for(size_t i=0;i<linePartCount;++i) if(lineZActive[i]){
                std::vector<uint8_t> tail=zstd_stream_encode(lineZC[i],empty,ZSTD_e_end);
                if(lineEncoded[i].empty()&&!tail.empty())w_linedef+=FRAME;
                w_linedef+=tail.size(); lineEncoded[i].insert(lineEncoded[i].end(),tail.begin(),tail.end());
            }
        }
        if(useMixedRegions&&nr){
            if(useAlphaLines&&!mixedRaw[1].empty()){
                if(alphaGaps.size()!=alphaLines.size()+1){fprintf(stderr,"alpha Line/gap count differs\n");return 2;}
                uint64_t rebuilt=alphaGapBytes.size();for(const auto&line:alphaLines)rebuilt+=line.size;
                if(rebuilt!=mixedRaw[1].size()){fprintf(stderr,"alpha input extent differs\n");return 2;}

                size_t ordinarySize=zstd_frame_encode(alphaZC,mixedRaw[1],zlevel,alphaOrdinaryEncoded);
                double ordinaryWire=ordinarySize+FRAME;
                alpha_line::Encoded literalKeywords=alpha_line::encode(alphaLines,alphaGaps,alphaGapBytes,false);
                alpha_line::Encoded parameterizedKeywords=alpha_line::encode(alphaLines,alphaGaps,alphaGapBytes,true);
                if(!literalKeywords.valid||!parameterizedKeywords.valid){fprintf(stderr,"alpha encode: %s%s%s\n",literalKeywords.error.c_str(),literalKeywords.valid?"":"; ",parameterizedKeywords.error.c_str());return 2;}
                std::vector<uint8_t>literalControlEncoded,literalDataEncoded,parameterizedControlEncoded,parameterizedDataEncoded;
                auto encodeAlphaCandidate=[&](const alpha_line::Encoded&candidate,std::vector<uint8_t>&control,std::vector<uint8_t>&data){
                    double wire=zstd_frame_encode(alphaZC,candidate.control,zlevel,control)+FRAME;
                    if(!candidate.data.empty())wire+=zstd_frame_encode(alphaZC,candidate.data,zlevel,data)+FRAME;
                    return wire;
                };
                double literalWire=encodeAlphaCandidate(literalKeywords,literalControlEncoded,literalDataEncoded);
                double parameterizedWire=encodeAlphaCandidate(parameterizedKeywords,parameterizedControlEncoded,parameterizedDataEncoded);
                double bestAlphaWire=std::min(literalWire,parameterizedWire),selectedWire=ordinaryWire;
                const alpha_line::Stats*selectedStats=nullptr;
                if(literalWire<selectedWire){alphaWireMode=1;selectedWire=literalWire;alphaControlEncoded=std::move(literalControlEncoded);alphaDataEncoded=std::move(literalDataEncoded);selectedStats=&literalKeywords.stats;}
                if(parameterizedWire<selectedWire){alphaWireMode=2;selectedWire=parameterizedWire;alphaControlEncoded=std::move(parameterizedControlEncoded);alphaDataEncoded=std::move(parameterizedDataEncoded);selectedStats=&parameterizedKeywords.stats;}
                if(alphaWireMode){alphaOrdinaryEncoded.clear();++alphaSelectedTus;if(alphaWireMode==1)++alphaLiteralKeywordTus;else ++alphaParameterizedKeywordTus;
                    alphaControlWire+=alphaControlEncoded.size()+FRAME;if(!alphaDataEncoded.empty())alphaDataWire+=alphaDataEncoded.size()+FRAME;
                    alphaStats.rules+=selectedStats->rules;alphaStats.instances+=selectedStats->instances;alphaStats.unique_slots+=selectedStats->unique_slots;
                    alphaStats.slot_occurrences+=selectedStats->slot_occurrences;alphaStats.literal_fallbacks+=selectedStats->literal_fallbacks;
                    alphaStats.template_input_bytes+=selectedStats->template_input_bytes;alphaStats.lexicon_entries+=selectedStats->lexicon_entries;
                    alphaStats.lexicon_references+=selectedStats->lexicon_references;
                }else ++alphaOrdinaryTus;
                alphaSelector.push_back(alphaWireMode);alphaOrdinaryCandidateWire+=ordinaryWire;alphaLiteralKeywordCandidateWire+=literalWire;
                alphaParameterizedKeywordCandidateWire+=parameterizedWire;alphaBestCandidateWire+=bestAlphaWire;alphaSelectedWire+=selectedWire;
                alphaInputRaw+=mixedRaw[1].size();alphaEligibleLines+=alphaLines.size();alphaGapRaw+=alphaGapBytes.size();
                w_linedef+=selectedWire+1;mixedPartWire[1]+=selectedWire;alphaSelectorWire+=1;
            }
            if(residualDump&&!mixedRaw[1].empty()&&
               fwrite(mixedRaw[1].data(),1,mixedRaw[1].size(),residualDump)!=mixedRaw[1].size()){
                fprintf(stderr,"short residual dump write\n");return 2;
            }
            for(size_t i=0;i<mixedPartCount;++i) if(mixedDumps[i]&&!mixedRaw[i].empty()&&
               fwrite(mixedRaw[i].data(),1,mixedRaw[i].size(),mixedDumps[i])!=mixedRaw[i].size()){
                fprintf(stderr,"short mixed stream dump write part=%zu\n",i);return 2;
            }
            if(splitControlCeiling){
                std::array<const uint8_t*,8>cursor{},end{};
                for(size_t i=0;i<splitControl.size();++i){cursor[i]=splitControl[i].data();end[i]=cursor[i]+splitControl[i].size();}
                auto take=[&](size_t stream){if(cursor[stream]>=end[stream]){fprintf(stderr,"split control truncated stream=%zu\n",stream);exit(2);}uint64_t value=get_varint(cursor[stream]);if(cursor[stream]>end[stream]){fprintf(stderr,"split control overrun stream=%zu\n",stream);exit(2);}return value;};
                std::vector<uint8_t>rebuilt;
                const uint64_t regions=take(0);put_varint(rebuilt,regions);
                if(regions!=missReg.size()){fprintf(stderr,"split control Region count differs\n");return 2;}
                for(uint64_t region=0;region<regions;++region){
                    const uint64_t rawLength=take(0),operations=take(0);put_varint(rebuilt,rawLength);
                    for(uint64_t operation=0;operation<operations;++operation){
                        if(cursor[1]>=end[1]){fprintf(stderr,"split control opcode truncated\n");return 2;}
                        const uint8_t opcode=*cursor[1]++;rebuilt.push_back(opcode);
                        auto copyValues=[&](size_t stream,size_t count){for(size_t value=0;value<count;++value)put_varint(rebuilt,take(stream));};
                        if(opcode==0)copyValues(2,1);
                        else if(opcode==1)copyValues(4,3);
                        else if(opcode==2)copyValues(3,1);
                        else if(opcode==3){}
                        else if(opcode==4){copyValues(5,2);const uint64_t flags=take(5);put_varint(rebuilt,flags);if(flags>uint64_t(end[5]-cursor[5])){fprintf(stderr,"split control flags truncated\n");return 2;}rebuilt.insert(rebuilt.end(),cursor[5],cursor[5]+flags);cursor[5]+=flags;}
                        else if(opcode==5)copyValues(6,2);
                        else if(opcode==6)copyValues(7,5);
                        else{fprintf(stderr,"split control bad opcode=%u\n",opcode);return 2;}
                    }
                }
                for(size_t i=0;i<splitControl.size();++i)if(cursor[i]!=end[i]){fprintf(stderr,"split control trailing stream=%zu bytes=%zu\n",i,size_t(end[i]-cursor[i]));return 2;}
                if(rebuilt!=mixedRaw[0]){fprintf(stderr,"split control reconstruction differs\n");return 2;}
                for(size_t i=0;i<splitControl.size();++i)splitControlAll[i].insert(splitControlAll[i].end(),splitControl[i].begin(),splitControl[i].end());
            }
            for(size_t i=0;i<mixedPartCount;++i) if((!useAlphaLines||i!=1)&&!(useLiteralGroups&&i==1)&&!mixedRaw[i].empty()){
                mixedEncoded[i]=zstd_stream_encode(mixedZC[i],mixedRaw[i],ZSTD_e_flush);mixedZActive[i]=1;
                double bytes=mixedEncoded[i].size()+FRAME;mixedPartWire[i]+=bytes;(i==0?w_regiondef:w_linedef)+=bytes;
            }
            if(useByteArrayLines){w_linedef+=1;mixedSelectorWire+=1;}
            if(useCompressedBlobs&&!compressedBlobs.empty()){
                double bytes=blobEncoded.size()+FRAME;
                w_linedef+=bytes;mixedBlobWire+=bytes;
                if(!blobPatchEncoded.empty()){bytes=blobPatchEncoded.size()+FRAME;
                    w_linedef+=bytes;mixedBlobPatchWire+=bytes;}
            }
        }
#if defined(WITH_BSC_GROUPS)
        if(usePlannedGroups){
            if(mixedRaw[1].size()!=literalGroups.per_tu_raw[t]){fprintf(stderr,"literal group plan differs at TU=%zu planned=%u actual=%zu\n",t,literalGroups.per_tu_raw[t],mixedRaw[1].size());return 2;}
            const LiteralGroupRecord&group=literalGroups.groups[t/literalGroupTus];
            if(t==group.first_tu&&group.wire_size){w_linedef+=group.wire_size;mixedPartWire[1]+=group.wire_size;}
        }else if(literalOnDemand){
            // T_current: this TU's literal frame, built from this TU's bytes, right now.
            onDemandFrame.clear();
            if(!mixedRaw[1].empty()){
                residual_group::Kind kind=residual_group::Kind::Zstd3;
                try{ onDemandFrame=onDemandCodec.encode(mixedRaw[1].data(),mixedRaw[1].size(),
                                                        &kind,literalGroupEvaluateZstd10); }
                catch(const std::exception&error){fprintf(stderr,"on-demand literal frame at TU=%zu: %s\n",t,error.what());return 2;}
                ++onDemandSelected[size_t(kind)];
            }
            w_linedef+=double(onDemandFrame.size());mixedPartWire[1]+=double(onDemandFrame.size());
        }
#endif
        if(useMixedRegions&&mixedDumpLengths)for(size_t i=0;i<mixedPartCount;++i){
            if(mixedRaw[i].size()>UINT32_MAX){fprintf(stderr,"mixed stream dump frame too large\n");return 2;}
            const uint32_t n=uint32_t(mixedRaw[i].size());
            const uint8_t le[4]={uint8_t(n),uint8_t(n>>8),uint8_t(n>>16),uint8_t(n>>24)};
            if(fwrite(le,1,sizeof le,mixedDumpLengths)!=sizeof le){fprintf(stderr,"short mixed stream length write\n");return 2;}
        }
        if(useMixedRegions&&endOfEntropyStream){
            const std::vector<uint8_t> empty;
            for(size_t i=0;i<mixedPartCount;++i) if((!useAlphaLines||i!=1)&&!(useLiteralGroups&&i==1)&&mixedZActive[i]){
                std::vector<uint8_t> tail=zstd_stream_encode(mixedZC[i],empty,ZSTD_e_end);
                double&wire=i==0?w_regiondef:w_linedef;if(mixedEncoded[i].empty()&&!tail.empty()){wire+=FRAME;mixedPartWire[i]+=FRAME;}
                wire+=tail.size();mixedPartWire[i]+=tail.size();mixedEncoded[i].insert(mixedEncoded[i].end(),tail.begin(),tail.end());
            }
        }
        // --- C -> F FILL, emitted AFTER the end-of-stream tail has been appended so the
        // physical frame carries every byte the accounting charged for this TU.
        if(cfSink.f){
            for(size_t i=0;i<mixedPartCount;++i) if(!mixedEncoded[i].empty())
                cfSink.emit(uint8_t(WT_FILL0+i),mixedEncoded[i]);
            if(useCompressedBlobs&&!compressedBlobs.empty()){
                cfSink.emit(WT_BLOB,blobEncoded);
                if(!blobPatchEncoded.empty())cfSink.emit(WT_BLOBPATCH,blobPatchEncoded);
            }
#if defined(WITH_BSC_GROUPS)
            if(usePlannedGroups){
                const LiteralGroupRecord&group=literalGroups.groups[t/literalGroupTus];
                // A group frame may only be sent once every TU it covers has been dispatched;
                // otherwise the stream carries bytes derived from TUs that do not exist yet.
                if(t+1==group.last_tu&&group.wire_size)
                    cfSink.emit(WT_LITGROUP,literalGroups.wire.data()+group.wire_offset,group.wire_size);
            }else if(literalOnDemand&&!onDemandFrame.empty()){
                cfSink.emit(WT_LITGROUP,onDemandFrame);
            }
#endif
        }
        if(!useMixedRegions) for(uint32_t r:missReg){ const uint32_t* lids=dict.region_ids_ptr(r); uint32_t c=dict.region_ids_count(r);
            put_varint(fill_regions,c); { int64_t prev=0; for(uint32_t j=0;j<c;++j){
                uint32_t wireLine=useSortedLines?at_line(ClineToF,lids[j]):lids[j]; if(!wireLine){fprintf(stderr,"missing Line mapping\n");return 2;}
                put_zigzag(fill_regions,int64_t(wireLine)-prev); prev=int64_t(wireLine); } } fknownReg[r]=1; ++nr;
            put_varint(fill_regions_raw,c); for(uint32_t j=0;j<c;++j){ uint32_t wireLine=useSortedLines?at_line(ClineToF,lids[j]):lids[j]; put_varint(fill_regions_raw,wireLine); }
            { put_varint(allRegionsRaw,c); for(uint32_t j=0;j<c;++j){ uint32_t wireLine=useSortedLines?at_line(ClineToF,lids[j]):lids[j]; put_varint(allRegionsRaw,wireLine); } }
        }
        for(uint32_t k:missBlk){ size_t L=boff2[k+1]-boff2[k];
            if(bcopy_ok[k]){ fill_blocks.push_back(1); put_varint(fill_blocks,bcopy_src[k]); put_varint(fill_blocks,L); }   // COPY(region-stream src,len)
            else { fill_blocks.push_back(0); put_varint(fill_blocks,L); for(size_t j=boff2[k];j<boff2[k+1];++j) put_varint(fill_blocks,bchild[j]); }
            fknownBlk[k]=1; ++nb; }
        if(np){ size_t n=zstd_size(z,fill_paths.data(),fill_paths.size(),zlevel,dst); w_pathdef += n; cfSink.emit(WT_PATHDEF,dst.data(),n); allPaths.insert(allPaths.end(),fill_paths.begin(),fill_paths.end()); }
        if(nl && !useSortedLines){ size_t n=zstd_size(z,fill_lines.data(),fill_lines.size(),zlevel,dst); w_linedef += n; cfSink.emit(WT_LINEDEF,dst.data(),n); allLineDefs.insert(allLineDefs.end(),fill_lines.begin(),fill_lines.end()); }
        bool reg_raw=false;
        if(nr&&!useMixedRegions){ double dz=zstd_size(z,fill_regions.data(),fill_regions.size(),zlevel,dst);
                double rz=zstd_size(z,fill_regions_raw.data(),fill_regions_raw.size(),zlevel,dst);
                reg_raw = rz<dz; w_regiondef += (reg_raw?rz:dz) + 1;   // +1 byte serialization flag (delta vs raw line-ids)
                allRegions.insert(allRegions.end(),fill_regions.begin(),fill_regions.end()); }
        if(nb){ size_t n=zstd_size(z,fill_blocks.data(),fill_blocks.size(),zlevel,dst); w_blockdef += n; cfSink.emit(WT_BLOCKDEF,dst.data(),n); allBlocks.insert(allBlocks.end(),fill_blocks.begin(),fill_blocks.end()); }
        // The nr site above is reachable only when !useMixedRegions, which the sink refuses.
        if(np||nl||nr||nb) w_framing += FRAME;
        // --- ROOT: token stream (region + block ids), already exposed to the F missing pass above ---
        if(!useKeyMap){w_root+=zstd_size(z,rootb.data(),rootb.size(),zlevel,dst);w_framing+=FRAME;allRoots.insert(allRoots.end(),rootb.begin(),rootb.end());}

        enc_s += std::chrono::duration<double>(Clock::now()-_te).count(); auto _td=Clock::now();
        // --- DECODER (F): install FILL from wire into F's OWN store, then expand ROOT tokens ---
        { const uint8_t* pp=fill_paths.data(); for(uint32_t k=0;k<np;++k){ uint64_t L=get_varint(pp); Fpaths.emplace_back((const char*)pp,(size_t)L); pp+=L; } }
        if(useMixedRegions){
          std::array<std::vector<uint8_t>,6> recovered;
          if(useAlphaLines&&!alphaSelector.empty()){
            if(alphaSelector.size()!=1||alphaSelector[0]>2){fprintf(stderr,"bad alpha selector\n");return 2;}
            uint8_t FalphaMode=alphaSelector[0];
            if(!FalphaMode){
              if(alphaOrdinaryEncoded.empty()||!alphaControlEncoded.empty()||!alphaDataEncoded.empty()){fprintf(stderr,"bad ordinary alpha frame set\n");return 2;}
              recovered[1]=zstd_frame_decode_sized(alphaZD,alphaOrdinaryEncoded);
            }else{
              if(!alphaOrdinaryEncoded.empty()||alphaControlEncoded.empty()){fprintf(stderr,"bad template alpha frame set\n");return 2;}
              std::vector<uint8_t>control=zstd_frame_decode_sized(alphaZD,alphaControlEncoded),data;
              if(!alphaDataEncoded.empty())data=zstd_frame_decode_sized(alphaZD,alphaDataEncoded);
              std::string error;if(!alpha_line::decode(control,data,recovered[1],error)){fprintf(stderr,"alpha decode: %s\n",error.c_str());return 2;}
            }
            if(recovered[1]!=mixedRaw[1]){fprintf(stderr,"alpha residual mismatch TU=%zu\n",t);return 2;}
          }else if(useAlphaLines&&!mixedRaw[1].empty()){fprintf(stderr,"missing alpha selector\n");return 2;}
          for(size_t i=0;i<mixedPartCount;++i) if((!useAlphaLines||i!=1)&&!(useLiteralGroups&&i==1)&&!mixedEncoded[i].empty()){
            size_t remaining=1;recovered[i]=zstd_stream_decode(mixedZD[i],mixedEncoded[i],remaining);
            if(recovered[i]!=mixedRaw[i]||(endOfEntropyStream&&remaining!=0)){fprintf(stderr,"mixed Region stream mismatch TU=%zu part=%zu\n",t,i);return 2;}
          }
          if(!recovered[4].empty()){
            const uint8_t*sp=recovered[4].data(),*se=sp+recovered[4].size(),*bp=recovered[5].data(),*be=bp+recovered[5].size();
            uint64_t sourceCount=get_varint(sp);
            for(uint64_t k=0;k<sourceCount;++k){
              uint64_t pathId=get_varint(sp),length=get_varint(sp);
              if(pathId>=Fpaths.size()||length>uint64_t(be-bp)||!mixedFSource.install(Fpaths[pathId],bp,length)){fprintf(stderr,"bad mixed source definition\n");return 2;}
              bp+=length;
            }
            if(sp!=se||bp!=be){fprintf(stderr,"mixed source definitions have trailing bytes\n");return 2;}
          } else if(!recovered[5].empty()){fprintf(stderr,"partial mixed source definition streams\n");return 2;}
          const uint8_t*ap=recovered[2].data(),*ae=ap+recovered[2].size();
          std::vector<ByteArrayStyle> mixedStyles;std::vector<ArrayWireRecord>arrayRecords;std::vector<CompressedBlob>Fblobs;std::vector<BlobPatch>FblobPatches;std::array<uint32_t,4>FmoRawSizes{};uint64_t arrayCount=0,arraysUsed=0;bool FblobEager=false,FblobOrdinary=false,FblobMo=false;int FblobCanonicalLevel=9;size_t expectedPatchRaw=0;
          if(!recovered[2].empty()){
            uint64_t styleCount=get_varint(ap);mixedStyles.reserve(styleCount);
            for(uint64_t k=0;k<styleCount;++k){ByteArrayStyle style;uint64_t format=get_varint(ap);if(format>HEX_UU){fprintf(stderr,"bad mixed array format\n");return 2;}style.format=uint8_t(format);
              for(std::string*field:{&style.prefix,&style.separator,&style.suffix}){uint64_t size=get_varint(ap);if(size>uint64_t(ae-ap)){fprintf(stderr,"bad mixed array style\n");return 2;}field->assign((const char*)ap,size);ap+=size;}mixedStyles.push_back(std::move(style));}
            arrayCount=get_varint(ap);
            if(arrayCount>UINT32_MAX){fprintf(stderr,"too many mixed arrays\n");return 2;}arrayRecords.reserve(arrayCount);
            for(uint64_t k=0;k<arrayCount;++k){uint64_t style=get_varint(ap),count=get_varint(ap);if(style>=mixedStyles.size()||count>UINT32_MAX){fprintf(stderr,"bad mixed array record\n");return 2;}arrayRecords.push_back({uint32_t(style),uint32_t(count)});}
            if(useCompressedBlobs&&!blobEncoded.empty()){uint64_t patchMode=get_varint(ap),canonicalVersion=get_varint(ap),canonicalLevel=get_varint(ap),blobCount=get_varint(ap);if(patchMode<1||patchMode>4||canonicalVersion!=ZLIB_VERNUM||canonicalLevel<1||canonicalLevel>9||blobCount>UINT32_MAX){fprintf(stderr,"bad compressed blob header\n");return 2;}FblobEager=patchMode==1;FblobOrdinary=patchMode==3;FblobMo=patchMode==4;FblobCanonicalLevel=int(canonicalLevel);Fblobs.reserve(blobCount);if(FblobEager)FblobPatches.reserve(blobCount);if(FblobMo)for(uint32_t&size:FmoRawSizes){uint64_t value=get_varint(ap);if(value>UINT32_MAX){fprintf(stderr,"bad MO factor part size\n");return 2;}size=uint32_t(value);}uint64_t expandedOffset=0,priorEnd=0;
              for(uint64_t k=0;k<blobCount;++k){CompressedBlob blob;uint64_t first=get_varint(ap),count=get_varint(ap),deflated=get_varint(ap),inflated=get_varint(ap);
                if(first>UINT32_MAX||count>UINT32_MAX||deflated>UINT32_MAX||inflated>UINT32_MAX||first<priorEnd||first+count>arrayCount){fprintf(stderr,"bad compressed blob descriptor\n");return 2;}
                blob.first_entry=uint32_t(first);blob.entry_count=uint32_t(count);blob.deflated_size=uint32_t(deflated);blob.inflated_offset=uint32_t(expandedOffset);blob.inflated_size=uint32_t(inflated);
                blob.digest_lo=get_u64le(ap,ae);blob.digest_hi=get_u64le(ap,ae);expandedOffset+=inflated;if(expandedOffset>UINT32_MAX){fprintf(stderr,"compressed blob payload too large\n");return 2;}priorEnd=first+count;Fblobs.push_back(blob);
                if(FblobEager){BlobPatch patch;uint64_t kind=get_varint(ap);if(kind>2){fprintf(stderr,"bad blob patch kind\n");return 2;}patch.kind=uint8_t(kind);
                  if(patch.kind){uint64_t prefix=get_varint(ap),suffix=get_varint(ap),size=get_varint(ap);if(prefix>UINT32_MAX||suffix>UINT32_MAX||size>UINT32_MAX||expectedPatchRaw+size>UINT32_MAX){fprintf(stderr,"bad blob patch extent\n");return 2;}patch.prefix=uint32_t(prefix);patch.suffix=uint32_t(suffix);patch.data_offset=uint32_t(expectedPatchRaw);patch.data_size=uint32_t(size);expectedPatchRaw+=size;}FblobPatches.push_back(patch);}
              }
            }
            if(ap!=ae){fprintf(stderr,"mixed array control has trailing bytes\n");return 2;}
          } else if(!recovered[3].empty()){fprintf(stderr,"partial mixed array streams\n");return 2;}
          size_t expectedBlobRaw=0;for(const auto&blob:Fblobs)expectedBlobRaw+=FblobOrdinary?blob.deflated_size:blob.inflated_size;
          std::vector<uint8_t>FblobRaw;
          if(!Fblobs.empty()){
            if(blobEncoded.empty()){fprintf(stderr,"missing compressed blob frame\n");return 2;}
            if(FblobOrdinary){
              if(blobEncoded.size()!=expectedBlobRaw){fprintf(stderr,"ordinary blob payload size differs\n");return 2;}
              FblobRaw=blobEncoded;
            } else if(FblobMo){
              size_t packedSize=0;for(uint32_t size:FmoRawSizes){if(size>SIZE_MAX-packedSize){fprintf(stderr,"MO factor packed size overflow\n");return 2;}packedSize+=size;}
              std::vector<uint8_t>packed=zstd_frame_decode_exact(blobZD,blobEncoded,packedSize);std::array<std::vector<uint8_t>,4>parts;size_t offset=0;
              for(size_t part=0;part<4;++part){parts[part].assign(packed.begin()+offset,packed.begin()+offset+FmoRawSizes[part]);offset+=FmoRawSizes[part];}
              std::vector<uint32_t>lengths;if(!Fmo.decode(parts[0],parts[1],parts[2],parts[3],FblobRaw,lengths)||lengths.size()!=Fblobs.size()){fprintf(stderr,"MO factor decode failed\n");return 2;}
              for(size_t index=0;index<Fblobs.size();++index)if(lengths[index]!=Fblobs[index].inflated_size){fprintf(stderr,"MO factor member length differs\n");return 2;}
              if(FblobRaw.size()!=expectedBlobRaw||FblobRaw!=blobRaw||Fmo.size()!=Cmo.size()){fprintf(stderr,"MO factor payload/state differs\n");return 2;}
            } else {
              FblobRaw=zstd_frame_decode_exact(blobZD,blobEncoded,expectedBlobRaw);
              if(FblobRaw!=blobRaw){fprintf(stderr,"compressed blob payload differs\n");return 2;}
            }
          }
          else if(!blobEncoded.empty()){fprintf(stderr,"unexpected compressed blob frame\n");return 2;}
          std::vector<uint8_t>FblobPatchRaw;
          if(expectedPatchRaw){if(blobPatchEncoded.empty()){fprintf(stderr,"missing blob patch frame\n");return 2;}FblobPatchRaw=zstd_frame_decode_exact(blobPatchZD,blobPatchEncoded,expectedPatchRaw);if(FblobPatchRaw!=blobPatchRaw){fprintf(stderr,"blob patch payload differs\n");return 2;}}
          else if(!blobPatchEncoded.empty()){fprintf(stderr,"unexpected blob patch frame\n");return 2;}
          std::vector<std::vector<uint8_t>>canonicalBlobs,regeneratedBlobs(Fblobs.size());std::vector<uint8_t>canonicalFailed(Fblobs.size());
          if(!FblobOrdinary)for(uint32_t index:generate_canonical_blobs(Fblobs,FblobRaw,blobThreads,FblobCanonicalLevel,canonicalBlobs))canonicalFailed[index]=1;
          std::vector<uint32_t>blobFallbacks;
          size_t ordinaryOffset=0;
          for(size_t index=0;index<Fblobs.size();++index){bool ok=false;
            if(FblobOrdinary){const auto&blob=Fblobs[index];if(ordinaryOffset+blob.deflated_size<=FblobRaw.size()){regeneratedBlobs[index].assign(FblobRaw.begin()+ordinaryOffset,FblobRaw.begin()+ordinaryOffset+blob.deflated_size);ordinaryOffset+=blob.deflated_size;ok=true;}}
            else if(FblobEager){const auto&patch=FblobPatches[index];if(!canonicalFailed[index]||patch.kind==2)ok=apply_blob_patch(patch,canonicalBlobs[index],FblobPatchRaw,regeneratedBlobs[index]);}
            else if(!canonicalFailed[index]){regeneratedBlobs[index]=std::move(canonicalBlobs[index]);ok=true;}
            if(ok){const auto&blob=Fblobs[index];auto digest=blob_digest(regeneratedBlobs[index].data(),regeneratedBlobs[index].size());ok=regeneratedBlobs[index].size()==blob.deflated_size&&digest.first==blob.digest_lo&&digest.second==blob.digest_hi;}
            if(!ok||(blobFallbackEvery&&(index+1)%blobFallbackEvery==0))blobFallbacks.push_back(uint32_t(index));
          }
          if(FblobOrdinary&&ordinaryOffset!=FblobRaw.size()){fprintf(stderr,"ordinary blob payload has trailing bytes\n");return 2;}
          if(!blobFallbacks.empty()){
            // F names only the blob ordinals that did not reproduce. C replies with their original
            // deflate bytes in the same order, so the initial descriptor lengths delimit the reply.
            std::vector<uint8_t>fallbackRequest;put_varint(fallbackRequest,blobFallbacks.size());uint32_t prior=0;
            for(size_t i=0;i<blobFallbacks.size();++i){uint32_t current=blobFallbacks[i];put_varint(fallbackRequest,i?current-prior:current);prior=current;}
            double requestWire=fallbackRequest.size()+FRAME;w_missing+=requestWire;mixedBlobFallbackRequestWire+=requestWire;
            fcSink.emit(WT_FBREQ,fallbackRequest);   // F -> C
            const uint8_t*request=fallbackRequest.data(),*requestEnd=request+fallbackRequest.size();uint64_t requested=get_varint(request);
            std::vector<uint32_t>requestedByC;requestedByC.reserve(requested);uint64_t requestedPrior=0;
            for(uint64_t i=0;i<requested;++i){uint64_t delta=get_varint(request);uint64_t current=i?requestedPrior+delta:delta;
              if(current>=compressedBlobs.size()||(i&&current<=requestedPrior)){fprintf(stderr,"bad blob fallback request\n");return 2;}requestedByC.push_back(uint32_t(current));requestedPrior=current;}
            if(request!=requestEnd||requestedByC!=blobFallbacks||compressedBlobs.size()!=Fblobs.size()){fprintf(stderr,"blob fallback request differs\n");return 2;}
            std::vector<uint8_t>fallbackReplyRaw;
            for(uint32_t index:requestedByC){const auto&blob=compressedBlobs[index];size_t begin=fallbackReplyRaw.size();
              for(size_t entry=blob.first_entry;entry<size_t(blob.first_entry)+blob.entry_count;++entry)fallbackReplyRaw.insert(fallbackReplyRaw.end(),mixedArrayEntries[entry].values.begin(),mixedArrayEntries[entry].values.end());
              if(fallbackReplyRaw.size()-begin!=blob.deflated_size){fprintf(stderr,"blob fallback source extent differs\n");return 2;}
            }
            auto fallbackCStart=Clock::now();size_t fallbackEncodedSize=zstd_size(z,fallbackReplyRaw.data(),fallbackReplyRaw.size(),zlevel,messageEncoded);
            fallback_c_s+=std::chrono::duration<double>(Clock::now()-fallbackCStart).count();
            double replyWire=fallbackEncodedSize+FRAME;w_missing+=replyWire;mixedBlobFallbackReplyWire+=replyWire;mixedBlobFallbacks+=blobFallbacks.size();
            cfSink.emit(WT_FBREPLY,messageEncoded.data(),fallbackEncodedSize);   // C -> F
            messageDecoded.resize(fallbackReplyRaw.size());size_t fallbackDecodedSize=ZSTD_decompressDCtx(messageD,messageDecoded.data(),messageDecoded.size(),messageEncoded.data(),fallbackEncodedSize);
            if(ZSTD_isError(fallbackDecodedSize)||fallbackDecodedSize!=fallbackReplyRaw.size()||messageDecoded!=fallbackReplyRaw){fprintf(stderr,"blob fallback reply differs\n");return 2;}
            const uint8_t*fallback=messageDecoded.data(),*fallbackEnd=fallback+messageDecoded.size();
            for(uint32_t index:blobFallbacks){const auto&blob=Fblobs[index];if(blob.deflated_size>size_t(fallbackEnd-fallback)){fprintf(stderr,"truncated blob fallback reply\n");return 2;}
              auto digest=blob_digest(fallback,blob.deflated_size);if(digest.first!=blob.digest_lo||digest.second!=blob.digest_hi){fprintf(stderr,"blob fallback digest differs\n");return 2;}
              regeneratedBlobs[index].assign(fallback,fallback+blob.deflated_size);fallback+=blob.deflated_size;}
            if(fallback!=fallbackEnd){fprintf(stderr,"blob fallback reply has trailing bytes\n");return 2;}
          }
          std::vector<uint8_t>mixedArrayValueBytes;size_t totalArrayValues=0;for(const auto&record:arrayRecords)totalArrayValues+=record.count;mixedArrayValueBytes.reserve(totalArrayValues);
          const uint8_t*residual=recovered[3].data(),*residualEnd=residual+recovered[3].size();size_t blobIndex=0;
          for(size_t entry=0;entry<arrayRecords.size();){
            if(blobIndex<Fblobs.size()&&Fblobs[blobIndex].first_entry==entry){const auto&blob=Fblobs[blobIndex];size_t values=0;for(size_t j=0;j<blob.entry_count;++j)values+=arrayRecords[entry+j].count;
              if(values!=blob.deflated_size||regeneratedBlobs[blobIndex].size()!=values){fprintf(stderr,"regenerated blob extent differs\n");return 2;}mixedArrayValueBytes.insert(mixedArrayValueBytes.end(),regeneratedBlobs[blobIndex].begin(),regeneratedBlobs[blobIndex].end());entry+=blob.entry_count;++blobIndex;
            }else{size_t count=arrayRecords[entry].count;if(count>size_t(residualEnd-residual)){fprintf(stderr,"truncated mixed array residual\n");return 2;}mixedArrayValueBytes.insert(mixedArrayValueBytes.end(),residual,residual+count);residual+=count;++entry;}
          }
          if(blobIndex!=Fblobs.size()||residual!=residualEnd||mixedArrayValueBytes.size()!=totalArrayValues){fprintf(stderr,"mixed array value assembly differs\n");return 2;}
          uint8_t emptyArrayValue=0;const uint8_t*vp=mixedArrayValueBytes.empty()?&emptyArrayValue:mixedArrayValueBytes.data(),*ve=vp+mixedArrayValueBytes.size();
          const uint8_t*groupedLiteralBegin=nullptr,*groupedLiteralEnd=nullptr;
#if defined(WITH_BSC_GROUPS)
          uint8_t groupedLiteralEmpty=0;
          if(usePlannedGroups){
            const LiteralGroupRecord&group=literalGroups.groups[t/literalGroupTus];
            const uint8_t*base=literalGroups.decoded.empty()?&groupedLiteralEmpty:literalGroups.decoded.data();
            if(literalGroupDecodedCursor<group.decoded_offset||literalGroupDecodedCursor>group.decoded_offset+group.decoded_size){fprintf(stderr,"literal group cursor outside group TU=%zu\n",t);return 2;}
            groupedLiteralBegin=base+literalGroupDecodedCursor;groupedLiteralEnd=base+group.decoded_offset+group.decoded_size;
          }else if(literalOnDemand){
            // F decodes the frame it was just sent -- nothing carried over, nothing planned.
            onDemandRaw.clear();
            if(!onDemandFrame.empty()){
              try{ residual_group::DecodedFrame decoded=onDemandCodec.decode(onDemandFrame.data(),onDemandFrame.size());
                   if(decoded.wire_bytes!=onDemandFrame.size()){fprintf(stderr,"on-demand literal frame length differs TU=%zu\n",t);return 2;}
                   onDemandRaw=std::move(decoded.raw); }
              catch(const std::exception&error){fprintf(stderr,"on-demand literal decode at TU=%zu: %s\n",t,error.what());return 2;}
            }
            const uint8_t*base=onDemandRaw.empty()?&groupedLiteralEmpty:onDemandRaw.data();
            groupedLiteralBegin=base;groupedLiteralEnd=base+onDemandRaw.size();
          }
#endif
          if(!recovered[0].empty()){
            const uint8_t*cp=recovered[0].data(),*ce=cp+recovered[0].size();const uint8_t*lp=useLiteralGroups?groupedLiteralBegin:recovered[1].data(),*le=useLiteralGroups?groupedLiteralEnd:lp+recovered[1].size();
            uint64_t regionCount=get_varint(cp);if(regionCount!=nr||regionCount!=missReg.size()){fprintf(stderr,"mixed Region count differs\n");return 2;}
            for(uint64_t k=0;k<regionCount;++k){
              uint32_t regionId=missReg[k];if(regionId>=FmixedRegions.size()||FmixedRegions[regionId].known){fprintf(stderr,"mixed Region identity differs\n");return 2;}
              uint64_t rawLength=get_varint(cp);size_t begin=FmixedRegionData.size();
              while(FmixedRegionData.size()-begin<rawLength){
                if(cp>=ce){fprintf(stderr,"truncated mixed Region control\n");return 2;}uint8_t op=*cp++;
                if(op==0){uint64_t length=get_varint(cp);if(length>uint64_t(le-lp)){fprintf(stderr,"truncated mixed literal\n");return 2;}FmixedRegionData.insert(FmixedRegionData.end(),lp,lp+length);lp+=length;}
                else if(op==1){int64_t source=int64_t(regionId)+get_zigzag(cp);uint64_t offset=get_varint(cp),length=get_varint(cp);
                  if(source<0||uint64_t(source)>=FmixedRegions.size()||!FmixedRegions[source].known||offset+length>FmixedRegions[source].length){fprintf(stderr,"bad mixed publish view\n");return 2;}
                  size_t sourceBegin=FmixedRegions[source].offset+offset,destination=FmixedRegionData.size();FmixedRegionData.resize(destination+length);memcpy(FmixedRegionData.data()+destination,FmixedRegionData.data()+sourceBegin,length);
                  FmixedPublic.push_back({uint32_t(source),uint32_t(offset),uint32_t(length)});
                } else if(op==2){uint64_t publicId=get_varint(cp);if(!publicId||publicId>=FmixedPublic.size()){fprintf(stderr,"bad mixed public ref\n");return 2;}
                  const MixedFLineView&view=FmixedPublic[publicId];if(!FmixedRegions[view.source_region].known){fprintf(stderr,"bad mixed public source\n");return 2;}
                  size_t sourceBegin=FmixedRegions[view.source_region].offset+view.source_offset,destination=FmixedRegionData.size();
                  FmixedRegionData.resize(destination+view.length);memcpy(FmixedRegionData.data()+destination,FmixedRegionData.data()+sourceBegin,view.length);
                } else if(op==3){if(arraysUsed>=arrayCount){fprintf(stderr,"missing mixed array record\n");return 2;}const auto&record=arrayRecords[arraysUsed];
                  if(record.style>=mixedStyles.size()||record.count>uint64_t(ve-vp)){fprintf(stderr,"bad mixed array record\n");return 2;}
                  append_rendered_byte_array(mixedStyles[record.style],vp,record.count,FmixedRegionData);vp+=record.count;++arraysUsed;
                } else if(op==4){uint64_t pathId=get_varint(cp),lineNumber=get_varint(cp),flagCount=get_varint(cp);
                  if(pathId>=Fpaths.size()||flagCount>uint64_t(ce-cp)){fprintf(stderr,"bad mixed marker record\n");return 2;}Marker marker;marker.path=Fpaths[pathId];marker.lineno=lineNumber;
                  marker.flags.assign(cp,cp+flagCount);cp+=flagCount;tmp.clear();emit_marker(marker,tmp);FmixedRegionData.insert(FmixedRegionData.end(),tmp.begin(),tmp.end());
                } else if(op==5){uint64_t pathId=get_varint(cp),lineIndex=get_varint(cp);if(pathId>=Fpaths.size()){fprintf(stderr,"bad mixed source path\n");return 2;}
                  const SourceText&source=mixedFSource.get(Fpaths[pathId]);if(!source.available||lineIndex+1>=source.offsets.size()){fprintf(stderr,"bad mixed source Line\n");return 2;}
                  uint32_t begin=source.offsets[lineIndex],end=source.offsets[lineIndex+1];FmixedRegionData.insert(FmixedRegionData.end(),source.bytes.begin()+begin,source.bytes.begin()+end);
                } else if(op==6){uint64_t pathId=get_varint(cp),lineIndex=get_varint(cp),prefix=get_varint(cp),suffix=get_varint(cp),middle=get_varint(cp);
                  if(pathId>=Fpaths.size()||middle>uint64_t(le-lp)){fprintf(stderr,"bad mixed source patch\n");return 2;}const SourceText&source=mixedFSource.get(Fpaths[pathId]);
                  if(!source.available||lineIndex+1>=source.offsets.size()){fprintf(stderr,"bad mixed source patch Line\n");return 2;}uint32_t begin=source.offsets[lineIndex],end=source.offsets[lineIndex+1];
                  if(prefix+suffix>end-begin){fprintf(stderr,"bad mixed source patch extent\n");return 2;}FmixedRegionData.insert(FmixedRegionData.end(),source.bytes.begin()+begin,source.bytes.begin()+begin+prefix);
                  FmixedRegionData.insert(FmixedRegionData.end(),lp,lp+middle);lp+=middle;FmixedRegionData.insert(FmixedRegionData.end(),source.bytes.begin()+end-suffix,source.bytes.begin()+end);
                } else {fprintf(stderr,"bad mixed Region opcode\n");return 2;}
                if(FmixedRegionData.size()-begin>rawLength){fprintf(stderr,"mixed Region overrun\n");return 2;}
              }
              if(rawLength!=dict.region_raw_len(regionId)||memcmp(FmixedRegionData.data()+begin,dict.region_data(regionId),rawLength)){fprintf(stderr,"mixed Region differs\n");return 2;}
              FmixedRegions[regionId]={begin,uint32_t(rawLength),true};
            }
            if(useLiteralGroups){
              const size_t consumed=size_t(lp-groupedLiteralBegin);
              if(consumed!=mixedRaw[1].size()||(consumed&&memcmp(groupedLiteralBegin,mixedRaw[1].data(),consumed))){fprintf(stderr,"grouped literal consumption differs TU=%zu planned=%zu consumed=%zu\n",t,mixedRaw[1].size(),consumed);return 2;}
#if defined(WITH_BSC_GROUPS)
              if(usePlannedGroups){
                literalGroupDecodedCursor+=consumed;const LiteralGroupRecord&group=literalGroups.groups[t/literalGroupTus];
                if(t+1==group.last_tu&&literalGroupDecodedCursor!=group.decoded_offset+group.decoded_size){fprintf(stderr,"literal group has trailing decoded bytes after TU=%zu\n",t);return 2;}
              }else if(consumed!=onDemandRaw.size()){fprintf(stderr,"on-demand literal frame has trailing decoded bytes after TU=%zu\n",t);return 2;}
#endif
            }
            if(cp!=ce||(!useLiteralGroups&&lp!=le)||vp!=ve||arraysUsed!=arrayCount||FmixedPublic.size()!=nextMixedPublic){fprintf(stderr,"mixed Region streams have trailing bytes or state differs\n");return 2;}
          } else if(nr||!mixedRaw[1].empty()||(!useLiteralGroups&&!recovered[1].empty())){fprintf(stderr,"partial mixed Region streams\n");return 2;}
        } else if(useSortedLines){
          std::array<std::vector<uint8_t>,5> recovered;
          for(size_t i=0;i<linePartCount;++i) if(!lineEncoded[i].empty()){
            size_t remaining=1; recovered[i]=zstd_stream_decode(lineZD[i],lineEncoded[i],remaining);
            if(recovered[i]!=lineRaw[i] || (endOfEntropyStream&&remaining!=0)){fprintf(stderr,"sorted Line stream mismatch TU=%zu part=%zu raw=%zu recovered=%zu encoded=%zu remaining=%zu\n",t,i,lineRaw[i].size(),recovered[i].size(),lineEncoded[i].size(),remaining);return 2;}
          }
          PackedLines restLines,arrayLines;
          if(!recovered[0].empty()){
            const uint8_t* lp=recovered[0].data(), *le=lp+recovered[0].size();
            const uint8_t* nptr=recovered[1].data(), *ne=nptr+recovered[1].size();
            const uint8_t* sp=recovered[2].data(), *se=sp+recovered[2].size();
            uint64_t count=get_varint(lp);std::vector<uint8_t> previousLine,currentLine;restLines.offsets.reserve(count+1);
            for(uint64_t k=0;k<count;++k){
              if(lp>=le||nptr>=ne){fprintf(stderr,"truncated sorted Line control\n");return 2;}
              uint64_t lcp=get_varint(lp),suffix=get_varint(nptr);
              if(lcp>previousLine.size()||suffix>uint64_t(se-sp)){fprintf(stderr,"bad sorted Line extent\n");return 2;}
              currentLine.assign(previousLine.begin(),previousLine.begin()+lcp);currentLine.insert(currentLine.end(),sp,sp+suffix);sp+=suffix;
              restLines.append(currentLine.data(),currentLine.size());previousLine.swap(currentLine);
            }
            if(lp!=le||nptr!=ne||sp!=se){fprintf(stderr,"sorted Line streams have trailing bytes\n");return 2;}
          } else if(!recovered[1].empty()||!recovered[2].empty()){fprintf(stderr,"partial sorted Line streams\n");return 2;}
          if(!recovered[3].empty()){
            const uint8_t* cp=recovered[3].data(),*ce=cp+recovered[3].size();const uint8_t* vp=recovered[4].data(),*ve=vp+recovered[4].size();
            uint64_t styleCount=get_varint(cp);std::vector<ByteArrayStyle> styles;styles.reserve(styleCount);
            for(uint64_t k=0;k<styleCount;++k){ ByteArrayStyle style;uint64_t format=get_varint(cp);if(format>HEX_UU){fprintf(stderr,"bad byte-array format\n");return 2;}style.format=uint8_t(format);
              for(std::string*field:{&style.prefix,&style.separator,&style.suffix}){uint64_t size=get_varint(cp);if(size>uint64_t(ce-cp)){fprintf(stderr,"bad byte-array style\n");return 2;}field->assign((const char*)cp,size);cp+=size;}styles.push_back(std::move(style)); }
            uint64_t lineCount=get_varint(cp);arrayLines.offsets.reserve(lineCount+1);arrayLines.bytes.reserve(recovered[4].size()*5);
            for(uint64_t k=0;k<lineCount;++k){uint64_t styleId=get_varint(cp),count=get_varint(cp);if(styleId>=styles.size()||count>uint64_t(ve-vp)){fprintf(stderr,"bad byte-array record\n");return 2;}
              const auto&style=styles[styleId];append_rendered_byte_array(style,vp,count,arrayLines.bytes);vp+=count;arrayLines.offsets.push_back(arrayLines.bytes.size());}
            if(cp!=ce||vp!=ve){fprintf(stderr,"byte-array streams have trailing bytes\n");return 2;}
          } else if(!recovered[4].empty()){fprintf(stderr,"partial byte-array streams\n");return 2;}
          size_t ri=0,ai=0,decoded=0;
          while(ri<restLines.size()||ai<arrayLines.size()){
            bool takeArray=ri==restLines.size() || (ai<arrayLines.size()&&packed_line_less(arrayLines,ai,restLines,ri));
            const PackedLines&source=takeArray?arrayLines:restLines;size_t index=takeArray?ai++:ri++;
            const uint8_t*line=source.line_data(index);size_t length=source.line_size(index);
            if(decoded>=newLineIds.size()){fprintf(stderr,"decoded too many Lines\n");return 2;}
            uint32_t ln=newLineIds[decoded++];const LineRef&truth=dict.ref(ln);
            if(length!=truth.len||memcmp(line,dict.line_data(truth.off),truth.len)){fprintf(stderr,"sorted Line differs\n");return 2;}
            Fline_data.insert(Fline_data.end(),line,line+length);Fline_off.push_back(Fline_data.size());
          }
          if(decoded!=newLineIds.size()){fprintf(stderr,"decoded Line count differs\n");return 2;}
        } else { const uint8_t* pp=fill_lines.data(), *pe=fill_lines.data()+fill_lines.size();
          while(pp<pe){ uint8_t kind=*pp++;
            if(kind==1){ uint64_t pid=get_varint(pp); uint64_t lineno=get_varint(pp); uint8_t nf=*pp++; Marker dm; dm.path=Fpaths[pid]; dm.lineno=lineno; for(uint8_t f=0;f<nf;++f) dm.flags.push_back(*pp++);
                tmp.clear(); emit_marker(dm,tmp); Fline_data.insert(Fline_data.end(),tmp.begin(),tmp.end()); Fline_off.push_back(Fline_data.size()); }
            else {
              if(useD2mine){ uint64_t sl=get_varint(pp); std::vector<uint8_t> lo; relF.decode(pp,sl,lo); pp+=sl; Fline_data.insert(Fline_data.end(),lo.begin(),lo.end()); Fline_off.push_back(Fline_data.size()); }
#ifdef HAVE_DEFCODEC
              else if(useD2){ uint64_t len=get_varint(pp); uint64_t sl=get_varint(pp); std::vector<uint8_t> lo; decF.decode(pp,sl,lo); pp+=sl; (void)len; Fline_data.insert(Fline_data.end(),lo.begin(),lo.end()); Fline_off.push_back(Fline_data.size()); }
#endif
              else { uint64_t len=get_varint(pp); Fline_data.insert(Fline_data.end(),pp,pp+len); pp+=len; Fline_off.push_back(Fline_data.size()); }
            } } }
        if(!useMixedRegions){
          if(reg_raw){ const uint8_t* pp=fill_regions_raw.data(), *pe=fill_regions_raw.data()+fill_regions_raw.size();
            while(pp<pe){ uint64_t c=get_varint(pp); for(uint64_t j=0;j<c;++j) Freg_child.push_back(uint32_t(get_varint(pp))); Freg_off.push_back(Freg_child.size()); } }
          else { const uint8_t* pp=fill_regions.data(), *pe=fill_regions.data()+fill_regions.size();
            while(pp<pe){ uint64_t c=get_varint(pp); int64_t prev=0; for(uint64_t j=0;j<c;++j){ prev+=get_zigzag(pp); Freg_child.push_back(uint32_t(prev)); } Freg_off.push_back(Freg_child.size()); } }
        }
        { const uint8_t* pp=fill_blocks.data(), *pe=fill_blocks.data()+fill_blocks.size();size_t decodedBlocks=0;
          while(pp<pe){if(decodedBlocks>=missBlk.size()){fprintf(stderr,"decoded too many Blocks\n");return 2;}uint32_t blockId=missBlk[decodedBlocks++];
            if(Fblocks.known(blockId)){fprintf(stderr,"duplicate Block definition\n");return 2;}uint8_t kind=*pp++;
            if(kind==1){ uint64_t src=get_varint(pp); uint64_t L=get_varint(pp);
              if(!Fblocks.install_copy(blockId,Freg_stream,src,L)){fprintf(stderr,"bad Block copy\n");return 2;} }
            else { uint64_t L=get_varint(pp); std::vector<uint32_t>kids;kids.reserve(size_t(L<4096?L:4096)); for(uint64_t j=0;j<L;++j) kids.push_back(uint32_t(get_varint(pp)));
              if(!Fblocks.install_children(blockId,kids.data(),kids.size())){fprintf(stderr,"duplicate Block definition\n");return 2;} }
          }
          if(decodedBlocks!=missBlk.size()){fprintf(stderr,"decoded too few Blocks\n");return 2;}
        }
        recon.clear();
        auto emitRegionF=[&](uint32_t r){ Freg_stream.push_back(r);
          if(useMixedRegions){if(r>=FmixedRegions.size()||!FmixedRegions[r].known){fprintf(stderr,"unknown mixed Root Region\n");exit(2);}const MixedFRegionView&view=FmixedRegions[r];recon.insert(recon.end(),FmixedRegionData.begin()+view.offset,FmixedRegionData.begin()+view.offset+view.length);}
          else for(size_t j=Freg_off[r];j<Freg_off[r+1];++j){ uint32_t ln=Freg_child[j]; recon.insert(recon.end(), Fline_data.begin()+Fline_off[ln-1], Fline_data.begin()+Fline_off[ln]); } };
        { const std::vector<uint8_t>&decodedRoot=useKeyMap?Frootb:rootb;const uint8_t* pp=decodedRoot.data(), *pe=decodedRoot.data()+decodedRoot.size();
          if(usePriorRoot){
            uint64_t expected=get_varint(pp), produced=0;
            while(pp<pe && produced<expected){ uint8_t op=*pp++;
              if(op==0){ emitRegionF(uint32_t(get_varint(pp))); ++produced; }
              else if(op==1){ uint64_t source=get_varint(pp), start=get_varint(pp), count=get_varint(pp);
                if(source+1>=Froot_off.size() || start+count>Froot_off[source+1]-Froot_off[source]){ fprintf(stderr,"bad ROOT_SLICE\n"); return 2; }
                for(uint64_t j=0;j<count;++j) emitRegionF(Froot_child[Froot_off[source]+start+j]);
                produced+=count;
              } else { fprintf(stderr,"bad ROOT_SLICE opcode\n"); return 2; }
            }
            if(pp!=pe || produced!=expected){ fprintf(stderr,"bad ROOT_SLICE extent\n"); return 2; }
            Froot_child.insert(Froot_child.end(),Freg_stream.end()-(roff[t+1]-roff[t]),Freg_stream.end()); Froot_off.push_back(Froot_child.size());
          } else while(pp<pe){ uint64_t wire=get_varint(pp);uint32_t tok;
            if(!wire_to_tag(wire,stableRootTags,stableRootTags?regionsAfterTu[t]:NREG,blocksAfterTu[t],tok)){fprintf(stderr,"bad decoded Root token\n");return 2;}
            if(!tag_is_block(tok)) emitRegionF(tag_id(tok));
            else { uint32_t k=tag_id(tok); if(!Fblocks.known(k)){fprintf(stderr,"Root names a Block this F does not hold\n");exit(2);} const uint32_t*kb=Fblocks.begin(k); for(uint32_t j=0,n=Fblocks.length(k);j<n;++j) emitRegionF(kb[j]); } }
        }
        dec_s += std::chrono::duration<double>(Clock::now()-_td).count();   // F-decode ends here; the verify below is harness-only (F doesn't have the original)
        const FileSpan&physicalFile=corpus.files[t%physicalTUs];const char* orig=corpus.bytes.data()+physicalFile.off; uint32_t olen=physicalFile.len;
        if(recon.size()!=olen || memcmp(recon.data(),orig,olen)!=0){ byteexact=false; if(t<5||TUs<10) fprintf(stderr,"BYTE-EXACT FAIL TU %zu (%zu vs %u)\n",t,recon.size(),olen); }
        cum_raw += olen;
        double cur_wire = w_root+w_linedef+w_regiondef+w_pathdef+w_blockdef+w_missing+w_framing;
        std::array<double,CW_COUNT>currentComponentWire=componentWireSnapshot();double componentWireTotal=0;
        for(size_t part=0;part<CW_COUNT;++part){
          double delta=currentComponentWire[part]-previousComponentWire[part];
          if(delta<0){fprintf(stderr,"negative component wire delta TU=%zu part=%s value=%.0f\n",t,componentWireNames[part],delta);return 2;}
          perTUComponentWire[t][part]=delta;componentWireTotal+=delta;
        }
        previousComponentWire=currentComponentWire;
        for(size_t part=0;part<6;++part)perTUComponentRaw[t][part]=mixedRaw[part].size();
        perTUComponentRaw[t][6]=blobRaw.size();perTUComponentRaw[t][7]=blobPatchRaw.size();
        if(std::fabs(componentWireTotal-(cur_wire-cum_wire))>0.5){fprintf(stderr,"component wire total differs TU=%zu parts=%.0f total=%.0f\n",t,componentWireTotal,cur_wire-cum_wire);return 2;}
        perTU_raw[t]=olen; perTU_wire[t]=cur_wire - cum_wire; cum_wire=cur_wire;
        // --- ONE PHYSICAL TRANSACTION PER TU.  The close carries the stream-presence mask
        // (the byte the accounting charges as the selector) and makes the TU boundary an
        // explicit, parseable offset rather than something the receiver has to infer.
        if(cfSink.f){
            uint8_t mask=0;
            for(size_t i=0;i<mixedPartCount;++i) if(!mixedEncoded[i].empty()) mask|=uint8_t(1u<<i);
            cfSink.emit(WT_TU_END,&mask,1); fcSink.emit(WT_TU_END,nullptr,0);
            if(sinkBuildTus&&(t+1)%sinkBuildTus==0){
                const uint32_t b=uint32_t(t/sinkBuildTus),c=uint32_t(t+1);
                const uint8_t p[8]={uint8_t(b),uint8_t(b>>8),uint8_t(b>>16),uint8_t(b>>24),
                                    uint8_t(c),uint8_t(c>>8),uint8_t(c>>16),uint8_t(c>>24)};
                cfSink.emit(WT_BUILD_CLOSE,p,sizeof p); fcSink.emit(WT_BUILD_CLOSE,p,sizeof p);
            }
            sinkCfOff[t]=cfSink.off; sinkFcOff[t]=fcSink.off;
            sinkCfFrames[t]=cfSink.frames; sinkFcFrames[t]=fcSink.frames;
        }
        while(ckidx<ck_f.size() && double(cum_raw)>=ck_f[ckidx]*corpus.raw){ ck.push_back({ck_f[ckidx], double(cum_raw)/cum_wire}); ++ckidx; }
      }
      // Moving the key check per-TU must not quietly check FEWER Regions than the sweep did:
      // by the last TU every Region has been admitted, so this must have reached NREG.
      if(keysIdentifyObjects&&keyedRegions!=NREG){fprintf(stderr,"Region key check covered %u of %u Regions\n",keyedRegions,NREG);return 2;}
      enc_s+=fallback_c_s;dec_s-=fallback_c_s;if(dec_s<0)dec_s=0;
#if defined(WITH_BSC_GROUPS)
      if(usePlannedGroups){
        if(literalGroupDecodedCursor!=literalGroups.decoded.size()){fprintf(stderr,"literal group final decoded extent differs\n");return 2;}
        enc_s+=literalGroups.encode_seconds;dec_s+=literalGroups.decode_seconds;
      }
#endif
      fprintf(stderr,"pass %d (%s) single-core encode+decode+verify: %.2fs = %.3f GB/s raw\n", pass, (pass+1==npass&&npass>1)?"WARM":"cold", secs(tpass), corpus.raw/1e9/secs(tpass));
      fprintf(stderr,"  split (2-proc per-stream proxy): C-encode %.3f GB/s | F-decode %.3f GB/s => pipelined min = %.3f GB/s\n",
              corpus.raw/1e9/enc_s, corpus.raw/1e9/dec_s, corpus.raw/1e9/std::max(enc_s,dec_s));
    }
    while(ck.size()<ck_f.size()) ck.push_back({ck_f[ck.size()], double(cum_raw)/cum_wire});
    if(splitControlCeiling){
      for(size_t i=0;i<splitControlAll.size();++i){
        splitControlRawBytes[i]=splitControlAll[i].size();
        if(splitControlAll[i].empty())continue;
        splitControlWireBytes[i]=zstd_message_roundtrip(z,messageD,splitControlAll[i],zlevel,messageEncoded,messageDecoded)+FRAME;
        if(messageDecoded!=splitControlAll[i]){fprintf(stderr,"split control aggregate roundtrip differs stream=%zu\n",i);return 2;}
        splitControlCeilingWire+=splitControlWireBytes[i];
      }
    }
    if(structureCeiling){
      std::vector<uint8_t>structureJoint;put_varint(structureJoint,allRoots.size());
      structureJoint.insert(structureJoint.end(),allRoots.begin(),allRoots.end());
      structureJoint.insert(structureJoint.end(),allBlocks.begin(),allBlocks.end());
      if(!allRoots.empty())structureBatchRoot=zstd_message_roundtrip(z,messageD,allRoots,zlevel,messageEncoded,messageDecoded)+FRAME;
      if(!allBlocks.empty())structureBatchBlock=zstd_message_roundtrip(z,messageD,allBlocks,zlevel,messageEncoded,messageDecoded)+FRAME;
      structureBatchJoint=zstd_message_roundtrip(z,messageD,structureJoint,zlevel,messageEncoded,messageDecoded)+FRAME;
      structureLdmJoint=zstd_ldm_frame_encode(z,structureJoint,zlevel,messageEncoded)+FRAME;
      if(zstd_frame_decode_exact(messageD,messageEncoded,structureJoint.size())!=structureJoint){fprintf(stderr,"structure LDM aggregate roundtrip differs\n");return 2;}
    }
    ZSTD_freeCCtx(z);
    ZSTD_freeDCtx(messageD);
    if(sourceCostZ)ZSTD_freeCCtx(sourceCostZ);
    if(useSortedLines) for(size_t i=0;i<linePartCount;++i){ ZSTD_freeCCtx(lineZC[i]); ZSTD_freeDCtx(lineZD[i]); }
    if(useMixedRegions) for(size_t i=0;i<mixedPartCount;++i){ ZSTD_freeCCtx(mixedZC[i]); ZSTD_freeDCtx(mixedZD[i]); }
    if(blobZC)ZSTD_freeCCtx(blobZC);
    if(blobZD)ZSTD_freeDCtx(blobZD);
    if(blobPatchZC)ZSTD_freeCCtx(blobPatchZC);
    if(blobPatchZD)ZSTD_freeDCtx(blobPatchZD);
    if(alphaZC)ZSTD_freeCCtx(alphaZC);
    if(alphaZD)ZSTD_freeDCtx(alphaZD);
    if(residualDump&&fclose(residualDump)!=0){perror(residualDumpPath);return 2;}
    if(blobDump&&fclose(blobDump)!=0){perror(blobDumpPath);return 2;}
    if(blobDumpLengths&&fclose(blobDumpLengths)!=0){fprintf(stderr,"blob length dump close failed\n");return 2;}
    for(FILE*f:mixedDumps)if(f&&fclose(f)!=0){fprintf(stderr,"mixed stream dump close failed\n");return 2;}
    if(mixedDumpLengths&&fclose(mixedDumpLengths)!=0){fprintf(stderr,"mixed stream length close failed\n");return 2;}

    double totalwire = w_root+w_linedef+w_regiondef+w_pathdef+w_blockdef+w_missing+w_framing;
    double MiB=1048576.0;
    const char* structure_name=usePriorRoot?"V1+P22(ROOT_SLICE)":(useS1?"V1+S1(LZ blocks)":"V1");
    const char* line_name=useMixedRegions?(useCompressedBlobs?(useMoFactor?"+P27(mixed-regions+BYTE_ARRAY+BLOB+MO)":"+P26(mixed-regions+BYTE_ARRAY+BLOB)"):(useByteArrayLines?"+P24(mixed-regions+BYTE_ARRAY)":"+P24(mixed-regions)")):(useByteArrayLines?"+P21(BYTE_ARRAY)":(useSortedLines?"+P9(sorted-lines)":""));
    printf("\n==== CODEC-50 (%s%s%s%s%s%s%s%s%s, z%d) — %s ====\n", structure_name, useD1?"+D1":"", line_name, useAlphaLines?"+P4(ALPHA_LINES)":"", useResidualLdm?"+RESIDUAL_LDM":"", useLiteralGroups?"+BSC_GROUPS":"", useProjectSource?"+SOURCE_PACKAGE":"", (useD2mine?"+D2(inline)":(useD2?"+D2(helper)":"")), useDirectOrdinals?"+DIRECT_ORDINALS":"", zlevel, manifest);
    printf("byte-exact=%s  TUs=%zu raw=%.1f MiB regions=%u distinct_lines=%u paths=%zu blocks=%zu (marker_lines=%llu literal_lines=%llu)\n",
        byteexact?"OK":"FAIL",TUs,corpus.raw/MiB,NREG,dict.distinct(),paths.size(),boff2.size()-1,(unsigned long long)n_marker,(unsigned long long)n_literal);
    printf("wire by category (post-z%d, bytes): root=%.0f line_def=%.0f region_def=%.0f block_def=%.0f path_def=%.0f missing=%.0f framing=%.0f  TOTAL=%.0f (%.2f MiB)\n",
        zlevel,w_root,w_linedef,w_regiondef,w_blockdef,w_pathdef,w_missing,w_framing,totalwire,totalwire/MiB);
    if(literalZLevel!=zlevel||arrayZLevel!=zlevel||blobZLevel!=zlevel)
      printf("material zstd levels: control=%d literal=%d array=%d blob=%d\n",zlevel,literalZLevel,arrayZLevel,blobZLevel);
    printf("FinalRatio (raw / total wire, one cold pass) = %.1fx\n", corpus.raw/totalwire);
    if(structureCeiling){
      double current=w_root+w_blockdef+double(TUs)*FRAME;
      double separate=structureBatchRoot+structureBatchBlock;
      double projectedSeparate=totalwire-current+separate;
      double projectedJoint=totalwire-current+structureBatchJoint;
      double projectedLdm=totalwire-current+structureLdmJoint;
      double projectedZero=totalwire-current;
      printf("structure ceiling (diagnostic whole-run floors; TU boundaries omitted): raw_root=%zu raw_block=%zu current_root+block+root_frames=%.0f\n",
          allRoots.size(),allBlocks.size(),current);
      printf("  batched separate: root=%.0f block=%.0f combined=%.0f saving=%.0f => total=%.0f ratio=%.2fx\n",
          structureBatchRoot,structureBatchBlock,separate,current-separate,projectedSeparate,corpus.raw/projectedSeparate);
      printf("  batched joint: wire=%.0f saving=%.0f => total=%.0f ratio=%.2fx\n",
          structureBatchJoint,current-structureBatchJoint,projectedJoint,corpus.raw/projectedJoint);
      printf("  batched joint + LDM/win27: wire=%.0f saving=%.0f => total=%.0f ratio=%.2fx\n",
          structureLdmJoint,current-structureLdmJoint,projectedLdm,corpus.raw/projectedLdm);
      printf("  impossible zero-byte structure bound: saving=%.0f => total=%.0f ratio=%.2fx\n",
          current,projectedZero,corpus.raw/projectedZero);
    }
    if(usePriorRoot) printf("ROOT_SLICE stats: copies=%llu copied_regions=%llu indexed_windows=%llu index_entries=%llu receiver_root_bytes=%zu\n",
        (unsigned long long)root_slices.copies,(unsigned long long)root_slices.copied_regions,
        (unsigned long long)root_slices.indexed_windows,(unsigned long long)root_slices.index_entries,
        Froot_child.size()*sizeof(uint32_t)+Froot_off.size()*sizeof(size_t));
    if(useMixedRegions)printf("mixed components: control=%.0f literal=%.0f array_control=%.0f array_values=%.0f source_control=%.0f source_files=%.0f selector=%.0f raw_literal=%llu raw_array_values=%llu raw_source_reused=%llu source_package_raw=%llu source_package_files=%llu public_lines=%u ops=[literal=%llu publish=%llu ref=%llu array=%llu marker=%llu source=%llu patch=%llu]\n",
        mixedPartWire[0],mixedPartWire[1],mixedPartWire[2],mixedPartWire[3],mixedPartWire[4],mixedPartWire[5],mixedSelectorWire,
        (unsigned long long)mixedLiteralRaw,(unsigned long long)mixedArrayValues,(unsigned long long)mixedSourceBytes,(unsigned long long)mixedSourcePackageRaw,(unsigned long long)mixedSourcePackageFiles,nextMixedPublic-1,
        (unsigned long long)mixedOps[0],(unsigned long long)mixedOps[1],(unsigned long long)mixedOps[2],(unsigned long long)mixedOps[3],(unsigned long long)mixedOps[4],(unsigned long long)mixedOps[5],(unsigned long long)mixedOps[6]);
    if(useCompressedBlobs)printf("compressed blobs: count=%llu deflated=%llu inflated=%llu wire=%.0f patch_raw=%llu patch_wire=%.0f canonical_exact=%llu corrected=%llu replaced=%llu transform_candidate_wire=%.0f ordinary_candidate_wire=%.0f transform_tus=%llu ordinary_tus=%llu fallbacks=%llu fallback_request_wire=%.0f fallback_reply_wire=%.0f threads=%u zstd_workers=%u zstd_job_mib=%u zstd_overlap_log=%u mode=%s canonical=zlib-%s-level%d\n",
        (unsigned long long)mixedBlobCount,(unsigned long long)mixedBlobDeflated,(unsigned long long)mixedBlobInflated,mixedBlobWire,
        (unsigned long long)mixedBlobPatchRaw,mixedBlobPatchWire,(unsigned long long)mixedBlobCanonicalExact,(unsigned long long)mixedBlobCorrected,(unsigned long long)mixedBlobReplaced,
        mixedBlobTransformCandidateWire,mixedBlobOrdinaryCandidateWire,(unsigned long long)mixedBlobTransformTus,(unsigned long long)mixedBlobOrdinaryTus,
        (unsigned long long)mixedBlobFallbacks,mixedBlobFallbackRequestWire,mixedBlobFallbackReplyWire,blobThreads,blobZstdWorkers,blobZstdJobMiB,blobZstdOverlapLog,useBlobEagerPatches?"eager-patch":"lazy-reply",zlibVersion(),blobCanonicalLevel);
    if(useMoFactor)printf("MO factor: policy_tus=%llu candidate_wire=%.0f selected_tus=%llu members=%llu member_raw=%llu new_originals=%llu C_dictionary=%u F_dictionary=%u C_string_bytes=%llu F_string_bytes=%llu\n",
        (unsigned long long)mixedBlobMoPolicyTus,mixedBlobMoCandidateWire,(unsigned long long)mixedBlobMoTus,(unsigned long long)mixedBlobMoMembers,(unsigned long long)mixedBlobMoBytes,
        (unsigned long long)mixedBlobMoDefinitions,Cmo.size(),Fmo.size(),(unsigned long long)Cmo.string_bytes(),(unsigned long long)Fmo.string_bytes());
    if(useAlphaLines)printf("alpha lines: input_raw=%llu eligible_lines=%llu gap_raw=%llu ordinary_candidate_wire=%.0f literal_keyword_candidate_wire=%.0f parameterized_keyword_candidate_wire=%.0f best_alpha_candidate_wire=%.0f selected_wire=%.0f selector_wire=%.0f control_wire=%.0f data_wire=%.0f selected_tus=%llu ordinary_tus=%llu literal_keyword_tus=%llu parameterized_keyword_tus=%llu rules=%llu instances=%llu covered_raw=%llu literal_fallbacks=%llu unique_slots=%llu slot_occurrences=%llu lexicon_entries=%llu lexicon_references=%llu\n",
        (unsigned long long)alphaInputRaw,(unsigned long long)alphaEligibleLines,(unsigned long long)alphaGapRaw,alphaOrdinaryCandidateWire,alphaLiteralKeywordCandidateWire,alphaParameterizedKeywordCandidateWire,alphaBestCandidateWire,alphaSelectedWire,alphaSelectorWire,alphaControlWire,alphaDataWire,
        (unsigned long long)alphaSelectedTus,(unsigned long long)alphaOrdinaryTus,(unsigned long long)alphaLiteralKeywordTus,(unsigned long long)alphaParameterizedKeywordTus,
        (unsigned long long)alphaStats.rules,(unsigned long long)alphaStats.instances,(unsigned long long)alphaStats.template_input_bytes,(unsigned long long)alphaStats.literal_fallbacks,(unsigned long long)alphaStats.unique_slots,(unsigned long long)alphaStats.slot_occurrences,(unsigned long long)alphaStats.lexicon_entries,(unsigned long long)alphaStats.lexicon_references);
    if(useResidualLdm)printf("residual LDM: window_log=27 literal_wire=%.0f raw_literal=%llu\n",mixedPartWire[1],(unsigned long long)mixedLiteralRaw);
#if defined(WITH_BSC_GROUPS)
    if(literalOnDemand)printf("literal frames on demand: frames=%zu wire=%.0f packed_header_bytes=%zu selector=[z3=%llu bsc=%llu z10=%llu] (no dump, no planning pass)\n",
        TUs,mixedPartWire[1],residual_group::kHeaderBytes,
        (unsigned long long)onDemandSelected[0],(unsigned long long)onDemandSelected[1],
        (unsigned long long)onDemandSelected[2]);
    if(usePlannedGroups)printf("literal groups: tus_per_group=%zu groups=%zu workers=%zu raw=%zu wire=%zu packed_header_bytes=%zucandidates=%s selected=[zstd3=%llu bsc=%llu zstd10=%llu] encode_seconds=%.6f decode_seconds=%.6f retained_wire=%s\n",
        literalGroupTus,literalGroups.groups.size(),literalGroups.workers,literalGroups.decoded.size(),literalGroups.wire.size(),residual_group::kHeaderBytes,
        literalGroups.evaluated_zstd10?"zstd3,bsc,zstd10":"zstd3,bsc",
        (unsigned long long)literalGroups.selected[0],(unsigned long long)literalGroups.selected[1],
        (unsigned long long)literalGroups.selected[2],literalGroups.encode_seconds,literalGroups.decode_seconds,
        literalGroupWirePath?literalGroupWirePath:"(memory only)");
#endif
    if(splitControlCeiling)printf("split control whole-run ceiling: wire=%.0f baseline=%.0f saving=%.0f raw=[%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu] wire_parts=[%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu]\n",
        splitControlCeilingWire,mixedPartWire[0],mixedPartWire[0]-splitControlCeilingWire,
        (unsigned long long)splitControlRawBytes[0],(unsigned long long)splitControlRawBytes[1],(unsigned long long)splitControlRawBytes[2],(unsigned long long)splitControlRawBytes[3],(unsigned long long)splitControlRawBytes[4],(unsigned long long)splitControlRawBytes[5],(unsigned long long)splitControlRawBytes[6],(unsigned long long)splitControlRawBytes[7],
        (unsigned long long)splitControlWireBytes[0],(unsigned long long)splitControlWireBytes[1],(unsigned long long)splitControlWireBytes[2],(unsigned long long)splitControlWireBytes[3],(unsigned long long)splitControlWireBytes[4],(unsigned long long)splitControlWireBytes[5],(unsigned long long)splitControlWireBytes[6],(unsigned long long)splitControlWireBytes[7]);
    if(useKeyMap)printf("key map: half_cold_bit=%d preloaded_regions=%llu preloaded_raw_bytes=%llu associated_regions=%llu association_wire=%.0f missing_reply_wire=%.0f\n",
        halfColdBit,(unsigned long long)preloadedRegionCount,(unsigned long long)preloadedRegionBytes,
        (unsigned long long)associatedRegionCount,mixedAssociationWire,mixedMissingRequestWire);
    if(useDirectOrdinals)printf("direct ordinals: generation_latched=1 region_namespaces=1 block_lifetime=generation\n");
    if(useProjectSource)printf("source admission: ratio=%u considered=%llu admitted=%llu observed_potential_raw=%llu estimated_independent_package_wire=%llu\n",
        sourceAdmitRatio,(unsigned long long)mixedSourceConsidered,(unsigned long long)mixedSourceAdmitted,(unsigned long long)mixedSourcePotentialRaw,(unsigned long long)mixedSourceEstimatedCost);
    if(!useSortedLines&&!useMixedRegions){ ZSTD_CCtx* z2=ZSTD_createCCtx(); std::vector<uint8_t> d2b;
      double bl=allLineDefs.empty()?0:zstd_size(z2,allLineDefs.data(),allLineDefs.size(),zlevel,d2b);
      double br=allRoots.empty()?0:zstd_size(z2,allRoots.data(),allRoots.size(),zlevel,d2b);
      // long-distance structure ceiling for line_def: z3 with unbounded window + LDM (what a perfect
      // relative-LZ OBJECT achieves — diagnostic only; the wire uses default-window per-message z3).
      double bl_ldm=0;
      if(!allLineDefs.empty()){ ZSTD_CCtx_reset(z2,ZSTD_reset_session_and_parameters); ZSTD_CCtx_setParameter(z2,ZSTD_c_compressionLevel,zlevel);
        ZSTD_CCtx_setParameter(z2,ZSTD_c_enableLongDistanceMatching,1); ZSTD_CCtx_setParameter(z2,ZSTD_c_windowLog,27);
        size_t bnd=ZSTD_compressBound(allLineDefs.size()); if(d2b.size()<bnd)d2b.resize(bnd); bl_ldm=double(ZSTD_compress2(z2,d2b.data(),d2b.size(),allLineDefs.data(),allLineDefs.size())); }
      // FULL streamed floor: batch-compress EVERY category (what a persistent shared-window streaming
      // codec reaches by capturing cross-message redundancy) — the real z3 ceiling for this structure.
      auto batch=[&](std::vector<uint8_t>&v){ return v.empty()?0.0:zstd_size(z2,v.data(),v.size(),zlevel,d2b); };
      double fl_line=bl, fl_root=br, fl_reg=batch(allRegions), fl_blk=batch(allBlocks), fl_path=batch(allPaths), fl_miss=batch(allMiss);
      ZSTD_freeCCtx(z2);
      double fullfloor=fl_line+fl_root+fl_reg+fl_blk+fl_path+fl_miss+w_framing;
      double alt=totalwire - w_linedef - w_root + bl + br;
      double altldm=totalwire - w_linedef + bl_ldm;
      printf("DIAG batched-z%d floor: line_def %.0f->%.0f  root %.0f->%.0f  => streamed TOTAL=%.0f ratio=%.0fx\n",zlevel,w_linedef,bl,w_root,br,alt,corpus.raw/alt);
      printf("DIAG FULL streamed floor (all cats batched z%d): line=%.2f reg=%.2f blk=%.2f path=%.2f miss=%.2f root=%.2f => %.2f MiB ratio=%.0fx\n",
        zlevel,fl_line/MiB,fl_reg/MiB,fl_blk/MiB,fl_path/MiB,fl_miss/MiB,fl_root/MiB,fullfloor/MiB,corpus.raw/fullfloor);
      // region_def headroom, delta-serialized (current wire) AND raw-line-id serialized (cross-region
      // subsequence-preserving -- per-region delta breaks shared-run matching at each run's first id).
      { auto zldm=[&](std::vector<uint8_t>&v){ if(v.empty())return 0.0; ZSTD_CCtx* zr=ZSTD_createCCtx(); std::vector<uint8_t> rb(ZSTD_compressBound(v.size())+64);
          ZSTD_CCtx_setParameter(zr,ZSTD_c_compressionLevel,zlevel); ZSTD_CCtx_setParameter(zr,ZSTD_c_enableLongDistanceMatching,1); ZSTD_CCtx_setParameter(zr,ZSTD_c_windowLog,27);
          double r=double(ZSTD_compress2(zr,rb.data(),rb.size(),v.data(),v.size())); ZSTD_freeCCtx(zr); return r; };
        double rl_delta=zldm(allRegions), rl_raw_z3=0, rl_raw_ldm=zldm(allRegionsRaw);
        { ZSTD_CCtx* zr=ZSTD_createCCtx(); std::vector<uint8_t> rb(ZSTD_compressBound(allRegionsRaw.size())+64); ZSTD_CCtx_setParameter(zr,ZSTD_c_compressionLevel,zlevel);
          rl_raw_z3=allRegionsRaw.empty()?0:double(ZSTD_compress2(zr,rb.data(),rb.size(),allRegionsRaw.data(),allRegionsRaw.size())); ZSTD_freeCCtx(zr); }
        printf("DIAG region_def headroom: delta-stream raw=%.2f batched-z%d=%.2f +LDM=%.2f | RAW-line-id z%d=%.2f +LDM+win27=%.2f MiB (cross-region SLICE ceiling)\n",
          allRegions.size()/MiB,zlevel,fl_reg/MiB,rl_delta/MiB,zlevel,rl_raw_z3/MiB,rl_raw_ldm/MiB); }
      printf("DIAG long-distance ceiling: line_def z%d+LDM+win27 = %.0f (%.2f MiB) => TOTAL=%.0f ratio=%.0fx  [what a perfect relative-LZ OBJECT could reach]\n",zlevel,bl_ldm,bl_ldm/MiB,altldm,corpus.raw/altldm);
      // entropy ladder on the distinct-line dictionary leg: is 8.47MB a z3-level wall or an information floor?
      if(deep && !allLineDefs.empty()){ printf("DIAG line_def entropy ladder (%.2f MiB raw, %.0f distinct literal lines):\n",allLineDefs.size()/MiB,double(n_literal));
        for(int lv:{3,9,19,22}){ ZSTD_CCtx* zc=ZSTD_createCCtx(); ZSTD_CCtx_setParameter(zc,ZSTD_c_compressionLevel,lv);
          ZSTD_CCtx_setParameter(zc,ZSTD_c_enableLongDistanceMatching,1); ZSTD_CCtx_setParameter(zc,ZSTD_c_windowLog,27);
          size_t bnd=ZSTD_compressBound(allLineDefs.size()); std::vector<uint8_t> ob(bnd);
          size_t r=ZSTD_compress2(zc,ob.data(),bnd,allLineDefs.data(),allLineDefs.size()); ZSTD_freeCCtx(zc);
          double t=totalwire - w_linedef + double(r);
          printf("    z%-2d = %.2f MiB (%.1f B/line)  => TOTAL ratio=%.0fx\n",lv,r/MiB,double(r)/double(n_literal),corpus.raw/t); }
        // Is z19's win reachable at z3 by a REORDER? Cluster similar distinct lines adjacent so z3's
        // greedy matcher finds what z19 finds by deep search. Sort lexicographically + front-code
        // (shared-prefix-len + suffix) — a fully z3-legal structural transform, not a level bump.
        uint32_t D=dict.distinct(); std::vector<uint32_t> ids; ids.reserve(D);
        for(uint32_t id=1;id<=D;++id) ids.push_back(id);
        auto txt=[&](uint32_t id,uint32_t&len){ const LineRef&r=dict.ref(id); len=r.len; return dict.line_data(r.off); };
        std::sort(ids.begin(),ids.end(),[&](uint32_t a,uint32_t b){ uint32_t la,lb; const char*pa=txt(a,la),*pb=txt(b,lb);
          int c=memcmp(pa,pb,la<lb?la:lb); return c!=0? c<0 : la<lb; });
        std::vector<uint8_t> sorted_cat, frontcoded; sorted_cat.reserve(dict.distinct_line_bytes());
        const char* prevp=nullptr; uint32_t prevl=0;
        for(uint32_t id:ids){ uint32_t l; const char*p=txt(id,l);
          sorted_cat.insert(sorted_cat.end(),p,p+l);
          uint32_t cp=0, m=l<prevl?l:prevl; while(cp<m && p[cp]==prevp[cp]) ++cp;
          put_varint(frontcoded,cp); put_varint(frontcoded,l-cp); frontcoded.insert(frontcoded.end(),p+cp,p+l);
          prevp=p; prevl=l; }
        ZSTD_CCtx* zs=ZSTD_createCCtx(); std::vector<uint8_t> sb(ZSTD_compressBound(std::max(sorted_cat.size(),frontcoded.size())));
        ZSTD_CCtx_setParameter(zs,ZSTD_c_compressionLevel,zlevel);
        size_t rs=ZSTD_compress2(zs,sb.data(),sb.size(),sorted_cat.data(),sorted_cat.size());
        ZSTD_CCtx_reset(zs,ZSTD_reset_session_and_parameters); ZSTD_CCtx_setParameter(zs,ZSTD_c_compressionLevel,zlevel);
        size_t rf=ZSTD_compress2(zs,sb.data(),sb.size(),frontcoded.data(),frontcoded.size()); ZSTD_freeCCtx(zs);
        printf("DIAG reorder test (z%d-legal structure vs z19's 5.96MiB):\n",zlevel);
        printf("    sorted-concat z%d       = %.2f MiB (%.1f B/line)\n",zlevel,rs/MiB,double(rs)/double(n_literal));
        printf("    sorted+front-coded z%d  = %.2f MiB (%.1f B/line)  [+perm cost ~%.2f MiB to send ids]\n",zlevel,rf/MiB,double(rf)/double(n_literal),double(D)*2.2/MiB); } }
    else printf("DIAG legacy Line ceilings omitted for the selected persistent definition format\n");
    printf("H200 f-checkpoints (cum raw fraction -> cumulative ratio):\n");
    for(auto&c:ck) printf("  f=%.2f  ratio=%.0fx\n",c.first,c.second);
    if(cfSink.f){
      cfSink.close(cfSinkPath); fcSink.close(fcSinkPath);
      if(sinkReplay)printf("SINK REPLAY OK: both streams delivered every frame the run consumed, in order, byte-identical, and were fully consumed\n");
      // The primary score is the C->F sink alone.  The reverse sink is reported beside it
      // and is never added in.
      printf("SINK cf_bytes=%llu cf_frames=%llu  fc_bytes=%llu fc_frames=%llu  "
             "accounting_TOTAL=%.0f  physical_minus_accounting=%+.0f\n",
          (unsigned long long)cfSink.off,(unsigned long long)cfSink.frames,
          (unsigned long long)fcSink.off,(unsigned long long)fcSink.frames,
          w_root+w_linedef+w_regiondef+w_pathdef+w_blockdef+w_missing+w_framing,
          double(cfSink.off+fcSink.off)-(w_root+w_linedef+w_regiondef+w_pathdef+w_blockdef+w_missing+w_framing));
      // Every emitted payload byte IS a message the accounting already charged for, and the
      // accounting additionally charges a 4-byte FRAME per message plus the selector byte.
      // So payload <= TOTAL is an invariant, not a hope; violating it means a frame was
      // written from a buffer whose size is not its encoded length.
      const double acct=w_root+w_linedef+w_regiondef+w_pathdef+w_blockdef+w_missing+w_framing;
      const double pay=double(cfSink.payload+fcSink.payload);
      printf("SINK payload=%.0f header_overhead=%llu close_frames=%llu\n",pay,
          (unsigned long long)(5*(cfSink.frames+fcSink.frames)),
          (unsigned long long)(cfSink.frames+fcSink.frames));
      if(pay>acct){fprintf(stderr,"sink payload %.0f exceeds the accounting total %.0f\n",pay,acct);return 2;}
      // A literal group cannot leave C until every TU it covers has been dispatched, so a
      // group of N TUs stalls the first N-1 of them.  Zero is the only deployable value.
      printf("SINK dispatch_lag_tus=%zu (a TU's literals are sendable this many TUs after it)\n",
          literalGroupTus?literalGroupTus-1:0);
      if(sinkCurvePath){
        FILE*sc=fopen(sinkCurvePath,"wb");if(!sc){perror(sinkCurvePath);return 2;}
        if(fprintf(sc,"tu\traw_bytes\tcf_offset\tfc_offset\tcf_frames\tfc_frames\tbuild_close\n")<0){fprintf(stderr,"sink curve header write failed\n");fclose(sc);return 2;}
        for(size_t t=0;t<TUs;++t)
          if(fprintf(sc,"%zu\t%llu\t%llu\t%llu\t%llu\t%llu\t%d\n",t+1,
              (unsigned long long)perTU_raw[t],(unsigned long long)sinkCfOff[t],
              (unsigned long long)sinkFcOff[t],(unsigned long long)sinkCfFrames[t],
              (unsigned long long)sinkFcFrames[t],
              int(sinkBuildTus&&(t+1)%sinkBuildTus==0))<0){fprintf(stderr,"sink curve row write failed\n");fclose(sc);return 2;}
        if(fclose(sc)!=0){perror(sinkCurvePath);return 2;}
        // offsets must be non-decreasing and end exactly at the file sizes
        for(size_t t=1;t<TUs;++t)if(sinkCfOff[t]<sinkCfOff[t-1]||sinkFcOff[t]<sinkFcOff[t-1]){fprintf(stderr,"sink offsets are not monotone at TU=%zu\n",t);return 2;}
        if(TUs&&(sinkCfOff[TUs-1]!=cfSink.off||sinkFcOff[TUs-1]!=fcSink.off)){fprintf(stderr,"final sink offset differs from the stream size\n");return 2;}
      }
    }
    if(selectorTsvPath){
      FILE*f=fopen(selectorTsvPath,"wb");if(!f){perror(selectorTsvPath);return 2;}
      if(fprintf(f,"tu\traw_root_bytes\traw_root_z\tglobal_root_bytes\tglobal_root_z\tglobal_new_blocks\tglobal_blockdef_bytes\tglobal_blockdef_z\tglobal_total_z\twinner\n")<0){fclose(f);return 2;}
      uint64_t rawCum=0,globalCum=0,rawWins=0,globalWins=0,ties=0;
      for(const SelRow&r:selRows){
        const uint64_t g=r.globalRootZ+r.defZ;
        // Deterministic tie rule: fewer newly-installed Blocks first, then RAW.
        const char*win = g<r.rawRootZ ? "GLOBAL_S1" : (g>r.rawRootZ ? "RAW" : (r.newBlocks?"RAW":"RAW"));
        if(g<r.rawRootZ)++globalWins; else if(g>r.rawRootZ)++rawWins; else ++ties;
        rawCum+=r.rawRootZ; globalCum+=g;
        if(fprintf(f,"%zu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%s\n",r.tu,
            (unsigned long long)r.rawRootRaw,(unsigned long long)r.rawRootZ,
            (unsigned long long)r.globalRootRaw,(unsigned long long)r.globalRootZ,
            (unsigned long long)r.newBlocks,(unsigned long long)r.defRaw,(unsigned long long)r.defZ,
            (unsigned long long)g,win)<0){fclose(f);return 2;}
      }
      if(fclose(f)!=0){perror(selectorTsvPath);return 2;}
      printf("SELECTOR per-TU costing: rows=%zu raw_cum=%llu global_cum=%llu wins[RAW=%llu GLOBAL_S1=%llu tie=%llu]\n",
          selRows.size(),(unsigned long long)rawCum,(unsigned long long)globalCum,
          (unsigned long long)rawWins,(unsigned long long)globalWins,(unsigned long long)ties);
    }
    if(curveTsvPath){
      FILE*curve=fopen(curveTsvPath,"wb");if(!curve){perror(curveTsvPath);return 2;}
      if(fprintf(curve,"tu\traw_bytes\twire_bytes\tcumulative_raw_bytes\tcumulative_wire_bytes\tcumulative_ratio\texact\n")<0){fprintf(stderr,"curve TSV header write failed\n");fclose(curve);return 2;}
      uint64_t curveRaw=0,curveWire=0;
      for(size_t t=0;t<TUs;++t){
        const uint64_t raw=uint64_t(perTU_raw[t]);const uint64_t wire=uint64_t(std::llround(perTU_wire[t]));curveRaw+=raw;curveWire+=wire;
        if(fprintf(curve,"%zu\t%llu\t%llu\t%llu\t%llu\t%.12g\t%s\n",t+1,
            (unsigned long long)raw,(unsigned long long)wire,(unsigned long long)curveRaw,
            (unsigned long long)curveWire,curveWire?double(curveRaw)/double(curveWire):0.0,byteexact?"true":"false")<0){fprintf(stderr,"curve TSV row write failed\n");fclose(curve);return 2;}
      }
      if(std::fabs(double(curveRaw)-double(cum_raw))>0.5||std::fabs(double(curveWire)-cum_wire)>0.5){fprintf(stderr,"curve TSV total differs\n");fclose(curve);return 2;}
      if(fclose(curve)!=0){perror(curveTsvPath);return 2;}
      printf("per-TU curve: %s rows=%zu cumulative_raw=%llu cumulative_wire=%llu\n",curveTsvPath,TUs,
          (unsigned long long)curveRaw,(unsigned long long)curveWire);
    }
    if(componentCurveTsvPath){
      FILE*curve=fopen(componentCurveTsvPath,"wb");if(!curve){perror(componentCurveTsvPath);return 2;}
      if(fprintf(curve,"tu\traw_bytes\twire_bytes")<0){fprintf(stderr,"component curve header write failed\n");fclose(curve);return 2;}
      for(const char*name:componentRawNames)if(fprintf(curve,"\t%s",name)<0){fprintf(stderr,"component raw header write failed\n");fclose(curve);return 2;}
      for(const char*name:componentWireNames)if(fprintf(curve,"\t%s",name)<0){fprintf(stderr,"component wire header write failed\n");fclose(curve);return 2;}
      if(fprintf(curve,"\tcumulative_raw_bytes\tcumulative_wire_bytes\texact\n")<0){fprintf(stderr,"component curve header finish failed\n");fclose(curve);return 2;}
      uint64_t curveRaw=0,curveWire=0;
      for(size_t t=0;t<TUs;++t){
        const uint64_t raw=uint64_t(perTU_raw[t]),wire=uint64_t(std::llround(perTU_wire[t]));curveRaw+=raw;curveWire+=wire;
        if(fprintf(curve,"%zu\t%llu\t%llu",t+1,(unsigned long long)raw,(unsigned long long)wire)<0){fprintf(stderr,"component curve prefix write failed\n");fclose(curve);return 2;}
        for(uint64_t value:perTUComponentRaw[t])if(fprintf(curve,"\t%llu",(unsigned long long)value)<0){fprintf(stderr,"component raw row write failed\n");fclose(curve);return 2;}
        uint64_t componentWire=0;
        for(double value:perTUComponentWire[t]){uint64_t rounded=uint64_t(std::llround(value));componentWire+=rounded;if(fprintf(curve,"\t%llu",(unsigned long long)rounded)<0){fprintf(stderr,"component wire row write failed\n");fclose(curve);return 2;}}
        if(componentWire!=wire){fprintf(stderr,"component curve row total differs TU=%zu parts=%llu wire=%llu\n",t,(unsigned long long)componentWire,(unsigned long long)wire);fclose(curve);return 2;}
        if(fprintf(curve,"\t%llu\t%llu\t%s\n",(unsigned long long)curveRaw,(unsigned long long)curveWire,byteexact?"true":"false")<0){fprintf(stderr,"component curve suffix write failed\n");fclose(curve);return 2;}
      }
      if(std::fabs(double(curveRaw)-double(cum_raw))>0.5||std::fabs(double(curveWire)-cum_wire)>0.5){fprintf(stderr,"component curve total differs\n");fclose(curve);return 2;}
      if(fclose(curve)!=0){perror(componentCurveTsvPath);return 2;}
      printf("per-TU component curve: %s rows=%zu cumulative_raw=%llu cumulative_wire=%llu\n",componentCurveTsvPath,TUs,
          (unsigned long long)curveRaw,(unsigned long long)curveWire);
    }
    // trailing-window (5% raw) ratio near the end
    { double win=0.05*corpus.raw, r=0,wsum=0; for(size_t t=TUs;t-->0;){ r+=perTU_raw[t]; wsum+=perTU_wire[t]; if(r>=win) break; } printf("trailing 5%%-raw window ratio (steady) = %.0fx\n", wsum>0?r/wsum:0); }
    struct rusage ru{}; getrusage(RUSAGE_SELF,&ru); fprintf(stderr,"peak RSS=%.1f MiB total=%.1fs\n",ru.ru_maxrss/1024.0,secs(t0));
    return byteexact?0:1;
}
