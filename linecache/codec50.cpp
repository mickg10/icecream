// codec50.cpp — real byte-exact "Protocol-50" codec for the icecream line-dedup bake-off (issue #16).
//
// This is the honest measurement harness the FINAL spec requires: a real encoder (C) that turns
// interned .ii structure into a self-describing, length-framed, z3-compressed wire byte stream, and a
// real decoder (F) that consumes ONLY those bytes, installs immutable objects, expands the root, and
// reconstructs the exact .ii bytes. Every wire byte is charged by category. FinalRatio = raw / wire,
// ONE cold chronological pass, reported per corpus at f = 0.10/0.25/0.50/0.75/1.00 with the H200
// trailing-window ratio. z <= 3 only. No corpus-name branches; no free dictionaries.
//
// Milestone-1 variants (this file): V1 = stable Lines + marker Regions; D1 = preprocessor-marker/path
// factoring of "# N \"path\" flags" lines into (path-object, lineno, flags). D2 (relative-LZ line
// codec, definition_codec.h) plugs into the line-definition leg when available. The real two-process
// socketpair + throughput and the S0/S1/S3 structure planes build on this same serializer/decoder.
//
// build: g++ -O3 -march=native -std=c++17 codec50.cpp -o codec50 -lzstd

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <immintrin.h>
#include <zstd.h>
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
static inline void put_varint(std::vector<uint8_t>&o,uint64_t v){ while(v>=0x80){o.push_back(uint8_t(v)|0x80);v>>=7;} o.push_back(uint8_t(v)); }
static inline uint64_t get_varint(const uint8_t*&p){ uint64_t v=0; int s=0; for(;;){ uint8_t b=*p++; v|=uint64_t(b&0x7f)<<s; if(!(b&0x80))break; s+=7; } return v; }
static inline void put_zigzag(std::vector<uint8_t>&o,int64_t v){ put_varint(o,(uint64_t(v)<<1)^uint64_t(v>>63)); }
static inline int64_t get_zigzag(const uint8_t*&p){ uint64_t u=get_varint(p); return int64_t(u>>1)^-int64_t(u&1); }
static inline size_t varint_size(uint64_t v){ size_t n=1; while(v>=0x80){++n;v>>=7;} return n; }
static size_t zstd_size(ZSTD_CCtx*c,const uint8_t*data,size_t n,int level,std::vector<uint8_t>&dst){ ZSTD_CCtx_reset(c,ZSTD_reset_session_and_parameters); ZSTD_CCtx_setParameter(c,ZSTD_c_compressionLevel,level);
    size_t bound=ZSTD_compressBound(n); if(dst.size()<bound)dst.resize(bound); size_t r=ZSTD_compress2(c,dst.data(),dst.size(),data?data:(const uint8_t*)"",n); if(ZSTD_isError(r)){fprintf(stderr,"zstd %s\n",ZSTD_getErrorName(r));exit(2);} return r; }

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
static bool parse_byte_array(const char*data,uint32_t length,GeneratedByteArray&out){
    const char*begin=data,*end=data+length; if(length<8||end[-1]!='\n')return false;
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
    if(out.suffix.empty()||out.values.size()<4||!haveFormat)return false;
    if(!haveSeparator)out.separator=",";
    if(numberFormat==DECIMAL)out.format=DECIMAL;
    else if(prefixUpper)out.format=sawLower?HEX_UL:HEX_UU;
    else out.format=sawLower?HEX_LL:HEX_LU;
    return true;
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

int main(int argc,char**argv){
    const char* manifest=nullptr; size_t max_files=SIZE_MAX; int zlevel=3; uint32_t sourceAdmitRatio=6; bool useD1=true, useD2=false, useS1=true, useD2mine=false, deep=false, warm=false, usePriorRoot=false, useSortedLines=false, useByteArrayLines=false, useMixedRegions=false, useProjectSource=false;
    for(int i=1;i<argc;++i){ if(!strcmp(argv[i],"--manifest")&&i+1<argc)manifest=argv[++i];
        else if(!strcmp(argv[i],"--z")&&i+1<argc)zlevel=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--no-d1"))useD1=false;
        else if(!strcmp(argv[i],"--v1"))useS1=false;   // V1 baseline: raw region-id root, no S1 blocks
        else if(!strcmp(argv[i],"--d2"))useD2mine=true;      // inline relative-LZ line codec
        else if(!strcmp(argv[i],"--d2helper"))useD2=true;    // helper's definition_codec (needs -DWITH_D2)
        else if(!strcmp(argv[i],"--prior-root"))usePriorRoot=true;
        else if(!strcmp(argv[i],"--sorted-lines"))useSortedLines=true;
        else if(!strcmp(argv[i],"--byte-array-lines")){useSortedLines=true;useByteArrayLines=true;}
        else if(!strcmp(argv[i],"--mixed-regions"))useMixedRegions=true;
        else if(!strcmp(argv[i],"--source-package"))useProjectSource=true;
        else if(!strcmp(argv[i],"--source-admit-ratio")&&i+1<argc){char*end=nullptr;unsigned long value=strtoul(argv[++i],&end,10);if(!end||*end||!value||value>1000){fprintf(stderr,"bad source admission ratio\n");return 2;}sourceAdmitRatio=uint32_t(value);}
        else if(!strcmp(argv[i],"--deep"))deep=true;         // run slow z19/z22 entropy ladder + reorder test
        else if(!strcmp(argv[i],"--warm"))warm=true;         // Basis C: 2nd pass with dict retained -> warm steady-state wire
        else if(!strcmp(argv[i],"--max-files")&&i+1<argc){ char*t=nullptr; unsigned long long v=strtoull(argv[++i],&t,10); if(!t||*t||!v){fprintf(stderr,"bad max-files\n");return 2;} max_files=size_t(v); }
        else { fprintf(stderr,"unknown %s\n",argv[i]); return 2; } }
    if(!manifest){ fprintf(stderr,"usage: %s --manifest F [--z 0|1|3] [--no-d1] [--d2] [--prior-root] [--sorted-lines|--byte-array-lines|--mixed-regions [--source-package [--source-admit-ratio N]]] [--max-files N]\n",argv[0]); return 2; }
    if(useProjectSource&&!useMixedRegions){fprintf(stderr,"--source-package requires --mixed-regions\n");return 2;}
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
    uint32_t NREG=uint32_t(dict.region_count()); size_t TUs=corpus.files.size();
    fprintf(stderr,"loaded+interned %.1fs TUs=%zu raw=%llu regions=%u region_occ=%zu distinct_lines=%u\n",secs(t0),TUs,(unsigned long long)corpus.raw,NREG,allreg.size(),dict.distinct());

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
    std::unordered_map<uint64_t,uint32_t> bdict;
    std::vector<uint32_t> tokstream; std::vector<size_t> tokoff; tokoff.push_back(0);
    std::vector<uint32_t> bcopy_src; std::vector<uint8_t> bcopy_ok;   // block k: def as COPY(src,len) if ok (source in prior TUs)
    if(useS1){
        size_t NS=allreg.size(); uint32_t MINMATCH=3, MAXCHAIN=64, hbits=22;
        std::vector<uint32_t> head(size_t(1)<<hbits, UINT32_MAX), prevp(NS, UINT32_MAX);
        auto kgram=[&](size_t i)->uint64_t{ uint64_t h=1469598103934665603ULL; for(uint32_t j=0;j<MINMATCH;++j){ h^=allreg[i+j]; h*=1099511628211ULL; } return (h*0x9E3779B97F4A7C15ULL)>>(64-hbits); };
        auto block_get=[&](const uint32_t*p,size_t L,uint32_t srcpos,uint8_t copyok)->uint32_t{ uint64_t h=1469598103934665603ULL^(L*0x100000001b3ULL); for(size_t j=0;j<L;++j){h^=p[j];h*=1099511628211ULL;}
            auto it=bdict.find(h); if(it!=bdict.end()){ uint32_t k=it->second; if(boff2[k+1]-boff2[k]==L && memcmp(&bchild[boff2[k]],p,L*4)==0) return NREG+k; }
            uint32_t k=uint32_t(boff2.size()-1); bchild.insert(bchild.end(),p,p+L); boff2.push_back(bchild.size()); bcopy_src.push_back(srcpos); bcopy_ok.push_back(copyok); if(it==bdict.end()) bdict.emplace(h,k); return NREG+k; };
        auto tb=Clock::now();
        for(size_t t=0;t<TUs;++t){ size_t a=roff[t],b=roff[t+1]; size_t i=a;
            while(i<b){ size_t bestL=0,bestP=0;
                if(i+MINMATCH<=b && i+MINMATCH<=NS){ uint32_t cand=head[kgram(i)],chain=0;
                    while(cand!=UINT32_MAX&&chain<MAXCHAIN){ if(cand<i){ size_t L=0,mx=b-i; while(L<mx&&allreg[cand+L]==allreg[i+L])++L; if(L>=MINMATCH&&L>bestL){bestL=L;bestP=cand;if(L==mx)break;} } cand=prevp[cand]; ++chain; } }
                size_t step; (void)bestP;
                if(bestL>=MINMATCH){ tokstream.push_back(block_get(&allreg[i],bestL,uint32_t(bestP),(bestP+bestL<=roff[t])?1:0)); step=bestL; }
                else { tokstream.push_back(allreg[i]); step=1; }
                for(size_t j=i;j<i+step;++j){ if(j+MINMATCH<=NS){ uint64_t g=kgram(j); prevp[j]=head[g]; head[g]=uint32_t(j); } }
                i+=step; }
            tokoff.push_back(tokstream.size()); }
        fprintf(stderr,"S1 LZ: %.1fs tokens=%zu blocks=%zu (%.4f tok/region)\n",secs(tb),tokstream.size(),boff2.size()-1,double(tokstream.size())/NS);
    } else { // V1: root = raw region-id sequence
        for(size_t t=0;t<TUs;++t){ for(size_t i=roff[t];i<roff[t+1];++i) tokstream.push_back(allreg[i]); tokoff.push_back(tokstream.size()); }
    }

    // ===== ENCODER (C) + DECODER (F): one cold chronological pass, PULL protocol (root -> MISSING -> FILL) =====
    // C tracks F's known sets (single F) so it computes exactly the missing closure. Every wire byte is
    // charged by category, compressed at z<=zlevel per message. F reconstructs .ii bytes byte-exact.
    ZSTD_CCtx* z=ZSTD_createCCtx();ZSTD_CCtx* sourceCostZ=useProjectSource?ZSTD_createCCtx():nullptr;std::vector<uint8_t> dst,sourceCostDst;
    std::vector<uint8_t> fknownLine(dict.distinct()+1,0), fknownReg(NREG,0);
    std::vector<uint32_t> ClineToF(dict.distinct()+1,0); uint32_t nextFline=1;
    std::array<ZSTD_CCtx*,5> lineZC{}; std::array<ZSTD_DCtx*,5> lineZD{};
    std::array<uint8_t,5> lineZActive{}; size_t linePartCount=useByteArrayLines?5:3;
    if(useSortedLines) for(size_t i=0;i<linePartCount;++i){
        lineZC[i]=ZSTD_createCCtx(); lineZD[i]=ZSTD_createDCtx();
        ZSTD_CCtx_setParameter(lineZC[i],ZSTD_c_compressionLevel,zlevel);
        ZSTD_CCtx_setParameter(lineZC[i],ZSTD_c_contentSizeFlag,0);
    }
    std::array<ZSTD_CCtx*,6> mixedZC{}; std::array<ZSTD_DCtx*,6> mixedZD{};
    std::array<uint8_t,6> mixedZActive{};size_t mixedPartCount=useProjectSource?6:(useByteArrayLines?4:2);
    if(useMixedRegions) for(size_t i=0;i<mixedPartCount;++i){
        mixedZC[i]=ZSTD_createCCtx(); mixedZD[i]=ZSTD_createDCtx();
        ZSTD_CCtx_setParameter(mixedZC[i],ZSTD_c_compressionLevel,zlevel);
        ZSTD_CCtx_setParameter(mixedZC[i],ZSTD_c_contentSizeFlag,0);
    }
    std::vector<MixedCLineState> mixedCLine;
    if(useMixedRegions)mixedCLine.resize(dict.distinct()+1);
    uint32_t nextMixedPublic=1;SourceTextStore mixedCSource(useProjectSource),mixedFSource;
    std::vector<uint8_t> mixedSourceSent;
    std::vector<SourceAdmission> mixedSourceAdmission;
    std::unordered_map<std::string,uint32_t> pathid; std::vector<std::string> paths;   // D1 path objects (both sides derive same order)
    // ---- F's OWN independent store, built ONLY from decoded wire bytes (proves self-describing) ----
    std::vector<uint8_t> Fline_data; std::vector<size_t> Fline_off; Fline_off.push_back(0);   // line id k (1-based) -> [off[k-1],off[k])
    Fline_data.reserve(64u<<20);
    std::vector<uint32_t> Freg_child; std::vector<size_t> Freg_off; Freg_off.push_back(0);     // region id k (0-based) -> [off[k],off[k+1])
    std::vector<uint8_t> FmixedRegionData; std::vector<size_t> FmixedRegionOff{0};
    std::vector<MixedFLineView> FmixedPublic(1);
    std::vector<std::string> Fpaths;
    std::vector<uint8_t> fknownBlk(useS1?boff2.size():1,0);   // C's model of F's known blocks
    std::vector<uint32_t> Fblk_child; std::vector<size_t> Fblk_off; Fblk_off.push_back(0);      // F block k -> region ids
    std::vector<uint32_t> Freg_stream; Freg_stream.reserve(allreg.size());   // F's reconstructed region occurrence stream (for block COPY defs)
    std::vector<uint32_t> Froot_child; std::vector<size_t> Froot_off; Froot_off.push_back(0);   // completed exact Roots for P22 slices
    double w_blockdef=0;
    // wire byte accumulators (post-z, per category) + f-checkpoint tracking
    double w_root=0, w_linedef=0, w_regiondef=0, w_pathdef=0, w_missing=0, w_framing=0;
    std::array<double,6> mixedPartWire{};std::array<uint64_t,7> mixedOps{};uint64_t mixedLiteralRaw=0,mixedArrayValues=0,mixedSourceBytes=0;
    uint64_t mixedSourcePackageRaw=0,mixedSourcePackageFiles=0,mixedSourcePotentialRaw=0,mixedSourceConsidered=0,mixedSourceAdmitted=0,mixedSourceEstimatedCost=0;double mixedSelectorWire=0;
    const double FRAME=4;   // 4-byte length prefix per framed message
    uint64_t cum_raw=0; double cum_wire=0;
    // f-checkpoints + H200 trailing window
    std::vector<double> ck_f={0.10,0.25,0.50,0.75,1.00}; std::vector<std::pair<double,double>> ck; // (cum_raw_frac target hit -> ratio) recorded
    size_t ckidx=0; std::vector<double> perTU_raw(TUs), perTU_wire(TUs);
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

    int npass = warm?2:1;   // --warm: pass 0 primes dict+F-stores (uncounted); final pass measures warm steady-state.
    for(int pass=0; pass<npass; ++pass){
      if(pass+1==npass && npass>1){   // reset all measurement state before the warm pass; keep fknown* flags + F-stores
        w_root=w_linedef=w_regiondef=w_pathdef=w_blockdef=w_missing=w_framing=0; cum_raw=0; cum_wire=0; n_marker=n_literal=0; byteexact=true;
        ck.clear(); ckidx=0; allLineDefs.clear(); allRoots.clear(); allRegions.clear(); allRegionsRaw.clear(); allBlocks.clear(); allPaths.clear(); allMiss.clear();
      }
      auto tpass=Clock::now(); double enc_s=0, dec_s=0;   // split C-encode vs F-decode wall (2-proc per-stream proxy)
      for(size_t t=0; t<TUs; ++t){
        auto _te=Clock::now();
        const uint32_t* tk=&tokstream[tokoff[t]]; size_t tn=tokoff[t+1]-tokoff[t];
        // --- collect NEW regions (incl. new blocks' child regions) + NEW blocks, topological order ---
        std::vector<uint32_t> missReg, missBlk;
        auto addRegion=[&](uint32_t r){ if(fknownReg[r])return; for(uint32_t x:missReg) if(x==r) return; missReg.push_back(r); };
        if(usePriorRoot){
            for(size_t i=roff[t];i<roff[t+1];++i) addRegion(allreg[i]);
        } else for(size_t i=0;i<tn;++i){ uint32_t tok=tk[i];
            if(tok<NREG) addRegion(tok);
            else { uint32_t k=tok-NREG; if(!fknownBlk[k]){ bool dup=false; for(uint32_t x:missBlk) if(x==k){dup=true;break;} if(!dup){ for(size_t j=boff2[k];j<boff2[k+1];++j) addRegion(bchild[j]); missBlk.push_back(k); } } } }
        // MISSING = unknown region ids + unknown block ids (F requests; real round-trip in 2-proc)
        { std::vector<uint8_t> mm; put_varint(mm,missReg.size()); for(uint32_t r:missReg) put_varint(mm,r); put_varint(mm,missBlk.size()); for(uint32_t k:missBlk) put_varint(mm,NREG+k);
          if(!missReg.empty()||!missBlk.empty()){ w_missing += zstd_size(z,mm.data(),mm.size(),zlevel,dst) + FRAME; allMiss.insert(allMiss.end(),mm.begin(),mm.end()); } }
        // --- FILL: new paths, new lines, new region defs, new block defs (topological) ---
        std::vector<uint8_t> fill_paths, fill_lines, fill_regions, fill_regions_raw, fill_blocks; uint32_t np=0,nl=0,nr=0,nb=0;
        std::vector<uint32_t> newLineIds;
        std::array<std::vector<uint8_t>,5> lineRaw, lineEncoded;
        std::array<std::vector<uint8_t>,6> mixedRaw, mixedEncoded;
        std::vector<GeneratedByteArray> mixedArrayEntries;
        std::vector<std::pair<uint32_t,const SourceText*>> mixedSourceDefinitions;
        if(!useMixedRegions) for(uint32_t r:missReg){ const uint32_t* lids=dict.region_ids_ptr(r); uint32_t c=dict.region_ids_count(r);
            for(uint32_t j=0;j<c;++j){ uint32_t ln=lids[j]; if(fknownLine[ln]) continue; fknownLine[ln]=1; ++nl;
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
            for(uint32_t r:missReg){
                const uint32_t* lids=dict.region_ids_ptr(r); uint32_t count=dict.region_ids_count(r),offset=0,literalLength=0;
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
                auto flushLiteral=[&](){ if(!literalLength)return; mixedRaw[0].push_back(0);put_varint(mixedRaw[0],literalLength);++mixedOps[0];literalLength=0; };
                for(uint32_t j=0;j<count;++j){
                    uint32_t lineId=lids[j];const LineRef&line=dict.ref(lineId);const char*text=dict.line_data(line.off);MixedCLineState&state=mixedCLine[lineId];
                    if(state.public_id){
                        flushLiteral();mixedRaw[0].push_back(2);put_varint(mixedRaw[0],state.public_id);++mixedOps[2];
                    } else if(state.source_region!=UINT32_MAX&&state.source_region!=r){
                        if(state.source_region>=r||state.source_offset+line.len>dict.region_raw_len(state.source_region)||
                           memcmp(dict.region_data(state.source_region)+state.source_offset,text,line.len)){
                            fprintf(stderr,"bad mixed source Line\n");return 2;
                        }
                        flushLiteral();mixedRaw[0].push_back(1);put_zigzag(mixedRaw[0],int64_t(state.source_region)-int64_t(r));
                        put_varint(mixedRaw[0],state.source_offset);put_varint(mixedRaw[0],line.len);
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
                            flushLiteral();mixedRaw[0].push_back(5);put_varint(mixedRaw[0],sourcePath);put_varint(mixedRaw[0],sourceLine);++mixedOps[5];mixedSourceBytes+=line.len;
                        } else if(arrayLine){
                            mixedArrayValues+=parsed.values.size();flushLiteral();mixedRaw[0].push_back(3);mixedArrayEntries.push_back(std::move(parsed));++mixedOps[3];++n_literal;
                        } else if(markerLine){
                            uint32_t pathId=ensurePathId(lineMarker.path);
                            flushLiteral();mixedRaw[0].push_back(4);put_varint(mixedRaw[0],pathId);put_varint(mixedRaw[0],lineMarker.lineno);
                            put_varint(mixedRaw[0],lineMarker.flags.size());for(uint8_t flag:lineMarker.flags)mixedRaw[0].push_back(flag);++mixedOps[4];++n_marker;
                        } else if(sourcePatch&&sourceAdmitted){
                            if(sourcePath==UINT32_MAX)sourcePath=ensurePathId(regionMarker.path);
                            ensureSourceDefinition(regionMarker.path,sourcePath,*regionSource);
                            flushLiteral();mixedRaw[0].push_back(6);put_varint(mixedRaw[0],sourcePath);put_varint(mixedRaw[0],sourceLine);
                            put_varint(mixedRaw[0],patchPrefix);put_varint(mixedRaw[0],patchSuffix);put_varint(mixedRaw[0],patchMiddle);mixedRaw[1].insert(mixedRaw[1].end(),text+patchPrefix,text+patchPrefix+patchMiddle);
                            ++mixedOps[6];mixedLiteralRaw+=patchMiddle;mixedSourceBytes+=patchPrefix+patchSuffix;++n_literal;
                        } else {mixedRaw[1].insert(mixedRaw[1].end(),text,text+line.len);literalLength+=line.len;mixedLiteralRaw+=line.len;++n_literal;}
                        if(admission&&!admission->admitted){
                            uint64_t benefit=sourceCopy?(arrayLine?arrayValueCount:(markerLine?0:line.len)):
                                ((!arrayLine&&!markerLine&&sourcePatch)?uint64_t(patchPrefix)+patchSuffix:0);
                            admission->observed_benefit+=benefit;admission->last_observed_tu=t;mixedSourcePotentialRaw+=benefit;
                        }
                    }
                    offset+=line.len;
                }
                flushLiteral();
                if(offset!=dict.region_raw_len(r)){fprintf(stderr,"mixed Region length differs\n");return 2;}
                fknownReg[r]=1;++nr;
            }
            if(!mixedArrayEntries.empty()){
                std::vector<ByteArrayStyle> styles;styles.reserve(mixedArrayEntries.size());
                for(const auto&value:mixedArrayEntries)styles.push_back({value.prefix,value.separator,value.suffix,value.format});
                std::sort(styles.begin(),styles.end());styles.erase(std::unique(styles.begin(),styles.end()),styles.end());
                put_varint(mixedRaw[2],styles.size());
                for(const auto&style:styles){put_varint(mixedRaw[2],style.format);
                    for(const std::string*field:{&style.prefix,&style.separator,&style.suffix}){put_varint(mixedRaw[2],field->size());mixedRaw[2].insert(mixedRaw[2].end(),field->begin(),field->end());}}
                put_varint(mixedRaw[2],mixedArrayEntries.size());
                for(const auto&value:mixedArrayEntries){ByteArrayStyle style{value.prefix,value.separator,value.suffix,value.format};
                    size_t styleId=std::lower_bound(styles.begin(),styles.end(),style)-styles.begin();put_varint(mixedRaw[2],styleId);put_varint(mixedRaw[2],value.values.size());
                    mixedRaw[3].insert(mixedRaw[3].end(),value.values.begin(),value.values.end());}
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
            for(uint32_t ln:newLineIds) ClineToF[ln]=nextFline++;
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
        if(useSortedLines && t+1==TUs){
            const std::vector<uint8_t> empty;
            for(size_t i=0;i<linePartCount;++i) if(lineZActive[i]){
                std::vector<uint8_t> tail=zstd_stream_encode(lineZC[i],empty,ZSTD_e_end);
                if(lineEncoded[i].empty()&&!tail.empty())w_linedef+=FRAME;
                w_linedef+=tail.size(); lineEncoded[i].insert(lineEncoded[i].end(),tail.begin(),tail.end());
            }
        }
        if(useMixedRegions&&nr){
            for(size_t i=0;i<mixedPartCount;++i) if(!mixedRaw[i].empty()){
                mixedEncoded[i]=zstd_stream_encode(mixedZC[i],mixedRaw[i],ZSTD_e_flush);mixedZActive[i]=1;
                double bytes=mixedEncoded[i].size()+FRAME;mixedPartWire[i]+=bytes;(i==0?w_regiondef:w_linedef)+=bytes;
            }
            if(useByteArrayLines){w_linedef+=1;mixedSelectorWire+=1;}
        }
        if(useMixedRegions&&t+1==TUs){
            const std::vector<uint8_t> empty;
            for(size_t i=0;i<mixedPartCount;++i) if(mixedZActive[i]){
                std::vector<uint8_t> tail=zstd_stream_encode(mixedZC[i],empty,ZSTD_e_end);
                double&wire=i==0?w_regiondef:w_linedef;if(mixedEncoded[i].empty()&&!tail.empty()){wire+=FRAME;mixedPartWire[i]+=FRAME;}
                wire+=tail.size();mixedPartWire[i]+=tail.size();mixedEncoded[i].insert(mixedEncoded[i].end(),tail.begin(),tail.end());
            }
        }
        if(!useMixedRegions) for(uint32_t r:missReg){ const uint32_t* lids=dict.region_ids_ptr(r); uint32_t c=dict.region_ids_count(r);
            put_varint(fill_regions,c); { int64_t prev=0; for(uint32_t j=0;j<c;++j){
                uint32_t wireLine=useSortedLines?ClineToF[lids[j]]:lids[j]; if(!wireLine){fprintf(stderr,"missing Line mapping\n");return 2;}
                put_zigzag(fill_regions,int64_t(wireLine)-prev); prev=int64_t(wireLine); } } fknownReg[r]=1; ++nr;
            put_varint(fill_regions_raw,c); for(uint32_t j=0;j<c;++j){ uint32_t wireLine=useSortedLines?ClineToF[lids[j]]:lids[j]; put_varint(fill_regions_raw,wireLine); }
            { put_varint(allRegionsRaw,c); for(uint32_t j=0;j<c;++j){ uint32_t wireLine=useSortedLines?ClineToF[lids[j]]:lids[j]; put_varint(allRegionsRaw,wireLine); } }
        }
        for(uint32_t k:missBlk){ size_t L=boff2[k+1]-boff2[k];
            if(bcopy_ok[k]){ fill_blocks.push_back(1); put_varint(fill_blocks,bcopy_src[k]); put_varint(fill_blocks,L); }   // COPY(region-stream src,len)
            else { fill_blocks.push_back(0); put_varint(fill_blocks,L); for(size_t j=boff2[k];j<boff2[k+1];++j) put_varint(fill_blocks,bchild[j]); }
            fknownBlk[k]=1; ++nb; }
        if(np){ w_pathdef += zstd_size(z,fill_paths.data(),fill_paths.size(),zlevel,dst); allPaths.insert(allPaths.end(),fill_paths.begin(),fill_paths.end()); }
        if(nl && !useSortedLines){ w_linedef += zstd_size(z,fill_lines.data(),fill_lines.size(),zlevel,dst); allLineDefs.insert(allLineDefs.end(),fill_lines.begin(),fill_lines.end()); }
        bool reg_raw=false;
        if(nr&&!useMixedRegions){ double dz=zstd_size(z,fill_regions.data(),fill_regions.size(),zlevel,dst);
                double rz=zstd_size(z,fill_regions_raw.data(),fill_regions_raw.size(),zlevel,dst);
                reg_raw = rz<dz; w_regiondef += (reg_raw?rz:dz) + 1;   // +1 byte serialization flag (delta vs raw line-ids)
                allRegions.insert(allRegions.end(),fill_regions.begin(),fill_regions.end()); }
        if(nb){ w_blockdef += zstd_size(z,fill_blocks.data(),fill_blocks.size(),zlevel,dst); allBlocks.insert(allBlocks.end(),fill_blocks.begin(),fill_blocks.end()); }
        if(np||nl||nr||nb) w_framing += FRAME;
        // --- ROOT: token stream (region + block ids) ---
        std::vector<uint8_t> rootb;
        if(usePriorRoot) rootb=root_slices.programs[t];
        else for(size_t i=0;i<tn;++i) put_varint(rootb,tk[i]);
        w_root += zstd_size(z,rootb.data(),rootb.size(),zlevel,dst); w_framing += FRAME; allRoots.insert(allRoots.end(),rootb.begin(),rootb.end());

        enc_s += std::chrono::duration<double>(Clock::now()-_te).count(); auto _td=Clock::now();
        // --- DECODER (F): install FILL from wire into F's OWN store, then expand ROOT tokens ---
        { const uint8_t* pp=fill_paths.data(); for(uint32_t k=0;k<np;++k){ uint64_t L=get_varint(pp); Fpaths.emplace_back((const char*)pp,(size_t)L); pp+=L; } }
        if(useMixedRegions){
          std::array<std::vector<uint8_t>,6> recovered;
          for(size_t i=0;i<mixedPartCount;++i) if(!mixedEncoded[i].empty()){
            size_t remaining=1;recovered[i]=zstd_stream_decode(mixedZD[i],mixedEncoded[i],remaining);
            if(recovered[i]!=mixedRaw[i]||(t+1==TUs&&remaining!=0)){fprintf(stderr,"mixed Region stream mismatch TU=%zu part=%zu\n",t,i);return 2;}
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
          const uint8_t*ap=recovered[2].data(),*ae=ap+recovered[2].size(),*vp=recovered[3].data(),*ve=vp+recovered[3].size();
          std::vector<ByteArrayStyle> mixedStyles;uint64_t arrayCount=0,arraysUsed=0;
          if(!recovered[2].empty()){
            uint64_t styleCount=get_varint(ap);mixedStyles.reserve(styleCount);
            for(uint64_t k=0;k<styleCount;++k){ByteArrayStyle style;uint64_t format=get_varint(ap);if(format>HEX_UU){fprintf(stderr,"bad mixed array format\n");return 2;}style.format=uint8_t(format);
              for(std::string*field:{&style.prefix,&style.separator,&style.suffix}){uint64_t size=get_varint(ap);if(size>uint64_t(ae-ap)){fprintf(stderr,"bad mixed array style\n");return 2;}field->assign((const char*)ap,size);ap+=size;}mixedStyles.push_back(std::move(style));}
            arrayCount=get_varint(ap);
          } else if(!recovered[3].empty()){fprintf(stderr,"partial mixed array streams\n");return 2;}
          if(!recovered[0].empty()){
            const uint8_t*cp=recovered[0].data(),*ce=cp+recovered[0].size();const uint8_t*lp=recovered[1].data(),*le=lp+recovered[1].size();
            uint64_t regionCount=get_varint(cp);if(regionCount!=nr||regionCount!=missReg.size()){fprintf(stderr,"mixed Region count differs\n");return 2;}
            for(uint64_t k=0;k<regionCount;++k){
              uint32_t regionId=uint32_t(FmixedRegionOff.size()-1);if(regionId!=missReg[k]){fprintf(stderr,"mixed Region identity differs\n");return 2;}
              uint64_t rawLength=get_varint(cp);size_t begin=FmixedRegionData.size();
              while(FmixedRegionData.size()-begin<rawLength){
                if(cp>=ce){fprintf(stderr,"truncated mixed Region control\n");return 2;}uint8_t op=*cp++;
                if(op==0){uint64_t length=get_varint(cp);if(length>uint64_t(le-lp)){fprintf(stderr,"truncated mixed literal\n");return 2;}FmixedRegionData.insert(FmixedRegionData.end(),lp,lp+length);lp+=length;}
                else if(op==1){int64_t source=int64_t(regionId)+get_zigzag(cp);uint64_t offset=get_varint(cp),length=get_varint(cp);
                  if(source<0||uint64_t(source+1)>=FmixedRegionOff.size()||offset+length>FmixedRegionOff[source+1]-FmixedRegionOff[source]){fprintf(stderr,"bad mixed publish view\n");return 2;}
                  size_t sourceBegin=FmixedRegionOff[source]+offset,destination=FmixedRegionData.size();FmixedRegionData.resize(destination+length);memcpy(FmixedRegionData.data()+destination,FmixedRegionData.data()+sourceBegin,length);
                  FmixedPublic.push_back({uint32_t(source),uint32_t(offset),uint32_t(length)});
                } else if(op==2){uint64_t publicId=get_varint(cp);if(!publicId||publicId>=FmixedPublic.size()){fprintf(stderr,"bad mixed public ref\n");return 2;}
                  const MixedFLineView&view=FmixedPublic[publicId];size_t sourceBegin=FmixedRegionOff[view.source_region]+view.source_offset,destination=FmixedRegionData.size();
                  FmixedRegionData.resize(destination+view.length);memcpy(FmixedRegionData.data()+destination,FmixedRegionData.data()+sourceBegin,view.length);
                } else if(op==3){if(arraysUsed>=arrayCount){fprintf(stderr,"missing mixed array record\n");return 2;}uint64_t styleId=get_varint(ap),count=get_varint(ap);
                  if(styleId>=mixedStyles.size()||count>uint64_t(ve-vp)){fprintf(stderr,"bad mixed array record\n");return 2;}
                  append_rendered_byte_array(mixedStyles[styleId],vp,count,FmixedRegionData);vp+=count;++arraysUsed;
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
              FmixedRegionOff.push_back(FmixedRegionData.size());
            }
            if(cp!=ce||lp!=le||ap!=ae||vp!=ve||arraysUsed!=arrayCount||FmixedPublic.size()!=nextMixedPublic){fprintf(stderr,"mixed Region streams have trailing bytes or state differs\n");return 2;}
          } else if(nr||!recovered[1].empty()){fprintf(stderr,"partial mixed Region streams\n");return 2;}
        } else if(useSortedLines){
          std::array<std::vector<uint8_t>,5> recovered;
          for(size_t i=0;i<linePartCount;++i) if(!lineEncoded[i].empty()){
            size_t remaining=1; recovered[i]=zstd_stream_decode(lineZD[i],lineEncoded[i],remaining);
            if(recovered[i]!=lineRaw[i] || (t+1==TUs && remaining!=0)){fprintf(stderr,"sorted Line stream mismatch TU=%zu part=%zu raw=%zu recovered=%zu encoded=%zu remaining=%zu\n",t,i,lineRaw[i].size(),recovered[i].size(),lineEncoded[i].size(),remaining);return 2;}
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
        { const uint8_t* pp=fill_blocks.data(), *pe=fill_blocks.data()+fill_blocks.size();
          while(pp<pe){ uint8_t kind=*pp++;
            if(kind==1){ uint64_t src=get_varint(pp); uint64_t L=get_varint(pp); for(uint64_t j=0;j<L;++j) Fblk_child.push_back(Freg_stream[src+j]); Fblk_off.push_back(Fblk_child.size()); }
            else { uint64_t L=get_varint(pp); for(uint64_t j=0;j<L;++j) Fblk_child.push_back(uint32_t(get_varint(pp))); Fblk_off.push_back(Fblk_child.size()); } } }
        recon.clear();
        auto emitRegionF=[&](uint32_t r){ Freg_stream.push_back(r);
          if(useMixedRegions)recon.insert(recon.end(),FmixedRegionData.begin()+FmixedRegionOff[r],FmixedRegionData.begin()+FmixedRegionOff[r+1]);
          else for(size_t j=Freg_off[r];j<Freg_off[r+1];++j){ uint32_t ln=Freg_child[j]; recon.insert(recon.end(), Fline_data.begin()+Fline_off[ln-1], Fline_data.begin()+Fline_off[ln]); } };
        { const uint8_t* pp=rootb.data(), *pe=rootb.data()+rootb.size();
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
          } else while(pp<pe){ uint32_t tok=uint32_t(get_varint(pp));
            if(tok<NREG) emitRegionF(tok);
            else { uint32_t k=tok-NREG; for(size_t j=Fblk_off[k];j<Fblk_off[k+1];++j) emitRegionF(Fblk_child[j]); } }
        }
        dec_s += std::chrono::duration<double>(Clock::now()-_td).count();   // F-decode ends here; the verify below is harness-only (F doesn't have the original)
        const char* orig=corpus.bytes.data()+corpus.files[t].off; uint32_t olen=corpus.files[t].len;
        if(recon.size()!=olen || memcmp(recon.data(),orig,olen)!=0){ byteexact=false; if(t<5||TUs<10) fprintf(stderr,"BYTE-EXACT FAIL TU %zu (%zu vs %u)\n",t,recon.size(),olen); }
        cum_raw += olen;
        double cur_wire = w_root+w_linedef+w_regiondef+w_pathdef+w_blockdef+w_missing+w_framing;
        perTU_raw[t]=olen; perTU_wire[t]=cur_wire - cum_wire; cum_wire=cur_wire;
        while(ckidx<ck_f.size() && double(cum_raw)>=ck_f[ckidx]*corpus.raw){ ck.push_back({ck_f[ckidx], double(cum_raw)/cum_wire}); ++ckidx; }
      }
      fprintf(stderr,"pass %d (%s) single-core encode+decode+verify: %.2fs = %.2f GB/s raw\n", pass, (pass+1==npass&&npass>1)?"WARM":"cold", secs(tpass), corpus.raw/1e9/secs(tpass));
      fprintf(stderr,"  split (2-proc per-stream proxy): C-encode %.2f GB/s | F-decode %.2f GB/s => pipelined min = %.2f GB/s\n",
              corpus.raw/1e9/enc_s, corpus.raw/1e9/dec_s, corpus.raw/1e9/std::max(enc_s,dec_s));
    }
    while(ck.size()<ck_f.size()) ck.push_back({ck_f[ck.size()], double(cum_raw)/cum_wire});
    ZSTD_freeCCtx(z);
    if(sourceCostZ)ZSTD_freeCCtx(sourceCostZ);
    if(useSortedLines) for(size_t i=0;i<linePartCount;++i){ ZSTD_freeCCtx(lineZC[i]); ZSTD_freeDCtx(lineZD[i]); }
    if(useMixedRegions) for(size_t i=0;i<mixedPartCount;++i){ ZSTD_freeCCtx(mixedZC[i]); ZSTD_freeDCtx(mixedZD[i]); }

    double totalwire = w_root+w_linedef+w_regiondef+w_pathdef+w_blockdef+w_missing+w_framing;
    double MiB=1048576.0;
    const char* structure_name=usePriorRoot?"V1+P22(ROOT_SLICE)":(useS1?"V1+S1(LZ blocks)":"V1");
    const char* line_name=useMixedRegions?(useByteArrayLines?"+P24(mixed-regions+BYTE_ARRAY)":"+P24(mixed-regions)"):(useByteArrayLines?"+P21(BYTE_ARRAY)":(useSortedLines?"+P9(sorted-lines)":""));
    printf("\n==== CODEC-50 (%s%s%s%s%s, z%d) — %s ====\n", structure_name, useD1?"+D1":"", line_name, useProjectSource?"+SOURCE_PACKAGE":"", (useD2mine?"+D2(inline)":(useD2?"+D2(helper)":"")), zlevel, manifest);
    printf("byte-exact=%s  TUs=%zu raw=%.1f MiB regions=%u distinct_lines=%u paths=%zu blocks=%zu (marker_lines=%llu literal_lines=%llu)\n",
        byteexact?"OK":"FAIL",TUs,corpus.raw/MiB,NREG,dict.distinct(),paths.size(),boff2.size()-1,(unsigned long long)n_marker,(unsigned long long)n_literal);
    printf("wire by category (post-z%d, bytes): root=%.0f line_def=%.0f region_def=%.0f block_def=%.0f path_def=%.0f missing=%.0f framing=%.0f  TOTAL=%.0f (%.2f MiB)\n",
        zlevel,w_root,w_linedef,w_regiondef,w_blockdef,w_pathdef,w_missing,w_framing,totalwire,totalwire/MiB);
    printf("FinalRatio (raw / total wire, one cold pass) = %.1fx\n", corpus.raw/totalwire);
    if(usePriorRoot) printf("ROOT_SLICE stats: copies=%llu copied_regions=%llu indexed_windows=%llu index_entries=%llu receiver_root_bytes=%zu\n",
        (unsigned long long)root_slices.copies,(unsigned long long)root_slices.copied_regions,
        (unsigned long long)root_slices.indexed_windows,(unsigned long long)root_slices.index_entries,
        Froot_child.size()*sizeof(uint32_t)+Froot_off.size()*sizeof(size_t));
    if(useMixedRegions)printf("mixed components: control=%.0f literal=%.0f array_control=%.0f array_values=%.0f source_control=%.0f source_files=%.0f selector=%.0f raw_literal=%llu raw_array_values=%llu raw_source_reused=%llu source_package_raw=%llu source_package_files=%llu public_lines=%u ops=[literal=%llu publish=%llu ref=%llu array=%llu marker=%llu source=%llu patch=%llu]\n",
        mixedPartWire[0],mixedPartWire[1],mixedPartWire[2],mixedPartWire[3],mixedPartWire[4],mixedPartWire[5],mixedSelectorWire,
        (unsigned long long)mixedLiteralRaw,(unsigned long long)mixedArrayValues,(unsigned long long)mixedSourceBytes,(unsigned long long)mixedSourcePackageRaw,(unsigned long long)mixedSourcePackageFiles,nextMixedPublic-1,
        (unsigned long long)mixedOps[0],(unsigned long long)mixedOps[1],(unsigned long long)mixedOps[2],(unsigned long long)mixedOps[3],(unsigned long long)mixedOps[4],(unsigned long long)mixedOps[5],(unsigned long long)mixedOps[6]);
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
    // trailing-window (5% raw) ratio near the end
    { double win=0.05*corpus.raw, r=0,wsum=0; for(size_t t=TUs;t-->0;){ r+=perTU_raw[t]; wsum+=perTU_wire[t]; if(r>=win) break; } printf("trailing 5%%-raw window ratio (steady) = %.0fx\n", wsum>0?r/wsum:0); }
    struct rusage ru{}; getrusage(RUSAGE_SELF,&ru); fprintf(stderr,"peak RSS=%.1f MiB total=%.1fs\n",ru.ru_maxrss/1024.0,secs(t0));
    return byteexact?0:1;
}
