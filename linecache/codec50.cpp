// codec50.cpp — local-oracle's real Protocol-50 bake-off contender (issue #16).
//
// The authoritative path is local_codec50::sender_main/receiver_main below. C and an exec'd F
// exchange actual four-byte-length-prefixed frames over a socketpair. F starts empty, receives no
// manifest, installs immutable Line/Region/Block objects, reconstructs every TU, and writes it through
// a pipe to an independent byte comparator. The cold chronological stream uses zstd 0/1/3, explicit
// charged wire bytes, proactive sticky-F fills, and a bounded in-flight TU window. Fixture loading is
// timed and printed separately from the first codec-consumed input byte through the final F output.
//
// The older model_main remains in this research file only as a non-authoritative comparison harness.
//
// build: g++ -O3 -march=native -std=c++17 codec50.cpp -o codec50 -lzstd -pthread

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <limits>
#include <string>
#include <thread>
#include <unordered_map>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
#include <immintrin.h>
#include <zstd.h>
#if __has_include("definition_codec.h")
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

// Append a controlled one-line common-header edit for each TU containing the
// stdc-predef marker.  Offsets, rather than pointers, keep the original spans valid
// if the backing vector moves.  The original Corpus::raw remains the cold-build size.
static std::vector<FileSpan> append_header_edit(Corpus& corpus, uint64_t& modified) {
    static constexpr char marker[] = "# 1 \"/usr/include/stdc-predef.h\" 1 3 4\n";
    static constexpr char inserted[] = "typedef int icecream_semantic_root_edit;\n";
    std::vector<FileSpan> edited;
    edited.reserve(corpus.files.size());
    corpus.bytes.resize(size_t(corpus.raw));
    corpus.bytes.reserve(size_t(corpus.raw) * 2 + corpus.files.size() * sizeof(inserted) + 64);
    modified = 0;
    for (const FileSpan& span : corpus.files) {
        std::string source(corpus.bytes.data() + span.off, span.len);
        size_t at = source.find(marker);
        uint64_t out_off = corpus.bytes.size();
        if (at == std::string::npos) {
            corpus.bytes.insert(corpus.bytes.end(), source.begin(), source.end());
        } else {
            at += sizeof(marker) - 1;
            corpus.bytes.insert(corpus.bytes.end(), source.begin(), source.begin() + at);
            corpus.bytes.insert(corpus.bytes.end(), inserted, inserted + sizeof(inserted) - 1);
            corpus.bytes.insert(corpus.bytes.end(), source.begin() + at, source.end());
            ++modified;
        }
        uint64_t out_len = corpus.bytes.size() - out_off;
        if (out_len > UINT32_MAX) {
            fprintf(stderr, "edited TU exceeds u32 length\n");
            exit(2);
        }
        edited.push_back({out_off, uint32_t(out_len)});
    }
    corpus.bytes.resize(corpus.bytes.size() + 64);
    return edited;
}
static inline void put_varint(std::vector<uint8_t>&o,uint64_t v){ while(v>=0x80){o.push_back(uint8_t(v)|0x80);v>>=7;} o.push_back(uint8_t(v)); }
static inline uint64_t get_varint(const uint8_t*&p){ uint64_t v=0; int s=0; for(;;){ uint8_t b=*p++; v|=uint64_t(b&0x7f)<<s; if(!(b&0x80))break; s+=7; } return v; }
static size_t zstd_size(ZSTD_CCtx*c,const uint8_t*data,size_t n,int level,std::vector<uint8_t>&dst){ ZSTD_CCtx_reset(c,ZSTD_reset_session_and_parameters); ZSTD_CCtx_setParameter(c,ZSTD_c_compressionLevel,level);
    size_t bound=ZSTD_compressBound(n); if(dst.size()<bound)dst.resize(bound); size_t r=ZSTD_compress2(c,dst.data(),dst.size(),data?data:(const uint8_t*)"",n); if(ZSTD_isError(r)){fprintf(stderr,"zstd %s\n",ZSTD_getErrorName(r));exit(2);} return r; }

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

[[maybe_unused]] static int model_main(int argc,char**argv){
    const char* manifest=nullptr; size_t max_files=SIZE_MAX; int zlevel=3; bool useD1=true, useD2=false;
    for(int i=1;i<argc;++i){ if(!strcmp(argv[i],"--manifest")&&i+1<argc)manifest=argv[++i];
        else if(!strcmp(argv[i],"--z")&&i+1<argc)zlevel=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--no-d1"))useD1=false;
        else if(!strcmp(argv[i],"--d2"))useD2=true;
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

    // ===== ENCODER (C) + DECODER (F): one cold chronological pass, PULL protocol (root -> MISSING -> FILL) =====
    // C tracks F's known sets (single F) so it computes exactly the missing closure. Every wire byte is
    // charged by category, compressed at z<=zlevel per message. F reconstructs .ii bytes byte-exact.
    ZSTD_CCtx* z=ZSTD_createCCtx(); std::vector<uint8_t> dst;
    std::vector<uint8_t> fknownLine(dict.distinct()+1,0), fknownReg(NREG,0);
    std::unordered_map<std::string,uint32_t> pathid; std::vector<std::string> paths;   // D1 path objects (both sides derive same order)
    // ---- F's OWN independent store, built ONLY from decoded wire bytes (proves self-describing) ----
    std::vector<uint8_t> Fline_data; std::vector<size_t> Fline_off; Fline_off.push_back(0);   // line id k (1-based) -> [off[k-1],off[k])
    Fline_data.reserve(64u<<20);
    std::vector<uint32_t> Freg_child; std::vector<size_t> Freg_off; Freg_off.push_back(0);     // region id k (0-based) -> [off[k],off[k+1])
    std::vector<std::string> Fpaths;
    // wire byte accumulators (post-z, per category) + f-checkpoint tracking
    double w_root=0, w_linedef=0, w_regiondef=0, w_pathdef=0, w_missing=0, w_framing=0;
    const double FRAME=4;   // 4-byte length prefix per framed message
    uint64_t cum_raw=0; double cum_wire=0;
    // f-checkpoints + H200 trailing window
    std::vector<double> ck_f={0.10,0.25,0.50,0.75,1.00}; std::vector<std::pair<double,double>> ck; // (cum_raw_frac target hit -> ratio) recorded
    size_t ckidx=0; std::vector<double> perTU_raw(TUs), perTU_wire(TUs);
    // decoder-side reconstruction store: line bytes (F), region->line composition (F)
    // F re-derives line bytes from the defs it receives; we verify against the interner's truth.
    Marker mk; std::vector<uint8_t> segbuf, msg, recon, tmp, expectbuf;
#ifdef HAVE_DEFCODEC
    DefCodec encC, decF;  // C-side and F-side relative-LZ line codecs (identical growing stores)
#endif
    bool byteexact=true; uint64_t n_marker=0,n_literal=0;

    for(size_t t=0; t<TUs; ++t){
        const uint32_t* rg=&allreg[roff[t]]; size_t rn=roff[t+1]-roff[t];
        // --- MISSING: region ids in this root that F does not know (first occurrence order) ---
        std::vector<uint32_t> missReg; msg.clear();
        for(size_t i=0;i<rn;++i){ uint32_t r=rg[i]; if(!fknownReg[r]){ // will be filled; mark provisionally after building fill
                bool already=false; for(uint32_t mr:missReg) if(mr==r){already=true;break;} if(!already) missReg.push_back(r); } }
        // MISSING message = varint(count)+region ids (charged; real round-trip in the 2-proc version)
        { std::vector<uint8_t> mm; put_varint(mm,missReg.size()); for(uint32_t r:missReg) put_varint(mm,r);
          if(!mm.empty()){ w_missing += (missReg.empty()?0:zstd_size(z,mm.data(),mm.size(),zlevel,dst)) + (missReg.empty()?0:FRAME); } }
        // --- FILL: topologically ordered closure of missReg: new paths, new lines, new region defs ---
        std::vector<uint8_t> fill_paths, fill_lines, fill_regions; uint32_t np=0,nl=0,nr=0;
        for(uint32_t r:missReg){ const uint32_t* lids=dict.region_ids_ptr(r); uint32_t c=dict.region_ids_count(r);
            // new lines in this region
            for(uint32_t j=0;j<c;++j){ uint32_t ln=lids[j]; if(fknownLine[ln]) continue; fknownLine[ln]=1; ++nl;
                const LineRef& lr=dict.ref(ln); const char* txt=dict.line_data(lr.off);
                if(useD1 && parse_marker(txt,lr.len,mk)){ // D1 MARKER kind
                    uint32_t pid; auto it=pathid.find(mk.path); if(it==pathid.end()){ pid=uint32_t(paths.size()); pathid.emplace(mk.path,pid); paths.push_back(mk.path);
                        put_varint(fill_paths,mk.path.size()); fill_paths.insert(fill_paths.end(),mk.path.begin(),mk.path.end()); ++np; } else pid=it->second;
                    fill_lines.push_back(1); put_varint(fill_lines,pid); put_varint(fill_lines,mk.lineno); fill_lines.push_back(uint8_t(mk.flags.size())); for(uint8_t f:mk.flags) fill_lines.push_back(f); ++n_marker;
                } else { // LITERAL kind (0). D2 relative-LZ optionally encodes the bytes.
                    fill_lines.push_back(0);
#ifdef HAVE_DEFCODEC
                    if(useD2){ segbuf.clear(); encC.encode((const uint8_t*)txt,lr.len,segbuf); put_varint(fill_lines,lr.len); put_varint(fill_lines,segbuf.size()); fill_lines.insert(fill_lines.end(),segbuf.begin(),segbuf.end()); }
                    else { put_varint(fill_lines,lr.len); fill_lines.insert(fill_lines.end(),txt,txt+lr.len); }
#else
                    put_varint(fill_lines,lr.len); fill_lines.insert(fill_lines.end(),txt,txt+lr.len);
#endif
                    ++n_literal;
                }
            }
            // region def = varint(count)+line ids
            put_varint(fill_regions,c); for(uint32_t j=0;j<c;++j) put_varint(fill_regions,lids[j]); fknownReg[r]=1; ++nr;
        }
        if(np){ w_pathdef += zstd_size(z,fill_paths.data(),fill_paths.size(),zlevel,dst); }
        if(nl){ w_linedef += zstd_size(z,fill_lines.data(),fill_lines.size(),zlevel,dst); }
        if(nr){ w_regiondef += zstd_size(z,fill_regions.data(),fill_regions.size(),zlevel,dst); }
        if(np||nl||nr) w_framing += FRAME;   // one FILL frame
        // --- ROOT: region-id sequence ---
        std::vector<uint8_t> rootb; for(size_t i=0;i<rn;++i) put_varint(rootb,rg[i]);
        w_root += zstd_size(z,rootb.data(),rootb.size(),zlevel,dst); w_framing += FRAME;

        // --- DECODER (F): install FILL from wire bytes into F's OWN store, then expand ROOT ---
        { const uint8_t* pp=fill_paths.data(); for(uint32_t k=0;k<np;++k){ uint64_t L=get_varint(pp); Fpaths.emplace_back((const char*)pp,(size_t)L); pp+=L; } }
        { const uint8_t* pp=fill_lines.data(), *pe=fill_lines.data()+fill_lines.size();
          while(pp<pe){ uint8_t kind=*pp++;
            if(kind==1){ uint64_t pid=get_varint(pp); uint64_t lineno=get_varint(pp); uint8_t nf=*pp++; Marker dm; dm.path=Fpaths[pid]; dm.lineno=lineno; for(uint8_t f=0;f<nf;++f) dm.flags.push_back(*pp++);
                tmp.clear(); emit_marker(dm,tmp); Fline_data.insert(Fline_data.end(),tmp.begin(),tmp.end()); Fline_off.push_back(Fline_data.size()); }
            else { uint64_t len=get_varint(pp);
#ifdef HAVE_DEFCODEC
              if(useD2){ uint64_t sl=get_varint(pp); std::vector<uint8_t> lo; decF.decode(pp,sl,lo); pp+=sl; (void)len; Fline_data.insert(Fline_data.end(),lo.begin(),lo.end()); Fline_off.push_back(Fline_data.size()); }
              else { Fline_data.insert(Fline_data.end(),pp,pp+len); pp+=len; Fline_off.push_back(Fline_data.size()); }
#else
              Fline_data.insert(Fline_data.end(),pp,pp+len); pp+=len; Fline_off.push_back(Fline_data.size());
#endif
            } } }
        { const uint8_t* pp=fill_regions.data(), *pe=fill_regions.data()+fill_regions.size();
          while(pp<pe){ uint64_t c=get_varint(pp); for(uint64_t j=0;j<c;++j) Freg_child.push_back(uint32_t(get_varint(pp))); Freg_off.push_back(Freg_child.size()); } }
        // expand ROOT (region ids -> F line ids -> F line bytes) entirely from F's decoded store
        recon.clear();
        { const uint8_t* pp=rootb.data(), *pe=rootb.data()+rootb.size();
          while(pp<pe){ uint32_t r=uint32_t(get_varint(pp)); for(size_t j=Freg_off[r];j<Freg_off[r+1];++j){ uint32_t ln=Freg_child[j]; recon.insert(recon.end(), Fline_data.begin()+Fline_off[ln-1], Fline_data.begin()+Fline_off[ln]); } } }
        const char* orig=corpus.bytes.data()+corpus.files[t].off; uint32_t olen=corpus.files[t].len;
        if(recon.size()!=olen || memcmp(recon.data(),orig,olen)!=0){ byteexact=false; if(t<5||TUs<10) fprintf(stderr,"BYTE-EXACT FAIL TU %zu (%zu vs %u)\n",t,recon.size(),olen); }
        // --- accounting checkpoints ---
        double tu_wire = 0; // recompute this TU's wire from the deltas we just added is awkward; track cumulative
        (void)tu_wire;
        cum_raw += olen;
        double cur_wire = w_root+w_linedef+w_regiondef+w_pathdef+w_missing+w_framing;
        perTU_raw[t]=olen; perTU_wire[t]=cur_wire - cum_wire; cum_wire=cur_wire;
        while(ckidx<ck_f.size() && double(cum_raw)>=ck_f[ckidx]*corpus.raw){ ck.push_back({ck_f[ckidx], double(cum_raw)/cum_wire}); ++ckidx; }
    }
    while(ck.size()<ck_f.size()) ck.push_back({ck_f[ck.size()], double(cum_raw)/cum_wire});
    ZSTD_freeCCtx(z);

    double totalwire = w_root+w_linedef+w_regiondef+w_pathdef+w_missing+w_framing;
    double MiB=1048576.0;
    printf("\n==== CODEC-50 (V1 Lines+Regions%s%s, z%d) — %s ====\n", useD1?"+D1":"", useD2?"+D2":"", zlevel, manifest);
    printf("byte-exact=%s  TUs=%zu raw=%.1f MiB regions=%u distinct_lines=%u paths=%zu (marker_lines=%llu literal_lines=%llu)\n",
        byteexact?"OK":"FAIL",TUs,corpus.raw/MiB,NREG,dict.distinct(),paths.size(),(unsigned long long)n_marker,(unsigned long long)n_literal);
    printf("wire by category (post-z%d, bytes): root=%.0f line_def=%.0f region_def=%.0f path_def=%.0f missing=%.0f framing=%.0f  TOTAL=%.0f (%.2f MiB)\n",
        zlevel,w_root,w_linedef,w_regiondef,w_pathdef,w_missing,w_framing,totalwire,totalwire/MiB);
    printf("FinalRatio (raw / total wire, one cold pass) = %.1fx\n", corpus.raw/totalwire);
    printf("H200 f-checkpoints (cum raw fraction -> cumulative ratio):\n");
    for(auto&c:ck) printf("  f=%.2f  ratio=%.0fx\n",c.first,c.second);
    // trailing-window (5% raw) ratio near the end
    { double win=0.05*corpus.raw, r=0,wsum=0; for(size_t t=TUs;t-->0;){ r+=perTU_raw[t]; wsum+=perTU_wire[t]; if(r>=win) break; } printf("trailing 5%%-raw window ratio (steady) = %.0fx\n", wsum>0?r/wsum:0); }
    struct rusage ru{}; getrusage(RUSAGE_SELF,&ru); fprintf(stderr,"peak RSS=%.1f MiB total=%.1fs\n",ru.ru_maxrss/1024.0,secs(t0));
    return byteexact?0:1;
}

// -----------------------------------------------------------------------------
// local-oracle contender
//
// Unlike model_main(), this path moves the compressed bytes through a real
// full-duplex socket to an exec'd receiver.  The receiver has no manifest and
// begins with an empty store.  Reconstructed bytes leave through a pipe and are
// compared byte-for-byte by an independent consumer.
// -----------------------------------------------------------------------------

namespace local_codec50 {

enum class Msg : uint8_t {
    Hello = 1,
    Dict = 2,
    Root = 3,
    Missing = 4,
    Fill = 5,
    Ack = 6,
    Done = 7,
    Final = 8,
    MissingLines = 9,
    FillLines = 10,
    MissingRegions = 11,
    FillRegions = 12,
    RootRef = 13,
};

static const char* msg_name(Msg m) {
    switch (m) {
    case Msg::Hello: return "HELLO";
    case Msg::Dict: return "DICT";
    case Msg::Root: return "ROOT";
    case Msg::Missing: return "MISSING";
    case Msg::Fill: return "FILL";
    case Msg::Ack: return "ACK";
    case Msg::Done: return "DONE";
    case Msg::Final: return "FINAL";
    case Msg::MissingLines: return "MISSING_LINES";
    case Msg::FillLines: return "FILL_LINES";
    case Msg::MissingRegions: return "MISSING_REGIONS";
    case Msg::FillRegions: return "FILL_REGIONS";
    case Msg::RootRef: return "ROOT_REF";
    }
    return "UNKNOWN";
}

static uint64_t elapsed_ns(Clock::time_point b) {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - b).count());
}

