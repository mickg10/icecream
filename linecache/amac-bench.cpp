// amac-bench — Swiss + AMAC rolling software pipeline (issue #16).
//
// Phase-batching (swiss2) overlaps the two memory misses (slot, arena) across a window
// but leaves the per-atom hash serial (~40% of the window). AMAC (Kocberber/Falsafi/Grot
// VLDB'15) keeps D atoms in flight through a stage machine, so hash-compute of freshly
// loaded atoms overlaps the memory-waits of in-flight ones. Stages per atom:
//   1: ctrl group prefetched -> SSE2 tag scan -> prefetch candidate slot
//   2: slot read -> prefetch arena bytes
//   3: AVX-512 masked byte-compare -> emit id (rare tag-collision/miss -> serial fallback)
// D (in-flight depth) is the knob. h0 hash, insert-only huge-page Swiss.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <chrono>
#include <immintrin.h>
#include <sys/mman.h>

#ifndef DEPTH
#define DEPTH 24
#endif

using clk = std::chrono::steady_clock;
static double s_since(clk::time_point t0){ return std::chrono::duration<double>(clk::now()-t0).count(); }

static inline uint64_t hashln(const char* p,size_t n){
    uint64_t h=0x9E3779B97F4A7C15ULL ^ (n*0xff51afd7ed558ccdULL); size_t i=0;
    for(;i+8<=n;i+=8){ uint64_t w; memcpy(&w,p+i,8); h=(h^w)*1099511628211ULL; }
    if(i<n){ uint64_t w=0; memcpy(&w,p+i,n-i); h=(h^w)*1099511628211ULL; }
    h^=h>>33; h*=0xff51afd7ed558ccdULL; h^=h>>33; return h;
}
static inline bool eqbytes(const char* a,const char* b,uint32_t n){
#ifdef __AVX512BW__
    if(n<=64){ __mmask64 m=(n==64)?~0ull:((1ull<<n)-1);
        __m512i va=_mm512_maskz_loadu_epi8(m,a), vb=_mm512_maskz_loadu_epi8(m,b);
        return _mm512_cmpeq_epi8_mask(va,vb)==(__mmask64)~0ull; }
#endif
    return memcmp(a,b,n)==0;
}
struct Slot{ uint32_t off,len,id,pad; };
static uint8_t* ctrl; static Slot* slots; static char* arena;
static uint64_t CAP,MASK,acur=0,nextid=1; static const uint8_t EMPTY=0x80;
static void* halloc(size_t n){ size_t HP=2u<<20,r=(n+HP-1)&~(HP-1);
    void* p=mmap(nullptr,r,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0); if(p==MAP_FAILED){perror("mmap");exit(1);} madvise(p,r,MADV_HUGEPAGE); return p; }
static inline void set_ctrl(uint64_t s,uint8_t t){ ctrl[s]=t; if(s<16) ctrl[s+CAP]=t; }
static inline uint32_t swiss_ins(const char* p,uint32_t n){
    uint64_t h=hashln(p,n); uint8_t tag=h&0x7F; uint64_t g=(h>>7)&MASK;
    for(;;){ __m128i grp=_mm_loadu_si128((const __m128i*)(ctrl+g));
        unsigned mm=_mm_movemask_epi8(_mm_cmpeq_epi8(grp,_mm_set1_epi8((char)tag)));
        while(mm){ int b=__builtin_ctz(mm); uint64_t s=(g+b)&MASK; if(slots[s].len==n&&eqbytes(arena+slots[s].off,p,n)) return slots[s].id; mm&=mm-1; }
        unsigned me=_mm_movemask_epi8(_mm_cmpeq_epi8(grp,_mm_set1_epi8((char)EMPTY)));
        if(me){ int b=__builtin_ctz(me); uint64_t s=(g+b)&MASK; uint32_t off=(uint32_t)acur; memcpy(arena+acur,p,n); acur+=n;
            slots[s]={off,n,(uint32_t)nextid,0}; set_ctrl(s,tag); return (uint32_t)nextid++; }
        g=(g+16)&MASK; }
}
static inline uint32_t swiss_lookup(const char* p,uint32_t n){
    uint64_t h=hashln(p,n); uint8_t tag=h&0x7F; uint64_t g=(h>>7)&MASK;
    for(;;){ __m128i grp=_mm_loadu_si128((const __m128i*)(ctrl+g));
        unsigned mm=_mm_movemask_epi8(_mm_cmpeq_epi8(grp,_mm_set1_epi8((char)tag)));
        while(mm){ int b=__builtin_ctz(mm); uint64_t s=(g+b)&MASK; if(slots[s].len==n&&eqbytes(arena+slots[s].off,p,n)) return slots[s].id; mm&=mm-1; }
        if(_mm_movemask_epi8(_mm_cmpeq_epi8(grp,_mm_set1_epi8((char)EMPTY)))) return 0; g=(g+16)&MASK; }
}

