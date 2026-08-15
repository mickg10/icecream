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
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <string>
#include <unordered_map>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
#include <thread>
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
    if(n<=8) return mix64(h^read_tail(p,n)); if(n<=16) return fold128(read64(p)^A,read64(p+n-8)^h);
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

class Interner {
public:
    static constexpr uint32_t TINY_CAP=1u<<10, SHORT_CAP=1u<<17, LINE_CAP=1u<<21, REGION_INITIAL_CAP=1u<<17;
    Interner(){ tiny_=(TinySlot*)huge_zeroed(sizeof(TinySlot)*TINY_CAP); short_=(ShortSlot*)huge_zeroed(sizeof(ShortSlot)*SHORT_CAP); lines_=(LineSlot*)huge_zeroed(sizeof(LineSlot)*LINE_CAP);
        region_index_.resize(REGION_INITIAL_CAP); region_mask_=REGION_INITIAL_CAP-1; region_records_.reserve(1u<<20); line_bytes_.reserve(64u<<20); region_bytes_.reserve(80u<<20); region_ids_.reserve(8u<<20); id_refs_.reserve(1000000); id_refs_.push_back({0,0}); }
    uint32_t distinct() const { return next_id_-1; }
    uint64_t region_count() const { return region_records_.size(); }
    const LineRef& ref(uint32_t id) const { return id_refs_[id]; }
    const char* line_data(uint32_t off) const { return line_bytes_.data()+off; }
    const uint32_t* region_ids_ptr(uint32_t rid) const { return region_ids_.data()+region_records_[rid].ids_off; }
    uint32_t region_ids_count(uint32_t rid) const { return region_records_[rid].ids_count; }
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
static size_t zstd_size(ZSTD_CCtx*c,const uint8_t*data,size_t n,int level,std::vector<uint8_t>&dst){ ZSTD_CCtx_reset(c,ZSTD_reset_session_and_parameters); ZSTD_CCtx_setParameter(c,ZSTD_c_compressionLevel,level);
    size_t bound=ZSTD_compressBound(n); if(dst.size()<bound)dst.resize(bound); size_t r=ZSTD_compress2(c,dst.data(),dst.size(),data?data:(const uint8_t*)"",n); if(ZSTD_isError(r)){fprintf(stderr,"zstd %s\n",ZSTD_getErrorName(r));exit(2);} return r; }
// Persistent shared-window streaming: feed one message into a retained stream + flush; returns the
// flushed output bytes (captures cross-message redundancy a per-message reset can't). Accounting only
// -- in-process F reads the raw buffer, so reconstruction stays byte-exact regardless.
static size_t stream_flush(ZSTD_CStream*cs,const uint8_t*data,size_t n,std::vector<uint8_t>&ob){
    if(ob.size()<ZSTD_CStreamOutSize()) ob.resize(ZSTD_CStreamOutSize());
    ZSTD_inBuffer in{data,n,0}; size_t total=0;
    while(in.pos<in.size){ ZSTD_outBuffer o{ob.data(),ob.size(),0}; size_t r=ZSTD_compressStream2(cs,&o,&in,ZSTD_e_continue); if(ZSTD_isError(r)){fprintf(stderr,"zstream %s\n",ZSTD_getErrorName(r));exit(2);} total+=o.pos; }
    for(;;){ ZSTD_outBuffer o{ob.data(),ob.size(),0}; size_t rem=ZSTD_compressStream2(cs,&o,&in,ZSTD_e_flush); if(ZSTD_isError(rem)){fprintf(stderr,"zstream %s\n",ZSTD_getErrorName(rem));exit(2);} total+=o.pos; if(rem==0) break; }
    return total; }

// ---- D1: preprocessor marker factoring. Parse "# <n> \"<path>\"<flags>\n" -> (path,n,flags), and
// reconstruct EXACT bytes; fall back to literal if reconstruction != original. ----
struct Marker{ std::string path; uint64_t lineno; std::vector<uint8_t> flags; };
static bool parse_marker(const char* s, uint32_t len, Marker& m){
    if(len<4 || s[0]!='#' || s[1]!=' ') return false; const char* e=s+len; const char* p=s+2;
    if(p>=e || *p<'0'||*p>'9') return false; uint64_t n=0; while(p<e && *p>='0'&&*p<='9'){ n=n*10+(*p-'0'); ++p; } m.lineno=n;
    if(p+2>e || p[0]!=' '||p[1]!='"') return false; p+=2; const char* q=p; while(q<e && *q!='"') ++q; if(q>=e) return false; m.path.assign(p,q); p=q+1;
    m.flags.clear(); while(p<e && *p==' '){ ++p; if(p>=e||*p<'0'||*p>'9') return false; uint8_t fl=0; while(p<e&&*p>='0'&&*p<='9'){ fl=fl*10+(*p-'0'); ++p; } m.flags.push_back(fl); }
    if(p>=e || *p!='\n' || p+1!=e) return false;   // must end exactly with newline
    return true;
}
static void emit_marker(const Marker& m, std::vector<uint8_t>& out){ char buf[32]; int l=snprintf(buf,sizeof buf,"# %llu \"",(unsigned long long)m.lineno); out.insert(out.end(),buf,buf+l); out.insert(out.end(),m.path.begin(),m.path.end()); out.push_back('"'); for(uint8_t f:m.flags){ out.push_back(' '); l=snprintf(buf,sizeof buf,"%u",f); out.insert(out.end(),buf,buf+l);} out.push_back('\n'); }

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

// ---- Real 2-process socketpair path: F-side independent store + standalone packet decoder. ----
// A per-TU wire packet = [u8 reg_raw][z paths][z lines][z regions][z blocks][z root] (each block =
// varint(zlen)+zstd bytes). decode_packet installs the defs into F's OWN store and reconstructs the
// exact .ii bytes -- the identical object model as the in-process decoder, driven only by wire bytes.
struct FState {
    std::vector<uint8_t> line_data; std::vector<size_t> line_off;   // line id (1-based) -> bytes
    std::vector<uint32_t> reg_child; std::vector<size_t> reg_off;   // region id -> line-id composition
    std::vector<std::string> paths;
    std::vector<uint32_t> blk_child; std::vector<size_t> blk_off;
    std::vector<uint32_t> reg_stream;                               // reconstructed region occurrence stream (block-COPY srcs)
    void init(){ line_data.clear(); line_off.assign(1,0); reg_child.clear(); reg_off.assign(1,0); paths.clear(); blk_child.clear(); blk_off.assign(1,0); reg_stream.clear(); line_data.reserve(64u<<20); }
};
static const uint8_t* unz(ZSTD_DCtx* d,const uint8_t* p,std::vector<uint8_t>& out){
    uint64_t zl=get_varint(p); unsigned long long rl=ZSTD_getFrameContentSize(p,(size_t)zl); out.resize((size_t)rl);
    size_t r=ZSTD_decompressDCtx(d,out.data(),out.size(),p,(size_t)zl); if(ZSTD_isError(r)){fprintf(stderr,"unz %s\n",ZSTD_getErrorName(r));exit(2);} return p+zl; }
static void decode_packet(uint32_t NREG,FState& F,const uint8_t* pkt,ZSTD_DCtx* d,
        std::vector<uint8_t>& fp,std::vector<uint8_t>& fl,std::vector<uint8_t>& fr,std::vector<uint8_t>& fb,std::vector<uint8_t>& rt,
        std::vector<uint8_t>& recon,std::vector<uint8_t>& tmp){
    const uint8_t* p=pkt; uint8_t reg_raw=*p++;
    p=unz(d,p,fp); p=unz(d,p,fl); p=unz(d,p,fr); p=unz(d,p,fb); p=unz(d,p,rt);
    { const uint8_t* pp=fp.data(),*pe=fp.data()+fp.size(); while(pp<pe){ uint64_t L=get_varint(pp); F.paths.emplace_back((const char*)pp,(size_t)L); pp+=L; } }
    { const uint8_t* pp=fl.data(),*pe=fl.data()+fl.size();
      while(pp<pe){ uint8_t kind=*pp++;
        if(kind==1){ uint64_t pid=get_varint(pp); uint64_t lineno=get_varint(pp); uint8_t nf=*pp++; Marker dm; dm.path=F.paths[pid]; dm.lineno=lineno; for(uint8_t f=0;f<nf;++f) dm.flags.push_back(*pp++);
            tmp.clear(); emit_marker(dm,tmp); F.line_data.insert(F.line_data.end(),tmp.begin(),tmp.end()); F.line_off.push_back(F.line_data.size()); }
        else { uint64_t len=get_varint(pp); F.line_data.insert(F.line_data.end(),pp,pp+len); pp+=len; F.line_off.push_back(F.line_data.size()); } } }
    if(reg_raw){ const uint8_t* pp=fr.data(),*pe=fr.data()+fr.size(); while(pp<pe){ uint64_t c=get_varint(pp); for(uint64_t j=0;j<c;++j) F.reg_child.push_back((uint32_t)get_varint(pp)); F.reg_off.push_back(F.reg_child.size()); } }
    else { const uint8_t* pp=fr.data(),*pe=fr.data()+fr.size(); while(pp<pe){ uint64_t c=get_varint(pp); int64_t prev=0; for(uint64_t j=0;j<c;++j){ prev+=get_zigzag(pp); F.reg_child.push_back((uint32_t)prev); } F.reg_off.push_back(F.reg_child.size()); } }
    { const uint8_t* pp=fb.data(),*pe=fb.data()+fb.size(); while(pp<pe){ uint8_t kind=*pp++;
        if(kind==1){ uint64_t src=get_varint(pp); uint64_t L=get_varint(pp); for(uint64_t j=0;j<L;++j) F.blk_child.push_back(F.reg_stream[src+j]); F.blk_off.push_back(F.blk_child.size()); }
        else { uint64_t L=get_varint(pp); for(uint64_t j=0;j<L;++j) F.blk_child.push_back((uint32_t)get_varint(pp)); F.blk_off.push_back(F.blk_child.size()); } } }
    recon.clear();
    auto emitR=[&](uint32_t r){ F.reg_stream.push_back(r); for(size_t j=F.reg_off[r];j<F.reg_off[r+1];++j){ uint32_t ln=F.reg_child[j]; recon.insert(recon.end(),F.line_data.begin()+F.line_off[ln-1],F.line_data.begin()+F.line_off[ln]); } };
    { const uint8_t* pp=rt.data(),*pe=rt.data()+rt.size(); while(pp<pe){ uint32_t tok=(uint32_t)get_varint(pp);
        if(tok<NREG) emitR(tok); else { uint32_t k=tok-NREG; for(size_t j=F.blk_off[k];j<F.blk_off[k+1];++j) emitR(F.blk_child[j]); } } }
}
// framed socket read/write (blocking, restart on EINTR)
static bool sock_wall(int fd,const void* p,size_t n){ const char* b=(const char*)p; while(n){ ssize_t w=write(fd,b,n); if(w<=0){ if(w<0&&errno==EINTR) continue; return false; } b+=w; n-=(size_t)w; } return true; }
static bool sock_rall(int fd,void* p,size_t n){ char* b=(char*)p; while(n){ ssize_t r=read(fd,b,n); if(r<=0){ if(r<0&&errno==EINTR) continue; return false; } b+=r; n-=(size_t)r; } return true; }

int main(int argc,char**argv){
    const char* manifest=nullptr; size_t max_files=SIZE_MAX; int zlevel=3; bool useD1=true, useD2=false, useS1=true, useD2mine=false, deep=false, warm=false, useS0=false, useStream=false, useSocket=false; int builds=1;
    for(int i=1;i<argc;++i){ if(!strcmp(argv[i],"--manifest")&&i+1<argc)manifest=argv[++i];
        else if(!strcmp(argv[i],"--z")&&i+1<argc)zlevel=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--no-d1"))useD1=false;
        else if(!strcmp(argv[i],"--v1"))useS1=false;   // V1 baseline: raw region-id root, no S1 blocks
        else if(!strcmp(argv[i],"--d2"))useD2mine=true;      // inline relative-LZ line codec
        else if(!strcmp(argv[i],"--d2helper"))useD2=true;    // helper's definition_codec (needs -DWITH_D2)
        else if(!strcmp(argv[i],"--deep"))deep=true;         // run slow z19/z22 entropy ladder + reorder test
        else if(!strcmp(argv[i],"--warm"))warm=true;         // Basis C: 2nd pass with dict retained -> warm steady-state wire
        else if(!strcmp(argv[i],"--stream"))useStream=true;  // shared-window streaming accounting (persistent zstd window across messages)
        else if(!strcmp(argv[i],"--socket"))useSocket=true;  // real 2-process socketpair + N-stream concurrency throughput sweep
        else if(!strcmp(argv[i],"--s0"))useS0=true;          // S0 semantic-Root memoization (ROOT_REF on exact-Root reuse)
        else if(!strcmp(argv[i],"--builds")&&i+1<argc)builds=atoi(argv[++i]);  // amortize S0 over N chronological builds
        else if(!strcmp(argv[i],"--max-files")&&i+1<argc){ char*t=nullptr; unsigned long long v=strtoull(argv[++i],&t,10); if(!t||*t||!v){fprintf(stderr,"bad max-files\n");return 2;} max_files=size_t(v); }
        else { fprintf(stderr,"unknown %s\n",argv[i]); return 2; } }
    if(!manifest){ fprintf(stderr,"usage: %s --manifest F [--z 0|1|3] [--no-d1] [--d2] [--max-files N]\n",argv[0]); return 2; }
#ifndef HAVE_DEFCODEC
    if(useD2){ fprintf(stderr,"note: --d2 requested but definition_codec.h not present; ignoring.\n"); useD2=false; }
#endif

    auto t0=Clock::now(); Corpus corpus=load_corpus(manifest,max_files); Interner dict;
    std::vector<uint32_t> allreg; std::vector<size_t> roff; roff.push_back(0);
    { uint32_t maxlen=0; for(auto&f:corpus.files) maxlen=std::max(maxlen,f.len); std::vector<uint32_t> out(size_t(maxlen)+1); uint64_t hits=0; std::vector<uint32_t> rs;
      for(auto&f:corpus.files){ size_t oc=0; rs.clear(); const char*p=corpus.bytes.data()+f.off; dict.process(p,p+f.len,out.data(),oc,hits,true,&rs); allreg.insert(allreg.end(),rs.begin(),rs.end()); roff.push_back(allreg.size()); } }
    uint32_t NREG=uint32_t(dict.region_count()); size_t TUs=corpus.files.size();
    fprintf(stderr,"loaded+interned %.1fs TUs=%zu raw=%llu regions=%u region_occ=%zu distinct_lines=%u\n",secs(t0),TUs,(unsigned long long)corpus.raw,NREG,allreg.size(),dict.distinct());

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
    ZSTD_CCtx* z=ZSTD_createCCtx(); std::vector<uint8_t> dst;
    ZSTD_CStream *cs_line=ZSTD_createCStream(), *cs_reg=ZSTD_createCStream(), *cs_root=ZSTD_createCStream(); std::vector<uint8_t> sob;   // shared-window streams (line/region/root)
    std::vector<uint8_t> fknownLine(dict.distinct()+1,0), fknownReg(NREG,0);
    std::unordered_map<std::string,uint32_t> pathid; std::vector<std::string> paths;   // D1 path objects (both sides derive same order)
    // ---- F's OWN independent store, built ONLY from decoded wire bytes (proves self-describing) ----
    std::vector<uint8_t> Fline_data; std::vector<size_t> Fline_off; Fline_off.push_back(0);   // line id k (1-based) -> [off[k-1],off[k])
    Fline_data.reserve(64u<<20);
    std::vector<uint32_t> Freg_child; std::vector<size_t> Freg_off; Freg_off.push_back(0);     // region id k (0-based) -> [off[k],off[k+1])
    std::vector<std::string> Fpaths;
    std::vector<uint8_t> fknownBlk(useS1?boff2.size():1,0);   // C's model of F's known blocks
    std::vector<uint32_t> Fblk_child; std::vector<size_t> Fblk_off; Fblk_off.push_back(0);      // F block k -> region ids
    std::vector<uint32_t> Freg_stream; Freg_stream.reserve(allreg.size());   // F's reconstructed region occurrence stream (for block COPY defs)
    double w_blockdef=0;
    // wire byte accumulators (post-z, per category) + f-checkpoint tracking
    double w_root=0, w_linedef=0, w_regiondef=0, w_pathdef=0, w_missing=0, w_framing=0, w_rootref=0;
    // S0 semantic-Root memoization: root_key (hash of the TU's RegionKey sequence) -> global TU seq at
    // first publish (strict online). ROOT_REF on an exact-Root reuse = 32 C->F + 24 F->C ACK = 56 B.
    std::unordered_map<uint64_t,std::pair<uint32_t,uint64_t>> rootmemo; std::unordered_map<uint64_t,std::vector<uint32_t>> Froot; uint32_t gtu=0; uint64_t n_rootref=0;
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

    int npass = useS0 ? builds : (warm?2:1);   // --warm: prime then measure. --s0 --builds N: sum wire over N chronological builds (amortized).
    for(int pass=0; pass<npass; ++pass){
      if(warm && pass+1==npass && npass>1){   // reset all measurement state before the warm pass; keep fknown* flags + F-stores
        w_root=w_linedef=w_regiondef=w_pathdef=w_blockdef=w_missing=w_framing=0; cum_raw=0; cum_wire=0; n_marker=n_literal=0; byteexact=true;
        ck.clear(); ckidx=0; allLineDefs.clear(); allRoots.clear(); allRegions.clear(); allRegionsRaw.clear(); allBlocks.clear(); allPaths.clear(); allMiss.clear();
      }
      if(useStream){ for(ZSTD_CStream*cs:{cs_line,cs_reg,cs_root}){ ZSTD_CCtx_reset(cs,ZSTD_reset_session_and_parameters); ZSTD_CCtx_setParameter(cs,ZSTD_c_compressionLevel,zlevel); } }
      auto tpass=Clock::now(); double enc_s=0, dec_s=0;   // split C-encode vs F-decode wall (2-proc per-stream proxy)
      for(size_t t=0; t<TUs; ++t){
        auto _te=Clock::now();
        const uint32_t* tk=&tokstream[tokoff[t]]; size_t tn=tokoff[t+1]-tokoff[t];
        uint32_t olen=corpus.files[t].len;
        // --- S0: semantic Root = the TU's RegionKey sequence (allreg[roff[t]..roff[t+1])). ---
        uint64_t rk=0;
        if(useS0){ rk=1469598103934665603ULL; for(size_t i=roff[t];i<roff[t+1];++i){ rk^=allreg[i]; rk*=1099511628211ULL; }
            uint64_t cov=1469598103934665603ULL; for(size_t i=0;i<tn;++i){ cov^=tk[i]; cov*=1099511628211ULL; }   // covering fingerprint (my S1 matchfinder is position-dependent -> verify the block covering matches, else publish)
            auto it=rootmemo.find(rk);
            if(it!=rootmemo.end() && it->second.first<gtu && it->second.second==cov){   // strict-online EXACT-Root reuse (same semantic seq AND same covering) -> ROOT_REF, no defs/root
                w_rootref += 56;   // 32 C->F {txid,raw_len,root_key} + 24 F->C ACK {txid,digest}
                ++n_rootref;
                recon.clear(); const std::vector<uint32_t>& seq=Froot[rk];
                for(uint32_t r:seq){ if(pass==0) Freg_stream.push_back(r);   // keep Freg_stream mirrored to allreg for block-COPY srcs during the cold build
                    for(size_t j=Freg_off[r];j<Freg_off[r+1];++j){ uint32_t ln=Freg_child[j]; recon.insert(recon.end(),Fline_data.begin()+Fline_off[ln-1],Fline_data.begin()+Fline_off[ln]); } }
                const char* orig=corpus.bytes.data()+corpus.files[t].off;
                if(recon.size()!=olen || memcmp(recon.data(),orig,olen)!=0){ byteexact=false; if(gtu<5) fprintf(stderr,"S0 ROOT_REF FAIL TU %zu\n",t); }
                dec_s += std::chrono::duration<double>(Clock::now()-_te).count();
                cum_raw += olen; double cw=w_root+w_linedef+w_regiondef+w_pathdef+w_blockdef+w_missing+w_framing+w_rootref;
                perTU_raw[t]=olen; perTU_wire[t]=cw-cum_wire; cum_wire=cw;
                while(ckidx<ck_f.size() && double(cum_raw)>=ck_f[ckidx]*(double)corpus.raw*npass){ ck.push_back({ck_f[ckidx],double(cum_raw)/cum_wire}); ++ckidx; }
                ++gtu; continue;
            }
            if(it==rootmemo.end()) rootmemo[rk]={gtu,cov};   // register on first publish; a same-seq/diff-covering TU publishes without overwriting
        }
        // --- collect NEW regions (incl. new blocks' child regions) + NEW blocks, topological order ---
        std::vector<uint32_t> missReg, missBlk;
        auto addRegion=[&](uint32_t r){ if(fknownReg[r])return; for(uint32_t x:missReg) if(x==r) return; missReg.push_back(r); };
        for(size_t i=0;i<tn;++i){ uint32_t tok=tk[i];
            if(tok<NREG) addRegion(tok);
            else { uint32_t k=tok-NREG; if(!fknownBlk[k]){ bool dup=false; for(uint32_t x:missBlk) if(x==k){dup=true;break;} if(!dup){ for(size_t j=boff2[k];j<boff2[k+1];++j) addRegion(bchild[j]); missBlk.push_back(k); } } } }
        // MISSING = unknown region ids + unknown block ids (F requests; real round-trip in 2-proc)
        { std::vector<uint8_t> mm; put_varint(mm,missReg.size()); for(uint32_t r:missReg) put_varint(mm,r); put_varint(mm,missBlk.size()); for(uint32_t k:missBlk) put_varint(mm,NREG+k);
          if(!missReg.empty()||!missBlk.empty()){ w_missing += zstd_size(z,mm.data(),mm.size(),zlevel,dst) + FRAME; allMiss.insert(allMiss.end(),mm.begin(),mm.end()); } }
        // --- FILL: new paths, new lines, new region defs, new block defs (topological) ---
        std::vector<uint8_t> fill_paths, fill_lines, fill_regions, fill_regions_raw, fill_blocks; uint32_t np=0,nl=0,nr=0,nb=0;
        for(uint32_t r:missReg){ const uint32_t* lids=dict.region_ids_ptr(r); uint32_t c=dict.region_ids_count(r);
            for(uint32_t j=0;j<c;++j){ uint32_t ln=lids[j]; if(fknownLine[ln]) continue; fknownLine[ln]=1; ++nl;
                const LineRef& lr=dict.ref(ln); const char* txt=dict.line_data(lr.off);
                if(useD1 && parse_marker(txt,lr.len,mk)){ uint32_t pid; auto it=pathid.find(mk.path); if(it==pathid.end()){ pid=uint32_t(paths.size()); pathid.emplace(mk.path,pid); paths.push_back(mk.path);
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
            put_varint(fill_regions,c); { int64_t prev=0; for(uint32_t j=0;j<c;++j){ put_zigzag(fill_regions,int64_t(lids[j])-prev); prev=int64_t(lids[j]); } } fknownReg[r]=1; ++nr;
            put_varint(fill_regions_raw,c); for(uint32_t j=0;j<c;++j) put_varint(fill_regions_raw,lids[j]);   // adaptive alt: raw line-ids (cross-region subsequence-preserving; z3 picks the smaller)
            { put_varint(allRegionsRaw,c); for(uint32_t j=0;j<c;++j) put_varint(allRegionsRaw,lids[j]); }   // diag
        }
        for(uint32_t k:missBlk){ size_t L=boff2[k+1]-boff2[k];
            if(bcopy_ok[k]){ fill_blocks.push_back(1); put_varint(fill_blocks,bcopy_src[k]); put_varint(fill_blocks,L); }   // COPY(region-stream src,len)
            else { fill_blocks.push_back(0); put_varint(fill_blocks,L); for(size_t j=boff2[k];j<boff2[k+1];++j) put_varint(fill_blocks,bchild[j]); }
            fknownBlk[k]=1; ++nb; }
        if(np){ w_pathdef += zstd_size(z,fill_paths.data(),fill_paths.size(),zlevel,dst); allPaths.insert(allPaths.end(),fill_paths.begin(),fill_paths.end()); }
        if(nl){ w_linedef += useStream? stream_flush(cs_line,fill_lines.data(),fill_lines.size(),sob) : zstd_size(z,fill_lines.data(),fill_lines.size(),zlevel,dst); allLineDefs.insert(allLineDefs.end(),fill_lines.begin(),fill_lines.end()); }
        bool reg_raw=false;
        if(nr){ double dz=zstd_size(z,fill_regions.data(),fill_regions.size(),zlevel,dst);
                double rz=zstd_size(z,fill_regions_raw.data(),fill_regions_raw.size(),zlevel,dst);
                reg_raw = rz<dz;
                if(useStream){ std::vector<uint8_t>& chosen = reg_raw?fill_regions_raw:fill_regions; w_regiondef += stream_flush(cs_reg,chosen.data(),chosen.size(),sob) + 1; }
                else w_regiondef += (reg_raw?rz:dz) + 1;   // +1 byte serialization flag (delta vs raw line-ids)
                allRegions.insert(allRegions.end(),fill_regions.begin(),fill_regions.end()); }
        if(nb){ w_blockdef += zstd_size(z,fill_blocks.data(),fill_blocks.size(),zlevel,dst); allBlocks.insert(allBlocks.end(),fill_blocks.begin(),fill_blocks.end()); }
        if(np||nl||nr||nb) w_framing += FRAME;
        // --- ROOT: token stream (region + block ids) ---
        std::vector<uint8_t> rootb; for(size_t i=0;i<tn;++i) put_varint(rootb,tk[i]);
        w_root += useStream? stream_flush(cs_root,rootb.data(),rootb.size(),sob) : zstd_size(z,rootb.data(),rootb.size(),zlevel,dst); w_framing += FRAME; allRoots.insert(allRoots.end(),rootb.begin(),rootb.end());

        enc_s += std::chrono::duration<double>(Clock::now()-_te).count(); auto _td=Clock::now();
        // --- DECODER (F): install FILL from wire into F's OWN store, then expand ROOT tokens ---
        { const uint8_t* pp=fill_paths.data(); for(uint32_t k=0;k<np;++k){ uint64_t L=get_varint(pp); Fpaths.emplace_back((const char*)pp,(size_t)L); pp+=L; } }
        { const uint8_t* pp=fill_lines.data(), *pe=fill_lines.data()+fill_lines.size();
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
        if(reg_raw){ const uint8_t* pp=fill_regions_raw.data(), *pe=fill_regions_raw.data()+fill_regions_raw.size();
          while(pp<pe){ uint64_t c=get_varint(pp); for(uint64_t j=0;j<c;++j) Freg_child.push_back(uint32_t(get_varint(pp))); Freg_off.push_back(Freg_child.size()); } }
        else { const uint8_t* pp=fill_regions.data(), *pe=fill_regions.data()+fill_regions.size();
          while(pp<pe){ uint64_t c=get_varint(pp); int64_t prev=0; for(uint64_t j=0;j<c;++j){ prev+=get_zigzag(pp); Freg_child.push_back(uint32_t(prev)); } Freg_off.push_back(Freg_child.size()); } }
        { const uint8_t* pp=fill_blocks.data(), *pe=fill_blocks.data()+fill_blocks.size();
          while(pp<pe){ uint8_t kind=*pp++;
            if(kind==1){ uint64_t src=get_varint(pp); uint64_t L=get_varint(pp); for(uint64_t j=0;j<L;++j) Fblk_child.push_back(Freg_stream[src+j]); Fblk_off.push_back(Fblk_child.size()); }
            else { uint64_t L=get_varint(pp); for(uint64_t j=0;j<L;++j) Fblk_child.push_back(uint32_t(get_varint(pp))); Fblk_off.push_back(Fblk_child.size()); } } }
        recon.clear(); size_t fs0=Freg_stream.size();
        auto emitRegionF=[&](uint32_t r){ Freg_stream.push_back(r); for(size_t j=Freg_off[r];j<Freg_off[r+1];++j){ uint32_t ln=Freg_child[j]; recon.insert(recon.end(), Fline_data.begin()+Fline_off[ln-1], Fline_data.begin()+Fline_off[ln]); } };
        { const uint8_t* pp=rootb.data(), *pe=rootb.data()+rootb.size();
          while(pp<pe){ uint32_t tok=uint32_t(get_varint(pp));
            if(tok<NREG) emitRegionF(tok);
            else { uint32_t k=tok-NREG; for(size_t j=Fblk_off[k];j<Fblk_off[k+1];++j) emitRegionF(Fblk_child[j]); } } }
        if(useS0) Froot[rk].assign(Freg_stream.begin()+fs0,Freg_stream.end());   // F stores the Root's RegionKey sequence for future ROOT_REF re-expand
        dec_s += std::chrono::duration<double>(Clock::now()-_td).count();   // F-decode ends here; the verify below is harness-only (F doesn't have the original)
        const char* orig=corpus.bytes.data()+corpus.files[t].off;
        if(recon.size()!=olen || memcmp(recon.data(),orig,olen)!=0){ byteexact=false; if(t<5||TUs<10) fprintf(stderr,"BYTE-EXACT FAIL TU %zu (%zu vs %u)\n",t,recon.size(),olen); }
        cum_raw += olen; ++gtu;
        double cur_wire = w_root+w_linedef+w_regiondef+w_pathdef+w_blockdef+w_missing+w_framing+w_rootref;
        perTU_raw[t]=olen; perTU_wire[t]=cur_wire - cum_wire; cum_wire=cur_wire;
        while(ckidx<ck_f.size() && double(cum_raw)>=ck_f[ckidx]*(double)corpus.raw*(useS0?npass:1)){ ck.push_back({ck_f[ckidx], double(cum_raw)/cum_wire}); ++ckidx; }
      }
      fprintf(stderr,"pass %d (%s) single-core encode+decode+verify: %.2fs = %.2f GB/s raw\n", pass, (pass+1==npass&&npass>1)?"WARM":"cold", secs(tpass), corpus.raw/1e9/secs(tpass));
      fprintf(stderr,"  split (2-proc per-stream proxy): C-encode %.2f GB/s | F-decode %.2f GB/s => pipelined min = %.2f GB/s\n",
              corpus.raw/1e9/enc_s, corpus.raw/1e9/dec_s, corpus.raw/1e9/std::max(enc_s,dec_s));
    }
    while(ck.size()<ck_f.size()) ck.push_back({ck_f[ck.size()], double(cum_raw)/cum_wire});
    ZSTD_freeCCtx(z); ZSTD_freeCStream(cs_line); ZSTD_freeCStream(cs_reg); ZSTD_freeCStream(cs_root);

    if(useSocket){
        // ===== REAL 2-process socketpair + N-stream concurrency sweep =====
        // Each stream = a real F process (own store) decoding the captured wire off an AF_UNIX socket a
        // feeder thread streams into it; F reconstructs byte-exact + times per-TU decode latency. Aggregate
        // = N*raw/wall; per-stream(min) is the gated >=1 GB/s number; watch the aggregate for F saturation.
        fprintf(stderr,"\n==== SOCKETPAIR THROUGHPUT (C-live end-to-end) — %s (in-proc byte-exact=%s, %zu TUs, raw=%.1f MiB) ====\n",
            manifest, byteexact?"OK":"FAIL", TUs, corpus.raw/1048576.0);
        uint64_t total_raw=corpus.raw;
        // Per-stream LIVE encoder: its OWN known-sets (independent C, like a real per-job client) over the
        // shared read-only interner. encode_tu builds TU t's real wire packet [reg_raw][z fp][z fl][z reg]
        // [z fb][z rt] -- identical serializer as the in-process pass, adaptive region + D1 + block-COPY.
        struct CState { std::vector<uint8_t> fknownLine,fknownReg,fknownBlk,inmiss,inmissB; std::unordered_map<std::string,uint32_t> pathid; std::vector<std::string> paths;
            std::vector<uint32_t> missReg,missBlk; std::vector<uint8_t> fp,fl,frd,frr,fb,rt,zt,pkt;
            void init(uint32_t nl,uint32_t nr,uint32_t nb){ fknownLine.assign(nl,0); fknownReg.assign(nr,0); fknownBlk.assign(nb,0); inmiss.assign(nr,0); inmissB.assign(nb,0); } };
        auto encode_tu=[&](CState& C,size_t t){
            const uint32_t* tk=&tokstream[tokoff[t]]; size_t tn=tokoff[t+1]-tokoff[t];
            C.fp.clear(); C.fl.clear(); C.frd.clear(); C.frr.clear(); C.fb.clear(); C.rt.clear();
            std::vector<uint32_t>& missReg=C.missReg; std::vector<uint32_t>& missBlk=C.missBlk; missReg.clear(); missBlk.clear();
            auto addRegion=[&](uint32_t r){ if(C.fknownReg[r]||C.inmiss[r])return; C.inmiss[r]=1; missReg.push_back(r); };   // O(1) stamp dedup (was O(n^2) scan)
            for(size_t i=0;i<tn;++i){ uint32_t tok=tk[i]; if(tok<NREG) addRegion(tok); else { uint32_t k=tok-NREG; if(!C.fknownBlk[k]&&!C.inmissB[k]){ C.inmissB[k]=1; for(size_t j=boff2[k];j<boff2[k+1];++j) addRegion(bchild[j]); missBlk.push_back(k); } } }
            Marker mk;
            for(uint32_t r:missReg){ const uint32_t* lids=dict.region_ids_ptr(r); uint32_t c=dict.region_ids_count(r);
                for(uint32_t j=0;j<c;++j){ uint32_t ln=lids[j]; if(C.fknownLine[ln]) continue; C.fknownLine[ln]=1;
                    const LineRef& lr=dict.ref(ln); const char* txt=dict.line_data(lr.off);
                    if(useD1 && parse_marker(txt,lr.len,mk)){ uint32_t pid; auto it=C.pathid.find(mk.path); if(it==C.pathid.end()){ pid=(uint32_t)C.paths.size(); C.pathid.emplace(mk.path,pid); C.paths.push_back(mk.path); put_varint(C.fp,mk.path.size()); C.fp.insert(C.fp.end(),mk.path.begin(),mk.path.end()); } else pid=it->second;
                        C.fl.push_back(1); put_varint(C.fl,pid); put_varint(C.fl,mk.lineno); C.fl.push_back((uint8_t)mk.flags.size()); for(uint8_t f:mk.flags) C.fl.push_back(f); }
                    else { C.fl.push_back(0); put_varint(C.fl,lr.len); C.fl.insert(C.fl.end(),txt,txt+lr.len); } }
                put_varint(C.frd,c); { int64_t prev=0; for(uint32_t j=0;j<c;++j){ put_zigzag(C.frd,int64_t(lids[j])-prev); prev=int64_t(lids[j]); } }
                put_varint(C.frr,c); for(uint32_t j=0;j<c;++j) put_varint(C.frr,lids[j]);
                C.fknownReg[r]=1; }
            for(uint32_t k:missBlk){ size_t L=boff2[k+1]-boff2[k]; if(bcopy_ok[k]){ C.fb.push_back(1); put_varint(C.fb,bcopy_src[k]); put_varint(C.fb,L); } else { C.fb.push_back(0); put_varint(C.fb,L); for(size_t j=boff2[k];j<boff2[k+1];++j) put_varint(C.fb,bchild[j]); } C.fknownBlk[k]=1; }
            for(size_t i=0;i<tn;++i) put_varint(C.rt,tk[i]);
            auto z3len=[&](const std::vector<uint8_t>& b)->size_t{ size_t bd=ZSTD_compressBound(b.size()); if(C.zt.size()<bd)C.zt.resize(bd); size_t zl=ZSTD_compress(C.zt.data(),C.zt.size(),b.empty()?(const uint8_t*)"":b.data(),b.size(),zlevel); return zl; };
            bool reg_raw = z3len(C.frr) < z3len(C.frd);
            C.pkt.clear(); C.pkt.push_back(reg_raw?1:0);
            auto azp=[&](const std::vector<uint8_t>& b){ size_t bd=ZSTD_compressBound(b.size()); if(C.zt.size()<bd)C.zt.resize(bd); size_t zl=ZSTD_compress(C.zt.data(),C.zt.size(),b.empty()?(const uint8_t*)"":b.data(),b.size(),zlevel); put_varint(C.pkt,zl); C.pkt.insert(C.pkt.end(),C.zt.data(),C.zt.data()+zl); };
            azp(C.fp); azp(C.fl); azp(reg_raw?C.frr:C.frd); azp(C.fb); azp(C.rt);
        };
        uint32_t nblk=(uint32_t)(useS1?boff2.size():1);
        for(int N : std::vector<int>{1,4,8,16,24}){
            double* shm=(double*)mmap(nullptr,sizeof(double)*8*N,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);
            std::vector<int> pfd(N); std::vector<pid_t> kids(N);
            for(int i=0;i<N;++i){ int sv[2]; if(socketpair(AF_UNIX,SOCK_STREAM,0,sv)){perror("socketpair");return 2;}
                pid_t pid=fork();
                if(pid==0){ close(sv[0]);
                    FState F; F.init(); ZSTD_DCtx* d=ZSTD_createDCtx();
                    std::vector<uint8_t> fp,fl,fr,fb,rt,recon,tmp,pktbuf; std::vector<double> lat; lat.reserve(TUs);
                    bool ok=true; auto c0=Clock::now();
                    for(size_t t=0;t<TUs;++t){ uint32_t plen; if(!sock_rall(sv[1],&plen,4)){ok=false;break;} pktbuf.resize(plen); if(!sock_rall(sv[1],pktbuf.data(),plen)){ok=false;break;}
                        auto q0=Clock::now(); decode_packet(NREG,F,pktbuf.data(),d,fp,fl,fr,fb,rt,recon,tmp);
                        const char* orig=corpus.bytes.data()+corpus.files[t].off; uint32_t ol=corpus.files[t].len;
                        if(recon.size()!=ol || memcmp(recon.data(),orig,ol)!=0) ok=false;
                        lat.push_back(std::chrono::duration<double,std::micro>(Clock::now()-q0).count()); }
                    double dsec=std::chrono::duration<double>(Clock::now()-c0).count(); std::sort(lat.begin(),lat.end());
                    double own=(double)F.line_data.size()+F.reg_child.size()*4.0+F.reg_off.size()*8.0+F.blk_child.size()*4.0+F.blk_off.size()*8.0+F.line_off.size()*8.0+F.reg_stream.size()*4.0; for(auto&s:F.paths) own+=s.size();
                    shm[i*8+0]=dsec; shm[i*8+1]=lat.empty()?0:lat[lat.size()/2]; shm[i*8+2]=lat.empty()?0:lat[(size_t)(lat.size()*0.95)]; shm[i*8+3]=lat.empty()?0:lat[(size_t)(lat.size()*0.99)]; shm[i*8+4]=ok?1:0; shm[i*8+5]=own; shm[i*8+6]=(double)lat.size();
                    ZSTD_freeDCtx(d); close(sv[1]); _exit(0); }
                close(sv[1]); pfd[i]=sv[0]; kids[i]=pid; }
            auto t0s=Clock::now(); std::vector<std::thread> encoders;
            for(int i=0;i<N;++i){ int fd=pfd[i]; encoders.emplace_back([&,fd]{ CState C; C.init(dict.distinct()+1,NREG,nblk);
                for(size_t t=0;t<TUs;++t){ encode_tu(C,t); uint32_t plen=(uint32_t)C.pkt.size(); if(!sock_wall(fd,&plen,4))break; if(!sock_wall(fd,C.pkt.data(),plen))break; } close(fd); }); }
            for(auto& th:encoders) th.join();
            for(int i=0;i<N;++i){ int st; waitpid(kids[i],&st,0); }
            double wall=std::chrono::duration<double>(Clock::now()-t0s).count();
            bool allok=true; double maxown=0,minstream=1e18,p50=0,p95=0,p99=0;
            for(int i=0;i<N;++i){ if(shm[i*8+4]<0.5)allok=false; double sps=total_raw/1e9/shm[i*8+0]; if(sps<minstream)minstream=sps; if(shm[i*8+5]>maxown)maxown=shm[i*8+5]; p50+=shm[i*8+1]; p95+=shm[i*8+2]; p99+=shm[i*8+3]; }
            double agg=(double)total_raw*N/1e9/wall;
            fprintf(stderr,"N=%2d  byte-exact=%s  per-stream(min)=%.2f GB/s  AGGREGATE=%.2f GB/s  lat us p50/95/99=%.1f/%.1f/%.1f  F-own-store=%.0f MiB  wall=%.2fs\n",
                N, allok?"OK":"FAIL", minstream, agg, p50/N, p95/N, p99/N, maxown/1048576.0, wall);
            munmap(shm,sizeof(double)*8*N);
        }
        return byteexact?0:1;
    }

    double totalwire = w_root+w_linedef+w_regiondef+w_pathdef+w_blockdef+w_missing+w_framing+w_rootref;
    double MiB=1048576.0;
    printf("\n==== CODEC-50 (%s%s%s%s, z%d) — %s ====\n", useS1?"V1+S1(LZ blocks)":"V1", useD1?"+D1":"", useS0?"+S0":"", (useD2mine?"+D2(inline)":(useD2?"+D2(helper)":"")), zlevel, manifest);
    printf("byte-exact=%s  TUs=%zu raw=%.1f MiB regions=%u distinct_lines=%u paths=%zu blocks=%zu (marker_lines=%llu literal_lines=%llu)\n",
        byteexact?"OK":"FAIL",TUs,corpus.raw/MiB,NREG,dict.distinct(),paths.size(),boff2.size()-1,(unsigned long long)n_marker,(unsigned long long)n_literal);
    printf("wire by category (post-z%d, bytes): root=%.0f line_def=%.0f region_def=%.0f block_def=%.0f path_def=%.0f missing=%.0f rootref=%.0f framing=%.0f  TOTAL=%.0f (%.2f MiB)\n",
        zlevel,w_root,w_linedef,w_regiondef,w_blockdef,w_pathdef,w_missing,w_rootref,w_framing,totalwire,totalwire/MiB);
    printf("FinalRatio (raw / total wire, one cold pass) = %.1fx\n", corpus.raw/totalwire);
    if(useS0) printf("S0 AMORTIZED over %d builds: raw=%.1f MiB total-wire=%.2f MiB ROOT_REFs=%llu/%llu (%.1f%%) => AmortizedRatio=%.0fx\n",
        npass, (double)corpus.raw*npass/MiB, totalwire/MiB, (unsigned long long)n_rootref,(unsigned long long)(TUs*(uint64_t)npass), 100.0*n_rootref/(TUs*(double)npass), (double)corpus.raw*npass/totalwire);
    { ZSTD_CCtx* z2=ZSTD_createCCtx(); std::vector<uint8_t> d2b;
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
      // z19+LDM twin of the SAME six batched categories -- a CEILING DIAGNOSTIC only (owner's rule is z<=3;
      // this shows the headroom the cap leaves). ADDITIVE: the z3 numbers above are untouched. --deep-gated
      // so --stream/--socket/--s0 runs pay no z19 cost.
      double f19_line=0,f19_root=0,f19_reg=0,f19_blk=0,f19_path=0,f19_miss=0,full19=0;
      if(deep){ auto batch19=[&](std::vector<uint8_t>&v)->double{ if(v.empty())return 0.0;
          ZSTD_CCtx_reset(z2,ZSTD_reset_session_and_parameters); ZSTD_CCtx_setParameter(z2,ZSTD_c_compressionLevel,19);
          ZSTD_CCtx_setParameter(z2,ZSTD_c_enableLongDistanceMatching,1); ZSTD_CCtx_setParameter(z2,ZSTD_c_windowLog,27);
          size_t bnd=ZSTD_compressBound(v.size()); if(d2b.size()<bnd)d2b.resize(bnd); return double(ZSTD_compress2(z2,d2b.data(),d2b.size(),v.data(),v.size())); };
        f19_line=batch19(allLineDefs); f19_root=batch19(allRoots); f19_reg=batch19(allRegions); f19_blk=batch19(allBlocks); f19_path=batch19(allPaths); f19_miss=batch19(allMiss);
        full19=f19_line+f19_root+f19_reg+f19_blk+f19_path+f19_miss+w_framing; }
      ZSTD_freeCCtx(z2);
      double fullfloor=fl_line+fl_root+fl_reg+fl_blk+fl_path+fl_miss+w_framing;
      double alt=totalwire - w_linedef - w_root + bl + br;
      double altldm=totalwire - w_linedef + bl_ldm;
      printf("DIAG batched-z%d floor: line_def %.0f->%.0f  root %.0f->%.0f  => streamed TOTAL=%.0f ratio=%.0fx\n",zlevel,w_linedef,bl,w_root,br,alt,corpus.raw/alt);
      printf("DIAG FULL streamed floor (all cats batched z%d): line=%.2f reg=%.2f blk=%.2f path=%.2f miss=%.2f root=%.2f => %.2f MiB ratio=%.0fx\n",
        zlevel,fl_line/MiB,fl_reg/MiB,fl_blk/MiB,fl_path/MiB,fl_miss/MiB,fl_root/MiB,fullfloor/MiB,corpus.raw/fullfloor);
      if(deep) printf("DIAG FULL z19+LDM ceiling (SAME cats batched z19+LDM+win27): line=%.2f reg=%.2f blk=%.2f path=%.2f miss=%.2f root=%.2f => %.2f MiB  cold_z19ldm FinalRatio=%.1fx  (z3-batched floor=%.1fx)\n",
        f19_line/MiB,f19_reg/MiB,f19_blk/MiB,f19_path/MiB,f19_miss/MiB,f19_root/MiB,full19/MiB,corpus.raw/full19,corpus.raw/fullfloor);
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
    printf("H200 f-checkpoints (cum raw fraction -> cumulative ratio):\n");
    for(auto&c:ck) printf("  f=%.2f  ratio=%.0fx\n",c.first,c.second);
    // trailing-window (5% raw) ratio near the end
    { double win=0.05*corpus.raw, r=0,wsum=0; for(size_t t=TUs;t-->0;){ r+=perTU_raw[t]; wsum+=perTU_wire[t]; if(r>=win) break; } printf("trailing 5%%-raw window ratio (steady) = %.0fx\n", wsum>0?r/wsum:0); }
    struct rusage ru{}; getrusage(RUSAGE_SELF,&ru); fprintf(stderr,"peak RSS=%.1f MiB total=%.1fs\n",ru.ru_maxrss/1024.0,secs(t0));
    return byteexact?0:1;
}
