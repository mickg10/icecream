// online-superblock.cpp
//
// ONLINE encode-then-learn benchmark for icecream issue #16 (cross-TU line-dedup +
// superblock compression transport). Authoritative spec: ONLINE-CORRECTION.md
// (local-oracle), with byte-accounting/per-F detail from FOUR-PASS-SPEC.md.
//
// THE REFRAME: the predictor is a martingale-adapted process. At TU t we have PERFECT
// knowledge of TU_t and all prior TUs 1..t-1 and ZERO knowledge of the future. The only
// lever is how fast we LEARN and AMORTIZE recurring structure from the past. So the figure
// of merit is a CONVERGENCE curve (online cumulative wire vs the offline batch region-BPE
// optimum = regret) and a RECOVERY curve after a one-line header edit.
//
// The predictor GROWS CONTINUOUSLY across all four passes; it NEVER freezes (frozen-after-A
// is a labelled control only). Published object IDs are immutable: "improve a block" =
// allocate a NEW immutable Block ID; never rebind an existing ID.
//
// Machinery (Interner / marker regions / stable IDs / byte accounting / byte-exact
// reconstruction / batch BPE) is reused from superblock-bench.cpp. The frozen train/score
// driver is superseded; the online loop is built around the reusable pieces.
//
// build: g++ -O3 -DNDEBUG -march=native -std=c++17 online-superblock.cpp -o online-superblock -lzstd

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <immintrin.h>
#include <zstd.h>

using Clock = std::chrono::steady_clock;
static double seconds_since(Clock::time_point b) { return std::chrono::duration<double>(Clock::now() - b).count(); }

// ------------------------------- hashing (verbatim from superblock-bench.cpp) -------------------------------
static inline uint64_t mix64(uint64_t x){ x^=x>>30; x*=0xbf58476d1ce4e5b9ULL; x^=x>>27; x*=0x94d049bb133111ebULL; return x^(x>>31); }
static inline uint64_t fold128(uint64_t a,uint64_t b){ __uint128_t p=__uint128_t(a)*b; return uint64_t(p)^uint64_t(p>>64); }
static inline uint64_t read64(const char*p){ uint64_t v; memcpy(&v,p,8); return v; }
static inline uint64_t read_tail(const char*p,uint32_t n){ uint64_t v=0; memcpy(&v,p,n); return v; }
static inline uint64_t sampled_hash(const char*p,uint32_t n){
    constexpr uint64_t A=0xa0761d6478bd642fULL,B=0xe7037ed1a0b428dbULL;
    uint64_t h=mix64(uint64_t(n)^A);
    if(n<=8) return mix64(h^read_tail(p,n));
    if(n<=16) return fold128(read64(p)^A,read64(p+n-8)^h);
    if(n<=32){ h=fold128(read64(p)^A,read64(p+8)^h); return fold128(read64(p+n-16)^B,read64(p+n-8)^h); }
    uint32_t mid=(n>>1)-4;
    h=fold128(read64(p)^A,read64(p+8)^h);
    h=fold128(read64(p+mid)^B,read64(p+n-16)^h);
    return fold128(read64(p+n-8)^A,h^B);
}
static inline uint64_t line_hash(const char*key,uint32_t len){
    constexpr uint64_t secret[3]={0x2d358dccaa6c78a5ULL,0x8bb84b93962eacc9ULL,0x4b33a62ed433d4a3ULL};
    uint64_t seed=0xbdd89aa982704029ULL^uint64_t(len);
    const char*p=key; uint32_t n=len;
    if(n<=16){ uint64_t a=0,b=0; if(n>=8){a=read64(p);b=read64(p+n-8);} else if(n){a=read_tail(p,n);b=a;} return fold128(a^secret[0],b^seed^secret[1]); }
    uint64_t a=read64(p)^secret[0], b=read64(p+8)^seed; p+=16; n-=16;
    while(n>=48){ seed=fold128(read64(p)^secret[0],read64(p+8)^seed); a=fold128(read64(p+16)^secret[1],read64(p+24)^a); b=fold128(read64(p+32)^secret[2],read64(p+40)^b); p+=48; n-=48; }
    while(n>=16){ seed=fold128(read64(p)^secret[0],read64(p+8)^seed); p+=16; n-=16; }
    if(n){ uint64_t x=n>=8?read64(p):read_tail(p,n); uint64_t y=n>=8?read64(p+n-8):x; seed=fold128(x^secret[1],y^seed); }
    return fold128(a^secret[0],b^seed^secret[2]);
}
static inline const char* next_region(const char*p,const char*end){
    const char*q=p+1;
#if defined(__AVX512BW__)
    const __m512i hashes=_mm512_set1_epi8('#');
    while(q+64<=end){ __m512i v=_mm512_loadu_si512((const void*)q); uint64_t mask=_mm512_cmpeq_epi8_mask(v,hashes);
        while(mask){ unsigned bit=(unsigned)__builtin_ctzll(mask); const char*c=q+bit; if(c[-1]=='\n'&&c+1<end&&c[1]==' ') return c; mask&=mask-1; } q+=64; }
#elif defined(__AVX2__)
    const __m256i hashes=_mm256_set1_epi8('#');
    while(q+32<=end){ __m256i v=_mm256_loadu_si256((const __m256i*)q); uint32_t mask=(uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v,hashes));
        while(mask){ unsigned bit=(unsigned)__builtin_ctz(mask); const char*c=q+bit; if(c[-1]=='\n'&&c+1<end&&c[1]==' ') return c; mask&=mask-1; } q+=32; }