struct Req{ const char* p; uint64_t h,g; uint32_t n,sidx,off,len,id; uint8_t tag,stage; };
static inline void amac_start(Req& r,const char* p,uint32_t n){
    r.p=p; r.n=n; r.h=hashln(p,n); r.tag=r.h&0x7F; r.g=(r.h>>7)&MASK; __builtin_prefetch(ctrl+r.g,0,3); r.stage=1;
}
static uint64_t amac_hot(const char** rp,const uint32_t* rn,size_t M){
    Req req[DEPTH]; size_t next=0; uint64_t sink=0; int active=0;
    for(int d=0; d<DEPTH && next<M; d++){ amac_start(req[d],rp[next],rn[next]); next++; active++; }
    while(active>0){
        for(int d=0; d<DEPTH; d++){ Req& r=req[d]; if(!r.stage) continue;
            if(r.stage==1){
                __m128i grp=_mm_loadu_si128((const __m128i*)(ctrl+r.g));
                unsigned mm=_mm_movemask_epi8(_mm_cmpeq_epi8(grp,_mm_set1_epi8((char)r.tag)));
                if(mm){ r.sidx=(uint32_t)((r.g+__builtin_ctz(mm))&MASK); __builtin_prefetch(&slots[r.sidx],0,3); r.stage=2; continue; }
                if(_mm_movemask_epi8(_mm_cmpeq_epi8(grp,_mm_set1_epi8((char)EMPTY)))){
                    sink+=swiss_lookup(r.p,r.n);
                    if(next<M){ amac_start(r,rp[next],rn[next]); next++; } else { r.stage=0; active--; } continue; }
                r.g=(r.g+16)&MASK; __builtin_prefetch(ctrl+r.g,0,3); continue;
            }
            if(r.stage==2){ Slot sl=slots[r.sidx]; r.off=sl.off; r.len=sl.len; r.id=sl.id;
                __builtin_prefetch(arena+r.off,0,3); r.stage=3; continue; }
            // stage 3
            if(r.len==r.n && eqbytes(arena+r.off,r.p,r.n)) sink+=r.id; else sink+=swiss_lookup(r.p,r.n);
            if(next<M){ amac_start(r,rp[next],rn[next]); next++; } else { r.stage=0; active--; }
        }
    }
    return sink;
}

int main(int argc,char**argv){
    const char* manifest=nullptr; for(int i=1;i<argc;i++) if(!strcmp(argv[i],"--manifest")&&i+1<argc) manifest=argv[++i];
    if(!manifest){ fprintf(stderr,"usage: amac-bench --manifest F\n"); return 2; }
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
    auto tc=clk::now();
    for(auto& s:srcs){ size_t i=0,n=s.size(); while(i<n){ size_t j=s.find('\n',i); size_t end=(j==std::string::npos)?n:j+1;
        uint32_t len=(uint32_t)(end-i); const char* p=s.data()+i; swiss_ins(p,len); rp.push_back(p); rn.push_back(len); i=end; } }
    double dtc=s_since(tc); size_t M=rp.size();
    fprintf(stderr,"DEPTH=%d TUs=%ld raw=%.1f MB lines=%zu distinct=%llu arena=%.0fMB\n",
        DEPTH,ntu,mb,M,(unsigned long long)(nextid-1),acur/1048576.0);
    fprintf(stderr,"  COLD serial : %.2f s  %.0f MB/s (%.2f GB/s)\n", dtc, mb/dtc, mb/dtc/1024.0);
    volatile uint64_t sink=0;
    for(int r=0;r<3;r++){ auto t0=clk::now(); uint64_t s=amac_hot(rp.data(),rn.data(),M); double dt=s_since(t0); sink+=s;
        fprintf(stderr,"  HOT amac r%d : %.2f s  %.0f MB/s (%.2f GB/s)\n", r, dt, mb/dt, mb/dt/1024.0); }
    fprintf(stderr,"  (sink=%llu)\n",(unsigned long long)sink);
    return 0;
}
