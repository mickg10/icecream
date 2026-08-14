// tiered-bench — tiered interner: tiny-direct + Swiss + AVX-512 compare (issue #16).
//
// Corpus fact: atoms <=4 bytes are 38.7% of ALL occurrences but only 32 distinct
// (blank '\n' alone = 33.3%). So tier them:
//   TIER 0 (len<=4): pack the <=4 bytes + len into a u64 key -> tiny open-addressing
//     table (32 live entries, 1024 slots -> ~1 probe, cache-resident). No line hash,
//     no arena compare. Retires 38.7% of atoms at ~a few cycles.
//   TIER 1 (len>4): Swiss/F14 open addressing (1-byte SSE2 tag, insert-only/no tombstone),
//     huge-page mmap arenas, inline {off,len,id} slot, wyhash-style hash, AVX-512 masked
//     byte-compare (_mm512_maskz_loadu_epi8 + _mm512_cmpeq_epi8_mask; no over-read).
// Shared id space (gate: total distinct must be 771055). Reports overall COLD/HOT plus a
// per-size-class HOT breakdown for the bake-off.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <chrono>
#include <algorithm>
#include <immintrin.h>
#include <sys/mman.h>

using clk = std::chrono::steady_clock;
static double s_since(clk::time_point t0){ return std::chrono::duration<double>(clk::now()-t0).count(); }

static inline uint64_t mulfold(uint64_t a,uint64_t b){ __uint128_t r=(__uint128_t)a*b; return (uint64_t)r ^ (uint64_t)(r>>64); }
static inline uint64_t hashln(const char* p,size_t n){
    const uint64_t P1=0xa0761d6478bd642full,P2=0xe7037ed1a0b428dbull; uint64_t h=n^P1; size_t i=0;
    for(;i+16<=n;i+=16){ uint64_t x,y; memcpy(&x,p+i,8); memcpy(&y,p+i+8,8); h=mulfold(x^P1,y^h^P2); }
    uint64_t x=0,y=0; size_t rem=n-i; if(rem){ memcpy(&x,p+i,rem<8?rem:8); if(rem>8) memcpy(&y,p+i+8,rem-8);} return mulfold(x^P1,y^h^P2);
}
static inline bool eqbytes(const char* a, const char* b, uint32_t n){
#ifdef __AVX512BW__
    if(n<=64){ __mmask64 m=(n==64)?~0ull:((1ull<<n)-1);
        __m512i va=_mm512_maskz_loadu_epi8(m,a), vb=_mm512_maskz_loadu_epi8(m,b);
        return _mm512_cmpeq_epi8_mask(va,vb)==(__mmask64)~0ull; }
#endif
    return memcmp(a,b,n)==0;
}

// ---- shared id ----
static uint64_t nextid=1;

// ---- TIER 0: tiny (len<=4) ----
static const uint32_t TINYMASK=1023;
static uint64_t tk[TINYMASK+1]; static uint32_t ti[TINYMASK+1];
static inline uint64_t tinykey(const char* p, uint32_t n){ uint32_t v=0; memcpy(&v,p,n); return ((uint64_t)n<<32)|v; }
static inline uint32_t tiny_ins(const char* p, uint32_t n){
    uint64_t key=tinykey(p,n); uint32_t b=(uint32_t)((key*0x9E3779B97F4A7C15ULL)>>50)&TINYMASK;
    for(;;){ if(tk[b]==key) return ti[b]; if(!tk[b]){ tk[b]=key; ti[b]=(uint32_t)nextid; return (uint32_t)nextid++; } b=(b+1)&TINYMASK; }
}
static inline uint32_t tiny_lookup(const char* p, uint32_t n){
    uint64_t key=tinykey(p,n); uint32_t b=(uint32_t)((key*0x9E3779B97F4A7C15ULL)>>50)&TINYMASK;
    for(;;){ if(tk[b]==key) return ti[b]; if(!tk[b]) return 0; b=(b+1)&TINYMASK; }
}

// ---- TIER 1: Swiss ----
struct Slot { uint32_t off,len,id,pad; };
static uint8_t* ctrl; static Slot* slots; static char* arena;
static uint64_t CAP,MASK,acur=0; static const uint8_t EMPTY=0x80;
static void* halloc(size_t n){ size_t HP=2u<<20,r=(n+HP-1)&~(HP-1);
    void* p=mmap(nullptr,r,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0); if(p==MAP_FAILED){perror("mmap");exit(1);} madvise(p,r,MADV_HUGEPAGE); return p; }