static bool write_all(int fd, const void* data, size_t size) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    while (size) {
        ssize_t n = ::write(fd, p, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        p += size_t(n);
        size -= size_t(n);
    }
    return true;
}

static bool read_all(int fd, void* data, size_t size) {
    uint8_t* p = static_cast<uint8_t*>(data);
    while (size) {
        ssize_t n = ::read(fd, p, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        p += size_t(n);
        size -= size_t(n);
    }
    return true;
}

static void put_u32(std::vector<uint8_t>& out, uint32_t v) {
    size_t at = out.size();
    out.resize(at + sizeof(v));
    memcpy(out.data() + at, &v, sizeof(v));
}

static void put_u64(std::vector<uint8_t>& out, uint64_t v) {
    size_t at = out.size();
    out.resize(at + sizeof(v));
    memcpy(out.data() + at, &v, sizeof(v));
}

static void put_uvar(std::vector<uint8_t>& out, uint64_t v) {
    while (v >= 0x80) {
        out.push_back(uint8_t(v) | 0x80);
        v >>= 7;
    }
    out.push_back(uint8_t(v));
}

static size_t uvar_size(uint64_t v) {
    size_t n = 1;
    while (v >= 0x80) { v >>= 7; ++n; }
    return n;
}

class Cursor {
public:
    explicit Cursor(const std::vector<uint8_t>& bytes)
        : p_(bytes.data()), end_(bytes.data() + bytes.size()) {}

    bool u8(uint8_t& v) {
        if (p_ == end_) return false;
        v = *p_++;
        return true;
    }

    bool u32(uint32_t& v) {
        if (size_t(end_ - p_) < sizeof(v)) return false;
        memcpy(&v, p_, sizeof(v));
        p_ += sizeof(v);
        return true;
    }

    bool u64(uint64_t& v) {
        if (size_t(end_ - p_) < sizeof(v)) return false;
        memcpy(&v, p_, sizeof(v));
        p_ += sizeof(v);
        return true;
    }

    bool uvar(uint64_t& v) {
        v = 0;
        unsigned shift = 0;
        while (p_ != end_ && shift <= 63) {
            uint8_t b = *p_++;
            v |= uint64_t(b & 0x7f) << shift;
            if (!(b & 0x80)) return true;
            shift += 7;
        }
        return false;
    }

    bool bytes(size_t n, const uint8_t*& p) {
        if (size_t(end_ - p_) < n) return false;
        p = p_;
        p_ += n;
        return true;
    }

    bool done() const { return p_ == end_; }

private:
    const uint8_t* p_;
    const uint8_t* end_;
};

struct Frame {
    Msg type = Msg::Hello;
    std::vector<uint8_t> payload;
    uint64_t wire_bytes = 0;
};

class FramedChannel {
public:
    FramedChannel(int fd, int level) : fd_(fd), level_(level) {
        cctx_ = ZSTD_createCCtx();
        dctx_ = ZSTD_createDCtx();
        if (!cctx_ || !dctx_) { fprintf(stderr, "zstd context allocation failed\n"); exit(2); }
    }

    ~FramedChannel() {
        ZSTD_freeCCtx(cctx_);
        ZSTD_freeDCtx(dctx_);
    }

    bool send(Msg type, const std::vector<uint8_t>& payload, bool may_compress = true) {
        uint8_t codec = 0;
        const uint8_t* encoded = payload.data();
        size_t encoded_size = payload.size();

        if (may_compress && level_ > 0 && payload.size() >= 96) {
            size_t bound = ZSTD_compressBound(payload.size());
            compressed_.resize(bound);
            auto z0 = Clock::now();
            ZSTD_CCtx_reset(cctx_, ZSTD_reset_session_only);
            ZSTD_CCtx_setParameter(cctx_, ZSTD_c_compressionLevel, level_);
            size_t n = ZSTD_compress2(cctx_, compressed_.data(), compressed_.size(),
                                      payload.data(), payload.size());
            compress_ns_ += elapsed_ns(z0);
            if (ZSTD_isError(n)) {
                fprintf(stderr, "zstd encode: %s\n", ZSTD_getErrorName(n));
                return false;
            }
            if (n + 8 < payload.size()) {
                codec = 1;
                encoded = compressed_.data();
                encoded_size = n;
            }
        }

        if (encoded_size > UINT32_MAX - 8 || payload.size() > UINT32_MAX) return false;
        uint32_t body_size = uint32_t(encoded_size + 8);
        wirebuf_.resize(size_t(body_size) + 4);
        memcpy(wirebuf_.data(), &body_size, 4);
        wirebuf_[4] = uint8_t(type);
        wirebuf_[5] = codec;
        wirebuf_[6] = wirebuf_[7] = 0;
        uint32_t raw_size = uint32_t(payload.size());
        memcpy(wirebuf_.data() + 8, &raw_size, 4);
        if (encoded_size) memcpy(wirebuf_.data() + 12, encoded, encoded_size);

        auto io0 = Clock::now();
        bool ok = write_all(fd_, wirebuf_.data(), wirebuf_.size());
        write_ns_ += elapsed_ns(io0);
        if (!ok) return false;
        sent_ += wirebuf_.size();
        sent_by_type_[uint8_t(type)] += wirebuf_.size();
        return true;
    }

    bool recv(Frame& frame) {
        uint32_t body_size = 0;
        auto io0 = Clock::now();
        if (!read_all(fd_, &body_size, sizeof(body_size))) return false;
        if (body_size < 8 || body_size > (1u << 30)) return false;
        incoming_.resize(body_size);
        if (!read_all(fd_, incoming_.data(), incoming_.size())) return false;
        read_ns_ += elapsed_ns(io0);

        frame.type = Msg(incoming_[0]);
        uint8_t codec = incoming_[1];
        uint32_t raw_size = 0;
        memcpy(&raw_size, incoming_.data() + 4, sizeof(raw_size));
        const uint8_t* encoded = incoming_.data() + 8;
        size_t encoded_size = incoming_.size() - 8;

        if (codec == 0) {
            if (encoded_size != raw_size) return false;
            frame.payload.assign(encoded, encoded + encoded_size);
        } else if (codec == 1) {
            frame.payload.resize(raw_size);
            auto z0 = Clock::now();
            size_t n = ZSTD_decompressDCtx(dctx_, frame.payload.data(), frame.payload.size(),
                                           encoded, encoded_size);
            decompress_ns_ += elapsed_ns(z0);
            if (ZSTD_isError(n) || n != raw_size) return false;
        } else {
            return false;
        }

        frame.wire_bytes = uint64_t(body_size) + 4;
        received_ += frame.wire_bytes;
        received_by_type_[uint8_t(frame.type)] += frame.wire_bytes;
        return true;
    }

    uint64_t sent() const { return sent_; }
    uint64_t received() const { return received_; }
    uint64_t compress_ns() const { return compress_ns_; }
    uint64_t decompress_ns() const { return decompress_ns_; }
    uint64_t write_ns() const { return write_ns_; }
    uint64_t read_ns() const { return read_ns_; }
    const std::array<uint64_t, 256>& sent_by_type() const { return sent_by_type_; }
    const std::array<uint64_t, 256>& received_by_type() const { return received_by_type_; }

private:
    int fd_;
    int level_;
    ZSTD_CCtx* cctx_ = nullptr;
    ZSTD_DCtx* dctx_ = nullptr;
    std::vector<uint8_t> compressed_;
    std::vector<uint8_t> wirebuf_;
    std::vector<uint8_t> incoming_;
    uint64_t sent_ = 0;
    uint64_t received_ = 0;
    uint64_t compress_ns_ = 0;
    uint64_t decompress_ns_ = 0;
    uint64_t write_ns_ = 0;
    uint64_t read_ns_ = 0;
    std::array<uint64_t, 256> sent_by_type_{};
    std::array<uint64_t, 256> received_by_type_{};
};

struct ByteSpan {
    uint64_t off = 0;
    uint32_t len = 0;
    bool present = false;
};

class ReceiverStore {
public:
    bool has_line(uint64_t key) const {
        return key < lines_.size() && lines_[size_t(key)].present;
    }

    bool has_region(uint64_t key) const {
        return key < regions_.size() && regions_[size_t(key)].present;
    }

    bool has_block(uint64_t key) const {
        return key < blocks_.size() && blocks_[size_t(key)].present;
    }

    bool has_root(uint64_t key) const {
        return key < roots_.size() && roots_[size_t(key)].present;
    }

    bool install_line(uint64_t key, const uint8_t* bytes, size_t len) {
        if (key > UINT32_MAX || len > UINT32_MAX) return false;
        ensure(lines_, key);
        ByteSpan& s = lines_[size_t(key)];
        if (s.present) {
            return s.len == len && memcmp(line_bytes_.data() + s.off, bytes, len) == 0;
        }
        s.off = line_bytes_.size();
        s.len = uint32_t(len);
        s.present = true;
        line_bytes_.insert(line_bytes_.end(), bytes, bytes + len);
        return true;
    }

    bool install_region(uint64_t key, const std::vector<uint64_t>& children) {
        if (key > UINT32_MAX || children.size() > UINT32_MAX) return false;
        ensure(regions_, key);
        ByteSpan& s = regions_[size_t(key)];
        if (s.present) {
            if (s.len != children.size()) return false;
            return memcmp(region_children_.data() + s.off, children.data(),
                          children.size() * sizeof(uint64_t)) == 0;
        }
        s.off = region_children_.size();
        s.len = uint32_t(children.size());
        s.present = true;
        region_children_.insert(region_children_.end(), children.begin(), children.end());
        return true;
    }

    bool install_block(uint64_t key, const std::vector<uint64_t>& children) {
        if (key > UINT32_MAX || children.size() > UINT32_MAX) return false;
        ensure(blocks_, key);
        ByteSpan& s = blocks_[size_t(key)];
        if (s.present) {
            if (s.len != children.size()) return false;
            return memcmp(block_children_.data() + s.off, children.data(),
                          children.size() * sizeof(uint64_t)) == 0;
        }
        s.off = block_children_.size();
        s.len = uint32_t(children.size());
        s.present = true;
        block_children_.insert(block_children_.end(), children.begin(), children.end());
        return true;
    }

    bool install_root(uint64_t key, const std::vector<uint64_t>& regions) {
        if (!key || key > UINT32_MAX || regions.size() > UINT32_MAX) return false;
        ensure(roots_, key);
        ByteSpan& s = roots_[size_t(key)];
        if (s.present) {
            if (s.len != regions.size()) return false;
            return memcmp(root_children_.data() + s.off, regions.data(),
                          regions.size() * sizeof(uint64_t)) == 0;
        }
        s.off = root_children_.size();
        s.len = uint32_t(regions.size());
        s.present = true;
        root_children_.insert(root_children_.end(), regions.begin(), regions.end());
        return true;
    }

    const ByteSpan* line(uint64_t key) const {
        return has_line(key) ? &lines_[size_t(key)] : nullptr;
    }

    const ByteSpan* region(uint64_t key) const {
        return has_region(key) ? &regions_[size_t(key)] : nullptr;
    }

    const ByteSpan* block(uint64_t key) const {
        return has_block(key) ? &blocks_[size_t(key)] : nullptr;
    }

    const ByteSpan* root(uint64_t key) const {
        return has_root(key) ? &roots_[size_t(key)] : nullptr;
    }

    const char* line_bytes(const ByteSpan& s) const { return line_bytes_.data() + s.off; }
    const uint64_t* region_children(const ByteSpan& s) const {
        return region_children_.data() + s.off;
    }
    const uint64_t* block_children(const ByteSpan& s) const {
        return block_children_.data() + s.off;
    }
    const uint64_t* root_children(const ByteSpan& s) const {
        return root_children_.data() + s.off;
    }

    bool materialize_region(uint64_t key) {
        if (!has_region(key)) return false;
        ensure(region_raw_, key);
        ByteSpan& raw = region_raw_[size_t(key)];
        if (raw.present) return true;
        const ByteSpan& region_span = regions_[size_t(key)];
        const uint64_t* children = region_children_.data() + region_span.off;
        uint64_t total = 0;
        for (uint32_t i = 0; i < region_span.len; ++i) {
            const ByteSpan* line_span = line(children[i]);
            if (!line_span) return false;
            total += line_span->len;
        }
        if (total > UINT32_MAX) return false;
        raw.off = region_bytes_.size();
        raw.len = uint32_t(total);
        raw.present = true;
        for (uint32_t i = 0; i < region_span.len; ++i) {
            const ByteSpan& line_span = lines_[size_t(children[i])];
            const char* bytes = line_bytes_.data() + line_span.off;
            region_bytes_.insert(region_bytes_.end(), bytes, bytes + line_span.len);
        }
        return true;
    }

    const ByteSpan* region_raw(uint64_t key) const {
        return key < region_raw_.size() && region_raw_[size_t(key)].present
            ? &region_raw_[size_t(key)] : nullptr;
    }

    const char* region_bytes(const ByteSpan& s) const { return region_bytes_.data() + s.off; }

    uint64_t retained_bytes() const {
        return line_bytes_.capacity() + region_children_.capacity() * sizeof(uint64_t) +
               block_children_.capacity() * sizeof(uint64_t) +
               root_children_.capacity() * sizeof(uint64_t) + region_bytes_.capacity() +
               lines_.capacity() * sizeof(ByteSpan) + regions_.capacity() * sizeof(ByteSpan) +
               blocks_.capacity() * sizeof(ByteSpan) + roots_.capacity() * sizeof(ByteSpan) +
               region_raw_.capacity() * sizeof(ByteSpan);
    }

private:
    static void ensure(std::vector<ByteSpan>& v, uint64_t key) {
        if (key >= v.size()) v.resize(size_t(key) + 1);
    }

    std::vector<char> line_bytes_;
    std::vector<char> region_bytes_;
    std::vector<uint64_t> region_children_;
    std::vector<uint64_t> block_children_;
    std::vector<uint64_t> root_children_;
    std::vector<ByteSpan> lines_{1};
    std::vector<ByteSpan> regions_{1};
    std::vector<ByteSpan> blocks_{1};
    std::vector<ByteSpan> roots_{1};
    std::vector<ByteSpan> region_raw_{1};
};

struct RootToken {
    bool block = false;
    uint32_t local = 0;
};

struct Transaction {
    uint32_t id = 0;
    uint64_t raw_len = 0;
    uint64_t root_key = 0;
    bool root_ref = false;
    std::vector<uint64_t> line_keys;
    std::vector<uint64_t> region_keys;
    std::vector<uint64_t> block_keys;
    std::vector<uint64_t> missing_lines;
    std::vector<uint64_t> missing_regions;
    std::vector<uint64_t> missing_blocks;
    std::vector<uint64_t> closure_regions;
    std::vector<RootToken> root;
};

static bool decode_dict(const std::vector<uint8_t>& payload, Transaction& tx) {
    Cursor c(payload);
    uint64_t count = 0;
    if (!c.u32(tx.id) || !c.u64(tx.raw_len) || !c.uvar(count) || count > UINT32_MAX) return false;
    tx.region_keys.resize(size_t(count));
    for (uint64_t& key : tx.region_keys) if (!c.u64(key)) return false;
    if (!c.uvar(count) || count > UINT32_MAX) return false;
    tx.block_keys.resize(size_t(count));
    for (uint64_t& key : tx.block_keys) if (!c.u64(key)) return false;
    return c.done();
}

static bool decode_root(const std::vector<uint8_t>& payload, Transaction& tx) {
    Cursor c(payload);
    uint32_t id = 0;
    uint64_t count = 0;
    uint8_t mode = 0;
    if (!c.u32(id) || id != tx.id || !c.u8(mode) || mode != 0 ||
        !c.u64(tx.root_key) || !c.uvar(count) || count > UINT32_MAX) return false;
    tx.root.resize(size_t(count));
    for (RootToken& token : tx.root) {
        uint64_t v = 0;
        if (!c.uvar(v)) return false;
        token.block = (v & 1) != 0;
        v >>= 1;
        if (v > UINT32_MAX || (!token.block && v >= tx.region_keys.size()) ||
            (token.block && v >= tx.block_keys.size())) return false;
        token.local = uint32_t(v);
    }
    return c.done();
}

static bool install_definition(Cursor& c, uint64_t key, ReceiverStore& store) {
    uint8_t kind = 0;
    if (!key || !c.u8(kind)) return false;
    if (kind == 0) {
        uint64_t len = 0;
        const uint8_t* bytes = nullptr;
        return c.uvar(len) && len <= UINT32_MAX && c.bytes(size_t(len), bytes) &&
               store.install_line(key, bytes, size_t(len));
    }
    if (kind != 1) return false;

    uint64_t base_key = 0, out_len = 0, prefix = 0, suffix = 0, middle_len = 0;
    const uint8_t* middle = nullptr;
    if (!c.u64(base_key) || !c.uvar(out_len) || !c.uvar(prefix) || !c.uvar(suffix) ||
        !c.uvar(middle_len) || !c.bytes(size_t(middle_len), middle)) return false;
    const ByteSpan* base = store.line(base_key);
    if (!base || prefix + suffix > base->len || prefix + suffix + middle_len != out_len ||
        out_len > UINT32_MAX) return false;
    std::vector<uint8_t> expanded;
    expanded.reserve(size_t(out_len));
    const char* b = store.line_bytes(*base);
    expanded.insert(expanded.end(), b, b + prefix);
    expanded.insert(expanded.end(), middle, middle + middle_len);
    expanded.insert(expanded.end(), b + base->len - suffix, b + base->len);
    return store.install_line(key, expanded.data(), expanded.size());
}

static bool install_region_fill(const std::vector<uint8_t>& payload, const Transaction& tx,
                                ReceiverStore& store) {
    Cursor c(payload);
    uint32_t id = 0;
    uint64_t count = 0;
    if (!c.u32(id) || id != tx.id || !c.uvar(count) || count > UINT32_MAX) return false;
    std::vector<uint64_t> children;
    for (uint64_t i = 0; i < count; ++i) {
        uint64_t key = 0, n = 0;
        if (!c.u64(key) || !key || !c.uvar(n) || n > UINT32_MAX) return false;
        children.resize(size_t(n));
        for (uint64_t& key : children) if (!c.u64(key) || !key) return false;
        if (!store.install_region(key, children)) return false;
    }
    if (!c.uvar(count) || count > UINT32_MAX) return false;
    for (uint64_t i = 0; i < count; ++i) {
        uint64_t key = 0, n = 0;
        if (!c.u64(key) || !key || !c.uvar(n) || n > UINT32_MAX) return false;
        children.resize(size_t(n));
        for (uint64_t& key : children) if (!c.u64(key) || !key) return false;
        if (!store.install_block(key, children)) return false;
    }
    if (!c.uvar(count) || count > UINT32_MAX) return false;
    for (uint64_t i = 0; i < count; ++i) {
        uint64_t key = 0, n = 0;
        if (!c.u64(key) || !key || !c.uvar(n) || n > UINT32_MAX) return false;
        children.resize(size_t(n));
        for (uint64_t& child : children) if (!c.u64(child) || !child) return false;
        if (!store.install_region(key, children)) return false;
    }
    // Normal cold path: C's sticky view of F is exact, so definitions needed by the
    // regions above ride in the same compressed FILL. MissingLines remains a fallback.
    if (!c.uvar(count) || count > UINT32_MAX) return false;
    for (uint64_t i = 0; i < count; ++i) {
        uint64_t key = 0;
        if (!c.u64(key) || !install_definition(c, key, store)) return false;
    }
    return c.done();
}

static bool install_line_fill(const std::vector<uint8_t>& payload, const Transaction& tx,
                              ReceiverStore& store) {
    Cursor c(payload);
    uint32_t id = 0;
    uint64_t count = 0;
    if (!c.u32(id) || id != tx.id || !c.uvar(count) || count != tx.missing_lines.size()) return false;
    for (size_t i = 0; i < tx.missing_lines.size(); ++i) {
        if (!install_definition(c, tx.missing_lines[i], store)) return false;
    }
    return c.done();
}

static bool install_closure_region_fill(const std::vector<uint8_t>& payload,
                                        const Transaction& tx, ReceiverStore& store) {
    Cursor c(payload);
    uint32_t id = 0;
    uint64_t count = 0;
    if (!c.u32(id) || id != tx.id || !c.uvar(count) || count != tx.closure_regions.size())
        return false;
    std::vector<uint64_t> children;
    for (size_t i = 0; i < tx.closure_regions.size(); ++i) {
        uint64_t n = 0;
        if (!c.uvar(n) || n > UINT32_MAX) return false;
        children.resize(size_t(n));
        for (uint64_t& key : children) if (!c.u64(key) || !key) return false;
        if (!store.install_region(tx.closure_regions[i], children)) return false;
    }
    return c.done();
}

static bool emit_transaction(const Transaction& tx, ReceiverStore& store, int out_fd,
                             uint64_t& emitted) {
    std::vector<ByteSpan> ordered;
    ordered.reserve(tx.root.size() * 4);
    std::vector<uint64_t> semantic_root;
    uint64_t total = 0;

    auto append_region = [&](uint64_t region_key) -> bool {
        if (!store.materialize_region(region_key)) return false;
        const ByteSpan* raw = store.region_raw(region_key);
        if (!raw) return false;
        ordered.push_back(*raw);
        total += raw->len;
        return true;
    };

    if (tx.root_ref) {
        const ByteSpan* root = store.root(tx.root_key);
        if (!root) return false;
        const uint64_t* children = store.root_children(*root);
        semantic_root.assign(children, children + root->len);
        for (uint64_t region_key : semantic_root)
            if (!append_region(region_key)) return false;
    } else {
        for (const RootToken& token : tx.root) {
            if (!token.block) {
                uint64_t region_key = tx.region_keys[token.local];
                semantic_root.push_back(region_key);
                if (!append_region(region_key)) return false;
                continue;
            }
            const ByteSpan* block = store.block(tx.block_keys[token.local]);
            if (!block) return false;
            const uint64_t* children = store.block_children(*block);
            semantic_root.insert(semantic_root.end(), children, children + block->len);
            for (uint32_t i = 0; i < block->len; ++i)
                if (!append_region(children[i])) return false;
        }
        if (tx.root_key && !store.install_root(tx.root_key, semantic_root)) return false;
    }
    if (total != tx.raw_len) return false;

    // All region materialization is complete, so region_bytes_ can no longer move.
    // Scatter-gather directly into the compiler/verifier pipe and avoid copying the
    // reconstructed TU through a second one-megabyte staging buffer.
    size_t index = 0;
    size_t skip = 0;
    while (index < ordered.size()) {
        while (index < ordered.size() && skip == ordered[index].len) {
            ++index;
            skip = 0;
        }
        if (index == ordered.size()) break;
        std::array<struct iovec, 512> iov{};
        size_t count = 0;
        for (size_t j = index; j < ordered.size() && count < iov.size(); ++j) {
            size_t local_skip = (j == index) ? skip : 0;
            if (local_skip == ordered[j].len) continue;
            iov[count].iov_base = const_cast<char*>(store.region_bytes(ordered[j]) + local_skip);
            iov[count].iov_len = ordered[j].len - local_skip;
            ++count;
        }
        if (!count) break;
        ssize_t n = ::writev(out_fd, iov.data(), int(count));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        size_t consumed = size_t(n);
        while (index < ordered.size() && consumed) {
            size_t available = ordered[index].len - skip;
            if (consumed < available) {
                skip += consumed;
                consumed = 0;
            } else {
                consumed -= available;
                ++index;
                skip = 0;
            }
        }
    }
    if (index != ordered.size()) return false;
    emitted += total;
    return true;
}

static int receiver_main(int fd, int out_fd, int zlevel) {
    FramedChannel channel(fd, zlevel);
    Frame frame;
    if (!channel.recv(frame) || frame.type != Msg::Hello) return 21;
    Cursor hello(frame.payload);
    uint32_t protocol = 0;
    uint64_t guid = 0;
    if (!hello.u32(protocol) || protocol != 50 || !hello.u64(guid) || !hello.done()) return 22;
    (void)guid;

    ReceiverStore store;
    uint64_t emitted = 0;
    uint64_t decode_ns = 0;
    uint64_t expand_ns = 0;

    for (;;) {
        if (!channel.recv(frame)) return 23;
        if (frame.type == Msg::Done) break;
        if (frame.type == Msg::RootRef) {
            auto d0 = Clock::now();
            Transaction tx;
            tx.root_ref = true;
            Cursor ref(frame.payload);
            if (!ref.u32(tx.id) || !ref.u64(tx.raw_len) || !ref.u64(tx.root_key) ||
                !tx.root_key || !ref.done() || !store.has_root(tx.root_key)) return 24;
            decode_ns += elapsed_ns(d0);

            auto e0 = Clock::now();
            if (!emit_transaction(tx, store, out_fd, emitted)) return 30;
            expand_ns += elapsed_ns(e0);

            std::vector<uint8_t> ack;
            put_u32(ack, tx.id);
            put_u64(ack, tx.raw_len);
            if (!channel.send(Msg::Ack, ack, false)) return 31;
            continue;
        }
        if (frame.type != Msg::Dict) return 24;

        auto d0 = Clock::now();
        Transaction tx;
        if (!decode_dict(frame.payload, tx)) return 25;
        for (uint64_t key : tx.region_keys) if (!store.has_region(key)) tx.missing_regions.push_back(key);
        for (uint64_t key : tx.block_keys) if (!store.has_block(key)) tx.missing_blocks.push_back(key);

        decode_ns += elapsed_ns(d0);

        if (!channel.recv(frame) || frame.type != Msg::Root || !decode_root(frame.payload, tx)) return 27;
        // C's per-conversation sticky-F view drives the normal proactive FILL. The FILL
        // carries explicit object keys, so F installs what actually arrived and validates
        // the complete closure below.
        if (!channel.recv(frame) || frame.type != Msg::Fill) return 28;
        d0 = Clock::now();
        if (!install_region_fill(frame.payload, tx, store)) return 29;

        std::vector<uint8_t> missing;
        if (!tx.missing_blocks.empty()) {
                for (uint64_t block_key : tx.block_keys) {
                    const ByteSpan* block = store.block(block_key);
                    if (!block) return 29;
                    const uint64_t* children = store.block_children(*block);
                    for (uint32_t i = 0; i < block->len; ++i)
                        if (!store.has_region(children[i])) tx.closure_regions.push_back(children[i]);
                }
                std::sort(tx.closure_regions.begin(), tx.closure_regions.end());
                tx.closure_regions.erase(std::unique(tx.closure_regions.begin(), tx.closure_regions.end()),
                                         tx.closure_regions.end());
                if (!tx.closure_regions.empty()) {
                    missing.clear();
                    put_u32(missing, tx.id);
                    put_uvar(missing, tx.closure_regions.size());
                    for (uint64_t key : tx.closure_regions) put_u64(missing, key);
                    if (!channel.send(Msg::MissingRegions, missing)) return 29;
                    if (!channel.recv(frame) || frame.type != Msg::FillRegions ||
                        !install_closure_region_fill(frame.payload, tx, store)) return 29;
                }
            }

        auto gather_missing_lines = [&](uint64_t region_key) -> bool {
                const ByteSpan* region = store.region(region_key);
                if (!region) return false;
                const uint64_t* children = store.region_children(*region);
                for (uint32_t i = 0; i < region->len; ++i)
                    if (!store.has_line(children[i])) tx.missing_lines.push_back(children[i]);
                return true;
            };
        for (uint64_t region_key : tx.missing_regions)
            if (!gather_missing_lines(region_key)) return 29;
        for (uint64_t region_key : tx.closure_regions)
            if (!gather_missing_lines(region_key)) return 29;
        std::sort(tx.missing_lines.begin(), tx.missing_lines.end());
        tx.missing_lines.erase(std::unique(tx.missing_lines.begin(), tx.missing_lines.end()),
                               tx.missing_lines.end());
        decode_ns += elapsed_ns(d0);

        // This frame is also the early READY signal when the count is zero. It lets C
        // start the next TU while F expands and emits this one; the later ACK verifies it.
        put_u32(missing, tx.id);
        put_uvar(missing, tx.missing_lines.size());
        for (uint64_t key : tx.missing_lines) put_u64(missing, key);
        if (!channel.send(Msg::MissingLines, missing)) return 29;
        if (!tx.missing_lines.empty()) {
            if (!channel.recv(frame) || frame.type != Msg::FillLines) return 29;
            d0 = Clock::now();
            if (!install_line_fill(frame.payload, tx, store)) return 29;
            decode_ns += elapsed_ns(d0);
        }

        auto e0 = Clock::now();
        if (!emit_transaction(tx, store, out_fd, emitted)) return 30;
        expand_ns += elapsed_ns(e0);

        std::vector<uint8_t> ack;
        put_u32(ack, tx.id);
        put_u64(ack, tx.raw_len);
        if (!channel.send(Msg::Ack, ack, false)) return 31;
    }

    struct rusage ru {};
    getrusage(RUSAGE_SELF, &ru);
    std::vector<uint8_t> final;
    put_u64(final, emitted);
    put_u64(final, decode_ns + channel.decompress_ns());
    put_u64(final, expand_ns);
    put_u64(final, store.retained_bytes());
    put_u64(final, uint64_t(ru.ru_maxrss) * 1024);
    if (!channel.send(Msg::Final, final, false)) return 32;
    ::close(out_fd);
    return 0;
}

class DenseIndex {
public:
    void begin(size_t size) {
        if (stamp_.size() < size) {
            stamp_.resize(size);
            value_.resize(size);
        }
        if (++epoch_ == 0) {
            std::fill(stamp_.begin(), stamp_.end(), 0);
            epoch_ = 1;
        }
    }

    bool add(uint32_t key, uint32_t local) {
        if (key >= stamp_.size()) return false;
        if (stamp_[key] == epoch_) return false;
        stamp_[key] = epoch_;
        value_[key] = local;
        return true;
    }

    uint32_t get(uint32_t key) const {
        return key < stamp_.size() && stamp_[key] == epoch_ ? value_[key] : UINT32_MAX;
    }

private:
    std::vector<uint32_t> stamp_;
    std::vector<uint32_t> value_;
    uint32_t epoch_ = 0;
};

static uint64_t bytes_hash(const char* p, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    while (n >= 8) {
        uint64_t v = 0;
        memcpy(&v, p, 8);
        h = mix64(h ^ v);
        p += 8;
        n -= 8;
    }
    uint64_t tail = 0;
    if (n) memcpy(&tail, p, n);
    return mix64(h ^ tail ^ uint64_t(n));
}

static uint64_t skeleton_hash(const char* p, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    uint8_t previous_class = 255;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = static_cast<unsigned char>(p[i]);
        uint8_t cls;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') cls = 1;
        else if (c >= '0' && c <= '9') cls = 2;
        else if (c == ' ' || c == '\t') cls = 3;
        else cls = 4;
        if (cls != previous_class || cls == 4) {
            h ^= cls == 4 ? uint64_t(c) + 256 : cls;
            h *= 1099511628211ULL;
            previous_class = cls;
        }
    }
    return mix64(h ^ n);
}

