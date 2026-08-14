// superblock-online-bench.cpp
//
// ONLINE encode-then-learn superblock measurement for the icecream line-dedup transport
// (issue #16), per linecache/ONLINE-CORRECTION.md (which overrides FOUR-PASS-SPEC.md).
//
// The superblock layer is a CONTINUOUSLY GROWING online predictor, not a frozen grammar. For
// each TU t, in build order:
//   1. the interner emits TU_t's stable marker-region ID sequence;
//   2. the encoder segments it over the CURRENTLY-published immutable blocks (stack-greedy
//      longest-merge; state built from TUs 1..t-1 ONLY);
//   3. we charge the full per-F wire (root ID stream + any new object definitions this F needs);
//   4. ONLY THEN do we feed TU_t into the learner, which promotes newly-worthwhile adjacent
//      region/block pairs into NEW immutable Block IDs available from TU t+1 onward.
// The predictor grows across all four passes (A cold, B warm-same, C header-edit, D changed-steady);
// it never freezes (a frozen-after-A snapshot is emitted only as a labelled control).
//
// IDENTITY INVARIANT: an already-published object ID never changes meaning. "Improving" a block
// allocates a NEW immutable Block ID; existing IDs are never rebound. Object IDs are stable
// generation-relative: one generation-local sequence shared by Line objects (id = interner line-id)
// and Block objects (id > all children => id order is topological => clean FILL closures).
//
// HEADLINES: (1) convergence curve — per-TU root-token cost falling toward the OFFLINE batch
// region-BPE ceiling as blocks accumulate; (2) recovery curve — full-wire + token cost by
// affected-TU ordinal after one high-fanout header edit.
//
// Reuses the accepted trace interner (linecache/trace-interner-bench.cpp) verbatim except for an
// optional region-id sink on process(). Byte-exact: every TU's root tokens expand to its exact
// line-id stream (FNV-1a digest anchored to raw source).
//
// build: g++ -O3 -march=native -std=c++17 superblock-online-bench.cpp -o superblock-online-bench -lzstd

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <immintrin.h>
#include <zstd.h>

using Clock = std::chrono::steady_clock;
static double secs(Clock::time_point b){ return std::chrono::duration<double>(Clock::now()-b).count(); }

static inline uint64_t mix64(uint64_t x){ x^=x>>30; x*=0xbf58476d1ce4e5b9ULL; x^=x>>27; x*=0x94d049bb133111ebULL; return x^(x>>31); }
static inline uint64_t fold128(uint64_t a,uint64_t b){ __uint128_t p=__uint128_t(a)*b; return uint64_t(p)^uint64_t(p>>64); }
static inline uint64_t read64(const char*p){ uint64_t v; memcpy(&v,p,8); return v; }
static inline uint64_t read_tail(const char*p,uint32_t n){ uint64_t v=0; memcpy(&v,p,n); return v; }

