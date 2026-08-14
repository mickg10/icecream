// mixture-bench.cpp
//
// Bake-off (issue #16): mixture-of-predictors encoder aiming for 400x raw/full-charged-wire.
// Candidate #1 = LZ longest-previous-factor over the stable marker-region ID stream (a hash-chain
// matchfinder = a tractable proxy for bigoracle's suffix-automaton longest-previous-factor): for
// each TU, in build order, cover its region-id sequence by references to the LONGEST span seen in
// ANY prior position (causal/prequential); each accepted span is materialized as a content-addressed
// immutable flat Block (children = region objects). Then residual region literals + the line/region/
// block definitions are charged as full wire. Byte-exact (root tokens expand to the exact line-id
// stream). F stays a dumb expander; object IDs never rebind.
//
// Compared on the SAME full-wire accounting as superblock-online-bench (root zstd-L3 + amortized
// line/region/block defs via per-kind global ratio + framing) so the ratio lift vs pair-promotion is
// apples-to-apples. Reports basis A (cold prequential full charged wire) and basis B (warm recurring
// root-only, dicts amortized).
//
// build: g++ -O3 -march=native -std=c++17 mixture-bench.cpp -o mixture-bench -lzstd -lzstd

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <unordered_map>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <immintrin.h>
#include <zstd.h>
#include <zdict.h>

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
static inline uint32_t varint_len(uint64_t v){ uint32_t n=1; while(v>=0x80){v>>=7;++n;} return n; }
static size_t zstd_size(ZSTD_CCtx*c,const uint8_t*data,size_t n,int level){ ZSTD_CCtx_reset(c,ZSTD_reset_session_and_parameters); ZSTD_CCtx_setParameter(c,ZSTD_c_compressionLevel,level);
    size_t bound=ZSTD_compressBound(n); static thread_local std::vector<uint8_t> dst; if(dst.size()<bound)dst.resize(bound); size_t r=ZSTD_compress2(c,dst.data(),dst.size(),data?data:(const uint8_t*)"",n); if(ZSTD_isError(r)){fprintf(stderr,"zstd %s\n",ZSTD_getErrorName(r));exit(2);} return r; }