static uint64_t edge_hash(const char* p, size_t n) {
    size_t a = std::min<size_t>(8, n);
    size_t b = std::min<size_t>(8, n - a);
    uint64_t lo = 0, hi = 0;
    if (a) memcpy(&lo, p, a);
    if (b) memcpy(&hi, p + n - b, b);
    return mix64(lo ^ (hi << 1) ^ (uint64_t(n / 16) << 48));
}

struct CandidateInfo {
    std::array<uint32_t, 4> ids{};
    uint8_t count = 0;
    uint64_t location = 0;
    uint64_t skeleton = 0;
    uint64_t edge = 0;
    bool seen = false;

    void add(uint32_t id) {
        if (!id) return;
        for (uint8_t i = 0; i < count; ++i) if (ids[i] == id) return;
        if (count < ids.size()) ids[count++] = id;
    }
};

class DefinitionPredictor {
public:
    std::vector<CandidateInfo> analyze(const char* begin, const char* end,
                                       const uint32_t* line_ids, size_t count,
                                       uint32_t first_new, uint32_t last_new) const {
        size_t new_count = last_new >= first_new ? size_t(last_new - first_new + 1) : 0;
        std::vector<CandidateInfo> result(new_count);
        const char* p = begin;
        size_t occurrence = 0;
        uint64_t path_hash = 0;
        uint64_t logical_line = 0;
        Marker marker;

        while (p < end && occurrence < count) {
            const void* hit = memchr(p, '\n', size_t(end - p));
            const char* q = hit ? static_cast<const char*>(hit) + 1 : end;
            uint32_t id = line_ids[occurrence++];
            bool is_new = new_count && id >= first_new && id <= last_new;
            if (is_new) {
                CandidateInfo& info = result[size_t(id - first_new)];
                if (!info.seen) {
                    info.seen = true;
                    info.location = mix64(path_hash ^ (logical_line * 0x9e3779b97f4a7c15ULL));
                    info.skeleton = skeleton_hash(p, size_t(q - p));
                    info.edge = edge_hash(p, size_t(q - p));
                    auto add_from = [&](const std::unordered_map<uint64_t, uint32_t>& map, uint64_t key) {
                        auto it = map.find(key);
                        if (it != map.end()) info.add(it->second);
                    };
                    add_from(by_location_, info.location);
                    add_from(by_skeleton_, info.skeleton);
                    add_from(by_edge_, info.edge);
                    info.add(recent_);
                }
            }

            if (parse_marker(p, uint32_t(q - p), marker)) {
                path_hash = bytes_hash(marker.path.data(), marker.path.size());
                logical_line = marker.lineno;
            } else {
                ++logical_line;
            }
            p = q;
        }
        if (occurrence != count || p != end) result.clear();
        return result;
    }