static inline uint64_t sampled_hash(const char*p,uint32_t n){
    constexpr uint64_t A=0xa0761d6478bd642fULL,B=0xe7037ed1a0b428dbULL;
    uint64_t h=mix64(uint64_t(n)^A);
    if(n<=8) return mix64(h^read_tail(p,n));
    if(n<=16) return fold128(read64(p)^A, read64(p+n-8)^h);
    if(n<=32){ h=fold128(read64(p)^A,read64(p+8)^h); return fold128(read64(p+n-16)^B,read64(p+n-8)^h); }
    uint32_t mid=(n>>1)-4;
    h=fold128(read64(p)^A,read64(p+8)^h);
    h=fold128(read64(p+mid)^B,read64(p+n-16)^h);
    return fold128(read64(p+n-8)^A,h^B);
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
static inline const char* next_region(const char*p,const char*end){
    const char*q=p+1;
#if defined(__AVX512BW__)
    const __m512i hh=_mm512_set1_epi8('#');
    while(q+64<=end){ __m512i v=_mm512_loadu_si512((const void*)q); uint64_t m=_mm512_cmpeq_epi8_mask(v,hh);
        while(m){ unsigned bit=__builtin_ctzll(m); const char*c=q+bit; if(c[-1]=='\n'&&c+1<end&&c[1]==' ') return c; m&=m-1; } q+=64; }
#elif defined(__AVX2__)
    const __m256i hh=_mm256_set1_epi8('#');
    while(q+32<=end){ __m256i v=_mm256_loadu_si256((const __m256i*)q); uint32_t m=_mm256_movemask_epi8(_mm256_cmpeq_epi8(v,hh));
        while(m){ unsigned bit=__builtin_ctz(m); const char*c=q+bit; if(c[-1]=='\n'&&c+1<end&&c[1]==' ') return c; m&=m-1; } q+=32; }
#endif
    while(q+1<end){ if(*q=='#'&&q[-1]=='\n'&&q[1]==' ') return q; ++q; }
    return end;
}
static void* huge_zeroed(size_t bytes){ constexpr size_t H=2u<<20; bytes=(bytes+H-1)&~(H-1);
    void*p=mmap(nullptr,bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0); if(p==MAP_FAILED){perror("mmap");exit(2);} madvise(p,bytes,MADV_HUGEPAGE); return p; }

struct FileSpan{ uint64_t off; uint32_t len; };
struct LineRef{ uint32_t off; uint32_t len; };
struct TinySlot{ uint64_t bytes; uint32_t id; uint8_t len; uint8_t pad[3]; };
struct ShortSlot{ uint64_t lo; uint64_t hi; uint32_t id; uint8_t len; uint8_t pad[3]; };
struct LineSlot{ uint64_t hash; uint32_t off; uint32_t len; uint32_t id; uint32_t pad; };
struct RegionRecord{ uint64_t hash; uint32_t raw_off; uint32_t raw_len; uint32_t ids_off; uint32_t ids_count; uint32_t next1; uint32_t next2; };

class Interner {
public:
    static constexpr uint32_t TINY_CAP=1u<<10, SHORT_CAP=1u<<17, LINE_CAP=1u<<21, REGION_INITIAL_CAP=1u<<17;
    Interner(){
        tiny_=(TinySlot*)huge_zeroed(sizeof(TinySlot)*TINY_CAP);
        short_=(ShortSlot*)huge_zeroed(sizeof(ShortSlot)*SHORT_CAP);
        lines_=(LineSlot*)huge_zeroed(sizeof(LineSlot)*LINE_CAP);
        region_index_.resize(REGION_INITIAL_CAP); region_mask_=REGION_INITIAL_CAP-1;
        region_records_.reserve(1u<<20); line_bytes_.reserve(64u<<20); region_bytes_.reserve(80u<<20);
        region_ids_.reserve(8u<<20); id_refs_.reserve(1000000); id_refs_.push_back({0,0});
    }
    ~Interner(){ munmap(tiny_,rounded(sizeof(TinySlot)*TINY_CAP)); munmap(short_,rounded(sizeof(ShortSlot)*SHORT_CAP)); munmap(lines_,rounded(sizeof(LineSlot)*LINE_CAP)); }
    Interner(const Interner&)=delete; Interner& operator=(const Interner&)=delete;

    uint32_t distinct() const { return next_id_-1; }
    uint64_t region_count() const { return region_records_.size(); }
    const LineRef& ref(uint32_t id) const { return id_refs_[id]; }
    const char* line_data(uint32_t off) const { return line_bytes_.data()+off; }
    const uint32_t* region_ids_ptr(uint32_t rid) const { return region_ids_.data()+region_records_[rid].ids_off; }
    uint32_t region_ids_count(uint32_t rid) const { return region_records_[rid].ids_count; }
    uint64_t distinct_line_bytes() const { return line_bytes_.size(); }

    void process(const char*begin,const char*end,uint32_t*out,size_t&out_count,uint64_t&region_hits,bool train,std::vector<uint32_t>*region_out){
        const char*p=begin; uint32_t previous=UINT32_MAX;
        while(p<end){
            bool found=false; uint32_t region_id=UINT32_MAX,n=0; uint64_t h=0;
            if(previous!=UINT32_MAX){
                const RegionRecord&prev=region_records_[previous]; const uint32_t cand[2]={prev.next1,prev.next2};
                for(uint32_t enc:cand){ if(!enc) continue; uint32_t cid=enc-1; const RegionRecord&c=region_records_[cid];
                    if(c.raw_len>uint64_t(end-p)) continue; const char*ce=p+c.raw_len;
                    bool eb= ce==end || (ce+1<end&&ce[-1]=='\n'&&ce[0]=='#'&&ce[1]==' ');
                    if(eb&&memcmp(region_bytes_.data()+c.raw_off,p,c.raw_len)==0){ region_id=cid; n=c.raw_len;
                        memcpy(out+out_count,region_ids_.data()+c.ids_off,size_t(c.ids_count)*4); out_count+=c.ids_count; ++region_hits; found=true; break; } }
            }
            if(!found){ const char*q=next_region(p,end); n=uint32_t(q-p); h=sampled_hash(p,n)|1ULL; uint32_t slot=uint32_t(h)&region_mask_,probes=0;
                for(;;){ if(++probes>region_index_.size()){fprintf(stderr,"region tbl full\n");exit(2);} uint32_t enc=region_index_[slot]; if(!enc) break;
                    RegionRecord&r=region_records_[enc-1]; if(r.hash==h&&r.raw_len==n&&memcmp(region_bytes_.data()+r.raw_off,p,n)==0){ region_id=enc-1;
                        memcpy(out+out_count,region_ids_.data()+r.ids_off,size_t(r.ids_count)*4); out_count+=r.ids_count; ++region_hits; found=true; break; } slot=(slot+1)&region_mask_; } }
            if(!found){ const char*q=p+n; uint32_t ids_off=uint32_t(region_ids_.size()); const char*lp=p;
                while(lp<q){ const void*hit=memchr(lp,'\n',size_t(q-lp)); const char*le=hit?(const char*)hit+1:q; uint32_t id=intern_line(lp,uint32_t(le-lp)); region_ids_.push_back(id); out[out_count++]=id; lp=le; }
                uint32_t raw_off=uint32_t(region_bytes_.size()); region_bytes_.insert(region_bytes_.end(),p,q); region_id=uint32_t(region_records_.size());
                region_records_.push_back({h,raw_off,n,ids_off,uint32_t(region_ids_.size()-ids_off),0,0}); insert_region_index(region_id); }
            if(region_out) region_out->push_back(region_id);
            if(train&&previous!=UINT32_MAX){ RegionRecord&prev=region_records_[previous]; uint32_t enc=region_id+1; if(!prev.next1)prev.next1=enc; else if(prev.next1!=enc&&!prev.next2)prev.next2=enc; }
            previous=region_id; p+=n;
        }
    }
    uint32_t intern_line(const char*p,uint32_t n){
        if(n<=4) return intern_tiny(p,n); if(n<=16) return intern_short(p,n);
        uint64_t h=line_hash(p,n)|1ULL; uint32_t slot=uint32_t(h)&(LINE_CAP-1),probes=0;
        for(;;){ if(++probes>LINE_CAP){fprintf(stderr,"line tbl full\n");exit(2);} LineSlot&s=lines_[slot];
            if(!s.id){ uint32_t id=add_line(p,n); s={h,id_refs_[id].off,n,id,0}; return id; }
            if(s.hash==h&&s.len==n&&memcmp(line_bytes_.data()+s.off,p,n)==0) return s.id; slot=(slot+1)&(LINE_CAP-1); }
    }
private:
    void insert_region_index(uint32_t region_id){
        if((region_records_.size()*10)>(region_index_.size()*7)){ std::vector<uint32_t> g(region_index_.size()*2); uint32_t nm=uint32_t(g.size()-1);
            for(uint32_t id=0;id<region_records_.size()-1;++id){ uint32_t slot=uint32_t(region_records_[id].hash)&nm; while(g[slot])slot=(slot+1)&nm; g[slot]=id+1; } region_index_.swap(g); region_mask_=nm; }
        uint32_t slot=uint32_t(region_records_[region_id].hash)&region_mask_; while(region_index_[slot])slot=(slot+1)&region_mask_; region_index_[slot]=region_id+1;
    }
    static size_t rounded(size_t n){ constexpr size_t H=2u<<20; return (n+H-1)&~(H-1); }
    uint32_t add_line(const char*p,uint32_t n){ if(next_id_==0){fprintf(stderr,"ID ovf\n");exit(2);} uint32_t off=uint32_t(line_bytes_.size()); line_bytes_.insert(line_bytes_.end(),p,p+n); uint32_t id=next_id_++; id_refs_.push_back({off,n}); return id; }
    uint32_t intern_tiny(const char*p,uint32_t n){ uint64_t bytes=read_tail(p,n); uint32_t slot=uint32_t(mix64(bytes^(uint64_t(n)<<56)))&(TINY_CAP-1),probes=0;
        for(;;){ if(++probes>TINY_CAP){fprintf(stderr,"tiny full\n");exit(2);} TinySlot&s=tiny_[slot]; if(!s.id){uint32_t id=add_line(p,n);s.bytes=bytes;s.id=id;s.len=uint8_t(n);return id;} if(s.len==n&&s.bytes==bytes)return s.id; slot=(slot+1)&(TINY_CAP-1); } }
    uint32_t intern_short(const char*p,uint32_t n){ uint64_t lo=n>=8?read64(p):read_tail(p,n); uint64_t hi=n>8?read_tail(p+8,n-8):lo; uint64_t h=fold128(lo^0xa0761d6478bd642fULL,hi^uint64_t(n)*0xe7037ed1a0b428dbULL); uint32_t slot=uint32_t(h)&(SHORT_CAP-1),probes=0;
        for(;;){ if(++probes>SHORT_CAP){fprintf(stderr,"short full\n");exit(2);} ShortSlot&s=short_[slot]; if(!s.id){uint32_t id=add_line(p,n);s.lo=lo;s.hi=hi;s.id=id;s.len=uint8_t(n);return id;} if(s.len==n&&s.lo==lo&&s.hi==hi)return s.id; slot=(slot+1)&(SHORT_CAP-1); } }
    TinySlot*tiny_=nullptr; ShortSlot*short_=nullptr; LineSlot*lines_=nullptr;
    std::vector<uint32_t> region_index_; std::vector<RegionRecord> region_records_;
    std::vector<char> line_bytes_, region_bytes_; std::vector<uint32_t> region_ids_; std::vector<LineRef> id_refs_;
    uint32_t next_id_=1, region_mask_=0;
};

struct Corpus{ std::vector<char> bytes; std::vector<FileSpan> files; uint64_t raw=0; };

static Corpus make_poisoned(const Corpus&src,uint64_t&modified){
    static constexpr char marker[]="# 1 \"/usr/include/stdc-predef.h\" 1 3 4\n";
    static constexpr char inserted[]="typedef int local_oracle_inserted_line;\n";
    Corpus r; r.files.reserve(src.files.size()); r.bytes.reserve(src.bytes.size()+src.files.size()*sizeof(inserted)); modified=0;
    for(const FileSpan&f:src.files){ const char*b=src.bytes.data()+f.off,*e=b+f.len; const char*at=std::search(b,e,marker,marker+sizeof(marker)-1); uint64_t oo=r.bytes.size();
        if(at==e) r.bytes.insert(r.bytes.end(),b,e); else { const char*after=at+sizeof(marker)-1; r.bytes.insert(r.bytes.end(),b,after); r.bytes.insert(r.bytes.end(),inserted,inserted+sizeof(inserted)-1); r.bytes.insert(r.bytes.end(),after,e); ++modified; }
        uint64_t ol=r.bytes.size()-oo; if(ol>UINT32_MAX){fprintf(stderr,"poison TU too big\n");exit(2);} r.files.push_back({oo,uint32_t(ol)}); }
    r.raw=r.bytes.size(); r.bytes.resize(r.bytes.size()+64); return r;
}
static Corpus load_corpus(const char*manifest,size_t max_files){
    FILE*mf=fopen(manifest,"r"); if(!mf){perror(manifest);exit(2);} std::vector<std::string> paths; char path[8192]; uint64_t total=0;
    while(fgets(path,sizeof path,mf)){ size_t n=strlen(path); while(n&&(path[n-1]=='\n'||path[n-1]=='\r'))path[--n]=0; if(!n)continue; struct stat st{}; if(stat(path,&st)!=0){perror(path);exit(2);} if(st.st_size<0||uint64_t(st.st_size)>UINT32_MAX){fprintf(stderr,"bad size %s\n",path);exit(2);} paths.emplace_back(path); total+=uint64_t(st.st_size); if(paths.size()==max_files)break; }
    fclose(mf); Corpus c; c.bytes.resize(size_t(total)+64); c.files.reserve(paths.size()); uint64_t off=0;
    for(auto&p:paths){ FILE*f=fopen(p.c_str(),"rb"); if(!f){perror(p.c_str());exit(2);} struct stat st{}; fstat(fileno(f),&st); size_t n=size_t(st.st_size); if(n&&fread(c.bytes.data()+off,1,n,f)!=n){fprintf(stderr,"short read %s\n",p.c_str());exit(2);} fclose(f); c.files.push_back({off,uint32_t(n)}); off+=n; }
    c.raw=off; return c;
}

// ---------- captured per-TU region + line streams ----------
struct Captured{ std::vector<uint32_t> line_ids, region_ids; std::vector<size_t> line_off, region_off; };
static Captured capture(Interner&dict,const Corpus&corpus,bool train){
    Captured cap; uint32_t maxlen=0; for(auto&f:corpus.files) maxlen=std::max(maxlen,f.len);
    std::vector<uint32_t> out(size_t(maxlen)+1); cap.line_off.push_back(0); cap.region_off.push_back(0); uint64_t hits=0; std::vector<uint32_t> rs;
    for(auto&f:corpus.files){ size_t oc=0; rs.clear(); const char*p=corpus.bytes.data()+f.off; dict.process(p,p+f.len,out.data(),oc,hits,train,&rs);
        cap.line_ids.insert(cap.line_ids.end(),out.data(),out.data()+oc); cap.line_off.push_back(cap.line_ids.size());
        cap.region_ids.insert(cap.region_ids.end(),rs.begin(),rs.end()); cap.region_off.push_back(cap.region_ids.size()); }
    return cap;
}

// ---------- varint / zstd ----------
static inline void put_varint(std::vector<uint8_t>&o,uint64_t v){ while(v>=0x80){o.push_back(uint8_t(v)|0x80);v>>=7;} o.push_back(uint8_t(v)); }
static inline uint32_t varint_len(uint64_t v){ uint32_t n=1; while(v>=0x80){v>>=7;++n;} return n; }
static uint64_t fnv1a(const uint8_t*p,size_t n){ uint64_t h=1469598103934665603ULL; for(size_t i=0;i<n;++i){h^=p[i];h*=1099511628211ULL;} return h; }
static size_t zstd_size(ZSTD_CCtx*c,const uint8_t*data,size_t n,int level,std::vector<uint8_t>&dst){
    ZSTD_CCtx_reset(c,ZSTD_reset_session_and_parameters); ZSTD_CCtx_setParameter(c,ZSTD_c_compressionLevel,level);
    size_t bound=ZSTD_compressBound(n); if(dst.size()<bound)dst.resize(bound); size_t r=ZSTD_compress2(c,dst.data(),dst.size(),data?data:(const uint8_t*)"",n);
    if(ZSTD_isError(r)){fprintf(stderr,"zstd err %s\n",ZSTD_getErrorName(r));exit(2);} return r;
}

// ---------- open-addressing uint64 -> uint32 map ----------
struct U64Map{
    std::vector<uint64_t> k; std::vector<uint32_t> v; size_t mask,cnt=0;
    U64Map(size_t cap=1<<16){ size_t c=1; while(c<cap)c<<=1; k.assign(c,UINT64_MAX); v.assign(c,0); mask=c-1; }
    void grow(){ std::vector<uint64_t> nk(k.size()*2,UINT64_MAX); std::vector<uint32_t> nv(k.size()*2,0); size_t nm=nk.size()-1;
        for(size_t i=0;i<k.size();++i) if(k[i]!=UINT64_MAX){ size_t j=mix64(k[i])&nm; while(nk[j]!=UINT64_MAX)j=(j+1)&nm; nk[j]=k[i]; nv[j]=v[i]; } k.swap(nk); v.swap(nv); mask=nm; }
    inline uint32_t get(uint64_t key) const { size_t i=mix64(key)&mask; while(k[i]!=UINT64_MAX){ if(k[i]==key) return v[i]; i=(i+1)&mask; } return UINT32_MAX; }
    inline void put(uint64_t key,uint32_t val){ if((cnt+1)*10>k.size()*7) grow(); size_t i=mix64(key)&mask; while(k[i]!=UINT64_MAX){ if(k[i]==key){v[i]=val;return;} i=(i+1)&mask; } k[i]=key; v[i]=val; ++cnt; }
    inline uint32_t inc(uint64_t key){ if((cnt+1)*10>k.size()*7) grow(); size_t i=mix64(key)&mask; while(k[i]!=UINT64_MAX){ if(k[i]==key) return ++v[i]; i=(i+1)&mask; } k[i]=key; v[i]=1; ++cnt; return 1; }
};

// ===================================================================================
// Online engine: stable object IDs (Line + immutable Block), online pair-promotion.
// ===================================================================================
struct OnlineEngine {
    const Interner* dict=nullptr;
    uint32_t Nlines=0, Nregions=0;
    uint32_t REGION_BASE=0, BLOCK_BASE=0;
    // blocks: obj id = BLOCK_BASE + k
    std::vector<uint32_t> blkL, blkR;      // child obj ids
    std::vector<uint32_t> blkLeaf;         // # of line leaves in expansion
    std::vector<uint16_t> blkDepth;        // block nesting depth (region=0)
    std::vector<uint32_t> blkCreatedTU;    // global TU ordinal at creation
    std::vector<uint32_t> blkUses;         // times referenced as a root token
    std::vector<uint32_t> blkFirstUseTU;   // global TU ordinal of 2nd appearance (first use after creation)
    U64Map merge;                          // pair(L,R) -> block obj id (published)
    U64Map paircount;                      // pair(L,R) -> accumulated adjacency count (unpromoted)
    uint32_t promote_count=4;              // occurrence gate (swept)
    bool learning=true;

    void init(const Interner* d, uint32_t nlines, uint32_t nregions){
        dict=d; Nlines=nlines; Nregions=nregions; REGION_BASE=Nlines+1; BLOCK_BASE=REGION_BASE+Nregions;
        blkL.clear(); blkR.clear(); blkLeaf.clear(); blkDepth.clear(); blkCreatedTU.clear(); blkUses.clear(); blkFirstUseTU.clear();
        merge=U64Map(1<<16); paircount=U64Map(1<<16); learning=true;
        blkL.reserve(1<<20); blkR.reserve(1<<20); blkLeaf.reserve(1<<20); blkDepth.reserve(1<<20);
        blkCreatedTU.reserve(1<<20); blkUses.reserve(1<<20); blkFirstUseTU.reserve(1<<20);
    }
    inline bool is_line(uint32_t o) const { return o<REGION_BASE; }
    inline bool is_region(uint32_t o) const { return o>=REGION_BASE && o<BLOCK_BASE; }
    inline bool is_block(uint32_t o) const { return o>=BLOCK_BASE; }
    inline uint32_t leafcount(uint32_t o) const {
        if(o<REGION_BASE) return 1;
        if(o<BLOCK_BASE) return dict->region_ids_count(o-REGION_BASE);
        return blkLeaf[o-BLOCK_BASE];
    }
    inline uint16_t depthof(uint32_t o) const { return o<BLOCK_BASE?0:blkDepth[o-BLOCK_BASE]; }
    uint32_t nblocks() const { return uint32_t(blkL.size()); }

    // encode a region-id sequence into root tokens via stack-greedy merge over published blocks.
    void encode(const uint32_t* rids, size_t n, std::vector<uint32_t>& toks) const {
        toks.clear(); toks.reserve(n);
        for(size_t i=0;i<n;++i){
            toks.push_back(REGION_BASE + rids[i]);
            for(;;){ size_t m=toks.size(); if(m<2) break; uint64_t key=(uint64_t(toks[m-2])<<32)|toks[m-1]; uint32_t p=merge.get(key); if(p==UINT32_MAX) break; toks[m-2]=p; toks.pop_back(); }
        }
    }
    // learn from a TU's root tokens: count adjacent pairs, promote those crossing the gate.
    uint32_t learn(const std::vector<uint32_t>& toks, uint32_t tu_ord){
        if(!learning) return 0;
        uint32_t created=0;
        for(size_t i=0;i+1<toks.size();++i){
            uint64_t key=(uint64_t(toks[i])<<32)|toks[i+1];
            if(merge.get(key)!=UINT32_MAX) continue;         // already a block (won't happen post-merge, but safe)
            uint32_t c=paircount.inc(key);
            if(c>=promote_count){
                uint32_t L=toks[i],R=toks[i+1];
                uint32_t id=BLOCK_BASE+uint32_t(blkL.size());
                blkL.push_back(L); blkR.push_back(R);
                blkLeaf.push_back(leafcount(L)+leafcount(R));
                blkDepth.push_back(uint16_t(1+std::max(depthof(L),depthof(R))));
                blkCreatedTU.push_back(tu_ord); blkUses.push_back(0); blkFirstUseTU.push_back(0);
                merge.put(key,id);
                ++created;
            }
        }
        return created;
    }
    // expand an object to its line-id leaves (for byte-exact verification).
    void expand(uint32_t o, std::vector<uint32_t>& out) const {
        if(o<REGION_BASE){ out.push_back(o); return; }
        if(o<BLOCK_BASE){ const uint32_t* ids=dict->region_ids_ptr(o-REGION_BASE); uint32_t c=dict->region_ids_count(o-REGION_BASE); out.insert(out.end(),ids,ids+c); return; }
        uint32_t k=o-BLOCK_BASE; expand(blkL[k],out); expand(blkR[k],out);
    }
};

// ---------- per-TU record captured during the single engine run ----------
struct TURec {
    uint8_t pass;            // 0=A 1=B 2=C 3=D
    uint32_t region_count;
    std::vector<uint32_t> toks;   // root tokens (obj ids)
    uint32_t blocks_after;
    uint32_t new_blocks;
    uint16_t max_depth;
    uint32_t deep_refs;      // # root tokens that are blocks
    uint32_t base_tokens;    // counterfactual: UNCHANGED input encoded with the SAME predictor (=toks for A/B)
    // byte sizes (root only; def/wire computed in per-F charging)
    uint32_t root_raw, root_z1, root_z3, root_z6;
    bool changed;            // (C/D) differs from its pass-A counterpart
};

// ===================================================================================
// Per-F wire charging (replayed from captured per-TU root tokens; independent of engine).
// ===================================================================================
struct WireTotals {
    // raw byte categories (summed)
    double root_raw=0, root_z3=0;      // per-TU root, summed (root_z3 = sum of independent per-TU zstd)
    double linedef=0, region_comp=0, block_comp=0;   // FILL closure defs (raw), by kind
    double missing=0, framing=0;
    uint64_t objs_sent=0;              // object definitions newly sent (summed over Fs)
    // derived
    double def_raw() const { return linedef+region_comp+block_comp; }
    double total_raw() const { return root_raw+def_raw()+missing+framing; }
};

// ---------- object def sizes (fixed, precomputed) ----------
struct ObjDefs {
    const OnlineEngine* E=nullptr;
    // returns raw def bytes for object o (by kind) and adds to the right WireTotals bucket
    inline uint32_t line_def_bytes(uint32_t o) const { return E->dict->ref(o).len; }
    inline uint32_t region_def_bytes(uint32_t o) const {
        uint32_t r=o-E->REGION_BASE; uint32_t c=E->dict->region_ids_count(r); const uint32_t* ids=E->dict->region_ids_ptr(r);
        uint32_t b=varint_len(c); for(uint32_t j=0;j<c;++j) b+=varint_len(ids[j]); return b;
    }
    inline uint32_t block_def_bytes(uint32_t o) const { uint32_t k=o-E->BLOCK_BASE; return varint_len(E->blkL[k])+varint_len(E->blkR[k]); }
};

// framing model (bytes charged per message frame) — MsgChannel header + MsgPack overhead.
static const double FRAME_ROOT = 12.0;   // per-TU root frame
static const double FRAME_FILL = 12.0;   // per-TU FILL frame (only if any missing objects)

int main(int argc,char**argv){
    const char* manifest=nullptr; size_t max_files=SIZE_MAX; uint32_t promote=4; const char* trace_path=nullptr;
    bool do_sweep=false;
    for(int i=1;i<argc;++i){
        if(!strcmp(argv[i],"--manifest")&&i+1<argc) manifest=argv[++i];
        else if(!strcmp(argv[i],"--promote")&&i+1<argc) promote=uint32_t(atoi(argv[++i]));
        else if(!strcmp(argv[i],"--trace")&&i+1<argc) trace_path=argv[++i];
        else if(!strcmp(argv[i],"--sweep")) do_sweep=true;
        else if(!strcmp(argv[i],"--max-files")&&i+1<argc){ char*t=nullptr; unsigned long long v=strtoull(argv[++i],&t,10); if(!t||*t||!v){fprintf(stderr,"bad --max-files\n");return 2;} max_files=size_t(v); }
        else { fprintf(stderr,"unknown %s\n",argv[i]); return 2; }
    }
    if(!manifest){ fprintf(stderr,"usage: %s --manifest F [--promote N] [--trace file] [--sweep] [--max-files N]\n",argv[0]); return 2; }

    auto t0=Clock::now();
    Corpus A=load_corpus(manifest,max_files);
    uint64_t modified=0; Corpus C=make_poisoned(A,modified);

    // ---- prescan: intern A then C with one warm dict to fix the (stable) object id space ----
    Interner dict;
    Captured capA=capture(dict,A,true);
    uint32_t Nlines_A=dict.distinct(); uint64_t Nregions_A=dict.region_count();
    Captured capC=capture(dict,C,true);
    uint32_t Nlines=dict.distinct(); uint32_t Nregions=uint32_t(dict.region_count());
    fprintf(stderr,"loaded+interned %.1fs: TUs=%zu rawA=%llu | A: lines=%u regions=%llu  | +edit: lines=%u regions=%u (modified TUs=%llu)\n",
        secs(t0),A.files.size(),(unsigned long long)A.raw,Nlines_A,(unsigned long long)Nregions_A,Nlines,Nregions,(unsigned long long)modified);

    // byte-exactness anchor: A line stream reconstructs raw source
    uint64_t rawhash=fnv1a((const uint8_t*)A.bytes.data(),A.raw);
    {
        uint64_t h=1469598103934665603ULL;
        for(uint32_t id:capA.line_ids){ const LineRef&r=dict.ref(id); const char*b=dict.line_data(r.off); for(uint32_t j=0;j<r.len;++j){h^=uint8_t(b[j]);h*=1099511628211ULL;} }
        if(h!=rawhash){ fprintf(stderr,"FATAL: A line stream != raw source\n"); return 1; }
    }
    fprintf(stderr,"byte-exactness anchor OK (A line stream == raw source)\n");

    // ================= OFFLINE CEILING: batch region-BPE over A region streams =================
    // (the target the online learner should approach; recomputed here in the same object accounting.)
    // Reuse the online engine's pair-promotion but train in retrospective batched rounds over ALL of A.
    uint64_t ceil_tokens=0, ceil_root_raw=0; uint32_t ceil_blocks=0;
    {
        OnlineEngine B; B.init(&dict,Nlines,Nregions); B.promote_count=2; // batch: promote any pair seen >=2 globally, iterated
        // iterate rounds: build token streams for all A TUs, count global pairs, promote top, repeat
        size_t TUs=capA.region_off.size()-1;
        // work streams (SENT-separated not needed; encode per TU each round using current merge map)
        std::vector<uint32_t> toks;
        for(int round=0;round<24;++round){
            U64Map pc(1<<20); size_t total_tok=0;
            for(size_t t=0;t<TUs;++t){ B.encode(capA.region_ids.data()+capA.region_off[t], capA.region_off[t+1]-capA.region_off[t], toks); total_tok+=toks.size();
                for(size_t i=0;i+1<toks.size();++i){ uint64_t key=(uint64_t(toks[i])<<32)|toks[i+1]; pc.inc(key); } }
            // collect pairs >= threshold (2), sort desc, promote (batched, non-overlap not enforced -> fine for ceiling)
            std::vector<std::pair<uint32_t,uint64_t>> cand;
            for(size_t i=0;i<pc.k.size();++i) if(pc.k[i]!=UINT64_MAX && pc.v[i]>=2) cand.push_back({pc.v[i],pc.k[i]});
            if(cand.empty()) break;
            std::sort(cand.begin(),cand.end(),std::greater<>());
            for(auto&c:cand){ uint32_t L=uint32_t(c.second>>32),R=uint32_t(c.second); uint64_t key=c.second; if(B.merge.get(key)!=UINT32_MAX) continue;
                uint32_t id=B.BLOCK_BASE+uint32_t(B.blkL.size()); B.blkL.push_back(L);B.blkR.push_back(R);B.blkLeaf.push_back(B.leafcount(L)+B.leafcount(R));B.blkDepth.push_back(uint16_t(1+std::max(B.depthof(L),B.depthof(R))));B.blkCreatedTU.push_back(0);B.blkUses.push_back(0);B.blkFirstUseTU.push_back(0); B.merge.put(key,id); }
        }
        // final encode of A
        ZSTD_CCtx* z=ZSTD_createCCtx(); std::vector<uint8_t> dst,msg;
        for(size_t t=0;t<TUs;++t){ B.encode(capA.region_ids.data()+capA.region_off[t], capA.region_off[t+1]-capA.region_off[t], toks); ceil_tokens+=toks.size(); msg.clear(); for(uint32_t o:toks) put_varint(msg,o); ceil_root_raw+=msg.size(); }
        (void)dst; ZSTD_freeCCtx(z);
        ceil_blocks=B.nblocks();
        fprintf(stderr,"OFFLINE ceiling (batch region-BPE over A): blocks=%u  A root tokens=%llu  root raw=%llu B(%.2f MiB)\n",
            ceil_blocks,(unsigned long long)ceil_tokens,(unsigned long long)ceil_root_raw, ceil_root_raw/1048576.0);
    }

    // ================= ONLINE ENGINE RUN (A -> B -> C -> D, predictor continuous) =================
    // returns per-TU records for the whole four-pass sequence.
    auto run_online = [&](OnlineEngine& E, uint32_t promote_count, std::vector<TURec>& recs, uint32_t& blocksA, uint32_t& blocksB, uint32_t& blocksC, uint32_t& blocksD, double& enc_ns, double& learn_ns, bool do_verify)->bool{
        E.init(&dict,Nlines,Nregions); E.promote_count=promote_count;
        ZSTD_CCtx* z=ZSTD_createCCtx(); std::vector<uint8_t> dst,msg; std::vector<uint32_t> toks, exp, btv;
        uint32_t gord=0; enc_ns=0; learn_ns=0;
        auto do_pass=[&](uint8_t pass,const Captured& cap, bool learn, const Captured* baseline)->bool{
            size_t TUs=cap.region_off.size()-1;
            for(size_t t=0;t<TUs;++t){
                const uint32_t* rids=cap.region_ids.data()+cap.region_off[t]; size_t rn=cap.region_off[t+1]-cap.region_off[t];
                auto e0=Clock::now(); E.encode(rids,rn,toks); enc_ns+=std::chrono::duration<double,std::nano>(Clock::now()-e0).count();
                const uint32_t* ll=cap.line_ids.data()+cap.line_off[t]; size_t ln=cap.line_off[t+1]-cap.line_off[t];
                // verify byte-exact against captured line stream (root tokens expand to the exact line-id stream)
                if(do_verify){ exp.clear(); for(uint32_t o:toks) E.expand(o,exp);
                    if(exp.size()!=ln || memcmp(exp.data(),ll,ln*4)!=0){ fprintf(stderr,"FATAL verify pass=%u TU=%zu (exp %zu vs %zu)\n",pass,t,exp.size(),ln); return false; } }
                // counterfactual: encode the UNCHANGED input (baseline) with the SAME (pre-learn) predictor.
                uint32_t base_tokens=uint32_t(toks.size());
                bool changed=false;
                if(baseline){ const uint32_t* brids=baseline->region_ids.data()+baseline->region_off[t]; size_t brn=baseline->region_off[t+1]-baseline->region_off[t];
                    E.encode(brids,brn,btv); base_tokens=uint32_t(btv.size());
                    const uint32_t* bl=baseline->line_ids.data()+baseline->line_off[t]; size_t bn=baseline->line_off[t+1]-baseline->line_off[t]; changed=(bn!=ln)||memcmp(bl,ll,ln*4)!=0; }
                // count block uses / first-use (from the ACTUAL scored tokens)
                uint16_t md=0; uint32_t deep=0;
                for(uint32_t o:toks){ if(E.is_block(o)){ uint32_t k=o-E.BLOCK_BASE; if(E.blkUses[k]==0) E.blkFirstUseTU[k]=gord; E.blkUses[k]++; md=std::max<uint16_t>(md,E.blkDepth[k]); ++deep; } }
                TURec r; r.pass=pass; r.region_count=uint32_t(rn); r.toks=toks; r.blocks_after=E.nblocks(); r.max_depth=md; r.deep_refs=deep; r.base_tokens=base_tokens; r.changed=changed;
                msg.clear(); for(uint32_t o:toks) put_varint(msg,o); r.root_raw=uint32_t(msg.size());
                r.root_z1=uint32_t(zstd_size(z,msg.data(),msg.size(),1,dst)); r.root_z3=uint32_t(zstd_size(z,msg.data(),msg.size(),3,dst)); r.root_z6=uint32_t(zstd_size(z,msg.data(),msg.size(),6,dst));
                uint32_t before=E.nblocks();
                if(learn){ auto l0=Clock::now(); E.learn(toks,gord); learn_ns+=std::chrono::duration<double,std::nano>(Clock::now()-l0).count(); }
                r.new_blocks=E.nblocks()-before;
                recs.push_back(std::move(r)); ++gord;
            }
            return true;
        };
        // A cold, learn. B warm same tree, learn. C edited tree, learn. D edited tree again, learn.
        if(!do_pass(0,capA,true,nullptr)) return false; blocksA=E.nblocks();
        if(!do_pass(1,capA,true,nullptr)) return false; blocksB=E.nblocks();
        if(!do_pass(2,capC,true,&capA))   return false; blocksC=E.nblocks();
        if(!do_pass(3,capC,true,&capA))   return false; blocksD=E.nblocks();
        ZSTD_freeCCtx(z);
        return true;
    };

    OnlineEngine Eprimary;
    std::vector<TURec> recs; uint32_t bA=0,bB=0,bC=0,bD=0; double enc_ns=0,learn_ns=0;
    if(!run_online(Eprimary,promote,recs,bA,bB,bC,bD,enc_ns,learn_ns,true)) return 1;
    fprintf(stderr,"online run OK (promote=%u): blocks after A=%u B=%u C=%u D=%u  encode=%.2fs learn=%.2fs  (all TUs byte-exact)\n",
        promote,bA,bB,bC,bD,enc_ns/1e9,learn_ns/1e9);

    // ================= PER-F CHARGING (replay captured root tokens) =================
    // The scoring engine Eprimary holds the final block table + per-block use counts.
    OnlineEngine& EF = Eprimary;
    ObjDefs defs; defs.E=&EF;

    // global def compressibility (for def zstd estimate): compress all line texts, region comps, block comps once.
    double line_ratio=1,region_ratio=1,block_ratio=1;
    {
        ZSTD_CCtx* z=ZSTD_createCCtx(); std::vector<uint8_t> dst,buf;
        buf.clear(); for(uint32_t id=1;id<=Nlines;++id){ const LineRef&r=dict.ref(id); buf.insert(buf.end(),dict.line_data(r.off),dict.line_data(r.off)+r.len);} if(!buf.empty()) line_ratio=double(zstd_size(z,buf.data(),buf.size(),3,dst))/buf.size();
        buf.clear(); for(uint32_t r=0;r<Nregions;++r){ uint32_t c=dict.region_ids_count(r); const uint32_t*ids=dict.region_ids_ptr(r); put_varint(buf,c); for(uint32_t j=0;j<c;++j) put_varint(buf,ids[j]); } if(!buf.empty()) region_ratio=double(zstd_size(z,buf.data(),buf.size(),3,dst))/buf.size();
        buf.clear(); for(uint32_t k=0;k<EF.nblocks();++k){ put_varint(buf,EF.blkL[k]); put_varint(buf,EF.blkR[k]); } if(!buf.empty()) block_ratio=double(zstd_size(z,buf.data(),buf.size(),3,dst))/buf.size();
        ZSTD_freeCCtx(z);
        fprintf(stderr,"def compressibility (L3): line=%.3f region=%.3f block=%.3f\n",line_ratio,region_ratio,block_ratio);
    }

    // charge one F-config; returns per-pass WireTotals[4]. policy: 0=round-robin,1=sticky(all to F0),2=shuffled
    auto charge_config=[&](uint32_t Fcount,int policy,bool reset_F_before_B, WireTotals out[4], std::vector<double>* cum_wire_1F)->void{
        std::vector<std::vector<uint8_t>> known(Fcount); // per-F bitset over obj id space
        uint32_t maxobj=EF.BLOCK_BASE+EF.nblocks();
        for(auto&kb:known) kb.assign(maxobj,0);
        for(int i=0;i<4;++i) out[i]=WireTotals{};
        std::vector<uint32_t> missing_stack; missing_stack.reserve(4096);
        double cum=0;
        // recursive-free closure: for each root token, DFS collect missing (children<parent so order fine)
        std::vector<uint32_t> stk;
        for(size_t ti=0; ti<recs.size(); ++ti){
            const TURec& r=recs[ti];
            if(reset_F_before_B && r.pass==1 && ti>0 && recs[ti-1].pass==0){ for(auto&kb:known) std::fill(kb.begin(),kb.end(),0); }
            uint32_t F;
            if(policy==1) F=0;                                   // sticky (single C -> one F)
            else if(policy==2) F=uint32_t(mix64(ti*2654435761u)% Fcount);  // deterministic "shuffled"
            else F=uint32_t(ti % Fcount);                        // round-robin
            std::vector<uint8_t>& kb=known[F];
            WireTotals& W=out[r.pass];
            // root bytes always
            W.root_raw += r.root_raw; W.root_z3 += r.root_z3;
            W.framing += FRAME_ROOT;
            // MISSING + FILL closure: for each root token unknown at F, request + send closure (missing descendants)
            double miss_bytes=0; bool any_missing=false;
            for(uint32_t o:r.toks){
                if(kb[o]) continue;
                any_missing=true; miss_bytes+=varint_len(o);
                // DFS closure of o, charging missing descendants (post-order: children first)
                stk.clear(); stk.push_back(o);
                // iterative post-order via explicit stack with state; simpler: recursive lambda
                // (depth is bounded/small)
                std::function<void(uint32_t)> emit=[&](uint32_t x){
                    if(kb[x]) return;
                    if(x>=EF.BLOCK_BASE){ uint32_t k=x-EF.BLOCK_BASE; emit(EF.blkL[k]); emit(EF.blkR[k]); if(kb[x])return; W.block_comp+=defs.block_def_bytes(x); }
                    else if(x>=EF.REGION_BASE){ // region: children are line objs
                        uint32_t rr=x-EF.REGION_BASE; const uint32_t* ids=EF.dict->region_ids_ptr(rr); uint32_t c=EF.dict->region_ids_count(rr);
                        for(uint32_t j=0;j<c;++j){ uint32_t ln=ids[j]; if(!kb[ln]){ W.linedef+=defs.line_def_bytes(ln); kb[ln]=1; ++W.objs_sent; } }
                        W.region_comp+=defs.region_def_bytes(x);
                    } else { // line
                        W.linedef+=defs.line_def_bytes(x);
                    }
                    kb[x]=1; ++W.objs_sent;
                };
                emit(o);
            }
            W.missing += miss_bytes;
            if(any_missing) W.framing += FRAME_FILL;
            if(cum_wire_1F && Fcount==1){
                // full-wire with zstd(root L3) + zstd(defs via ratio) + missing + framing
                double defz = W.linedef*line_ratio + W.region_comp*region_ratio + W.block_comp*block_ratio; (void)defz;
                cum += r.root_z3 + (any_missing? FRAME_FILL:0) + FRAME_ROOT + miss_bytes;
                // note: def bytes accounted in W; cum here tracks root+frame+missing incremental; we add def incrementally:
                cum_wire_1F->push_back(cum); // def added separately below
            }
        }
    };

    // ---- run the primary config: 1 persistent F, round-robin (single F) for the curves ----
    WireTotals oneF[4]; std::vector<double> cumIgnore;
    charge_config(1,0,false,oneF,nullptr);

    // ================= REPORT =================
    double MiB=1048576.0;
    printf("\n==== ONLINE SUPERBLOCK REPORT ====\n");
    printf("corpus: %s\n",manifest);
    printf("TUs(build)=%zu rawA=%.1f MiB  distinct_lines(A/final)=%u/%u  distinct_regions(A/final)=%llu/%u  promote_count=%u\n",
        A.files.size(),A.raw/MiB,Nlines_A,Nlines,(unsigned long long)Nregions_A,Nregions,promote);
    printf("OFFLINE region-BPE ceiling: blocks=%u  A-root-tokens=%llu  root-raw=%.3f MiB\n",ceil_blocks,(unsigned long long)ceil_tokens,ceil_root_raw/MiB);
    printf("ONLINE blocks after A=%u B=%u C=%u D=%u   encode=%.2fs learn=%.2fs\n",bA,bB,bC,bD,enc_ns/1e9,learn_ns/1e9);

    // per-pass aggregates
    const char* pn[4]={"A COLD_FIRST","B WARM_SAME","C HEADER_EDIT","D CHANGED_STEADY"};
    // per-pass root token totals + block coverage from recs
    uint64_t passTUs[4]={0,0,0,0}, passRegion[4]={0,0,0,0}, passTok[4]={0,0,0,0}, passDeep[4]={0,0,0,0}, passRootRaw[4]={0}, passRootZ3[4]={0};
    for(auto&r:recs){ passTUs[r.pass]++; passRegion[r.pass]+=r.region_count; passTok[r.pass]+=r.toks.size(); passDeep[r.pass]+=r.deep_refs; passRootRaw[r.pass]+=r.root_raw; passRootZ3[r.pass]+=r.root_z3; }
    printf("\n-- per-pass root stream (online, 1 persistent F) --\n");
    printf("%-16s %6s %10s %10s %9s %8s | %12s %12s %12s %12s\n","pass","TUs","regions","roottoks","tok/reg","blkcov%","root_z3","linedef","reg+blk_def","total_raw");
    for(int p=0;p<4;++p){ WireTotals&W=oneF[p];
        double tokreg=passRegion[p]?double(passTok[p])/passRegion[p]:0; double blkcov=passRegion[p]?100.0*double(passRegion[p]-passTok[p])/passRegion[p]:0;
        printf("%-16s %6llu %10llu %10llu %9.3f %8.1f | %12.0f %12.0f %12.0f %12.0f\n",pn[p],(unsigned long long)passTUs[p],(unsigned long long)passRegion[p],(unsigned long long)passTok[p],tokreg,blkcov,
            W.root_z3,W.linedef,W.region_comp+W.block_comp,W.total_raw());
    }
    printf("(blkcov%% = 1 - roottoks/regions = fraction of region tokens absorbed into block refs.)\n");
    printf("ceiling tok/reg (offline, A) = %.3f  -> online A ends at ~%.3f; B (fully warm) at %.3f\n",
        double(ceil_tokens)/double(passRegion[0]?passRegion[0]:1), passTUs[0]?double(passTok[0])/passRegion[0]:0, passTUs[1]?double(passTok[1])/passRegion[1]:0);

    // ---- CONVERGENCE CURVE (pass A): tok/reg over TU ordinal, moving avgs ----
    printf("\n-- CONVERGENCE curve (pass A): tok/region by build progress --\n");
    printf("   cum = cumulative tokens/regions over TUs 1..k (running wire efficiency);\n");
    printf("   inst = instantaneous, moving-avg over last 32 TUs (approaches the ceiling asymptote).\n");
    {
        size_t nA=passTUs[0]; std::vector<double> tr(nA); std::vector<uint64_t> ctok(nA+1,0),creg(nA+1,0);
        for(size_t i=0;i<nA;++i){ const TURec&r=recs[i]; tr[i]= r.region_count? double(r.toks.size())/r.region_count : 1.0;
            ctok[i+1]=ctok[i]+r.toks.size(); creg[i+1]=creg[i]+r.region_count; }
        int marks[]={1,2,5,10,25,50,75,100}; double ceil_tr=double(ceil_tokens)/double(passRegion[0]?passRegion[0]:1);
        printf("%8s %10s %10s %10s %10s\n","progress","TU_ord","cum","inst_ma32","gap_to_ceil");
        for(int m:marks){ size_t idx= (m>=100)? nA-1 : (nA*m/100); if(idx>=nA) idx=nA-1;
            size_t w0= idx>=32? idx-31:0; double s=0; int c=0; for(size_t j=w0;j<=idx;++j){s+=tr[j];++c;}
            double ma=c?s/c:0; double cum= creg[idx+1]? double(ctok[idx+1])/creg[idx+1]:0;
            printf("%7d%% %10zu %10.3f %10.3f %10.3f\n",m,idx+1,cum,ma, ma-ceil_tr); }
        printf("ceiling tok/reg = %.3f (offline batch region-BPE over A)\n",ceil_tr);
    }

    // ---- RECOVERY CURVE (pass C): pure edit damage by affected-TU ordinal ----
    // Damage is isolated via the counterfactual: base_tokens = the UNCHANGED input encoded with the
    // SAME pre-learn predictor. delta = actual root tokens - base_tokens = extra tokens caused purely
    // by the edit at that predictor maturity (removes the confound that the predictor keeps learning).
    printf("\n-- RECOVERY curve (pass C header edit): pure edit damage (delta vs same-predictor UNCHANGED) --\n");
    {
        size_t Coff=passTUs[0]+passTUs[1];
        size_t nC=passTUs[2]; size_t changed=0; uint64_t new_blocks_C=0;
        for(size_t i=0;i<nC;++i){ const TURec&rc=recs[Coff+i]; if(rc.changed) ++changed; new_blocks_C+=rc.new_blocks; }
        printf("modified TUs in C=%llu of %zu; new blocks created during C=%llu\n",(unsigned long long)changed,nC,(unsigned long long)new_blocks_C);
        printf("%8s %8s %10s %10s %8s %9s\n","chg_ord","TU","act_toks","base_toks","dTok","new_blk");
        size_t shown=0; long first_dt=-1;
        for(size_t i=0;i<nC && shown<14;++i){ const TURec&rc=recs[Coff+i]; if(!rc.changed) continue;
            long dt=long(rc.toks.size())-long(rc.base_tokens); if(first_dt<0) first_dt=dt;
            printf("%8zu %8zu %10zu %10u %+8ld %9u\n",++shown,i,rc.toks.size(),rc.base_tokens,dt,rc.new_blocks); }
        // aggregate pure edit damage across all changed C TUs, and recovery: how many changed TUs until dTok<=0
        long total_dt=0, worst=0; size_t recovered_at=0; size_t seen=0;
        for(size_t i=0;i<nC;++i){ const TURec&rc=recs[Coff+i]; if(!rc.changed) continue; long dt=long(rc.toks.size())-long(rc.base_tokens); total_dt+=dt; if(dt>worst)worst=dt; ++seen; if(dt<=0 && recovered_at==0 && seen>1) recovered_at=seen; }
        printf("C pure edit damage: first-changed dTok=%+ld  worst=%+ld  total=%+ld (%.4f/changed-TU)  recovered@changed-ord=%zu\n",
            first_dt,worst,total_dt,changed?double(total_dt)/changed:0,recovered_at);
        // D: pure damage after one full changed-build loop (should be ~0 = fully recovered steady state)
        size_t Doff=passTUs[0]+passTUs[1]+passTUs[2]; long dD=0; size_t dchg=0;
        for(size_t i=0;i<passTUs[3];++i){ const TURec&rd=recs[Doff+i]; if(!rd.changed) continue; dD+=long(rd.toks.size())-long(rd.base_tokens); ++dchg; }
        printf("D pure edit damage (after one changed loop): total=%+ld (%.4f/changed-TU) -> new steady state\n",dD,dchg?double(dD)/dchg:0);
    }

    // ---- block lifetime quality (from charging engine EF uses) ----
    {
        uint64_t used=0, never=0, reuse_ge2=0; uint64_t leafsum=0; uint32_t maxdepth=0; uint64_t neverbytes=0;
        for(uint32_t k=0;k<EF.nblocks();++k){ if(EF.blkUses[k]==0) {never++; neverbytes+=defs.block_def_bytes(EF.BLOCK_BASE+k);} else {used++; if(EF.blkUses[k]>=2) reuse_ge2++;} leafsum+=EF.blkLeaf[k]; maxdepth=std::max<uint32_t>(maxdepth,EF.blkDepth[k]); }
        printf("\n-- block lifetime -- total=%u used>=1=%llu used>=2=%llu never=%llu (never-def-bytes=%llu) mean_leaf=%.1f max_depth=%u\n",
            EF.nblocks(),(unsigned long long)used,(unsigned long long)reuse_ge2,(unsigned long long)never,(unsigned long long)neverbytes,EF.nblocks()?double(leafsum)/EF.nblocks():0,maxdepth);
    }

    // ---- F-sweep: full-wire per pass under 1/4/8/16/32 F, round-robin + sticky ----
    printf("\n-- PER-F full-wire (raw bytes, def sent once per F that needs it) --\n");
    printf("%-16s %6s %8s | %14s %14s %14s %14s %14s\n","pass","F","policy","root_raw","def_raw","missing","framing","TOTAL_raw");
    int Fs[5]={1,4,8,16,32};
    for(int fi=0;fi<5;++fi){ for(int pol=0;pol<2;++pol){ // 0=RR, 1=sticky
        WireTotals W[4]; charge_config(uint32_t(Fs[fi]),pol,false,W,nullptr);
        for(int p=0;p<4;++p) printf("%-16s %6d %8s | %14.0f %14.0f %14.0f %14.0f %14.0f\n",pn[p],Fs[fi],pol?"sticky":"rr",W[p].root_raw,W[p].def_raw(),W[p].missing,W[p].framing,W[p].total_raw());
    } }
    printf("(sticky=all TUs of the single build-C to one F => equals 1 warm F regardless of F count.)\n");

    // ---- threshold sweep (final steady-state B tok/reg + blocks) ----
    if(do_sweep){
        printf("\n-- promote-threshold sweep (B tok/reg = warm steady state; lower=better, ceiling=%.3f) --\n",double(ceil_tokens)/double(passRegion[0]?passRegion[0]:1));
        printf("%10s %10s %12s %12s\n","promote","blocksB","B_tok/reg","B_root_z3");
        for(uint32_t T : {2u,3u,4u,8u,16u,32u}){
            OnlineEngine Esw; std::vector<TURec> rr; uint32_t a,b,c,d; double e,l; if(!run_online(Esw,T,rr,a,b,c,d,e,l,false)) continue;
            uint64_t reg=0,tok=0,z3=0; size_t nA=0; for(auto&r:rr) if(r.pass==0)++nA; for(auto&r:rr) if(r.pass==1){reg+=r.region_count;tok+=r.toks.size();z3+=r.root_z3;}
            printf("%10u %10u %12.3f %12llu\n",T,b,reg?double(tok)/reg:0,(unsigned long long)z3);
        }
    }

    // ---- per-TU machine-readable trace ----
    if(trace_path){
        FILE* tf=fopen(trace_path,"w"); if(!tf){perror(trace_path);} else {
            fprintf(tf,"pass\ttu_ord\tregion_count\troot_tokens\tblocks_after\tnew_blocks\tmax_depth\tdeep_refs\troot_raw\troot_z1\troot_z3\troot_z6\tchanged\ttok_per_reg\n");
            uint32_t ord=0; for(auto&r:recs){ fprintf(tf,"%u\t%u\t%u\t%zu\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%d\t%.4f\n",
                r.pass,ord,r.region_count,r.toks.size(),r.blocks_after,r.new_blocks,r.max_depth,r.deep_refs,r.root_raw,r.root_z1,r.root_z3,r.root_z6,r.changed?1:0, r.region_count?double(r.toks.size())/r.region_count:0); ++ord; }
            fclose(tf); fprintf(stderr,"wrote per-TU trace: %s (%zu rows)\n",trace_path,recs.size());
        }
    }

    struct rusage ru{}; getrusage(RUSAGE_SELF,&ru);
    fprintf(stderr,"peak RSS=%.2f MiB  total=%.1fs\n",ru.ru_maxrss/1024.0,secs(t0));
    return 0;
}