int main(int argc,char**argv){
    const char* manifest=nullptr; size_t max_files=SIZE_MAX; uint32_t MINMATCH=3, MAXCHAIN=64;
    for(int i=1;i<argc;++i){ if(!strcmp(argv[i],"--manifest")&&i+1<argc)manifest=argv[++i];
        else if(!strcmp(argv[i],"--min-match")&&i+1<argc)MINMATCH=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--max-chain")&&i+1<argc)MAXCHAIN=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--max-files")&&i+1<argc){ char*t=nullptr; unsigned long long v=strtoull(argv[++i],&t,10); if(!t||*t||!v){fprintf(stderr,"bad max-files\n");return 2;} max_files=size_t(v); }
        else { fprintf(stderr,"unknown %s\n",argv[i]); return 2; } }
    if(!manifest){ fprintf(stderr,"usage: %s --manifest F [--min-match N] [--max-chain N] [--max-files N]\n",argv[0]); return 2; }

    auto t0=Clock::now(); Corpus corpus=load_corpus(manifest,max_files); Interner dict;
    // cold intern -> per-TU region streams (flat S + offsets)
    std::vector<uint32_t> S; std::vector<size_t> roff; roff.push_back(0);
    { uint32_t maxlen=0; for(auto&f:corpus.files) maxlen=std::max(maxlen,f.len); std::vector<uint32_t> out(size_t(maxlen)+1); uint64_t hits=0; std::vector<uint32_t> rs;
      for(auto&f:corpus.files){ size_t oc=0; rs.clear(); const char*p=corpus.bytes.data()+f.off; dict.process(p,p+f.len,out.data(),oc,hits,true,&rs); S.insert(S.end(),rs.begin(),rs.end()); roff.push_back(S.size()); } }
    uint32_t NREG=uint32_t(dict.region_count()); size_t NS=S.size(); size_t TUs=corpus.files.size();
    fprintf(stderr,"loaded+interned %.1fs TUs=%zu raw=%llu regions=%u region_occ=%zu distinct_lines=%u\n",secs(t0),TUs,(unsigned long long)corpus.raw,NREG,NS,dict.distinct());

    // ---- LZ hash-chain matchfinder over region-id stream S (causal/prequential) ----
    // token space: t<NREG = region id ; t>=NREG = block id (flat span of region ids), stored in CSR.
    std::vector<uint32_t> bchild; std::vector<size_t> boff2; boff2.push_back(0);   // block k -> region-id span
    std::unordered_map<uint64_t,uint32_t> bdict;                                    // content hash -> block id (exact-compared)
    uint32_t hbits=22; std::vector<uint32_t> head(size_t(1)<<hbits, UINT32_MAX); std::vector<uint32_t> prevp(NS, UINT32_MAX);
    auto kgram=[&](size_t i)->uint64_t{ uint64_t h=1469598103934665603ULL; for(uint32_t j=0;j<MINMATCH;++j){ h^=S[i+j]; h*=1099511628211ULL; } return (h*0x9E3779B97F4A7C15ULL)>>(64-hbits); };
    auto span_hash=[&](const uint32_t*p,size_t L)->uint64_t{ uint64_t h=1469598103934665603ULL^(L*0x100000001b3ULL); for(size_t j=0;j<L;++j){ h^=p[j]; h*=1099511628211ULL; } return h; };
    auto block_get=[&](const uint32_t*p,size_t L)->uint32_t{ uint64_t h=span_hash(p,L); auto it=bdict.find(h);
        while(it!=bdict.end()&&it->first==h){ uint32_t k=it->second; if(boff2[k+1]-boff2[k]==L && memcmp(&bchild[boff2[k]],p,L*4)==0) return NREG+k; ++it; if(it==bdict.end()||it->first!=h)break; }
        uint32_t k=uint32_t(boff2.size()-1); bchild.insert(bchild.end(),p,p+L); boff2.push_back(bchild.size()); bdict.emplace(h,k); return NREG+k; };

    // per-TU root token streams
    std::vector<uint32_t> roots; std::vector<size_t> root_off; root_off.push_back(0);
    uint64_t total_root_tok=0, total_lit=0, total_blkref=0, matched_regions=0;
    auto tb=Clock::now();
    for(size_t t=0;t<TUs;++t){ size_t a=roff[t], b=roff[t+1];
        size_t i=a;
        while(i<b){
            // longest previous match at i (source pos p<i), min length MINMATCH, within [i,b)
            size_t bestL=0, bestP=0;
            if(i+MINMATCH<=b && i+MINMATCH<=NS){ uint32_t cand=head[kgram(i)]; uint32_t chain=0;
                while(cand!=UINT32_MAX && chain<MAXCHAIN){ if(cand<i){ // causal
                        size_t L=0, maxL=b-i; while(L<maxL && S[cand+L]==S[i+L]) ++L; if(L>=MINMATCH && L>bestL){ bestL=L; bestP=cand; if(L==maxL) break; } }
                    cand=prevp[cand]; ++chain; } }
            size_t step;
            if(bestL>=MINMATCH){ uint32_t bt=block_get(&S[i],bestL); roots.push_back(bt); ++total_blkref; matched_regions+=bestL; step=bestL; }
            else { roots.push_back(S[i]); ++total_lit; step=1; }
            // insert covered positions into the matchfinder (so future positions can reference them)
            for(size_t j=i;j<i+step;++j){ if(j+MINMATCH<=NS){ uint64_t g=kgram(j); prevp[j]=head[g]; head[g]=uint32_t(j); } }
            i+=step;
        }
        root_off.push_back(roots.size());
    }
    total_root_tok=roots.size();
    fprintf(stderr,"LZ parse %.1fs: root_tok=%llu (lit=%llu blkref=%llu) blocks=%zu matched_region_frac=%.1f%%\n",
        secs(tb),(unsigned long long)total_root_tok,(unsigned long long)total_lit,(unsigned long long)total_blkref,boff2.size()-1, 100.0*matched_regions/NS);

    // ---- byte-exact verify: expand roots -> region ids -> line ids ; compare to interner line stream ----
    { ZSTD_CCtx*z=ZSTD_createCCtx(); (void)z; uint64_t occ=0; bool ok=true;
      std::vector<uint32_t> exp;
      for(size_t t=0;t<TUs && ok;++t){ exp.clear();
        for(size_t r=root_off[t];r<root_off[t+1];++r){ uint32_t tk=roots[r];
            if(tk<NREG){ const uint32_t* ids=dict.region_ids_ptr(tk); exp.insert(exp.end(),ids,ids+dict.region_ids_count(tk)); }
            else { uint32_t k=tk-NREG; for(size_t j=boff2[k];j<boff2[k+1];++j){ uint32_t rg=bchild[j]; const uint32_t* ids=dict.region_ids_ptr(rg); exp.insert(exp.end(),ids,ids+dict.region_ids_count(rg)); } } }
        // compare to the interner's line stream for this TU: re-derive by expanding S[a..b)
        std::vector<uint32_t> ref; for(size_t rr=roff[t];rr<roff[t+1];++rr){ uint32_t rg=S[rr]; const uint32_t* ids=dict.region_ids_ptr(rg); ref.insert(ref.end(),ids,ids+dict.region_ids_count(rg)); }
        if(exp!=ref){ ok=false; fprintf(stderr,"VERIFY FAIL TU %zu (%zu vs %zu)\n",t,exp.size(),ref.size()); }
        occ+=exp.size(); }
      ZSTD_freeCCtx(z); fprintf(stderr,"byte-exact: %s (line-occ=%llu)\n", ok?"OK":"FAIL",(unsigned long long)occ); if(!ok) return 1; }

    // ---- full-wire accounting (matches superblock-online-bench): root z3 per-TU + amortized defs + framing ----
    ZSTD_CCtx*z=ZSTD_createCCtx();
    // per-kind compression ratios (global): line text, region composition, block composition
    double line_ratio=1,region_ratio=1,block_ratio=1;
    { std::vector<uint8_t> buf;
      for(uint32_t id=1;id<=dict.distinct();++id){ const LineRef&r=dict.ref(id); buf.insert(buf.end(),dict.line_data(r.off),dict.line_data(r.off)+r.len);} if(!buf.empty()) line_ratio=double(zstd_size(z,buf.data(),buf.size(),3))/buf.size();
      buf.clear(); for(uint32_t r=0;r<NREG;++r){ uint32_t c=dict.region_ids_count(r); const uint32_t*ids=dict.region_ids_ptr(r); put_varint(buf,c); for(uint32_t j=0;j<c;++j) put_varint(buf,ids[j]); } if(!buf.empty()) region_ratio=double(zstd_size(z,buf.data(),buf.size(),3))/buf.size();
      buf.clear(); for(size_t k=0;k+1<boff2.size();++k){ put_varint(buf,boff2[k+1]-boff2[k]); for(size_t j=boff2[k];j<boff2[k+1];++j) put_varint(buf,bchild[j]); } if(!buf.empty()) block_ratio=double(zstd_size(z,buf.data(),buf.size(),3))/buf.size();
    }
    // trained zstd dict on distinct line texts (residual line-def leg)
    double line_dict_ratio=line_ratio; size_t zdict_sz=0;
    { size_t ND=dict.distinct(); if(ND>=16){ std::vector<uint8_t> flat; std::vector<size_t> sizes; flat.reserve(dict.distinct_line_bytes());
        for(uint32_t id=1;id<=ND;++id){ const LineRef&r=dict.ref(id); flat.insert(flat.end(),dict.line_data(r.off),dict.line_data(r.off)+r.len); sizes.push_back(r.len); }
        std::vector<uint8_t> db(256*1024); size_t ds=ZDICT_trainFromBuffer(db.data(),db.size(),flat.data(),sizes.data(),unsigned(sizes.size()));
        if(!ZDICT_isError(ds)){ zdict_sz=ds; ZSTD_CDict*cd=ZSTD_createCDict(db.data(),ds,3); ZSTD_CCtx*c2=ZSTD_createCCtx(); std::vector<uint8_t> dst; size_t tot=0,traw=0;
            for(uint32_t id=1;id<=ND;++id){ const LineRef&r=dict.ref(id); size_t bnd=ZSTD_compressBound(r.len); if(dst.size()<bnd)dst.resize(bnd); size_t rr=ZSTD_compress_usingCDict(c2,dst.data(),dst.size(),dict.line_data(r.off),r.len,cd); if(!ZSTD_isError(rr)){tot+=rr;traw+=r.len;} }
            if(traw) line_dict_ratio=double(tot)/traw; ZSTD_freeCCtx(c2); ZSTD_freeCDict(cd); } }
    }

    // single cold F: charge line/region/block defs on first appearance; root stream every TU.
    std::vector<uint8_t> knownL(dict.distinct()+1,0), knownR(NREG,0), knownB(boff2.size(),0);
    double sum_root_raw=0,sum_root_z=0, def_line=0,def_region=0,def_block=0, missing=0,framing=0;
    const double FRAME_ROOT=12, FRAME_FILL=12;
    std::vector<uint8_t> mb;
    for(size_t t=0;t<TUs;++t){ mb.clear(); bool any=false;
        for(size_t r=root_off[t];r<root_off[t+1];++r){ uint32_t tk=roots[r]; put_varint(mb,tk); }
        sum_root_raw+=mb.size(); sum_root_z+=zstd_size(z,mb.data(),mb.size(),3); framing+=FRAME_ROOT;
        // closure: for each root token, if new -> charge def(s)
        auto chargeRegion=[&](uint32_t rg){ if(knownR[rg])return; const uint32_t*ids=dict.region_ids_ptr(rg); uint32_t c=dict.region_ids_count(rg);
            for(uint32_t j=0;j<c;++j){ uint32_t ln=ids[j]; if(!knownL[ln]){ def_line+=dict.ref(ln).len; knownL[ln]=1; } }
            // region composition def
            def_region += varint_len(c); for(uint32_t j=0;j<c;++j) def_region+=varint_len(ids[j]); knownR[rg]=1; };
        for(size_t r=root_off[t];r<root_off[t+1];++r){ uint32_t tk=roots[r];
            if(tk<NREG){ if(!knownR[tk]){ any=true; missing+=varint_len(tk); chargeRegion(tk); } }
            else { uint32_t k=tk-NREG; if(!knownB[k]){ any=true; missing+=varint_len(tk);
                    for(size_t j=boff2[k];j<boff2[k+1];++j) chargeRegion(bchild[j]);
                    def_block += varint_len(boff2[k+1]-boff2[k]); for(size_t j=boff2[k];j<boff2[k+1];++j) def_block+=varint_len(bchild[j]); knownB[k]=1; } } }
        if(any) framing+=FRAME_FILL;
    }
    double def_line_z = def_line*line_ratio, def_region_z=def_region*region_ratio, def_block_z=def_block*block_ratio;
    double basisA = sum_root_z + def_line_z + def_region_z + def_block_z + missing + framing;   // cold, everything counted
    double basisC = sum_root_z + def_block_z + missing + framing;   // warm: Line+Region dicts amortized; block defs + root counted
    double basisB = sum_root_z + framing;                            // warm: all dicts amortized -> root stream only
    double line_dict_floor = corpus.raw / (def_line*line_ratio);     // line-dict ceiling (can't beat on cold)
    ZSTD_freeCCtx(z);

    double MiB=1048576.0;
    printf("\n==== MIXTURE (LZ longest-factor over regions) ====\n");
    printf("corpus=%s TUs=%zu raw=%.1f MiB regions=%u region_occ=%zu distinct_lines=%u\n",manifest,TUs,corpus.raw/MiB,NREG,NS,dict.distinct());
    printf("LZ: min_match=%u max_chain=%u  root_tok=%llu (%.4f/region) blocks=%zu matched_regions=%.1f%%\n",MINMATCH,MAXCHAIN,(unsigned long long)total_root_tok,double(total_root_tok)/NS,boff2.size()-1,100.0*matched_regions/NS);
    printf("ratios: line(batch)=%.3f line(per-line+zdict)=%.3f region=%.3f block=%.3f zdict_sz=%zu\n",line_ratio,line_dict_ratio,region_ratio,block_ratio,zdict_sz);
    printf("wire bytes: root_z=%.0f def_line_z=%.0f def_region_z=%.0f def_block_z=%.0f missing=%.0f framing=%.0f\n",sum_root_z,def_line_z,def_region_z,def_block_z,missing,framing);
    printf("BASIS A (cold, EVERYTHING counted): total=%.0f  ratio=%.0fx   [line-dict ceiling=%.0fx -> line-text bound]\n",basisA, corpus.raw/basisA, line_dict_floor);
    printf("BASIS C (warm: Line+Region dicts amortized; block-defs+root+framing counted): total=%.0f  ratio=%.0fx\n",basisC, corpus.raw/basisC);
    printf("BASIS B (warm: ALL dicts amortized; recurring root stream only): total=%.0f  ratio=%.0fx\n",basisB, corpus.raw/basisB);
    struct rusage ru{}; getrusage(RUSAGE_SELF,&ru); fprintf(stderr,"peak RSS=%.1f MiB total=%.1fs\n",ru.ru_maxrss/1024.0,secs(t0));
    return 0;
}