    void commit(uint32_t first_new, const std::vector<CandidateInfo>& infos) {
        for (size_t i = 0; i < infos.size(); ++i) {
            const CandidateInfo& info = infos[i];
            if (!info.seen) continue;
            uint32_t id = first_new + uint32_t(i);
            by_location_[info.location] = id;
            by_skeleton_[info.skeleton] = id;
            by_edge_[info.edge] = id;
            recent_ = id;
        }
    }

private:
    std::unordered_map<uint64_t, uint32_t> by_location_;
    std::unordered_map<uint64_t, uint32_t> by_skeleton_;
    std::unordered_map<uint64_t, uint32_t> by_edge_;
    uint32_t recent_ = 0;
};

struct DefinitionStats {
    uint64_t literals = 0;
    uint64_t deltas = 0;
    uint64_t literal_bytes = 0;
    uint64_t delta_middle_bytes = 0;
    uint64_t copied_bytes = 0;
};

static void encode_definition(std::vector<uint8_t>& out, uint32_t line_id,
                              const Interner& dict, const CandidateInfo* candidates,
                              const std::vector<uint8_t>& receiver_known, bool use_delta,
                              DefinitionStats& stats) {
    const LineRef& ref = dict.ref(line_id);
    const char* text = dict.line_data(ref.off);
    size_t literal_size = 1 + uvar_size(ref.len) + ref.len;
    uint32_t best_base = 0;
    uint32_t best_prefix = 0;
    uint32_t best_suffix = 0;
    size_t best_size = literal_size;

    if (use_delta && candidates) {
        for (uint8_t ci = 0; ci < candidates->count; ++ci) {
            uint32_t base_id = candidates->ids[ci];
            if (!base_id || base_id >= receiver_known.size() || !receiver_known[base_id]) continue;
            const LineRef& base = dict.ref(base_id);
            const char* b = dict.line_data(base.off);
            uint32_t prefix = 0;
            uint32_t max_common = std::min(ref.len, base.len);
            while (prefix < max_common && text[prefix] == b[prefix]) ++prefix;
            uint32_t suffix = 0;
            while (suffix < max_common - prefix &&
                   text[ref.len - suffix - 1] == b[base.len - suffix - 1]) ++suffix;
            uint32_t middle = ref.len - prefix - suffix;
            size_t candidate_size = 1 + sizeof(uint64_t) + uvar_size(ref.len) +
                                    uvar_size(prefix) + uvar_size(suffix) +
                                    uvar_size(middle) + middle;
            if (candidate_size < best_size) {
                best_size = candidate_size;
                best_base = base_id;
                best_prefix = prefix;
                best_suffix = suffix;
            }
        }
    }

    if (!best_base) {
        out.push_back(0);
        put_uvar(out, ref.len);
        out.insert(out.end(), text, text + ref.len);
        ++stats.literals;
        stats.literal_bytes += ref.len;
        return;
    }

    uint32_t middle = ref.len - best_prefix - best_suffix;
    out.push_back(1);
    put_u64(out, best_base);
    put_uvar(out, ref.len);
    put_uvar(out, best_prefix);
    put_uvar(out, best_suffix);
    put_uvar(out, middle);
    out.insert(out.end(), text + best_prefix, text + best_prefix + middle);
    ++stats.deltas;
    stats.delta_middle_bytes += middle;
    stats.copied_bytes += best_prefix + best_suffix;
}