static inline void set_ctrl(uint64_t s,uint8_t t){ ctrl[s]=t; if(s<16) ctrl[s+CAP]=t; }
static inline uint32_t swiss_ins(const char* p, uint32_t n){
    uint64_t h=hashln(p,n); uint8_t tag=(uint8_t)(h&0x7F); uint64_t g=(h>>7)&MASK;
    for(;;){ __m128i grp=_mm_loadu_si128((const __m128i*)(ctrl+g));
        unsigned mm=(unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(grp,_mm_set1_epi8((char)tag)));
        while(mm){ int b=__builtin_ctz(mm); uint64_t s=(g+b)&MASK; if(slots[s].len==n&&eqbytes(arena+slots[s].off,p,n)) return slots[s].id; mm&=mm-1; }
        unsigned me=(unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(grp,_mm_set1_epi8((char)EMPTY)));
        if(me){ int b=__builtin_ctz(me); uint64_t s=(g+b)&MASK; uint32_t off=(uint32_t)acur; memcpy(arena+acur,p,n); acur+=n;
            slots[s]={off,n,(uint32_t)nextid,0}; set_ctrl(s,tag); return (uint32_t)nextid++; }
        g=(g+16)&MASK; }
}
static inline uint32_t swiss_lookup(const char* p, uint32_t n){
    uint64_t h=hashln(p,n); uint8_t tag=(uint8_t)(h&0x7F); uint64_t g=(h>>7)&MASK;
    for(;;){ __m128i grp=_mm_loadu_si128((const __m128i*)(ctrl+g));
        unsigned mm=(unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(grp,_mm_set1_epi8((char)tag)));
        while(mm){ int b=__builtin_ctz(mm); uint64_t s=(g+b)&MASK; if(slots[s].len==n&&eqbytes(arena+slots[s].off,p,n)) return slots[s].id; mm&=mm-1; }
        if(_mm_movemask_epi8(_mm_cmpeq_epi8(grp,_mm_set1_epi8((char)EMPTY)))) return 0; g=(g+16)&MASK; }
}
static inline uint32_t ins(const char* p,uint32_t n){ return n<=4? tiny_ins(p,n) : swiss_ins(p,n); }
static inline uint32_t lookup(const char* p,uint32_t n){ return n<=4? tiny_lookup(p,n) : swiss_lookup(p,n); }
static int cls(uint32_t n){ return n<=4?0 : n<=16?1 : n<=48?2 : 3; }

int main(int argc,char**argv){
    const char* manifest=nullptr; for(int i=1;i<argc;i++) if(!strcmp(argv[i],"--manifest")&&i+1<argc) manifest=argv[++i];
    if(!manifest){ fprintf(stderr,"usage: tiered-bench --manifest F\n"); return 2; }
    CAP=1u<<21; MASK=CAP-1; ctrl=(uint8_t*)halloc(CAP+64); memset(ctrl,EMPTY,CAP+16);
    slots=(Slot*)halloc(CAP*sizeof(Slot)); arena=(char*)halloc(256u<<20);

    FILE* mf=fopen(manifest,"r"); if(!mf){ perror("manifest"); return 2; }
    char line[8192]; std::vector<std::string> srcs; size_t raw=0; long ntu=0;
    while(fgets(line,sizeof line,mf)){ size_t L=strlen(line); while(L&&(line[L-1]=='\n'||line[L-1]=='\r'))line[--L]=0; if(!L)continue;
        FILE* tf=fopen(line,"rb"); if(!tf)continue; std::string s; fseek(tf,0,SEEK_END); long sz=ftell(tf); fseek(tf,0,SEEK_SET);
        if(sz>0){ s.resize(sz); if(fread(&s[0],1,sz,tf)!=(size_t)sz) s.clear(); } fclose(tf);
        if(!s.empty()){ raw+=s.size(); srcs.push_back(std::move(s)); ntu++; } }
    fclose(mf);
    double mb=raw/1048576.0;

    std::vector<const char*> rp; std::vector<uint32_t> rn; rp.reserve(140000000); rn.reserve(140000000);
    std::vector<const char*> cp[4]; std::vector<uint32_t> cn[4];
    auto tc=clk::now();
    for(auto& s:srcs){ size_t i=0,n=s.size(); while(i<n){ size_t j=s.find('\n',i); size_t end=(j==std::string::npos)?n:j+1;
        uint32_t len=(uint32_t)(end-i); const char* p=s.data()+i; ins(p,len);
        rp.push_back(p); rn.push_back(len); i=end; } }
    double dtc=s_since(tc); size_t M=rp.size();
    // per-class CONTIGUOUS ref arrays (untimed) so per-class timing isn't polluted by gather
    double cbytes[4]={0,0,0,0}; for(size_t k=0;k<M;k++){ int c=cls(rn[k]); cp[c].push_back(rp[k]); cn[c].push_back(rn[k]); cbytes[c]+=rn[k]; }
    fprintf(stderr,"TUs=%ld raw=%.1f MB lines=%zu distinct=%llu (tiny+swiss)  arena=%.0fMB\n",
        ntu,mb,M,(unsigned long long)(nextid-1),acur/1048576.0);
    fprintf(stderr,"  COLD serial (tiered): %.2f s  %.0f MB/s (%.2f GB/s)\n", dtc, mb/dtc, mb/dtc/1024.0);

    volatile uint64_t sink=0;
    { auto t0=clk::now(); uint64_t s=0; for(size_t k=0;k<M;k++) s+=lookup(rp[k],rn[k]); double dt=s_since(t0);
      sink+=s; fprintf(stderr,"  HOT serial  (tiered): %.2f s  %.0f MB/s (%.2f GB/s)\n", dt, mb/dt, mb/dt/1024.0); }

    const char* nm[4]={"tiny <=4","small 5-16","medium 17-48","large >48"};
    for(int c=0;c<4;c++){ size_t Q=cp[c].size(); auto t0=clk::now(); uint64_t s=0;
        for(size_t q=0;q<Q;q++) s+=lookup(cp[c][q],cn[c][q]); double dt=s_since(t0);
        sink+=s; double cmb=cbytes[c]/1048576.0;
        fprintf(stderr,"    %-13s HOT: %.2f GB/s  %.1f Mlookup/s  (%.1f ns/lookup, %.1f%% occ)\n",
            nm[c], cmb/1024.0/dt, Q/dt/1e6, dt*1e9/Q, 100.0*Q/M); }
    fprintf(stderr,"  (sink=%llu)\n",(unsigned long long)sink);
    return 0;
}