#endif
    while(q+1<end){ if(*q=='#'&&q[-1]=='\n'&&q[1]==' ') return q; ++q; }
    return end;
}
static void* huge_zeroed(size_t bytes){ constexpr size_t H=2u<<20; bytes=(bytes+H-1)&~(H-1);
    void*p=mmap(nullptr,bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(p==MAP_FAILED){ perror("mmap"); exit(2);} madvise(p,bytes,MADV_HUGEPAGE); return p; }

struct FileSpan{ uint64_t off; uint32_t len; };
struct LineRef { uint32_t off; uint32_t len; };
struct TinySlot{ uint64_t bytes; uint32_t id; uint8_t len; uint8_t pad[3]; };
struct ShortSlot{ uint64_t lo; uint64_t hi; uint32_t id; uint8_t len; uint8_t pad[3]; };
struct LineSlot{ uint64_t hash; uint32_t off; uint32_t len; uint32_t id; uint32_t pad; };
struct RegionRecord{ uint64_t hash; uint32_t raw_off; uint32_t raw_len; uint32_t ids_off; uint32_t ids_count; uint32_t next1; uint32_t next2; };

// ------------------------------- Interner (verbatim, region_out sink) -------------------------------
class Interner{
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
    const char* region_raw_ptr(uint32_t rid) const { return region_bytes_.data()+region_records_[rid].raw_off; }
    uint32_t region_raw_len(uint32_t rid) const { return region_records_[rid].raw_len; }

    void process(const char*begin,const char*end,uint32_t*out,size_t&out_count,uint64_t&region_hits,bool train_predictor,std::vector<uint32_t>*region_out){
        const char*p=begin; uint32_t previous=UINT32_MAX;
        while(p<end){
            bool found=false; uint32_t region_id=UINT32_MAX; uint32_t n=0; uint64_t h=0;
            if(previous!=UINT32_MAX){
                const RegionRecord&prev=region_records_[previous];
                const uint32_t candidates[2]={prev.next1,prev.next2};
                for(uint32_t encoded:candidates){
                    if(!encoded) continue; uint32_t cid=encoded-1; const RegionRecord&cand=region_records_[cid];
                    if(cand.raw_len>uint64_t(end-p)) continue; const char*ce=p+cand.raw_len;
                    bool eb= ce==end || (ce+1<end && ce[-1]=='\n' && ce[0]=='#' && ce[1]==' ');
                    if(eb && memcmp(region_bytes_.data()+cand.raw_off,p,cand.raw_len)==0){
                        region_id=cid; n=cand.raw_len;
                        memcpy(out+out_count,region_ids_.data()+cand.ids_off,size_t(cand.ids_count)*sizeof(uint32_t));
                        out_count+=cand.ids_count; ++region_hits; found=true; break;
                    }
                }
            }
            if(!found){
                const char*q=next_region(p,end); n=uint32_t(q-p); h=sampled_hash(p,n)|1ULL;
                uint32_t slot=uint32_t(h)&region_mask_; uint32_t probes=0;
                for(;;){ if(++probes>region_index_.size()){ fprintf(stderr,"region table exhausted\n"); exit(2);} uint32_t enc=region_index_[slot]; if(!enc) break;
                    RegionRecord&r=region_records_[enc-1];
                    if(r.hash==h&&r.raw_len==n&&memcmp(region_bytes_.data()+r.raw_off,p,n)==0){
                        region_id=enc-1; memcpy(out+out_count,region_ids_.data()+r.ids_off,size_t(r.ids_count)*sizeof(uint32_t)); out_count+=r.ids_count; ++region_hits; found=true; break; }
                    slot=(slot+1)&region_mask_; }
            }
            if(!found){
                const char*q=p+n; uint32_t ids_off=uint32_t(region_ids_.size()); const char*lp=p;
                while(lp<q){ const void*hit=memchr(lp,'\n',size_t(q-lp)); const char*le=hit?(const char*)hit+1:q; uint32_t id=intern_line(lp,uint32_t(le-lp)); region_ids_.push_back(id); out[out_count++]=id; lp=le; }
                uint32_t raw_off=uint32_t(region_bytes_.size()); region_bytes_.insert(region_bytes_.end(),p,q);
                region_id=uint32_t(region_records_.size());
                region_records_.push_back({h,raw_off,n,ids_off,uint32_t(region_ids_.size()-ids_off),0,0}); insert_region_index(region_id);
            }
            if(region_out) region_out->push_back(region_id);
            if(train_predictor && previous!=UINT32_MAX){ RegionRecord&prev=region_records_[previous]; uint32_t enc=region_id+1; if(!prev.next1) prev.next1=enc; else if(prev.next1!=enc&&!prev.next2) prev.next2=enc; }
            previous=region_id; p+=n;
        }
    }
    uint32_t intern_line(const char*p,uint32_t n){
        if(n<=4) return intern_tiny(p,n);
        if(n<=16) return intern_short(p,n);
        uint64_t h=line_hash(p,n)|1ULL; uint32_t slot=uint32_t(h)&(LINE_CAP-1); uint32_t probes=0;
        for(;;){ if(++probes>LINE_CAP){ fprintf(stderr,"line table exhausted\n"); exit(2);} LineSlot&s=lines_[slot];
            if(!s.id){ uint32_t id=add_line(p,n); s={h,id_refs_[id].off,n,id,0}; return id; }
            if(s.hash==h&&s.len==n&&memcmp(line_bytes_.data()+s.off,p,n)==0) return s.id; slot=(slot+1)&(LINE_CAP-1); }
    }
private:
    void insert_region_index(uint32_t region_id){
        if((region_records_.size()*10)>(region_index_.size()*7)){ std::vector<uint32_t> grown(region_index_.size()*2); uint32_t nm=uint32_t(grown.size()-1);
            for(uint32_t id=0;id<region_records_.size()-1;++id){ uint32_t slot=uint32_t(region_records_[id].hash)&nm; while(grown[slot]) slot=(slot+1)&nm; grown[slot]=id+1; }
            region_index_.swap(grown); region_mask_=nm; }
        uint32_t slot=uint32_t(region_records_[region_id].hash)&region_mask_; while(region_index_[slot]) slot=(slot+1)&region_mask_; region_index_[slot]=region_id+1;
    }
    static size_t rounded(size_t n){ constexpr size_t H=2u<<20; return (n+H-1)&~(H-1); }
    uint32_t add_line(const char*p,uint32_t n){ if(next_id_==0){ fprintf(stderr,"ID overflow\n"); exit(2);} uint32_t off=uint32_t(line_bytes_.size()); line_bytes_.insert(line_bytes_.end(),p,p+n); uint32_t id=next_id_++; id_refs_.push_back({off,n}); return id; }
    uint32_t intern_tiny(const char*p,uint32_t n){ uint64_t bytes=read_tail(p,n); uint32_t slot=uint32_t(mix64(bytes^(uint64_t(n)<<56)))&(TINY_CAP-1); uint32_t probes=0;
        for(;;){ if(++probes>TINY_CAP){ fprintf(stderr,"tiny table exhausted\n"); exit(2);} TinySlot&s=tiny_[slot]; if(!s.id){ uint32_t id=add_line(p,n); s.bytes=bytes; s.id=id; s.len=uint8_t(n); return id; } if(s.len==n&&s.bytes==bytes) return s.id; slot=(slot+1)&(TINY_CAP-1); } }
    uint32_t intern_short(const char*p,uint32_t n){ uint64_t lo=n>=8?read64(p):read_tail(p,n); uint64_t hi=n>8?read_tail(p+8,n-8):lo; uint64_t h=fold128(lo^0xa0761d6478bd642fULL,hi^uint64_t(n)*0xe7037ed1a0b428dbULL); uint32_t slot=uint32_t(h)&(SHORT_CAP-1); uint32_t probes=0;
        for(;;){ if(++probes>SHORT_CAP){ fprintf(stderr,"short table exhausted\n"); exit(2);} ShortSlot&s=short_[slot]; if(!s.id){ uint32_t id=add_line(p,n); s.lo=lo; s.hi=hi; s.id=id; s.len=uint8_t(n); return id; } if(s.len==n&&s.lo==lo&&s.hi==hi) return s.id; slot=(slot+1)&(SHORT_CAP-1); } }
    TinySlot*tiny_=nullptr; ShortSlot*short_=nullptr; LineSlot*lines_=nullptr;
    std::vector<uint32_t> region_index_; std::vector<RegionRecord> region_records_;
    std::vector<char> line_bytes_; std::vector<char> region_bytes_; std::vector<uint32_t> region_ids_; std::vector<LineRef> id_refs_;
    uint32_t next_id_=1; uint32_t region_mask_=0;
};

// ------------------------------- corpus -------------------------------
struct Corpus{ std::vector<char> bytes; std::vector<FileSpan> files; uint64_t raw=0; };

static Corpus load_corpus(const char*manifest,size_t max_files){
    FILE*mf=fopen(manifest,"r"); if(!mf){ perror(manifest); exit(2);} std::vector<std::string> paths; char path[8192]; uint64_t total=0;
    while(fgets(path,sizeof path,mf)){ size_t n=strlen(path); while(n&&(path[n-1]=='\n'||path[n-1]=='\r')) path[--n]=0; if(!n) continue;
        struct stat st{}; if(stat(path,&st)!=0){ perror(path); exit(2);} if(st.st_size<0||uint64_t(st.st_size)>UINT32_MAX){ fprintf(stderr,"bad size %s\n",path); exit(2);} paths.emplace_back(path); total+=uint64_t(st.st_size); if(paths.size()==max_files) break; }
    fclose(mf); Corpus c; c.bytes.resize(size_t(total)+64); c.files.reserve(paths.size()); uint64_t off=0;
    for(const auto&p:paths){ FILE*f=fopen(p.c_str(),"rb"); if(!f){ perror(p.c_str()); exit(2);} struct stat st{}; fstat(fileno(f),&st); size_t n=size_t(st.st_size); if(n&&fread(c.bytes.data()+off,1,n,f)!=n){ fprintf(stderr,"short read %s\n",p.c_str()); exit(2);} fclose(f); c.files.push_back({off,uint32_t(n)}); off+=n; }
    c.raw=off; return c;
}
// permuted TU order (shares bytes; returns only the reordered file spans). order: 0=orig,1=reverse,2+=shuffle(seed).
static std::vector<FileSpan> permuted_files(const Corpus& src,int order,uint64_t seed){
    size_t n=src.files.size(); std::vector<uint32_t> idx(n); for(size_t i=0;i<n;++i) idx[i]=uint32_t(i);
    if(order==1){ std::reverse(idx.begin(),idx.end()); }
    else if(order>=2){ uint64_t s=seed?seed:0x9e3779b97f4a7c15ULL;
        for(size_t i=n;i>1;--i){ s=mix64(s+ i*0x100000001b3ULL); size_t j=size_t(s%(i)); std::swap(idx[i-1],idx[j]); } }
    std::vector<FileSpan> nf(n); for(size_t i=0;i<n;++i) nf[i]=src.files[idx[i]]; return nf;
}

// controlled edit form (i): insert one line after the common stdc-predef marker (spec form i).
static Corpus make_poisoned_corpus(const Corpus&src,uint64_t&modified){
    static constexpr char marker[]="# 1 \"/usr/include/stdc-predef.h\" 1 3 4\n";
    static constexpr char inserted[]="typedef int local_oracle_inserted_line;\n";
    Corpus r; r.files.reserve(src.files.size()); r.bytes.reserve(src.bytes.size()+src.files.size()*sizeof(inserted)+1024); modified=0;
    for(const FileSpan&f:src.files){ const char*b=src.bytes.data()+f.off; const char*e=b+f.len; const char*at=std::search(b,e,marker,marker+sizeof(marker)-1); uint64_t oo=r.bytes.size();
        if(at==e){ r.bytes.insert(r.bytes.end(),b,e);} else { const char*after=at+sizeof(marker)-1; r.bytes.insert(r.bytes.end(),b,after); r.bytes.insert(r.bytes.end(),inserted,inserted+sizeof(inserted)-1); r.bytes.insert(r.bytes.end(),after,e); ++modified; }
        uint64_t ol=r.bytes.size()-oo; if(ol>UINT32_MAX){ fprintf(stderr,"poisoned TU too large\n"); exit(2);} r.files.push_back({oo,uint32_t(ol)}); }
    r.raw=r.bytes.size(); r.bytes.resize(r.bytes.size()+64); return r;
}

// edit form (ii): pick a high-fanout region (appears in the most distinct TUs) and append one line
// to it, changing that region's bytes in EVERY TU that includes it (a real project-header analog).
// Byte-exact: we reconstruct the EDITED bytes. Returns affected-TU count and the target region.
static Corpus make_region_edit_corpus(const Corpus& src,const char* needle,uint32_t needle_len,
                                      uint64_t& modified,std::vector<uint8_t>& affected){
    static const char insert[]="// icecream_header_edit\n";
    Corpus r; r.files.reserve(src.files.size()); r.bytes.reserve(src.bytes.size()+src.files.size()*sizeof(insert)+1024); modified=0;
    affected.assign(src.files.size(),0);
    for(size_t fi=0;fi<src.files.size();++fi){ const FileSpan& f=src.files[fi]; const char* b=src.bytes.data()+f.off; const char* e=b+f.len; uint64_t oo=r.bytes.size(); const char* p=b; bool hit=false;
        while(p<e){ const char* m=(const char*)memmem(p,size_t(e-p),needle,needle_len); if(!m){ r.bytes.insert(r.bytes.end(),p,e); break; }
            r.bytes.insert(r.bytes.end(),p,m+needle_len); r.bytes.insert(r.bytes.end(),insert,insert+sizeof(insert)-1); p=m+needle_len; hit=true; }
        if(hit){ ++modified; affected[fi]=1; }
        uint64_t ol=r.bytes.size()-oo; if(ol>UINT32_MAX){ fprintf(stderr,"edited TU too large\n"); exit(2);} r.files.push_back({oo,uint32_t(ol)}); }
    r.raw=r.bytes.size(); r.bytes.resize(r.bytes.size()+64); return r;
}

// ------------------------------- varint / zstd -------------------------------
static inline void put_varint(std::vector<uint8_t>&o,uint64_t v){ while(v>=0x80){ o.push_back(uint8_t(v)|0x80); v>>=7;} o.push_back(uint8_t(v)); }
static inline uint32_t varint_len(uint64_t v){ uint32_t n=1; while(v>=0x80){ v>>=7; ++n;} return n; }
static uint64_t hash_bytes(const uint8_t*p,size_t n){ uint64_t h=1469598103934665603ULL; for(size_t i=0;i<n;++i){ h^=p[i]; h*=1099511628211ULL;} return h; }

static size_t zstd_ctx(ZSTD_CCtx*c,const uint8_t*d,size_t n,int level,std::vector<uint8_t>&dst){
    ZSTD_CCtx_reset(c,ZSTD_reset_session_and_parameters); ZSTD_CCtx_setParameter(c,ZSTD_c_compressionLevel,level);
    size_t bound=ZSTD_compressBound(n); if(dst.size()<bound) dst.resize(bound);
    size_t r=ZSTD_compress2(c,dst.data(),dst.size(),d?d:(const uint8_t*)"",n); if(ZSTD_isError(r)){ fprintf(stderr,"zstd: %s\n",ZSTD_getErrorName(r)); exit(2);} return r; }

// ------------------------------- object codes -------------------------------
// region code = rid<<1 ; block code = (bidx<<1)|1  (LSB kind tag keeps varints tight)
static inline uint32_t region_code(uint32_t rid){ return rid<<1; }
static inline uint32_t block_code(uint32_t bidx){ return (bidx<<1)|1u; }
static inline bool is_block(uint32_t code){ return code&1u; }
static inline uint32_t code_index(uint32_t code){ return code>>1; }
static constexpr uint32_t SENT=0xFFFFFFFFu;

// ------------------------------- fast open-addressing maps -------------------------------
struct U64Map { // key(u64,!=EMPTY) -> u64 value
    static constexpr uint64_t EMPTY=UINT64_MAX;
    std::vector<uint64_t> k,v; size_t mask,cnt=0;
    explicit U64Map(size_t cap=1<<16){ size_t c=1; while(c<cap) c<<=1; k.assign(c,EMPTY); v.assign(c,0); mask=c-1; }
    inline uint64_t* find(uint64_t key){ size_t i=mix64(key)&mask; for(;;){ if(k[i]==key) return &v[i]; if(k[i]==EMPTY) return nullptr; i=(i+1)&mask; } }
    inline uint64_t& add_inc(uint64_t key){ // returns ref to value (0 if new), caller increments
        if((cnt*10)>=(k.size()*7)) grow();
        size_t i=mix64(key)&mask; for(;;){ if(k[i]==key) return v[i]; if(k[i]==EMPTY){ k[i]=key; v[i]=0; ++cnt; return v[i]; } i=(i+1)&mask; } }
    void set(uint64_t key,uint64_t val){ if((cnt*10)>=(k.size()*7)) grow(); size_t i=mix64(key)&mask; for(;;){ if(k[i]==key){ v[i]=val; return;} if(k[i]==EMPTY){ k[i]=key; v[i]=val; ++cnt; return;} i=(i+1)&mask; } }
    void grow(){ std::vector<uint64_t> nk(k.size()*2,EMPTY),nv(k.size()*2,0); size_t nmask=nk.size()-1; for(size_t i=0;i<k.size();++i) if(k[i]!=EMPTY){ size_t j=mix64(k[i])&nmask; while(nk[j]!=EMPTY) j=(j+1)&nmask; nk[j]=k[i]; nv[j]=v[i]; } k.swap(nk); v.swap(nv); mask=nmask; }
    void clear(){ std::fill(k.begin(),k.end(),EMPTY); std::fill(v.begin(),v.end(),0); cnt=0; }
    size_t size() const { return cnt; }
};

// ------------------------------- BlockStore (shared by online learner + batch ceiling) -------------------------------
struct BlockStore {
    std::vector<std::pair<uint32_t,uint32_t>> child;   // bidx -> (codeA, codeB) for pair blocks
    std::vector<uint32_t> leaf_len;                    // #region leaves
    std::vector<uint32_t> created_tu, use_tu_first, use_count, ref_count;
    U64Map pair2block;                                 // (codeA<<32|codeB) -> bidx+1
    uint32_t nblocks() const { return uint32_t(child.size()); }
    uint32_t add(uint32_t ca,uint32_t cb,uint32_t tu){
        uint32_t bidx=uint32_t(child.size());
        child.push_back({ca,cb});
        leaf_len.push_back(leaf_len_code(ca)+leaf_len_code(cb));
        created_tu.push_back(tu); use_tu_first.push_back(UINT32_MAX); use_count.push_back(0); ref_count.push_back(0);
        pair2block.set((uint64_t(ca)<<32)|cb, bidx+1);
        return bidx;
    }
    inline uint32_t leaf_len_code(uint32_t code) const { return is_block(code)? leaf_len[code_index(code)] : 1; }
    inline uint32_t lookup(uint32_t ca,uint32_t cb) const {
        uint64_t* p=const_cast<U64Map&>(pair2block).find((uint64_t(ca)<<32)|cb); return p? uint32_t(*p)-1 : UINT32_MAX; }
    // fixpoint greedy re-tiling; byte-exact (each block expands to its two children).
    void tokenize(const std::vector<uint32_t>& regions, std::vector<uint32_t>& out) const {
        out.clear(); out.reserve(regions.size()); for(uint32_t r:regions) out.push_back(region_code(r));
        if(child.empty()) return;
        std::vector<uint32_t> nt; bool changed=true;
        while(changed){ changed=false; nt.clear(); nt.reserve(out.size()); size_t i=0,N=out.size();
            while(i<N){ if(i+1<N){ uint32_t b=lookup(out[i],out[i+1]); if(b!=UINT32_MAX){ nt.push_back(block_code(b)); i+=2; changed=true; continue; } } nt.push_back(out[i]); ++i; }
            out.swap(nt); }
    }
    void expand(uint32_t code,std::vector<uint32_t>& regions_out) const {
        if(!is_block(code)){ regions_out.push_back(code_index(code)); return; }
        const auto& c=child[code_index(code)]; expand(c.first,regions_out); expand(c.second,regions_out);
    }
    uint32_t depth(uint32_t code) const { if(!is_block(code)) return 0; const auto&c=child[code_index(code)]; return 1+std::max(depth(c.first),depth(c.second)); }
};

// closure of block codes referenced by a root token list (block descendants included), sorted asc bidx
static void block_closure(const BlockStore& bs,const std::vector<uint32_t>& root,std::vector<uint32_t>& out,std::vector<uint8_t>& seen){
    out.clear(); if(seen.size()<bs.nblocks()) seen.assign(bs.nblocks(),0);
    // stack-based
    static thread_local std::vector<uint32_t> stk; stk.clear();
    for(uint32_t c:root) if(is_block(c)) stk.push_back(code_index(c));
    while(!stk.empty()){ uint32_t b=stk.back(); stk.pop_back(); if(seen[b]) continue; seen[b]=1; out.push_back(b);
        const auto& ch=bs.child[b]; if(is_block(ch.first)) stk.push_back(code_index(ch.first)); if(is_block(ch.second)) stk.push_back(code_index(ch.second)); }
    for(uint32_t b:out) seen[b]=0;   // reset for reuse
    std::sort(out.begin(),out.end());
}

// ------------------------------- per-TU record -------------------------------
struct TURec {
    uint32_t ord=0;
    std::vector<uint32_t> region_seq;   // region leaves in order
    std::vector<uint32_t> root_codes;   // tokenized root stream
    std::vector<uint32_t> blk_clos;     // block indices referenced (closure, asc)
    uint32_t new_lines=0,new_regions=0,new_blocks=0;
    uint32_t deepest_leaf=0;            // deepest block leaf_len used
    uint32_t root_tok=0;
};

// ------------------------------- F store (per-F known object sets) -------------------------------
struct FStore {
    std::vector<uint8_t> lines, regions, blocks;
    void ensure(size_t nl,size_t nr,size_t nb){ if(lines.size()<nl) lines.resize(nl,0); if(regions.size()<nr) regions.resize(nr,0); if(blocks.size()<nb) blocks.resize(nb,0); }
    void reset(){ std::fill(lines.begin(),lines.end(),0); std::fill(regions.begin(),regions.end(),0); std::fill(blocks.begin(),blocks.end(),0); }
};

// byte accounting for one TU charged to one F
struct Acc {
    uint64_t root_raw=0, root_z1=0, root_z3=0, root_z6=0;
    uint64_t line_def=0, region_def=0, block_def=0;   // FILL payload components (raw)
    uint64_t missing=0;                                // MISSING key request bytes
    uint64_t fill_raw=0, fill_z3=0;                    // FILL closure payload (raw & zstd-L3)
    uint64_t framing=0;
    uint64_t total_raw=0, total_z=0;                   // total wire (raw defs / zstd defs)
    uint32_t objs_sent=0;
    void operator+=(const Acc&a){ root_raw+=a.root_raw; root_z1+=a.root_z1; root_z3+=a.root_z3; root_z6+=a.root_z6; line_def+=a.line_def; region_def+=a.region_def; block_def+=a.block_def; missing+=a.missing; fill_raw+=a.fill_raw; fill_z3+=a.fill_z3; framing+=a.framing; total_raw+=a.total_raw; total_z+=a.total_z; objs_sent+=a.objs_sent; }
};

static constexpr uint64_t FRAME_BYTES=16;   // modeled MsgPack map + MsgChannel length-prefix per frame

// charge one TU (already encoded) to one F; updates F known-sets; fills Acc.
static void charge_TU(const Interner& dict,const BlockStore& bs,const TURec& tu,FStore& F,Acc& a,
                      ZSTD_CCtx* cc,std::vector<uint8_t>& tmp,std::vector<uint8_t>& fill_blob,std::vector<uint8_t>& root_blob){
    F.ensure(dict.distinct()+1, dict.region_count(), bs.nblocks());
    fill_blob.clear();
    // regions (and their lines) in first-need order within the TU
    for(uint32_t r: tu.region_seq){
        if(F.regions[r]) continue; F.regions[r]=1;
        uint32_t cnt=dict.region_ids_count(r); const uint32_t* ids=dict.region_ids_ptr(r);
        // lines first (children before parent)
        for(uint32_t j=0;j<cnt;++j){ uint32_t l=ids[j]; if(!F.lines[l]){ F.lines[l]=1; const LineRef& lr=dict.ref(l); a.line_def+=lr.len; fill_blob.insert(fill_blob.end(),dict.line_data(lr.off),dict.line_data(lr.off)+lr.len); a.objs_sent++; } }
        // region composition def: varint(count)+child line-object ids
        uint64_t before=fill_blob.size(); put_varint(fill_blob,cnt); for(uint32_t j=0;j<cnt;++j) put_varint(fill_blob,ids[j]);
        a.region_def += fill_blob.size()-before; a.missing += varint_len(region_code(r)); a.objs_sent++;
    }
    // blocks (closure asc == topological: children have lower bidx)
    for(uint32_t b: tu.blk_clos){ if(F.blocks[b]) continue; F.blocks[b]=1; const auto& ch=bs.child[b];
        uint64_t before=fill_blob.size(); put_varint(fill_blob,ch.first); put_varint(fill_blob,ch.second); a.block_def += fill_blob.size()-before; a.missing += varint_len(block_code(b)); a.objs_sent++; }
    // root stream
    root_blob.clear(); for(uint32_t c: tu.root_codes) put_varint(root_blob,c);
    a.root_raw = root_blob.size();
    a.root_z1 = zstd_ctx(cc,root_blob.data(),root_blob.size(),1,tmp);
    a.root_z3 = zstd_ctx(cc,root_blob.data(),root_blob.size(),3,tmp);
    a.root_z6 = zstd_ctx(cc,root_blob.data(),root_blob.size(),6,tmp);
    a.fill_raw = fill_blob.size();
    a.fill_z3 = fill_blob.empty()?0:zstd_ctx(cc,fill_blob.data(),fill_blob.size(),3,tmp);
    a.framing = FRAME_BYTES*(1 + (fill_blob.empty()?0:1));
    a.total_raw = a.root_z3 + a.fill_raw + a.framing + a.missing;
    a.total_z   = a.root_z3 + a.fill_z3  + a.framing + a.missing;
}

// charge a list of pre-built TURecs through ONE fresh cold F; return aggregate Acc + per-TU wire + cum.
static Acc charge_recs(const Interner& dict,const BlockStore& bs,const std::vector<TURec>& recs,
                       std::vector<uint64_t>* per_tu_wire=nullptr,std::vector<uint64_t>* cum_wire=nullptr){
    FStore F; ZSTD_CCtx* cc=ZSTD_createCCtx(); std::vector<uint8_t> tmp,fill_blob,root_blob; Acc tot; uint64_t cum=0;
    for(const auto& r:recs){ Acc a; charge_TU(dict,bs,r,F,a,cc,tmp,fill_blob,root_blob); tot+=a; cum+=a.total_z; if(per_tu_wire) per_tu_wire->push_back(a.total_z); if(cum_wire) cum_wire->push_back(cum); }
    ZSTD_freeCCtx(cc); return tot;
}

// ------------------------------- online learner -------------------------------
struct OnlineLearner {
    BlockStore bs; U64Map pair_count{1<<20};
    uint64_t promotions=0;
    void tokenize(const std::vector<uint32_t>& regions,std::vector<uint32_t>& out){ bs.tokenize(regions,out); }
    size_t pairs() const { return pair_count.size(); }
    void learn_tu(const std::vector<uint32_t>&,const std::vector<uint32_t>& root,uint32_t tu,int mode,uint32_t K,uint32_t expF){ learn(root,tu,mode,K,expF); }
    // promote pairs of the given (already-encoded) token stream; blocks available next TU.
    // mode 0: count threshold K ; mode 1: full-cost gate.
    void learn(const std::vector<uint32_t>& toks,uint32_t tu,int mode,uint32_t K,uint32_t exp_F){
        size_t N=toks.size();
        for(size_t i=0;i+1<N;++i){ uint32_t a=toks[i],b=toks[i+1]; uint64_t key=(uint64_t(a)<<32)|b; uint64_t& cnt=pair_count.add_inc(key); ++cnt;
            if(bs.lookup(a,b)!=UINT32_MAX) continue;   // already promoted
            bool go=false;
            if(mode==0){ go = (cnt>=K); }
            else { // full-cost: saved_so_far vs def cost * expected F
                uint32_t saved_per = varint_len(a)+varint_len(b) - varint_len(block_code(bs.nblocks()));
                if(saved_per<1) saved_per=1;
                uint64_t def_cost = (uint64_t)(varint_len(a)+varint_len(b)) * exp_F + FRAME_BYTES;
                go = ((cnt-1)*saved_per) >= def_cost && cnt>=2;
            }
            if(go){ bs.add(a,b,tu); ++promotions; }
        }
    }
};

// ------------------------------- phrase-trie / LZ learner (shares BlockStore) -------------------------------
// Longest-known-phrase learner: a trie over region sequences. A depth-d node that crosses the
// benefit threshold is materialized as a LEFT-LEANING immutable Block = (depth-(d-1) phrase block,
// one more region), so phrase blocks nest and share prefixes in the SAME BlockStore. Encoding is
// longest-match greedy over the trie (variable-length), vs pair-promotion's binary re-tiling.
struct PhraseLearner {
    BlockStore bs;
    struct Node { uint32_t parent; uint32_t depth; uint32_t region; uint64_t count; uint32_t block; };
    std::vector<Node> nodes;          // node 0 = root
    U64Map edge{1<<20};               // (parent_node<<32 | region_id) -> node_id+1
    uint32_t maxphrase=32;
    PhraseLearner(){ nodes.push_back({0,0,0,0,UINT32_MAX}); }
    size_t pairs() const { return nodes.size(); }
    inline uint32_t child(uint32_t parent,uint32_t region) const { uint64_t* p=const_cast<U64Map&>(edge).find((uint64_t(parent)<<32)|region); return p? uint32_t(*p)-1 : UINT32_MAX; }
    uint32_t child_create(uint32_t parent,uint32_t region){ uint64_t key=(uint64_t(parent)<<32)|region; uint64_t* p=edge.find(key); if(p) return uint32_t(*p)-1; uint32_t id=uint32_t(nodes.size()); nodes.push_back({parent,nodes[parent].depth+1,region,0,UINT32_MAX}); edge.set(key,id+1); return id; }
    // ensure the block for a node exists (recursively builds the prefix chain); returns child code.
    uint32_t materialize(uint32_t node,uint32_t tu){
        if(node==0) return UINT32_MAX;
        if(nodes[node].depth==1) return region_code(nodes[node].region);   // single region, not a block
        if(nodes[node].block!=UINT32_MAX) return nodes[node].block;
        uint32_t left=materialize(nodes[node].parent,tu);                  // depth d-1 phrase
        uint32_t right=region_code(nodes[node].region);
        uint32_t bidx=bs.add(left,right,tu); nodes[node].block=block_code(bidx); return nodes[node].block;
    }
    void tokenize(const std::vector<uint32_t>& regions,std::vector<uint32_t>& out){
        out.clear(); size_t N=regions.size(); size_t i=0;
        while(i<N){ uint32_t cur=0,best_block=UINT32_MAX,best_len=0; size_t j=i;
            while(j<N){ uint32_t nx=child(cur,regions[j]); if(nx==UINT32_MAX) break; cur=nx; if(nodes[cur].block!=UINT32_MAX){ best_block=nodes[cur].block; best_len=nodes[cur].depth; } ++j; }
            if(best_len>=2){ out.push_back(best_block); i+=best_len; } else { out.push_back(region_code(regions[i])); ++i; } }
    }
    // learn over the RAW region sequence (phrases are region runs); promote nodes crossing K.
    void learn_regions(const std::vector<uint32_t>& regions,uint32_t tu,uint32_t K){
        size_t N=regions.size();
        for(size_t i=0;i<N;++i){ uint32_t cur=0; uint32_t lim=uint32_t(std::min<size_t>(N,i+maxphrase));
            for(size_t j=i;j<lim;++j){ cur=child_create(cur,regions[j]); uint64_t c=++nodes[cur].count;
                if(c>=K && nodes[cur].block==UINT32_MAX && nodes[cur].depth>=2) materialize(cur,tu); } }
    }
    void learn_tu(const std::vector<uint32_t>& regions,const std::vector<uint32_t>&,uint32_t tu,int,uint32_t K,uint32_t){ learn_regions(regions,tu,K); }
};

// ------------------------------- batch ceiling (offline region-BPE) -------------------------------
// Reproduces superblock-bench's S_bpe_region: rounds of pair promotion over the SENT-joined
// region stream, re-tiling in one pass per round. Loads accepted rules into a BlockStore and
// returns the per-TU tiled token streams (the offline optimum, for regret).
static void build_batch_ceiling(const std::vector<uint32_t>& region_ids,const std::vector<size_t>& region_off,
                                uint32_t min_count,int max_rounds,BlockStore& out_bs,
                                std::vector<std::vector<uint32_t>>& per_tu_tokens,double& secs,uint64_t& rules){
    auto t0=Clock::now();
    size_t TUs=region_off.size()-1;
    std::vector<uint32_t> tok; tok.reserve(region_ids.size()+TUs);
    for(size_t t=0;t<TUs;++t){ for(size_t i=region_off[t];i<region_off[t+1];++i) tok.push_back(region_code(region_ids[i])); tok.push_back(SENT); }
    for(int round=0;round<max_rounds;++round){
        U64Map pc(1<<20);
        for(size_t i=0;i+1<tok.size();++i){ uint32_t a=tok[i],b=tok[i+1]; if(a==SENT||b==SENT) continue; ++pc.add_inc((uint64_t(a)<<32)|b); }
        std::vector<std::pair<uint64_t,uint64_t>> cands;
        for(size_t i=0;i<pc.k.size();++i) if(pc.k[i]!=U64Map::EMPTY && pc.v[i]>=min_count) cands.push_back({pc.v[i],pc.k[i]});
        if(cands.empty()) break;
        std::sort(cands.begin(),cands.end(),std::greater<>());
        U64Map accept(1<<16);
        for(auto& c:cands){ uint32_t a=uint32_t(c.second>>32),b=uint32_t(c.second); uint32_t bidx=out_bs.add(a,b,0); accept.set(c.second, block_code(bidx)+1ull); }
        std::vector<uint32_t> nt; nt.reserve(tok.size());
        for(size_t i=0;i<tok.size();){ uint32_t a=tok[i]; if(a!=SENT&&i+1<tok.size()){ uint32_t b=tok[i+1]; if(b!=SENT){ uint64_t* s=accept.find((uint64_t(a)<<32)|b); if(s){ nt.push_back(uint32_t(*s-1)); i+=2; continue; } } } nt.push_back(a); ++i; }
        tok.swap(nt);
    }
    rules=out_bs.nblocks(); secs=seconds_since(t0);
    // split per TU on SENT
    per_tu_tokens.assign(TUs,{}); size_t t=0; std::vector<uint32_t> cur;
    for(uint32_t x:tok){ if(x==SENT){ per_tu_tokens[t++]=cur; cur.clear(); } else cur.push_back(x); }
}

// ------------------------------- byte-exact verify: expand root -> regions == region_seq -------------------------------
static bool verify_tu(const BlockStore& bs,const TURec& tu){
    static thread_local std::vector<uint32_t> exp; exp.clear();
    for(uint32_t c:tu.root_codes) bs.expand(c,exp);
    return exp.size()==tu.region_seq.size() && memcmp(exp.data(),tu.region_seq.data(),exp.size()*sizeof(uint32_t))==0;
}

// ------------------------------- online pass -------------------------------
// Processes a corpus in TU order through the persistent interner + learner, single persistent F.
// For each TU: (1) interner emits region seq; (2) tokenize with CURRENT blocks (pre-t state);
// (3) charge full per-F wire to the single persistent F; (4) learn from the TU; (5) verify byte-exact.
struct PassStats {
    uint64_t root_tok=0, root_raw=0, root_z3=0;
    uint64_t line_def=0, region_def=0, block_def=0, missing=0, fill_z3=0, framing=0, total_z=0, total_raw=0;
    uint64_t new_lines=0,new_regions=0,new_blocks=0;
    uint64_t ceiling_tok=0;  // batch-optimum tokens over same region seqs
    double encode_s=0, learn_s=0;
};

struct OnlinePassOut {
    PassStats st;
    std::vector<TURec> recs;                 // per-TU (kept for multi-F replay / recovery tables)
    std::vector<uint64_t> cum_total_z;       // cumulative online wire (single F) by TU ordinal
    std::vector<uint32_t> per_tu_root_tok;   // online root tokens by TU
    std::vector<uint64_t> per_tu_total_z;    // online wire by TU
    std::vector<uint32_t> per_tu_new_regions;
    std::vector<uint32_t> per_tu_new_blocks;
    std::vector<uint32_t> per_tu_deepest;
    std::vector<uint32_t> per_tu_src;         // uncompressed TU source bytes (x-axis)
    std::vector<uint32_t> per_tu_pairs;       // predictor pair/trie-node count after TU
    std::vector<uint32_t> per_tu_blocks;      // block count after TU
};

template<class Learner>
static void run_online_pass(Interner& dict,Learner& L,FStore& F,const Corpus& corpus,
                            bool learn_on,int promote_mode,uint32_t K,uint32_t exp_F,
                            OnlinePassOut& out,FILE* trace){
    uint32_t max_len=0; for(const auto&f:corpus.files) max_len=std::max(max_len,f.len);
    std::vector<uint32_t> lineout(size_t(max_len)+1);
    ZSTD_CCtx* cc=ZSTD_createCCtx(); std::vector<uint8_t> tmp,fill_blob,root_blob; std::vector<uint8_t> clos_seen;
    uint64_t hits=0, cum=0;
    out.recs.reserve(corpus.files.size());
    for(size_t t=0;t<corpus.files.size();++t){
        uint32_t lines0=dict.distinct(); uint64_t regions0=dict.region_count();
        TURec rec; rec.ord=uint32_t(t);
        // (1) interner emits region seq (perfect knowledge of TU_t)
        size_t oc=0; std::vector<uint32_t> region_scratch;
        const char*p=corpus.bytes.data()+corpus.files[t].off;
        auto te=Clock::now();
        dict.process(p,p+corpus.files[t].len,lineout.data(),oc,hits,true,&region_scratch);
        rec.region_seq.swap(region_scratch);
        rec.new_lines = dict.distinct()-lines0;
        rec.new_regions = uint32_t(dict.region_count()-regions0);
        // (2) tokenize with CURRENT (pre-t) blocks
        L.tokenize(rec.region_seq,rec.root_codes);
        rec.root_tok=uint32_t(rec.root_codes.size());
        // deepest block leaf used
        for(uint32_t c:rec.root_codes) if(is_block(c)) rec.deepest_leaf=std::max(rec.deepest_leaf,L.bs.leaf_len[code_index(c)]);
        // block closure + reference stats
        block_closure(L.bs,rec.root_codes,rec.blk_clos,clos_seen);
        for(uint32_t b:rec.blk_clos){ L.bs.ref_count[b]++; if(L.bs.use_tu_first[b]==UINT32_MAX) L.bs.use_tu_first[b]=uint32_t(t); L.bs.use_count[b]++; }
        out.st.encode_s += seconds_since(te);
        // (3) verify byte-exact
        if(!verify_tu(L.bs,rec)){ fprintf(stderr,"FATAL: TU %zu not byte-exact\n",t); exit(1); }
        // (4) charge to single persistent F
        uint32_t nblk_before=L.bs.nblocks();
        Acc a; charge_TU(dict,L.bs,rec,F,a,cc,tmp,fill_blob,root_blob);
        cum += a.total_z;
        // aggregate
        out.st.root_tok+=rec.root_tok; out.st.root_raw+=a.root_raw; out.st.root_z3+=a.root_z3;
        out.st.line_def+=a.line_def; out.st.region_def+=a.region_def; out.st.block_def+=a.block_def;
        out.st.missing+=a.missing; out.st.fill_z3+=a.fill_z3; out.st.framing+=a.framing; out.st.total_z+=a.total_z; out.st.total_raw+=a.total_raw;
        out.st.new_lines+=rec.new_lines; out.st.new_regions+=rec.new_regions;
        // (5) learn AFTER scoring
        auto tl=Clock::now();
        if(learn_on) L.learn_tu(rec.region_seq,rec.root_codes,uint32_t(t),promote_mode,K,exp_F);
        out.st.learn_s += seconds_since(tl);
        rec.new_blocks = L.bs.nblocks()-nblk_before; out.st.new_blocks+=rec.new_blocks;
        // record curves
        out.cum_total_z.push_back(cum);
        out.per_tu_root_tok.push_back(rec.root_tok);
        out.per_tu_total_z.push_back(a.total_z);
        out.per_tu_new_regions.push_back(rec.new_regions);
        out.per_tu_new_blocks.push_back(rec.new_blocks);
        out.per_tu_deepest.push_back(rec.deepest_leaf);
        out.per_tu_src.push_back(corpus.files[t].len);
        out.per_tu_pairs.push_back(uint32_t(L.pairs()));
        out.per_tu_blocks.push_back(L.bs.nblocks());
        if(trace){
            fprintf(trace,"%zu\t%u\t%u\t%u\t%u\t%u\t%u\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%u\t%u\t%u\n",
                t, uint32_t(rec.region_seq.size()), rec.root_tok, rec.new_lines, rec.new_regions, rec.new_blocks, rec.deepest_leaf,
                (unsigned long long)a.root_z3,(unsigned long long)a.line_def,(unsigned long long)a.region_def,(unsigned long long)a.block_def,
                (unsigned long long)a.missing,(unsigned long long)a.fill_z3,(unsigned long long)a.total_z,(unsigned long long)cum,
                L.bs.nblocks(), uint32_t(L.pairs()), corpus.files[t].len);
        }
        out.recs.push_back(std::move(rec));
    }
    ZSTD_freeCCtx(cc);
}

// frozen apply: tokenize each TU with a FROZEN block store (no learning), single F, aggregate wire.
static void run_frozen_pass(Interner& dict,const BlockStore& bs,FStore& F,const Corpus& corpus,PassStats& st){
    uint32_t max_len=0; for(const auto&f:corpus.files) max_len=std::max(max_len,f.len);
    std::vector<uint32_t> lineout(size_t(max_len)+1);
    ZSTD_CCtx* cc=ZSTD_createCCtx(); std::vector<uint8_t> tmp,fill_blob,root_blob,clos_seen; uint64_t hits=0;
    for(size_t t=0;t<corpus.files.size();++t){
        TURec rec; rec.ord=uint32_t(t); size_t oc=0; std::vector<uint32_t> rs;
        const char*p=corpus.bytes.data()+corpus.files[t].off; dict.process(p,p+corpus.files[t].len,lineout.data(),oc,hits,true,&rs); rec.region_seq.swap(rs);
        bs.tokenize(rec.region_seq,rec.root_codes); rec.root_tok=uint32_t(rec.root_codes.size());
        block_closure(bs,rec.root_codes,rec.blk_clos,clos_seen);
        if(!verify_tu(bs,rec)){ fprintf(stderr,"FATAL frozen TU %zu not exact\n",t); exit(1);}
        Acc a; charge_TU(dict,bs,rec,F,a,cc,tmp,fill_blob,root_blob);
        st.root_tok+=rec.root_tok; st.root_z3+=a.root_z3; st.line_def+=a.line_def; st.region_def+=a.region_def; st.block_def+=a.block_def; st.missing+=a.missing; st.fill_z3+=a.fill_z3; st.framing+=a.framing; st.total_z+=a.total_z;
    }
    ZSTD_freeCCtx(cc);
}

// ------------------------------- multi-F replay -------------------------------
// Replays per-TU records under F_count independent caches, given an assignment strategy.
// strat: 0=round-robin, 1=deterministic-shuffle, 2=sticky(contiguous slices).
struct MultiFOut { Acc total; uint64_t max_F_mem=0; };
static MultiFOut replay_multiF(const Interner& dict,const BlockStore& bs,const std::vector<TURec>& recs,uint32_t F_count,int strat){
    std::vector<FStore> Fs(F_count);
    std::vector<uint32_t> assign(recs.size());
    if(strat==0){ for(size_t t=0;t<recs.size();++t) assign[t]=uint32_t(t%F_count); }
    else if(strat==1){ // deterministic shuffle via mix64
        for(size_t t=0;t<recs.size();++t) assign[t]=uint32_t(mix64(t*0x9e3779b97f4a7c15ULL+1)%F_count); }
    else { size_t per=(recs.size()+F_count-1)/F_count; for(size_t t=0;t<recs.size();++t) assign[t]=uint32_t(std::min<size_t>(t/per,F_count-1)); }
    ZSTD_CCtx* cc=ZSTD_createCCtx(); std::vector<uint8_t> tmp,fill_blob,root_blob; MultiFOut mo;
    for(size_t t=0;t<recs.size();++t){ Acc a; charge_TU(dict,bs,recs[t],Fs[assign[t]],a,cc,tmp,fill_blob,root_blob); mo.total+=a; }
    ZSTD_freeCCtx(cc);
    for(auto& F:Fs) mo.max_F_mem=std::max(mo.max_F_mem,(uint64_t)(F.lines.size()+F.regions.size()+F.blocks.size()));
    return mo;
}

// ------------------------------- percentile helper -------------------------------
static uint64_t pct(std::vector<uint64_t> v,double q){ if(v.empty()) return 0; std::sort(v.begin(),v.end()); size_t i=size_t(q*(v.size()-1)+0.5); return v[i]; }

// ------------------------------- reporting helpers -------------------------------
// Convergence: online cumulative WIRE vs batch-ceiling cumulative WIRE (regret), plus token curves.
static void print_convergence(const char* corpname,const char* tracedir,const char* tag,
                              const OnlinePassOut& A,const std::vector<uint32_t>& ceil_tok,
                              const std::vector<uint64_t>& ceil_cum_wire,const std::vector<uint64_t>& ceil_tu_wire){
    size_t T=A.per_tu_root_tok.size();
    printf("\n== CONVERGENCE (pass A COLD_FIRST) %s ==\n",corpname);
    printf("online cumulative WIRE vs batch-region-BPE-ceiling cumulative WIRE (regret), sampled by TU ordinal\n");
    printf("%8s %14s %14s %11s | %12s %12s %11s\n","TU","cum_onl_wire","cum_ceil_wire","wire_reg%","cum_onl_tok","cum_ceil_tok","tok_reg%");
    // precompute cumulative token arrays
    std::vector<uint64_t> con_tok(T),ccl_tok(T); uint64_t ct=0,cc=0;
    for(size_t t=0;t<T;++t){ ct+=A.per_tu_root_tok[t]; cc+=(t<ceil_tok.size()?ceil_tok[t]:0); con_tok[t]=ct; ccl_tok[t]=cc; }
    // TSV
    char tp[4096]; FILE* f=nullptr; if(tracedir){ snprintf(tp,sizeof tp,"%s/convergence-%s.tsv",tracedir,tag); f=fopen(tp,"w"); if(f) fprintf(f,"tu\tcum_src\tsrcbytes\tonl_tu_wire\tceil_tu_wire\tcum_onl_wire\tcum_ceil_wire\tonl_tu_tok\tceil_tu_tok\tcum_onl_tok\tcum_ceil_tok\tblocks\tpairs\n"); }
    uint64_t cum_src=0;
    if(f) for(size_t t=0;t<T;++t){ cum_src+=(t<A.per_tu_src.size()?A.per_tu_src[t]:0);
        fprintf(f,"%zu\t%llu\t%u\t%llu\t%llu\t%llu\t%llu\t%u\t%u\t%llu\t%llu\t%u\t%u\n",t+1,
        (unsigned long long)cum_src,(t<A.per_tu_src.size()?A.per_tu_src[t]:0),
        (unsigned long long)A.per_tu_total_z[t],(unsigned long long)(t<ceil_tu_wire.size()?ceil_tu_wire[t]:0),
        (unsigned long long)A.cum_total_z[t],(unsigned long long)(t<ceil_cum_wire.size()?ceil_cum_wire[t]:0),
        A.per_tu_root_tok[t],(t<ceil_tok.size()?ceil_tok[t]:0),(unsigned long long)con_tok[t],(unsigned long long)ccl_tok[t],
        (t<A.per_tu_blocks.size()?A.per_tu_blocks[t]:0),(t<A.per_tu_pairs.size()?A.per_tu_pairs[t]:0)); }
    if(f) fclose(f);
    // console: ~16 evenly spaced unique ordinals incl last
    size_t last=SIZE_MAX;
    for(int s=1;s<=16;++s){ size_t t=(T==0)?0: size_t((double(s)/16.0)*T+0.5); if(t==0) t=1; if(t>T) t=T; if(t-1==last) continue; last=t-1;
        uint64_t ow=A.cum_total_z[t-1], cw=(t-1<ceil_cum_wire.size()?ceil_cum_wire[t-1]:0);
        double wr=cw?100.0*(double(ow)-double(cw))/double(cw):0.0;
        double tr=ccl_tok[t-1]?100.0*(double(con_tok[t-1])-double(ccl_tok[t-1]))/double(ccl_tok[t-1]):0.0;
        printf("%8zu %14llu %14llu %10.1f%% | %12llu %12llu %10.1f%%\n",t,(unsigned long long)ow,(unsigned long long)cw,wr,(unsigned long long)con_tok[t-1],(unsigned long long)ccl_tok[t-1],tr); }
    uint64_t OW=A.cum_total_z.back(), CW=ceil_cum_wire.empty()?0:ceil_cum_wire.back();
    double wr=CW?100.0*(double(OW)-double(CW))/double(CW):0.0;
    printf("FINAL online wire=%.3f MiB  ceiling wire=%.3f MiB  WIRE REGRET=%.1f%%   (tokens: online=%llu ceiling=%llu)\n",
        OW/1048576.0,CW/1048576.0,wr,(unsigned long long)con_tok.back(),(unsigned long long)ccl_tok.back());
}

// Recovery report: per affected-TU ordinal, absolute edit-pass wire/tokens + extra over warm baseline.
static void report_recovery(const char* tag,const char* form,const char* tracedir,
                            const OnlinePassOut& C,const OnlinePassOut& baseline,const std::vector<uint8_t>* affmask,
                            FILE* summ,const char* skey){
    printf("\n== RECOVERY (edit %s) %s ==\n",form,tag);
    std::vector<size_t> aff; std::vector<uint64_t> extra; std::vector<uint64_t> abswire; std::vector<uint32_t> abstok;
    size_t T=C.per_tu_total_z.size();
    for(size_t t=0;t<T;++t){ bool a = affmask? (t<affmask->size() && (*affmask)[t]) : (C.per_tu_new_regions[t]>0||C.per_tu_new_blocks[t]>0);
        uint64_t base=t<baseline.per_tu_total_z.size()?baseline.per_tu_total_z[t]:0; int64_t d=int64_t(C.per_tu_total_z[t])-int64_t(base);
        if(a || d>0){ aff.push_back(t); extra.push_back(uint64_t(std::max<int64_t>(0,d))); abswire.push_back(C.per_tu_total_z[t]); abstok.push_back(C.per_tu_root_tok[t]); } }
    char tp[4096]; FILE* f=nullptr; if(tracedir){ snprintf(tp,sizeof tp,"%s/recovery-%s-%s.tsv",tracedir,tag,form); f=fopen(tp,"w"); if(f) fprintf(f,"aff_ord\ttu\tC_wire\tB_wire\textra\tC_tok\tB_tok\tnew_regions\tnew_blocks\tdeepest\n"); }
    if(f){ for(size_t k=0;k<aff.size();++k){ size_t t=aff[k]; fprintf(f,"%zu\t%zu\t%llu\t%llu\t%llu\t%u\t%u\t%u\t%u\t%u\n",k+1,t+1,(unsigned long long)C.per_tu_total_z[t],
        (unsigned long long)(t<baseline.per_tu_total_z.size()?baseline.per_tu_total_z[t]:0),(unsigned long long)extra[k],C.per_tu_root_tok[t],
        t<baseline.per_tu_root_tok.size()?baseline.per_tu_root_tok[t]:0,C.per_tu_new_regions[t],C.per_tu_new_blocks[t],C.per_tu_deepest[t]); } fclose(f); }
    printf("affected TUs=%zu (of %zu). first-10 by affected ordinal (edit-pass wire vs warm baseline):\n",aff.size(),T);
    printf("%6s %8s %12s %12s %10s %8s\n","aff#","TU","C_wire","B_wire","extra","C_tok");
    for(size_t k=0;k<aff.size()&&k<10;++k){ size_t t=aff[k]; printf("%6zu %8zu %12llu %12llu %10llu %8u\n",k+1,t+1,(unsigned long long)C.per_tu_total_z[t],(unsigned long long)(t<baseline.per_tu_total_z.size()?baseline.per_tu_total_z[t]:0),(unsigned long long)extra[k],C.per_tu_root_tok[t]); }
    uint64_t tot=0; for(uint64_t e:extra) tot+=e;
    printf("first-affected extra=%llu  p50=%llu  p95=%llu  max=%llu  CUM edit penalty=%llu (%.1f KiB) over %zu TUs\n",
        extra.empty()?0ull:(unsigned long long)extra[0],(unsigned long long)pct(extra,0.5),(unsigned long long)pct(extra,0.95),(unsigned long long)pct(extra,1.0),(unsigned long long)tot,tot/1024.0,aff.size());
    if(summ){ char k[96];
        snprintf(k,sizeof k,"%s_first_extra",skey); fprintf(summ,"%s\t%llu\n",k,extra.empty()?0ull:(unsigned long long)extra[0]);
        snprintf(k,sizeof k,"%s_p50_extra",skey);   fprintf(summ,"%s\t%llu\n",k,(unsigned long long)pct(extra,0.5));
        snprintf(k,sizeof k,"%s_p95_extra",skey);   fprintf(summ,"%s\t%llu\n",k,(unsigned long long)pct(extra,0.95));
        snprintf(k,sizeof k,"%s_cum_extra",skey);   fprintf(summ,"%s\t%llu\n",k,(unsigned long long)tot);
        snprintf(k,sizeof k,"%s_affected",skey);    fprintf(summ,"%s\t%zu\n",k,aff.size()); }
}

int main(int argc,char**argv){
    const char* manifest=nullptr; size_t max_files=SIZE_MAX; uint32_t K=8, batch_min=16; int promote_mode=0; uint32_t exp_F=1; const char* tag="corpus"; bool do_multiF=true; const char* tracedir=nullptr;
    std::vector<uint32_t> sweepK;
    for(int i=1;i<argc;++i){
        if(!strcmp(argv[i],"--manifest")&&i+1<argc) manifest=argv[++i];
        else if(!strcmp(argv[i],"--max-files")&&i+1<argc) max_files=strtoull(argv[++i],nullptr,10);
        else if(!strcmp(argv[i],"--K")&&i+1<argc) K=uint32_t(atoi(argv[++i]));
        else if(!strcmp(argv[i],"--sweepK")&&i+1<argc){ char*s=argv[++i]; for(char*t=strtok(s,",");t;t=strtok(nullptr,",")) sweepK.push_back(uint32_t(atoi(t))); }
        else if(!strcmp(argv[i],"--batch-min")&&i+1<argc) batch_min=uint32_t(atoi(argv[++i]));
        else if(!strcmp(argv[i],"--promote-mode")&&i+1<argc) promote_mode=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--exp-F")&&i+1<argc) exp_F=uint32_t(atoi(argv[++i]));
        else if(!strcmp(argv[i],"--tag")&&i+1<argc) tag=argv[++i];
        else if(!strcmp(argv[i],"--tracedir")&&i+1<argc) tracedir=argv[++i];
        else if(!strcmp(argv[i],"--no-multiF")) do_multiF=false;
        else { fprintf(stderr,"unknown %s\n",argv[i]); return 2; }
    }
    if(!manifest){ fprintf(stderr,"usage: %s --manifest F [--tag T] [--K N] [--sweepK a,b,c] [--batch-min N] [--promote-mode 0|1] [--exp-F N] [--tracedir D] [--no-multiF] [--max-files N]\n",argv[0]); return 2; }
    if(sweepK.empty()) sweepK={K};

    auto t0=Clock::now();
    Corpus corpus=load_corpus(manifest,max_files);
    fprintf(stderr,"[%s] loaded %zu TUs raw=%.1f MiB in %.1fs\n",tag,corpus.files.size(),corpus.raw/1048576.0,seconds_since(t0));
    uint64_t ref_hash=hash_bytes((const uint8_t*)corpus.bytes.data(),corpus.raw);

    // ---- reference region-id stream (batch ceiling needs full region seq); build with a scratch interner
    // We use ONE persistent interner for the online passes; but the batch ceiling must see the SAME region
    // ids. So: first do pass A online (which populates the interner + captures region seqs), then build the
    // ceiling from pass A's captured region seqs. This guarantees identical region identities.

    Interner dict; OnlineLearner L; FStore F;
    char tpath[4096];
    FILE* summ=nullptr; if(tracedir){ snprintf(tpath,sizeof tpath,"%s/summary-%s.tsv",tracedir,tag); summ=fopen(tpath,"w"); if(summ) fprintf(summ,"key\tvalue\n"); }
    auto S=[&](const char* k,double v){ if(summ) fprintf(summ,"%s\t%.6g\n",k,v); };
    auto Su=[&](const char* k,unsigned long long v){ if(summ) fprintf(summ,"%s\t%llu\n",k,v); };
    Su("TUs",corpus.files.size()); S("raw_MiB",corpus.raw/1048576.0);
    FILE* trA=nullptr;
    if(tracedir){ snprintf(tpath,sizeof tpath,"%s/trace-%s-A.tsv",tracedir,tag); trA=fopen(tpath,"w"); if(trA) fprintf(trA,"tu\tregions\troot_tok\tnew_lines\tnew_regions\tnew_blocks\tdeepest\troot_z3\tline_def\tregion_def\tblock_def\tmissing\tfill_z3\ttotal_z\tcum_z\tblocks\tpairs\tsrcbytes\n"); }

    // ===== PASS A: COLD_FIRST =====
    OnlinePassOut A; run_online_pass(dict,L,F,corpus,true,promote_mode,K,exp_F,A,trA);
    if(trA) fclose(trA);
    // byte-exact anchor over whole corpus (line-id reconstruction)
    fprintf(stderr,"[%s] pass A done: root_tok=%llu total_z=%.3f MiB blocks=%u encode=%.1fs learn=%.1fs\n",
        tag,(unsigned long long)A.st.root_tok,A.st.total_z/1048576.0,L.bs.nblocks(),A.st.encode_s,A.st.learn_s);

    // ---- build batch ceiling from pass A region seqs ----
    std::vector<uint32_t> region_ids; std::vector<size_t> region_off; region_off.push_back(0);
    for(auto& r:A.recs){ region_ids.insert(region_ids.end(),r.region_seq.begin(),r.region_seq.end()); region_off.push_back(region_ids.size()); }
    BlockStore ceil_bs; std::vector<std::vector<uint32_t>> ceil_tokens; double ceil_secs=0; uint64_t ceil_rules=0;
    build_batch_ceiling(region_ids,region_off,batch_min,20,ceil_bs,ceil_tokens,ceil_secs,ceil_rules);
    std::vector<uint32_t> ceil_tok(ceil_tokens.size()); uint64_t ceil_tok_total=0;
    for(size_t t=0;t<ceil_tokens.size();++t){ ceil_tok[t]=uint32_t(ceil_tokens[t].size()); ceil_tok_total+=ceil_tok[t]; }
    fprintf(stderr,"[%s] batch ceiling: %llu rules in %.2fs, ceiling tokens=%llu\n",tag,(unsigned long long)ceil_rules,ceil_secs,(unsigned long long)ceil_tok_total);

    // batch-ceiling WIRE: build ceiling TURecs (same region seqs, ceiling tokenization) and charge cold single-F
    std::vector<TURec> ceil_recs(ceil_tokens.size()); std::vector<uint8_t> cseen;
    for(size_t t=0;t<ceil_tokens.size();++t){ TURec& r=ceil_recs[t]; r.ord=uint32_t(t); r.region_seq=A.recs[t].region_seq; r.root_codes=ceil_tokens[t]; r.root_tok=uint32_t(r.root_codes.size()); block_closure(ceil_bs,r.root_codes,r.blk_clos,cseen);
        if(!verify_tu(ceil_bs,r)){ fprintf(stderr,"FATAL: batch ceiling TU %zu not byte-exact\n",t); return 1; } }
    std::vector<uint64_t> ceil_tu_wire, ceil_cum_wire;
    Acc ceil_acc=charge_recs(dict,ceil_bs,ceil_recs,&ceil_tu_wire,&ceil_cum_wire);
    fprintf(stderr,"[%s] batch ceiling wire=%.3f MiB (root_z3=%llu block_def=%llu region_def=%llu line_def=%llu)\n",tag,ceil_acc.total_z/1048576.0,
        (unsigned long long)ceil_acc.root_z3,(unsigned long long)ceil_acc.block_def,(unsigned long long)ceil_acc.region_def,(unsigned long long)ceil_acc.line_def);

    Su("ceil_rules",ceil_rules); Su("ceil_tokens",ceil_tok_total); Su("ceil_wire",ceil_acc.total_z);
    Su("A_root_tok",A.st.root_tok); Su("A_wire",A.st.total_z); Su("A_blocks",L.bs.nblocks());
    S("A_encode_s",A.st.encode_s); S("A_learn_s",A.st.learn_s); Su("A_pairs",L.pairs());
    Su("A_final_cum_wire",A.cum_total_z.empty()?0:A.cum_total_z.back()); Su("ceil_final_cum_wire",ceil_cum_wire.empty()?0:ceil_cum_wire.back());
    { double wr=(!ceil_cum_wire.empty()&&ceil_cum_wire.back())?100.0*(double(A.cum_total_z.back())-double(ceil_cum_wire.back()))/double(ceil_cum_wire.back()):0.0; S("A_wire_regret_pct",wr); }
    print_convergence(tag,tracedir,tag,A,ceil_tok,ceil_cum_wire,ceil_tu_wire);

    // ===== PASS B: WARM_SAME (continue exact state, recompile unchanged tree) =====
    // snapshot a frozen-after-A block store for the control row
    BlockStore frozenA = L.bs;   // copy (immutable snapshot); pair2block copied too
    OnlinePassOut B; FILE* trB=nullptr;
    if(tracedir){ snprintf(tpath,sizeof tpath,"%s/trace-%s-B.tsv",tracedir,tag); trB=fopen(tpath,"w"); if(trB) fprintf(trB,"tu\tregions\troot_tok\tnew_lines\tnew_regions\tnew_blocks\tdeepest\troot_z3\tline_def\tregion_def\tblock_def\tmissing\tfill_z3\ttotal_z\tcum_z\tblocks\tpairs\tsrcbytes\n"); }
    run_online_pass(dict,L,F,corpus,true,promote_mode,K,exp_F,B,trB); if(trB) fclose(trB);
    fprintf(stderr,"[%s] pass B (warm, learning) total_z=%.3f MiB new_blocks=%llu\n",tag,B.st.total_z/1048576.0,(unsigned long long)B.st.new_blocks);
    // frozen-after-A control on a fresh warm F (F already has A's objects: reuse F copy semantics -> use a COPY of F)
    { FStore Ffroz=F; PassStats fz; run_frozen_pass(dict,frozenA,Ffroz,corpus,fz);
      fprintf(stderr,"[%s] pass B frozen-after-A control total_z=%.3f MiB root_tok=%llu\n",tag,fz.total_z/1048576.0,(unsigned long long)fz.root_tok);
      printf("\n== PASS B WARM_SAME %s ==\n",tag);
      printf("online(learning):  total_z=%llu  root_tok=%llu  new_blocks=%llu\n",(unsigned long long)B.st.total_z,(unsigned long long)B.st.root_tok,(unsigned long long)B.st.new_blocks);
      printf("frozen-after-A ctl: total_z=%llu  root_tok=%llu (blocks frozen=%u)\n",(unsigned long long)fz.total_z,(unsigned long long)fz.root_tok,frozenA.nblocks());
      Su("B_online_wire",B.st.total_z); Su("B_online_root_tok",B.st.root_tok); Su("B_new_blocks",B.st.new_blocks);
      Su("B_frozenA_wire",fz.total_z); Su("B_frozenA_root_tok",fz.root_tok); Su("frozenA_blocks",frozenA.nblocks());
    }

    // ===== multi-F sweep on WARM pass (B records) =====
    if(do_multiF){
        printf("\n== PER-F CHARGING (pass-B records; each F starts COLD; total wire bytes) %s ==\n",tag);
        printf("Total wire under 3 assignment strategies (charge each artifact once per F that needs it):\n");
        printf("%3s | %15s %15s %15s\n","F","round-robin","shuffle","sticky-contig");
        FILE* pf=nullptr; if(tracedir){ snprintf(tpath,sizeof tpath,"%s/perF-%s.tsv",tracedir,tag); pf=fopen(tpath,"w"); if(pf) fprintf(pf,"F\tstrat\ttotal_z\troot_z3\tline_def\tregion_def\tblock_def\tfill_z3\tmissing\tframing\n"); }
        for(uint32_t fc: {1u,4u,8u,16u,32u}){
            MultiFOut rr=replay_multiF(dict,L.bs,B.recs,fc,0);
            MultiFOut sh=replay_multiF(dict,L.bs,B.recs,fc,1);
            MultiFOut st=replay_multiF(dict,L.bs,B.recs,fc,2);
            printf("%3u | %15llu %15llu %15llu\n",fc,(unsigned long long)rr.total.total_z,(unsigned long long)sh.total.total_z,(unsigned long long)st.total.total_z);
            char kb[64]; snprintf(kb,sizeof kb,"perF_rr_%u",fc); Su(kb,rr.total.total_z); snprintf(kb,sizeof kb,"perF_sticky_%u",fc); Su(kb,st.total.total_z);
            if(pf){ auto row=[&](const char* s,const MultiFOut& m){ fprintf(pf,"%u\t%s\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\n",fc,s,(unsigned long long)m.total.total_z,(unsigned long long)m.total.root_z3,(unsigned long long)m.total.line_def,(unsigned long long)m.total.region_def,(unsigned long long)m.total.block_def,(unsigned long long)m.total.fill_z3,(unsigned long long)m.total.missing,(unsigned long long)m.total.framing); }; row("rr",rr); row("shuffle",sh); row("sticky",st); }
        }
        if(pf) fclose(pf);
        printf("\nDEF-MULTIPLICATION breakdown (round-robin; root is F-independent, defs multiply with F):\n");
        printf("%3s | %13s %13s %13s %13s %13s\n","F","root_z3","line_def","region_def","block_def","fill_z3");
        for(uint32_t fc: {1u,4u,8u,16u,32u}){
            MultiFOut rr=replay_multiF(dict,L.bs,B.recs,fc,0);
            printf("%3u | %13llu %13llu %13llu %13llu %13llu\n",fc,(unsigned long long)rr.total.root_z3,(unsigned long long)rr.total.line_def,(unsigned long long)rr.total.region_def,(unsigned long long)rr.total.block_def,(unsigned long long)rr.total.fill_z3);
        }
        printf("(sticky keeps a C's jobs on one F -> less def multiplication; round-robin re-sends defs to every F.)\n");
    }

    // ===== PASS C: ROOT_HEADER_EDIT (controlled stdc-predef insert) =====
    uint64_t modified=0; Corpus pc=make_poisoned_corpus(corpus,modified);
    // verify poisoned corpus reconstructs to ITS OWN bytes through the interner (byte-exact on edit)
    uint64_t pc_ref=hash_bytes((const uint8_t*)pc.bytes.data(),pc.raw);
    OnlinePassOut C; FILE* trC=nullptr;
    if(tracedir){ snprintf(tpath,sizeof tpath,"%s/trace-%s-C.tsv",tracedir,tag); trC=fopen(tpath,"w"); if(trC) fprintf(trC,"tu\tregions\troot_tok\tnew_lines\tnew_regions\tnew_blocks\tdeepest\troot_z3\tline_def\tregion_def\tblock_def\tmissing\tfill_z3\ttotal_z\tcum_z\tblocks\tpairs\tsrcbytes\n"); }
    uint32_t blocks_before_C=L.bs.nblocks(); uint64_t regions_before_C=dict.region_count(); uint32_t lines_before_C=dict.distinct();
    run_online_pass(dict,L,F,pc,true,promote_mode,K,exp_F,C,trC); if(trC) fclose(trC);
    // reconstruct check on edited corpus
    { std::vector<uint32_t> exp; for(auto& r:C.recs){ for(uint32_t c:r.root_codes) L.bs.expand(c,exp); }
      // expand region-ids -> line-ids -> bytes hash
      uint64_t h=1469598103934665603ULL; for(uint32_t rid:exp){ const uint32_t* ids=dict.region_ids_ptr(rid); uint32_t cnt=dict.region_ids_count(rid); for(uint32_t j=0;j<cnt;++j){ const LineRef& lr=dict.ref(ids[j]); const char* d=dict.line_data(lr.off); for(uint32_t b=0;b<lr.len;++b){ h^=uint8_t(d[b]); h*=1099511628211ULL; } } }
      fprintf(stderr,"[%s] pass C edited byte-exact: %s (modified TUs=%llu new_lines=%u new_regions=%llu new_blocks=%u)\n",tag, h==pc_ref?"OK":"FAIL",(unsigned long long)modified, dict.distinct()-lines_before_C,(unsigned long long)(dict.region_count()-regions_before_C),L.bs.nblocks()-blocks_before_C);
      if(h!=pc_ref){ fprintf(stderr,"FATAL: edited corpus not byte-exact\n"); return 1; }
    }
    report_recovery(tag,"i-stdc-predef",tracedir,C,B,nullptr,summ,"recI");

    // ===== PASS D: CHANGED_STEADY (recompile edited tree again; then revert recovery check) =====
    OnlinePassOut D; run_online_pass(dict,L,F,pc,true,promote_mode,K,exp_F,D,nullptr);
    fprintf(stderr,"[%s] pass D changed-steady total_z=%.3f MiB new_blocks=%llu\n",tag,D.st.total_z/1048576.0,(unsigned long long)D.st.new_blocks);
    OnlinePassOut Drev; run_online_pass(dict,L,F,corpus,true,promote_mode,K,exp_F,Drev,nullptr);
    printf("\n== PASS D CHANGED_STEADY %s ==\n",tag);
    printf("re-run edited tree (steady):    total_z=%llu  new_blocks=%llu\n",(unsigned long long)D.st.total_z,(unsigned long long)D.st.new_blocks);
    printf("revert to original (recovery):  total_z=%llu  new_blocks=%llu (old blocks immediately reusable => low wire)\n",(unsigned long long)Drev.st.total_z,(unsigned long long)Drev.st.new_blocks);
    Su("D_changed_wire",D.st.total_z); Su("D_revert_wire",Drev.st.total_z);
    { Corpus dead; dead.bytes.swap(pc.bytes); dead.files.swap(pc.files); }  // free pass-C poisoned corpus (~3.6 GB) before edit(ii)

    // ---- edit form (ii): high-fanout project-header analog (change one common region's bytes) ----
    // pick the region appearing in the most distinct TUs from pass A; continues the (never-frozen) learner state.
    {
        std::vector<uint32_t> tu_of(dict.region_count(),UINT32_MAX); std::vector<uint32_t> fanout(dict.region_count(),0);
        for(size_t t=0;t<A.recs.size();++t){ for(uint32_t r:A.recs[t].region_seq){ if(tu_of[r]!=uint32_t(t)){ tu_of[r]=uint32_t(t); fanout[r]++; } } }
        uint32_t best=0; for(uint32_t r=1;r<fanout.size();++r) if(fanout[r]>fanout[best]) best=r;
        uint32_t nl=dict.region_raw_len(best);
        std::vector<char> needle(dict.region_raw_ptr(best),dict.region_raw_ptr(best)+nl);
        fprintf(stderr,"[%s] edit(ii): target region %u fanout=%u TUs raw_len=%u\n",tag,best,fanout[best],nl);
        uint64_t mod2=0; std::vector<uint8_t> aff2; Corpus rc=make_region_edit_corpus(corpus,needle.data(),nl,mod2,aff2);
        uint64_t rc_ref=hash_bytes((const uint8_t*)rc.bytes.data(),rc.raw);
        uint32_t blk_b=L.bs.nblocks(); uint64_t reg_b=dict.region_count();
        OnlinePassOut C2; run_online_pass(dict,L,F,rc,true,promote_mode,K,exp_F,C2,nullptr);
        { std::vector<uint32_t> exp; for(auto& r:C2.recs) for(uint32_t c:r.root_codes) L.bs.expand(c,exp);
          uint64_t h=1469598103934665603ULL; for(uint32_t rid:exp){ const uint32_t* ids=dict.region_ids_ptr(rid); uint32_t cnt=dict.region_ids_count(rid); for(uint32_t j=0;j<cnt;++j){ const LineRef& lr=dict.ref(ids[j]); const char* d=dict.line_data(lr.off); for(uint32_t b=0;b<lr.len;++b){ h^=uint8_t(d[b]); h*=1099511628211ULL; } } }
          fprintf(stderr,"[%s] edit(ii) byte-exact: %s (modified TUs=%llu new_regions=%llu new_blocks=%u)\n",tag,h==rc_ref?"OK":"FAIL",(unsigned long long)mod2,(unsigned long long)(dict.region_count()-reg_b),L.bs.nblocks()-blk_b);
          if(h!=rc_ref){ fprintf(stderr,"FATAL: edit(ii) not byte-exact\n"); return 1; } }
        report_recovery(tag,"ii-region-fanout",tracedir,C2,B,&aff2,summ,"recII");
        Su("editII_target_fanout",fanout[best]);
    }

    // ===== K sweep on a FRESH engine per K (cold pass A wire + block count) =====
    if(sweepK.size()>1 || sweepK[0]!=K){}
    printf("\n== PROMOTION THRESHOLD SWEEP (fresh cold pass A per K) %s ==\n",tag);
    printf("%6s %12s %12s %14s %14s %12s\n","K","blocks","root_tok","total_z(A)","ceil_tok","tok_regret%");
    FILE* kf=nullptr; if(tracedir){ snprintf(tpath,sizeof tpath,"%s/ksweep-%s.tsv",tracedir,tag); kf=fopen(tpath,"w"); if(kf) fprintf(kf,"K\tblocks\troot_tok\twire\tceil_tok\tceil_wire\ttok_regret_pct\tnever_pct\n"); }
    for(uint32_t k:sweepK){
        Interner d2; OnlineLearner L2; FStore F2; OnlinePassOut A2; run_online_pass(d2,L2,F2,corpus,true,promote_mode,k,exp_F,A2,nullptr);
        double reg = ceil_tok_total? 100.0*(double(A2.st.root_tok)-double(ceil_tok_total))/double(ceil_tok_total):0.0;
        uint32_t nb=L2.bs.nblocks(); uint64_t never=0; for(uint32_t b=0;b<nb;++b) if(L2.bs.ref_count[b]==0) ++never;
        double npct=nb?100.0*never/nb:0.0;
        printf("%6u %12u %12llu %14llu %14llu %11.1f%%\n",k,nb,(unsigned long long)A2.st.root_tok,(unsigned long long)A2.st.total_z,(unsigned long long)ceil_tok_total,reg);
        if(kf) fprintf(kf,"%u\t%u\t%llu\t%llu\t%llu\t%llu\t%.3f\t%.3f\n",k,nb,(unsigned long long)A2.st.root_tok,(unsigned long long)A2.cum_total_z.back(),(unsigned long long)ceil_tok_total,(unsigned long long)(ceil_cum_wire.empty()?0:ceil_cum_wire.back()),reg,npct);
    }
    if(kf) fclose(kf);

    // ===== algorithm comparison summary (warm, single persistent F already holding all objects) =====
    printf("\n== ALGORITHM COMPARISON (warm single-F recurring wire, corpus %s) ==\n",tag);
    {
        // algo1 lines-only: per-TU line-id stream (all lines warm => 0 defs); the structure floor.
        ZSTD_CCtx* cc=ZSTD_createCCtx(); std::vector<uint8_t> tmp,body; uint64_t a1_root=0,a1_z=0,a1_tok=0;
        for(auto& r:A.recs){ body.clear(); for(uint32_t rid:r.region_seq){ const uint32_t* ids=dict.region_ids_ptr(rid); uint32_t cnt=dict.region_ids_count(rid); for(uint32_t j=0;j<cnt;++j){ put_varint(body,ids[j]); ++a1_tok; } }
            a1_root+=body.size(); a1_z+=zstd_ctx(cc,body.data(),body.size(),3,tmp)+FRAME_BYTES; }
        ZSTD_freeCCtx(cc);
        // algo2 regions-only and algo3 regions+blocks: frozen apply on warm F copy
        BlockStore empty; FStore Fr=F; PassStats r2; run_frozen_pass(dict,empty,Fr,corpus,r2);
        FStore Fb=F; PassStats r3; run_frozen_pass(dict,L.bs,Fb,corpus,r3);
        printf("algo1 lines-only (floor)  : root_tok=%llu total_z=%llu (line-id stream, warm; no blocks)\n",(unsigned long long)a1_tok,(unsigned long long)a1_z);
        printf("algo2 marker-regions-only : root_tok=%llu total_z=%llu (root_z3=%llu block_def=%llu)\n",(unsigned long long)r2.root_tok,(unsigned long long)r2.total_z,(unsigned long long)r2.root_z3,(unsigned long long)r2.block_def);
        printf("algo3 regions+online-block: root_tok=%llu total_z=%llu (root_z3=%llu block_def=%llu)\n",(unsigned long long)r3.root_tok,(unsigned long long)r3.total_z,(unsigned long long)r3.root_z3,(unsigned long long)r3.block_def);
        double d21 = a1_z? 100.0*(double(a1_z)-double(r2.total_z))/double(a1_z):0.0;
        double d32 = r2.total_z? 100.0*(double(r2.total_z)-double(r3.total_z))/double(r2.total_z):0.0;
        printf("regions cut the lines floor by %.1f%%; online-blocks cut regions-only by %.1f%% (warm, single F).\n",d21,d32);
        Su("algo1_lines_wire",a1_z); Su("algo2_regions_wire",r2.total_z); Su("algo3_blocks_wire",r3.total_z);
        Su("algo2_root_tok",r2.root_tok); Su("algo3_root_tok",r3.root_tok); S("blocks_vs_regions_pct",d32);
    }

    // ===== block-lifetime quality (from the persistent online block store) =====
    {
        printf("\n== BLOCK-LIFETIME QUALITY (%s) ==\n",tag);
        uint32_t nb=L.bs.nblocks(); std::vector<uint64_t> uses; uint64_t never=0,c2u=0,c2u_n=0; std::vector<uint64_t> gap; std::vector<uint64_t> depths;
        uint64_t bytes_never=0;
        for(uint32_t b=0;b<nb;++b){ uint32_t uc=L.bs.use_count[b]; uses.push_back(uc); depths.push_back(L.bs.depth(block_code(b)));
            uint32_t defb=varint_len(L.bs.child[b].first)+varint_len(L.bs.child[b].second);
            if(L.bs.ref_count[b]==0){ ++never; bytes_never+=defb; }
            if(L.bs.use_tu_first[b]!=UINT32_MAX && L.bs.created_tu[b]!=UINT32_MAX){ gap.push_back(L.bs.use_tu_first[b]>=L.bs.created_tu[b]?L.bs.use_tu_first[b]-L.bs.created_tu[b]:0); }
        }
        size_t pair_mem=L.pairs()*16; size_t blk_mem=nb*(size_t)(sizeof(std::pair<uint32_t,uint32_t>)+5*4);
        printf("blocks=%u  never-referenced=%llu (%.1f%%, def bytes wasted=%llu)\n",nb,(unsigned long long)never, nb?100.0*never/nb:0.0,(unsigned long long)bytes_never);
        printf("use_count  p50=%llu p95=%llu max=%llu\n",(unsigned long long)pct(uses,0.5),(unsigned long long)pct(uses,0.95),(unsigned long long)pct(uses,1.0));
        printf("depth      p50=%llu p95=%llu max=%llu\n",(unsigned long long)pct(depths,0.5),(unsigned long long)pct(depths,0.95),(unsigned long long)pct(depths,1.0));
        printf("create->first-use gap (TUs) p50=%llu p95=%llu max=%llu\n",(unsigned long long)pct(gap,0.5),(unsigned long long)pct(gap,0.95),(unsigned long long)pct(gap,1.0));
        printf("predictor memory: pair_count~%.1f MiB + blocks~%.1f MiB = %.1f MiB for %llu learned TUs\n",
            pair_mem/1048576.0,blk_mem/1048576.0,(pair_mem+blk_mem)/1048576.0,(unsigned long long)(A.recs.size()*4));
        Su("life_blocks",nb); S("life_never_pct",nb?100.0*never/nb:0.0); Su("life_bytes_never",bytes_never);
        Su("life_use_p50",pct(uses,0.5)); Su("life_use_p95",pct(uses,0.95)); Su("life_use_max",pct(uses,1.0));
        Su("life_depth_p50",pct(depths,0.5)); Su("life_depth_p95",pct(depths,0.95)); Su("life_depth_max",pct(depths,1.0));
        Su("life_gap_p50",pct(gap,0.5)); Su("life_gap_p95",pct(gap,0.95));
        S("predictor_mem_MiB",(pair_mem+blk_mem)/1048576.0);
    }

    // ===== PHRASE-TRIE / LZ learner comparison (cold pass A on a fresh engine) =====
    {
        printf("\n== LEARNER COMPARISON: pair-promotion vs phrase-trie (fresh cold pass A) %s ==\n",tag);
        Interner dp; PhraseLearner P; FStore Fp; OnlinePassOut AP; run_online_pass(dp,P,Fp,corpus,true,promote_mode,K,exp_F,AP,nullptr);
        double regp = ceil_tok_total? 100.0*(double(AP.st.root_tok)-double(ceil_tok_total))/double(ceil_tok_total):0.0;
        double rega = ceil_tok_total? 100.0*(double(A.st.root_tok)-double(ceil_tok_total))/double(ceil_tok_total):0.0;
        printf("%-24s %12s %12s %14s %12s\n","learner","blocks","root_tok","total_z(A)","tok_reg%");
        printf("%-24s %12u %12llu %14llu %11.1f%%\n","pair-promotion(K)",L.bs.nblocks(),(unsigned long long)A.st.root_tok,(unsigned long long)A.st.total_z,rega);
        printf("%-24s %12u %12llu %14llu %11.1f%%\n","phrase-trie/LZ(K)",P.bs.nblocks(),(unsigned long long)AP.st.root_tok,(unsigned long long)AP.st.total_z,regp);
        printf("(both cold pass A, same K=%u; batch-ceiling tokens=%llu)\n",K,(unsigned long long)ceil_tok_total);
        Su("phrase_blocks",P.bs.nblocks()); Su("phrase_root_tok",AP.st.root_tok); Su("phrase_wire",AP.st.total_z);
        Su("pair_blocks_A",L.bs.nblocks()); Su("pair_wire_A",A.st.total_z);
    }

    // ===== ORDER-SENSITIVITY: fresh cold pass A under different TU orders (band plot) =====
    {
        printf("\n== ORDER SENSITIVITY (fresh cold pass A; final wire under different TU orders) %s ==\n",tag);
        struct OrdSpec{ const char* name; int ord; uint64_t seed; };
        std::vector<OrdSpec> orders={ {"original",0,0}, {"reverse",1,0}, {"shuffle-s1",2,0x1234567ull}, {"shuffle-s2",2,0x9abcdefull}, {"sched-proxy-s3",2,0xfeedbeefull} };
        FILE* of=nullptr; if(tracedir){ snprintf(tpath,sizeof tpath,"%s/order-%s.tsv",tracedir,tag); of=fopen(tpath,"w"); if(of) fprintf(of,"order\ttu\tcum_src\tper_tu_wire\tcum_wire\n"); }
        printf("%-16s %14s %12s %12s\n","order","final_wire","blocks","root_tok");
        std::vector<uint64_t> finals; std::vector<FileSpan> saved_files=corpus.files;
        for(auto& o:orders){ corpus.files=permuted_files(corpus,o.ord,o.seed);
            Interner d2; OnlineLearner L2; FStore F2; OnlinePassOut AO; run_online_pass(d2,L2,F2,corpus,true,promote_mode,K,exp_F,AO,nullptr);
            finals.push_back(AO.cum_total_z.back());
            printf("%-16s %14llu %12u %12llu\n",o.name,(unsigned long long)AO.cum_total_z.back(),L2.bs.nblocks(),(unsigned long long)AO.st.root_tok);
            char kb[64]; snprintf(kb,sizeof kb,"order_%s_wire",o.name); Su(kb,AO.cum_total_z.back());
            if(of){ uint64_t cs=0; for(size_t t=0;t<AO.per_tu_total_z.size();++t){ cs+=AO.per_tu_src[t]; fprintf(of,"%s\t%zu\t%llu\t%llu\t%llu\n",o.name,t+1,(unsigned long long)cs,(unsigned long long)AO.per_tu_total_z[t],(unsigned long long)AO.cum_total_z[t]); } }
        }
        corpus.files=saved_files;   // restore original order (was permuted in place to avoid byte copies)
        if(of) fclose(of);
        std::sort(finals.begin(),finals.end());
        printf("order band: min=%llu median=%llu max=%llu (spread=%.1f%%)\n",(unsigned long long)finals.front(),(unsigned long long)finals[finals.size()/2],(unsigned long long)finals.back(),
            finals.front()?100.0*(double(finals.back())-double(finals.front()))/double(finals.front()):0.0);
        Su("order_min_wire",finals.front()); Su("order_median_wire",finals[finals.size()/2]); Su("order_max_wire",finals.back());
    }

    S("total_runtime_s",seconds_since(t0)); Su("ref_hash",ref_hash);
    S("frame_bytes_model",FRAME_BYTES); Su("K",K); Su("batch_min",batch_min);
    if(summ) fclose(summ);
    fprintf(stderr,"[%s] ALL DONE in %.1fs (ref_hash=%llx)\n",tag,seconds_since(t0),(unsigned long long)ref_hash);
    return 0;
}