struct VerifyResult {
    bool exact = true;
    uint64_t bytes = 0;
};

struct EncodedToken {
    bool block = false;
    uint32_t id = 0;
};

class OnlineBlocks {
public:
    explicit OnlineBlocks(bool enabled) : enabled_(enabled), head_(size_t(1) << HASH_BITS, UINT32_MAX) {
        block_off_.push_back(0);
    }

    std::vector<EncodedToken> encode(const std::vector<uint32_t>& regions) {
        std::vector<EncodedToken> root;
        root.reserve(regions.size());
        if (!enabled_) {
            for (uint32_t region : regions) root.push_back({false, region});
            learn(regions);
            return root;
        }

        size_t i = 0;
        while (i < regions.size()) {
            size_t best = 0;
            if (i + MIN_MATCH <= regions.size() && history_.size() >= MIN_MATCH) {
                uint32_t candidate = head_[kgram(regions.data() + i)];
                unsigned chain = 0;
                while (candidate != UINT32_MAX && chain++ < MAX_CHAIN) {
                    size_t limit = std::min(regions.size() - i, history_.size() - candidate);
                    size_t length = 0;
                    while (length < limit && history_[candidate + length] == regions[i + length]) ++length;
                    if (length > best) best = length;
                    if (best == regions.size() - i) break;
                    candidate = previous_[candidate];
                }
            }

            if (best >= MIN_MATCH) {
                root.push_back({true, get_block(regions.data() + i, best)});
                i += best;
            } else {
                root.push_back({false, regions[i++]});
            }
        }
        learn(regions);
        return root;
    }

    size_t block_count() const { return block_off_.size() - 1; }
    const uint32_t* block_children(uint32_t id) const { return block_children_.data() + block_off_[id]; }
    uint32_t block_size(uint32_t id) const { return uint32_t(block_off_[id + 1] - block_off_[id]); }

private:
    static constexpr unsigned HASH_BITS = 20;
    static constexpr unsigned MIN_MATCH = 3;
    static constexpr unsigned MAX_CHAIN = 16;

    static uint32_t kgram(const uint32_t* p) {
        uint64_t h = uint64_t(p[0]) * 0x9e3779b185ebca87ULL;
        h ^= uint64_t(p[1]) * 0xc2b2ae3d27d4eb4fULL;
        h ^= uint64_t(p[2]) * 0x165667b19e3779f9ULL;
        return uint32_t(mix64(h) >> (64 - HASH_BITS));
    }

    static uint64_t span_hash(const uint32_t* p, size_t n) {
        uint64_t h = 1469598103934665603ULL ^ (uint64_t(n) * 0x9e3779b97f4a7c15ULL);
        for (size_t i = 0; i < n; ++i) {
            h ^= p[i];
            h *= 1099511628211ULL;
        }
        return h;
    }

    uint32_t get_block(const uint32_t* p, size_t n) {
        uint64_t hash = span_hash(p, n);
        auto range = blocks_by_hash_.equal_range(hash);
        for (auto it = range.first; it != range.second; ++it) {
            uint32_t id = it->second;
            if (block_size(id) == n &&
                memcmp(block_children(id), p, n * sizeof(uint32_t)) == 0) return id;
        }
        uint32_t id = uint32_t(block_count());
        block_children_.insert(block_children_.end(), p, p + n);
        block_off_.push_back(block_children_.size());
        blocks_by_hash_.emplace(hash, id);
        return id;
    }

    void learn(const std::vector<uint32_t>& regions) {
        if (regions.empty()) return;
        if (history_.size() + regions.size() > UINT32_MAX) {
            fprintf(stderr, "region history exceeds u32 position space\n");
            exit(2);
        }
        size_t base = history_.size();
        history_.insert(history_.end(), regions.begin(), regions.end());
        previous_.resize(history_.size(), UINT32_MAX);
        for (size_t i = base; i + MIN_MATCH <= history_.size(); ++i) {
            uint32_t bucket = kgram(history_.data() + i);
            previous_[i] = head_[bucket];
            head_[bucket] = uint32_t(i);
        }
    }

    bool enabled_;
    std::vector<uint32_t> head_;
    std::vector<uint32_t> previous_;
    std::vector<uint32_t> history_;
    std::vector<uint32_t> block_children_;
    std::vector<size_t> block_off_;
    std::unordered_multimap<uint64_t, uint32_t> blocks_by_hash_;
};

// Semantic roots are keyed by their complete Region sequence, not by the current
// Block covering.  A root published after TU t can therefore be reused by any
// later identical TU even if the online Block learner has since changed its parse.
class SemanticRoots {
public:
    uint32_t find(const std::vector<uint32_t>& regions) const {
        uint64_t hash = span_hash(regions.data(), regions.size());
        auto range = by_hash_.equal_range(hash);
        for (auto it = range.first; it != range.second; ++it) {
            uint32_t id = it->second;
            if (size(id) == regions.size() &&
                (!regions.size() || memcmp(children(id), regions.data(),
                                            regions.size() * sizeof(uint32_t)) == 0)) return id;
        }
        return 0;
    }

    uint32_t publish(const std::vector<uint32_t>& regions) {
        uint32_t existing = find(regions);
        if (existing) return existing;
        if (offsets_.size() > UINT32_MAX) {
            fprintf(stderr, "semantic root id space exhausted\n");
            exit(2);
        }
        uint32_t id = uint32_t(offsets_.size()); // one-based: offsets_[id-1..id]
        data_.insert(data_.end(), regions.begin(), regions.end());
        offsets_.push_back(data_.size());
        by_hash_.emplace(span_hash(regions.data(), regions.size()), id);
        return id;
    }

    size_t count() const { return offsets_.size() - 1; }
    size_t retained_bytes() const {
        return data_.capacity() * sizeof(uint32_t) + offsets_.capacity() * sizeof(size_t) +
               by_hash_.size() * (sizeof(uint64_t) + sizeof(uint32_t) + 2 * sizeof(void*));
    }

private:
    static uint64_t span_hash(const uint32_t* p, size_t n) {
        uint64_t h = 1469598103934665603ULL ^ (uint64_t(n) * 0x9e3779b97f4a7c15ULL);
        for (size_t i = 0; i < n; ++i) {
            h ^= p[i];
            h *= 1099511628211ULL;
        }
        return mix64(h);
    }

    size_t size(uint32_t id) const { return offsets_[id] - offsets_[id - 1]; }
    const uint32_t* children(uint32_t id) const { return data_.data() + offsets_[id - 1]; }

    std::vector<uint32_t> data_;
    std::vector<size_t> offsets_{0};
    std::unordered_multimap<uint64_t, uint32_t> by_hash_;
};

static void verify_output(int fd, const Corpus* corpus, const std::vector<FileSpan>* schedule,
                          VerifyResult* result) {
    std::vector<char> buffer(1u << 20);
    uint64_t total_expected = 0;
    for (const FileSpan& span : *schedule) total_expected += span.len;
    uint64_t offset = 0;
    size_t span_index = 0;
    size_t span_offset = 0;
    for (;;) {
        ssize_t n = ::read(fd, buffer.data(), buffer.size());
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { result->exact = false; break; }
        if (n == 0) break;
        size_t consumed = 0;
        while (consumed < size_t(n)) {
            while (span_index < schedule->size() && span_offset == (*schedule)[span_index].len) {
                ++span_index;
                span_offset = 0;
            }
            if (span_index == schedule->size()) {
                result->exact = false;
                consumed = size_t(n);
                break;
            }
            const FileSpan& span = (*schedule)[span_index];
            size_t chunk = std::min<size_t>(size_t(n) - consumed, size_t(span.len) - span_offset);
            if (memcmp(buffer.data() + consumed,
                       corpus->bytes.data() + span.off + span_offset, chunk) != 0)
                result->exact = false;
            consumed += chunk;
            span_offset += chunk;
            offset += chunk;
        }
    }
    result->bytes = offset;
    if (offset != total_expected) result->exact = false;
    ::close(fd);
}

struct Options {
    const char* manifest = nullptr;
    size_t max_files = SIZE_MAX;
    int zlevel = 1;
    unsigned passes = 1;
    bool delta = false;
    bool blocks = true;
    bool root_memo = true;
    bool edit_cycle = false;
    int order_mode = 0; // 0=original, 1=reverse after cold, 2=seeded shuffles, 3=mixed
    bool pipeline = true;
    uint32_t pipeline_window = 8;
    bool receiver = false;
    int fd = -1;
    int out_fd = -1;
};

static bool parse_options(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--manifest") && i + 1 < argc) o.manifest = argv[++i];
        else if (!strcmp(argv[i], "--max-files") && i + 1 < argc) o.max_files = strtoull(argv[++i], nullptr, 10);
        else if (!strcmp(argv[i], "--z") && i + 1 < argc) o.zlevel = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--passes") && i + 1 < argc) o.passes = unsigned(atoi(argv[++i]));
        else if (!strcmp(argv[i], "--delta")) o.delta = true;
        else if (!strcmp(argv[i], "--no-delta")) o.delta = false;
        else if (!strcmp(argv[i], "--no-blocks")) o.blocks = false;
        else if (!strcmp(argv[i], "--no-root-memo")) o.root_memo = false;
        else if (!strcmp(argv[i], "--edit-cycle")) o.edit_cycle = true;
        else if (!strcmp(argv[i], "--order") && i + 1 < argc) {
            const char* mode = argv[++i];
            if (!strcmp(mode, "original")) o.order_mode = 0;
            else if (!strcmp(mode, "reverse")) o.order_mode = 1;
            else if (!strcmp(mode, "shuffle")) o.order_mode = 2;
            else if (!strcmp(mode, "mixed")) o.order_mode = 3;
            else return false;
        }
        else if (!strcmp(argv[i], "--sync")) o.pipeline = false;
        else if (!strcmp(argv[i], "--window") && i + 1 < argc)
            o.pipeline_window = uint32_t(strtoul(argv[++i], nullptr, 10));
        else if (!strcmp(argv[i], "--receiver")) o.receiver = true;
        else if (!strcmp(argv[i], "--fd") && i + 1 < argc) o.fd = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--out-fd") && i + 1 < argc) o.out_fd = atoi(argv[++i]);
        else return false;
    }
    if (o.zlevel != 0 && o.zlevel != 1 && o.zlevel != 3) return false;
    if (!o.passes || !o.pipeline_window) return false;
    return o.receiver ? o.fd >= 0 && o.out_fd >= 0 : o.manifest != nullptr;
}

static std::vector<FileSpan> ordered_files(const std::vector<FileSpan>& input,
                                           unsigned pass, int mode) {
    std::vector<FileSpan> files = input;
    if (pass == 0 || mode == 0) return files;
    if (mode == 1 || (mode == 3 && pass == 1)) {
        std::reverse(files.begin(), files.end());
        return files;
    }
    uint64_t state = mix64(0x243f6a8885a308d3ULL ^ uint64_t(pass) * 0x9e3779b97f4a7c15ULL);
    for (size_t i = files.size(); i > 1; --i) {
        state = mix64(state + i * 0x100000001b3ULL);
        std::swap(files[i - 1], files[size_t(state % i)]);
    }
    return files;
}

static bool drain_pipeline_reply(FramedChannel& channel, uint32_t expected_id,
                                 uint64_t expected_len) {
    Frame frame;
    if (!channel.recv(frame) || frame.type != Msg::MissingLines) return false;
    Cursor ready(frame.payload);
    uint32_t ready_id = 0;
    uint64_t count = 0;
    if (!ready.u32(ready_id) || ready_id != expected_id || !ready.uvar(count) ||
        count != 0 || !ready.done()) return false;
    if (!channel.recv(frame) || frame.type != Msg::Ack) return false;
    Cursor ack(frame.payload);
    uint32_t ack_id = 0;
    uint64_t ack_len = 0;
    return ack.u32(ack_id) && ack_id == expected_id && ack.u64(ack_len) &&
           ack_len == expected_len && ack.done();
}

static bool drain_transaction_reply(FramedChannel& channel, uint32_t expected_id,
                                    uint64_t expected_len, bool root_ref) {
    if (!root_ref) return drain_pipeline_reply(channel, expected_id, expected_len);
    Frame frame;
    if (!channel.recv(frame) || frame.type != Msg::Ack) return false;
    Cursor ack(frame.payload);
    uint32_t ack_id = 0;
    uint64_t ack_len = 0;
    return ack.u32(ack_id) && ack_id == expected_id && ack.u64(ack_len) &&
           ack_len == expected_len && ack.done();
}

static int sender_main(const Options& options) {
    signal(SIGPIPE, SIG_IGN);
    int sockets[2] = {-1, -1};
    int output[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0 || pipe(output) != 0) {
        perror("socketpair/pipe");
        return 2;
    }
#ifdef F_SETPIPE_SZ
    (void)fcntl(output[0], F_SETPIPE_SZ, 1 << 20);
#endif

    pid_t child = fork();
    if (child < 0) { perror("fork"); return 2; }
    if (child == 0) {
        ::close(sockets[0]);
        ::close(output[0]);
        char fd_arg[32], out_arg[32], z_arg[16];
        snprintf(fd_arg, sizeof(fd_arg), "%d", sockets[1]);
        snprintf(out_arg, sizeof(out_arg), "%d", output[1]);
        snprintf(z_arg, sizeof(z_arg), "%d", options.zlevel);
        char* const args[] = {
            const_cast<char*>("codec50"), const_cast<char*>("--receiver"),
            const_cast<char*>("--fd"), fd_arg,
            const_cast<char*>("--out-fd"), out_arg,
            const_cast<char*>("--z"), z_arg, nullptr
        };
        execv("/proc/self/exe", args);
        _exit(127);
    }
    ::close(sockets[1]);
    ::close(output[1]);

    auto total_start = Clock::now();
    auto load_start = Clock::now();
    Corpus corpus = load_corpus(options.manifest, options.max_files);
    uint64_t load_ns = elapsed_ns(load_start);

    uint64_t edited_tus = 0;
    uint64_t edit_fixture_ns = 0;
    std::vector<FileSpan> edited_files;
    if (options.edit_cycle) {
        auto e0 = Clock::now();
        edited_files = append_header_edit(corpus, edited_tus);
        edit_fixture_ns = elapsed_ns(e0);
    }

    std::vector<FileSpan> schedule;
    std::vector<uint64_t> pass_raw;
    schedule.reserve(corpus.files.size() * size_t(options.passes));
    for (unsigned pass = 0; pass < options.passes; ++pass) {
        bool edited = options.edit_cycle && (pass == 2 || pass == 3);
        const std::vector<FileSpan>& source = edited ? edited_files : corpus.files;
        std::vector<FileSpan> ordered = ordered_files(source, pass, options.order_mode);
        uint64_t raw_bytes = 0;
        for (const FileSpan& span : ordered) raw_bytes += span.len;
        pass_raw.push_back(raw_bytes);
        schedule.insert(schedule.end(), ordered.begin(), ordered.end());
    }

    VerifyResult verification;
    std::thread verifier(verify_output, output[0], &corpus, &schedule, &verification);
    FramedChannel channel(sockets[0], options.zlevel);

    uint64_t guid = mix64(uint64_t(getpid()) ^ uint64_t(total_start.time_since_epoch().count()));
    std::vector<uint8_t> payload;
    put_u32(payload, 50);
    put_u64(payload, guid);
    if (!channel.send(Msg::Hello, payload, false)) return 3;

    Interner dict;
    OnlineBlocks blocks(options.blocks);
    SemanticRoots semantic_roots;
    DefinitionPredictor predictor;
    DefinitionStats definition_stats;
    DenseIndex region_index, block_index, eager_index;
    std::vector<uint8_t> receiver_known(1, 0);
    std::vector<uint8_t> receiver_known_regions(1, 0);
    std::vector<uint8_t> receiver_known_blocks(1, 0);
    uint32_t max_len = 0;
    for (const FileSpan& f : schedule) max_len = std::max(max_len, f.len);
    std::vector<uint32_t> line_occurrences(size_t(max_len) + 1);
    std::vector<uint32_t> regions;
    std::vector<uint64_t> region_keys, block_keys, missing_lines, missing_regions, missing_blocks,
                          closure_regions, eager_regions, eager_lines;
    std::vector<uint64_t> transaction_lengths;
    std::vector<uint8_t> transaction_root_refs;
    std::vector<EncodedToken> root_tokens;
    std::vector<CandidateInfo> candidates;
    uint64_t intern_ns = 0, plan_ns = 0, fill_ns = 0, wait_ns = 0;
    uint64_t region_hits = 0;
    uint32_t txid = 0;
    uint32_t replies_drained = 0;
    uint32_t pending_ack = 0;
    uint64_t pending_ack_len = 0;
    uint64_t root_defs = 0;
    uint64_t root_refs = 0;
    struct PassRow {
        uint64_t raw = 0;
        uint64_t wire = 0;
        uint64_t defs = 0;
        uint64_t refs = 0;
    };
    std::vector<PassRow> pass_rows;
    bool ok = true;

    for (unsigned pass = 0; pass < options.passes && ok; ++pass) {
        uint64_t pass_wire_before = channel.sent() + channel.received();
        uint64_t pass_defs_before = root_defs;
        uint64_t pass_refs_before = root_refs;
        size_t pass_begin = size_t(pass) * corpus.files.size();
        size_t pass_end = pass_begin + corpus.files.size();
        for (size_t scheduled = pass_begin; scheduled < pass_end; ++scheduled) {
            const FileSpan& file = schedule[scheduled];
            ++txid;
            transaction_lengths.push_back(file.len);
            transaction_root_refs.push_back(0);
            const char* begin = corpus.bytes.data() + file.off;
            const char* end = begin + file.len;
            uint32_t before_lines = dict.distinct();

            auto i0 = Clock::now();
            size_t occurrence_count = 0;
            regions.clear();
            dict.process(begin, end, line_occurrences.data(), occurrence_count,
                         region_hits, pass == 0, &regions);
            intern_ns += elapsed_ns(i0);
            uint32_t after_lines = dict.distinct();
            if (dict.region_count() + 1 > receiver_known_regions.size())
                receiver_known_regions.resize(size_t(dict.region_count()) + 1, 0);

            auto p0 = Clock::now();
            // Score the current TU against roots published by prior TUs only.  Publication
            // happens below, after this choice, so a TU can never memoize itself.
            uint32_t root_key = options.root_memo ? semantic_roots.find(regions) : 0;
            bool root_ref = root_key != 0;
            root_tokens = blocks.encode(regions);
            if (options.root_memo && !root_ref) {
                root_key = semantic_roots.publish(regions);
                ++root_defs;
            }

            if (root_ref) {
                transaction_root_refs.back() = 1;
                payload.clear();
                put_u32(payload, txid);
                put_u64(payload, file.len);
                put_u64(payload, root_key);
                if (!channel.send(Msg::RootRef, payload)) { ok = false; break; }
                ++root_refs;
                plan_ns += elapsed_ns(p0);

                if (options.pipeline) {
                    if (txid - replies_drained >= options.pipeline_window) {
                        auto w0 = Clock::now();
                        uint32_t expected_id = replies_drained + 1;
                        ok = drain_transaction_reply(channel, expected_id,
                                                     transaction_lengths[size_t(expected_id - 1)],
                                                     transaction_root_refs[size_t(expected_id - 1)] != 0);
                        wait_ns += elapsed_ns(w0);
                        if (!ok) break;
                        ++replies_drained;
                    }
                } else {
                    // A full-root transaction leaves its final ACK pending to overlap the next
                    // encode.  Consume it before the standalone RootRef exchange.
                    if (pending_ack) {
                        Frame prior;
                        auto w0 = Clock::now();
                        if (!channel.recv(prior) || prior.type != Msg::Ack) { ok = false; break; }
                        wait_ns += elapsed_ns(w0);
                        Cursor ack(prior.payload);
                        uint32_t ack_id = 0;
                        uint64_t ack_len = 0;
                        if (!ack.u32(ack_id) || ack_id != pending_ack || !ack.u64(ack_len) ||
                            ack_len != pending_ack_len || !ack.done()) { ok = false; break; }
                        pending_ack = 0;
                    }
                    auto w0 = Clock::now();
                    ok = drain_transaction_reply(channel, txid, file.len, true);
                    wait_ns += elapsed_ns(w0);
                    if (!ok) break;
                }
                continue;
            }
            if (after_lines + 1 > receiver_known.size()) receiver_known.resize(size_t(after_lines) + 1, 0);
            if (blocks.block_count() + 1 > receiver_known_blocks.size())
                receiver_known_blocks.resize(blocks.block_count() + 1, 0);
            uint32_t first_new = before_lines + 1;
            if (options.delta)
                candidates = predictor.analyze(begin, end, line_occurrences.data(), occurrence_count,
                                               first_new, after_lines);
            else
                candidates.clear();

            region_index.begin(size_t(dict.region_count()));
            region_keys.clear();
            for (const EncodedToken& token : root_tokens) {
                if (!token.block && region_index.add(token.id, uint32_t(region_keys.size())))
                    region_keys.push_back(uint64_t(token.id) + 1);
            }
            block_index.begin(blocks.block_count());
            block_keys.clear();
            for (const EncodedToken& token : root_tokens) {
                if (token.block && block_index.add(token.id, uint32_t(block_keys.size())))
                    block_keys.push_back(uint64_t(token.id) + 1);
            }

            missing_regions.clear();
            for (uint64_t key : region_keys)
                if (!receiver_known_regions[size_t(key)]) missing_regions.push_back(key);
            missing_blocks.clear();
            for (uint64_t key : block_keys)
                if (!receiver_known_blocks[size_t(key)]) missing_blocks.push_back(key);

            payload.clear();
            put_u32(payload, txid);
            put_u64(payload, file.len);
            put_uvar(payload, region_keys.size());
            for (uint64_t key : region_keys) put_u64(payload, key);
            put_uvar(payload, block_keys.size());
            for (uint64_t key : block_keys) put_u64(payload, key);
            if (!channel.send(Msg::Dict, payload)) { ok = false; break; }

            payload.clear();
            put_u32(payload, txid);
            payload.push_back(0); // full semantic-root definition, not a RootRef
            put_u64(payload, root_key);
            put_uvar(payload, root_tokens.size());
            for (const EncodedToken& token : root_tokens) {
                uint32_t local = token.block ? block_index.get(token.id) : region_index.get(token.id);
                put_uvar(payload, (uint64_t(local) << 1) | (token.block ? 1 : 0));
            }
            if (!channel.send(Msg::Root, payload)) { ok = false; break; }
            plan_ns += elapsed_ns(p0);

            Frame frame;
            uint32_t reply_id = 0;
            uint64_t count = 0;
            auto w0 = Clock::now();

            missing_lines.clear();
            bool current_acked = false;
            {
                auto f0 = Clock::now();
                eager_index.begin(size_t(dict.region_count()) + 1);
                eager_regions.clear();
                for (uint64_t key : missing_regions) eager_index.add(uint32_t(key), 0);
                for (uint64_t block_key : missing_blocks) {
                    if (!block_key || block_key - 1 >= blocks.block_count()) { ok = false; break; }
                    uint32_t block_id = uint32_t(block_key - 1);
                    const uint32_t* children = blocks.block_children(block_id);
                    uint32_t child_count = blocks.block_size(block_id);
                    for (uint32_t j = 0; j < child_count; ++j) {
                        uint64_t key = uint64_t(children[j]) + 1;
                        if (!receiver_known_regions[size_t(key)] &&
                            eager_index.add(uint32_t(key), uint32_t(eager_regions.size())))
                            eager_regions.push_back(key);
                    }
                }
                if (!ok) break;

                eager_lines.clear();
                auto gather_eager_lines = [&](uint64_t region_key) -> bool {
                    if (!region_key || region_key - 1 >= dict.region_count()) return false;
                    uint32_t region_id = uint32_t(region_key - 1);
                    uint32_t child_count = dict.region_ids_count(region_id);
                    const uint32_t* children = dict.region_ids_ptr(region_id);
                    for (uint32_t j = 0; j < child_count; ++j) {
                        uint64_t line_key = children[j];
                        if (!line_key || line_key >= receiver_known.size()) return false;
                        if (!receiver_known[size_t(line_key)]) eager_lines.push_back(line_key);
                    }
                    return true;
                };
                for (uint64_t key : missing_regions)
                    if (!gather_eager_lines(key)) { ok = false; break; }
                for (uint64_t key : eager_regions)
                    if (!gather_eager_lines(key)) { ok = false; break; }
                if (!ok) break;
                std::sort(eager_lines.begin(), eager_lines.end());
                eager_lines.erase(std::unique(eager_lines.begin(), eager_lines.end()), eager_lines.end());

                payload.clear();
                put_u32(payload, txid);
                put_uvar(payload, missing_regions.size());
                for (uint64_t key : missing_regions) {
                    if (!key || key - 1 >= dict.region_count()) { ok = false; break; }
                    uint32_t region_id = uint32_t(key - 1);
                    uint32_t child_count = dict.region_ids_count(region_id);
                    const uint32_t* children = dict.region_ids_ptr(region_id);
                    put_u64(payload, key);
                    put_uvar(payload, child_count);
                    for (uint32_t j = 0; j < child_count; ++j) put_u64(payload, children[j]);
                    if (!ok) break;
                }
                put_uvar(payload, missing_blocks.size());
                for (uint64_t key : missing_blocks) {
                    if (!key || key - 1 >= blocks.block_count()) { ok = false; break; }
                    uint32_t block_id = uint32_t(key - 1);
                    uint32_t child_count = blocks.block_size(block_id);
                    const uint32_t* children = blocks.block_children(block_id);
                    put_u64(payload, key);
                    put_uvar(payload, child_count);
                    for (uint32_t j = 0; j < child_count; ++j) put_u64(payload, uint64_t(children[j]) + 1);
                }
                put_uvar(payload, eager_regions.size());
                for (uint64_t key : eager_regions) {
                    put_u64(payload, key);
                    uint32_t region_id = uint32_t(key - 1);
                    uint32_t child_count = dict.region_ids_count(region_id);
                    const uint32_t* children = dict.region_ids_ptr(region_id);
                    put_uvar(payload, child_count);
                    for (uint32_t j = 0; j < child_count; ++j) put_u64(payload, children[j]);
                }
                put_uvar(payload, eager_lines.size());
                for (uint64_t key : eager_lines) {
                    put_u64(payload, key);
                    const CandidateInfo* info = nullptr;
                    if (key >= first_new && key <= after_lines && !candidates.empty())
                        info = &candidates[size_t(uint32_t(key) - first_new)];
                    encode_definition(payload, uint32_t(key), dict, info, receiver_known,
                                      options.delta, definition_stats);
                }
                if (!ok || !channel.send(Msg::Fill, payload)) { ok = false; break; }
                fill_ns += elapsed_ns(f0);

                for (uint64_t key : missing_regions) receiver_known_regions[size_t(key)] = 1;
                for (uint64_t key : eager_regions) receiver_known_regions[size_t(key)] = 1;
                for (uint64_t key : missing_blocks) receiver_known_blocks[size_t(key)] = 1;
                for (uint64_t key : eager_lines) receiver_known[size_t(key)] = 1;

                if (!options.pipeline) {
                w0 = Clock::now();
                for (;;) {
                    if (!channel.recv(frame)) { ok = false; break; }
                    if (frame.type != Msg::Ack) break;
                    Cursor ack(frame.payload);
                    uint32_t ack_id = 0;
                    uint64_t ack_len = 0;
                    if (!pending_ack || !ack.u32(ack_id) || ack_id != pending_ack ||
                        !ack.u64(ack_len) || ack_len != pending_ack_len || !ack.done()) {
                        ok = false;
                        break;
                    }
                    pending_ack = 0;
                }
                wait_ns += elapsed_ns(w0);
                if (!ok) break;
                if (frame.type == Msg::MissingRegions) {
                    Cursor region_missing(frame.payload);
                    if (!region_missing.u32(reply_id) || reply_id != txid ||
                        !region_missing.uvar(count) || count > UINT32_MAX) { ok = false; break; }
                    closure_regions.resize(size_t(count));
                    for (uint64_t& key : closure_regions)
                        if (!region_missing.u64(key)) { ok = false; break; }
                    if (!ok || !region_missing.done()) { ok = false; break; }
                    if (!closure_regions.empty()) {
                        f0 = Clock::now();
                        payload.clear();
                        put_u32(payload, txid);
                        put_uvar(payload, closure_regions.size());
                        for (uint64_t key : closure_regions) {
                            if (!key || key - 1 >= dict.region_count()) { ok = false; break; }
                            uint32_t region_id = uint32_t(key - 1);
                            uint32_t child_count = dict.region_ids_count(region_id);
                            const uint32_t* children = dict.region_ids_ptr(region_id);
                            put_uvar(payload, child_count);
                            for (uint32_t j = 0; j < child_count; ++j) put_u64(payload, children[j]);
                        }
                        if (!ok || !channel.send(Msg::FillRegions, payload)) { ok = false; break; }
                        fill_ns += elapsed_ns(f0);
                        for (uint64_t key : closure_regions)
                            receiver_known_regions[size_t(key)] = 1;
                    }
                    w0 = Clock::now();
                    if (!channel.recv(frame)) { ok = false; break; }
                    wait_ns += elapsed_ns(w0);
                }

                if (frame.type == Msg::Ack) {
                    Cursor ack(frame.payload);
                    uint32_t ack_id = 0;
                    uint64_t ack_len = 0;
                    current_acked = ack.u32(ack_id) && ack_id == txid &&
                                    ack.u64(ack_len) && ack_len == file.len && ack.done();
                    if (!current_acked) { ok = false; break; }
                } else if (frame.type == Msg::MissingLines) {
                    Cursor line_missing(frame.payload);
                    if (!line_missing.u32(reply_id) || reply_id != txid ||
                        !line_missing.uvar(count) || count > UINT32_MAX) { ok = false; break; }
                    missing_lines.resize(size_t(count));
                    for (uint64_t& key : missing_lines)
                        if (!line_missing.u64(key)) { ok = false; break; }
                    if (!ok || !line_missing.done()) { ok = false; break; }

                    if (!missing_lines.empty()) {
                        f0 = Clock::now();
                        payload.clear();
                        put_u32(payload, txid);
                        put_uvar(payload, missing_lines.size());
                        for (uint64_t key : missing_lines) {
                            if (!key || key > dict.distinct()) { ok = false; break; }
                            const CandidateInfo* info = nullptr;
                            if (key >= first_new && key <= after_lines && !candidates.empty())
                                info = &candidates[size_t(uint32_t(key) - first_new)];
                            encode_definition(payload, uint32_t(key), dict, info, receiver_known,
                                              options.delta, definition_stats);
                        }
                        if (!ok || !channel.send(Msg::FillLines, payload)) { ok = false; break; }
                        fill_ns += elapsed_ns(f0);
                    }
                } else {
                    ok = false;
                    break;
                }
                }
            }

            for (uint64_t key : missing_lines) receiver_known[size_t(key)] = 1;
            if (options.delta && pass == 0 && after_lines >= first_new && !candidates.empty())
                predictor.commit(first_new, candidates);
            if (!options.pipeline && !current_acked) {
                pending_ack = txid;
                pending_ack_len = file.len;
            }
            if (options.pipeline && txid - replies_drained >= options.pipeline_window) {
                auto w0 = Clock::now();
                uint32_t expected_id = replies_drained + 1;
                ok = drain_transaction_reply(channel, expected_id,
                                             transaction_lengths[size_t(expected_id - 1)],
                                             transaction_root_refs[size_t(expected_id - 1)] != 0);
                wait_ns += elapsed_ns(w0);
                if (!ok) break;
                ++replies_drained;
            }
        }

        // Put every pass boundary on a complete-wire boundary.  This makes the learning
        // curve an exact per-build measurement rather than assigning delayed replies to
        // whichever build happened to follow them.
        if (ok && options.pipeline) {
            while (replies_drained < txid) {
                uint32_t expected_id = replies_drained + 1;
                auto w0 = Clock::now();
                ok = drain_transaction_reply(channel, expected_id,
                                             transaction_lengths[size_t(expected_id - 1)],
                                             transaction_root_refs[size_t(expected_id - 1)] != 0);
                wait_ns += elapsed_ns(w0);
                if (!ok) break;
                ++replies_drained;
            }
        } else if (ok && pending_ack) {
            Frame ack_frame;
            auto w0 = Clock::now();
            if (!channel.recv(ack_frame) || ack_frame.type != Msg::Ack) ok = false;
            wait_ns += elapsed_ns(w0);
            if (ok) {
                Cursor ack(ack_frame.payload);
                uint32_t ack_id = 0;
                uint64_t ack_len = 0;
                ok = ack.u32(ack_id) && ack_id == pending_ack && ack.u64(ack_len) &&
                     ack_len == pending_ack_len && ack.done();
            }
            pending_ack = 0;
        }
        if (ok) {
            uint64_t now_wire = channel.sent() + channel.received();
            pass_rows.push_back({pass_raw[pass], now_wire - pass_wire_before,
                                 root_defs - pass_defs_before,
                                 root_refs - pass_refs_before});
        }
    }

    Frame final_frame;
    if (ok && options.pipeline) {
        while (replies_drained < txid) {
            uint32_t expected_id = replies_drained + 1;
            auto w0 = Clock::now();
            ok = drain_transaction_reply(channel, expected_id,
                                         transaction_lengths[size_t(expected_id - 1)],
                                         transaction_root_refs[size_t(expected_id - 1)] != 0);
            wait_ns += elapsed_ns(w0);
            if (!ok) break;
            ++replies_drained;
        }
    } else if (ok) {
        if (pending_ack) {
            auto w0 = Clock::now();
            if (!channel.recv(final_frame) || final_frame.type != Msg::Ack) ok = false;
            wait_ns += elapsed_ns(w0);
            if (ok) {
                Cursor ack(final_frame.payload);
                uint32_t ack_id = 0;
                uint64_t ack_len = 0;
                ok = ack.u32(ack_id) && ack_id == pending_ack && ack.u64(ack_len) &&
                     ack_len == pending_ack_len && ack.done();
                pending_ack = 0;
            }
        }
    }
    if (ok) {
        payload.clear();
        ok = channel.send(Msg::Done, payload, false) && channel.recv(final_frame) &&
             final_frame.type == Msg::Final;
    }
    ::shutdown(sockets[0], SHUT_RDWR);
    ::close(sockets[0]);
    verifier.join();

    int child_status = 0;
    waitpid(child, &child_status, 0);
    bool child_ok = WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0;

    uint64_t receiver_emitted = 0, receiver_decode_ns = 0, receiver_expand_ns = 0;
    uint64_t receiver_retained = 0, receiver_peak = 0;
    if (ok) {
        Cursor final(final_frame.payload);
        ok = final.u64(receiver_emitted) && final.u64(receiver_decode_ns) &&
             final.u64(receiver_expand_ns) && final.u64(receiver_retained) &&
             final.u64(receiver_peak) && final.done();
    }

    uint64_t total_ns = elapsed_ns(total_start);
    uint64_t fixture_ns = load_ns + edit_fixture_ns;
    uint64_t codec_ns = total_ns > fixture_ns ? total_ns - fixture_ns : 0;
    uint64_t raw = 0;
    for (uint64_t bytes : pass_raw) raw += bytes;
    uint64_t wire = channel.sent() + channel.received();
    bool exact = ok && child_ok && verification.exact && verification.bytes == raw &&
                 receiver_emitted == raw;

    printf("\n==== LOCAL-ORACLE CODEC50 (actual C/F frames, zstd-%d, delta=%s, %s) ====\n",
           options.zlevel, options.delta ? "pre-TU" : "off",
           options.pipeline ? (std::string("window=") + std::to_string(options.pipeline_window)).c_str()
                            : "lockstep");
    const char* order_name = options.order_mode == 0 ? "original" :
                             options.order_mode == 1 ? "reverse-after-cold" :
                             options.order_mode == 2 ? "shuffled-after-cold" : "mixed-after-cold";
    printf("manifest=%s TUs=%zu passes=%u order=%s edit_cycle=%s modified_TUs=%llu raw=%llu byte-exact=%s child=%s\n",
           options.manifest, corpus.files.size(), options.passes, order_name,
           options.edit_cycle ? "yes" : "no", (unsigned long long)edited_tus,
           (unsigned long long)raw, exact ? "PASS" : "FAIL", child_ok ? "PASS" : "FAIL");
    printf("full wall=%.6f s throughput=%.3f GB/s (%.3f GiB/s) wire=%llu ratio=%.2fx\n",
           total_ns / 1e9, total_ns ? double(raw) / double(total_ns) : 0.0,
           total_ns ? double(raw) / double(total_ns) * 1e9 / double(1ull << 30) : 0.0,
           (unsigned long long)wire, wire ? double(raw) / double(wire) : 0.0);
    printf("codec wall=%.6f s throughput=%.3f GB/s (fixture load reported separately)\n",
           codec_ns / 1e9, codec_ns ? double(raw) / double(codec_ns) : 0.0);
    printf("C->F=%llu F->C=%llu load=%.3fs edit_fixture=%.3fs intern=%.3fs plan=%.3fs fill=%.3fs wait=%.3fs\n",
           (unsigned long long)channel.sent(), (unsigned long long)channel.received(),
           load_ns / 1e9, edit_fixture_ns / 1e9, intern_ns / 1e9, plan_ns / 1e9,
           fill_ns / 1e9, wait_ns / 1e9);
    printf("C zstd=%.3fs C write=%.3fs C read+decompress=%.3fs F decode=%.3fs F expand+emit=%.3fs\n",
           channel.compress_ns() / 1e9, channel.write_ns() / 1e9,
           (channel.read_ns() + channel.decompress_ns()) / 1e9,
           receiver_decode_ns / 1e9, receiver_expand_ns / 1e9);
    printf("definitions: literal=%llu delta=%llu literal_bytes=%llu middle_bytes=%llu copied_bytes=%llu\n",
           (unsigned long long)definition_stats.literals,
           (unsigned long long)definition_stats.deltas,
           (unsigned long long)definition_stats.literal_bytes,
           (unsigned long long)definition_stats.delta_middle_bytes,
           (unsigned long long)definition_stats.copied_bytes);
    printf("semantic roots: enabled=%s definitions=%llu references=%llu retained=%.1f MiB\n",
           options.root_memo ? "yes" : "no", (unsigned long long)root_defs,
           (unsigned long long)root_refs, semantic_roots.retained_bytes() / 1048576.0);
    uint64_t curve_wire = 0;
    uint64_t curve_raw = 0;
    printf("learning curve (completed build -> pass wire, pass ratio, cumulative ratio, root defs/refs):\n");
    for (size_t i = 0; i < pass_rows.size(); ++i) {
        curve_wire += pass_rows[i].wire;
        curve_raw += pass_rows[i].raw;
        double pass_ratio = pass_rows[i].wire ? double(pass_rows[i].raw) / pass_rows[i].wire : 0.0;
        double cumulative_ratio = curve_wire ? double(curve_raw) / double(curve_wire) : 0.0;
        printf("  pass=%zu observed_TUs=%zu wire=%llu pass_ratio=%.1fx cumulative_ratio=%.1fx roots=%llu/%llu\n",
               i + 1, i * corpus.files.size(), (unsigned long long)pass_rows[i].wire,
               pass_ratio, cumulative_ratio, (unsigned long long)pass_rows[i].defs,
               (unsigned long long)pass_rows[i].refs);
    }
    printf("receiver retained=%.1f MiB peak=%.1f MiB\n",
           receiver_retained / 1048576.0, receiver_peak / 1048576.0);
    printf("wire by message:");
    for (unsigned i = 1; i <= unsigned(Msg::RootRef); ++i) {
        uint64_t bytes = channel.sent_by_type()[i] + channel.received_by_type()[i];
        if (bytes) printf(" %s=%llu", msg_name(Msg(i)), (unsigned long long)bytes);
    }
    printf("\n");
    return exact ? 0 : 1;
}

} // namespace local_codec50

int main(int argc, char** argv) {
    local_codec50::Options options;
    if (!local_codec50::parse_options(argc, argv, options)) {
        fprintf(stderr, "usage: %s --manifest FILE [--max-files N] [--z 0|1|3] [--passes N] [--order original|reverse|shuffle|mixed] [--edit-cycle] [--delta] [--no-blocks] [--no-root-memo] [--window N|--sync]\n", argv[0]);
        return 2;
    }
    if (options.receiver) return local_codec50::receiver_main(options.fd, options.out_fd, options.zlevel);
    return local_codec50::sender_main(options);
}
