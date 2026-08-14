// swisshash2-bench — Swiss table, huge-page arenas + inline slot (issue #16).
//
// Fixes over v1: (1) REAL huge pages — mmap each arena 2MB-aligned and madvise
// MADV_HUGEPAGE BEFORE first touch, so faults land as 2MB pages (v1 madvised vectors
// AFTER they were faulted as 4KB -> AnonHugePages stayed 0). (2) Collapse the dependent
// chain: the slot inlines {off,len,id} so a lookup is ctrl -> slot -> arena (2 hops),
// not ctrl -> sid -> aoff -> alen -> arena (4 hops). (3) 3-stage prefetch pipeline over
// a window hides the slot miss and the arena miss. Insert-only (no tombstones), 1-byte
// SSE2 tag (AMD/Zen-portable), no random malloc (fixed mmap regions, known locations).
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

#ifndef HASHV
#define HASHV 0
#endif
#ifndef WIN
#define WIN 16
#endif

using clk = std::chrono::steady_clock;
static double s_since(clk::time_point t0){ return std::chrono::duration<double>(clk::now()-t0).count(); }

static inline uint64_t rotl(uint64_t x,int r){ return (x<<r)|(x>>(64-r)); }
static inline uint64_t mulfold(uint64_t a, uint64_t b){ __uint128_t r=(__uint128_t)a*b; return (uint64_t)r ^ (uint64_t)(r>>64); }
// rapidhash primitives (Nicoshev)
static inline void rmum(uint64_t*A,uint64_t*B){ __uint128_t r=(__uint128_t)*A * *B; *A=(uint64_t)r; *B=(uint64_t)(r>>64); }
static inline uint64_t rmix(uint64_t A,uint64_t B){ rmum(&A,&B); return A^B; }
static inline uint64_t rd64(const uint8_t*p){ uint64_t v; memcpy(&v,p,8); return v; }
static inline uint64_t rd32(const uint8_t*p){ uint32_t v; memcpy(&v,p,4); return v; }
static inline uint64_t rdS(const uint8_t*p,size_t k){ return ((uint64_t)p[0]<<56)|((uint64_t)p[k>>1]<<32)|p[k-1]; }
static const uint64_t RSEC[3]={0x2d358dccaa6c78a5ull,0x8bb84b93962eacc9ull,0x4b33a62ed433d4a3ull};
static inline uint64_t rapidhash(const char* key,size_t len){
    const uint8_t* p=(const uint8_t*)key; uint64_t seed=0xbdd89aa982704029ull; seed^=rmix(seed^RSEC[0],RSEC[1])^len; uint64_t a,b;
    if(len<=16){ if(len>=4){ const uint8_t* pl=p+len-4; a=(rd32(p)<<32)|rd32(pl); uint64_t d=((len&24)>>(len>>3)); b=(rd32(p+d)<<32)|rd32(pl-d); }
        else if(len>0){ a=rdS(p,len); b=0; } else a=b=0; }
    else { size_t i=len; const uint8_t* pp=p;
        if(i>48){ uint64_t s1=seed,s2=seed; do{ seed=rmix(rd64(pp)^RSEC[0],rd64(pp+8)^seed);
            s1=rmix(rd64(pp+16)^RSEC[1],rd64(pp+24)^s1); s2=rmix(rd64(pp+32)^RSEC[2],rd64(pp+40)^s2); pp+=48; i-=48;}while(i>=48); seed^=s1^s2; }
        if(i>16){ seed=rmix(rd64(pp)^RSEC[2],rd64(pp+8)^seed^RSEC[1]); if(i>32) seed=rmix(rd64(pp+16)^RSEC[2],rd64(pp+24)^seed); }
        a=rd64(p+len-16); b=rd64(p+len-8); }
    a^=RSEC[1]; b^=seed; rmum(&a,&b); return rmix(a^RSEC[0]^len,b^RSEC[1]);
}
static inline uint64_t hashln(const char* p, size_t n){
#if HASHV==0
    uint64_t h=0x9E3779B97F4A7C15ULL ^ (n*0xff51afd7ed558ccdULL);
    size_t i=0; for(;i+8<=n;i+=8){ uint64_t w; memcpy(&w,p+i,8); h=(h^w)*1099511628211ULL; }
    if(i<n){ uint64_t w=0; memcpy(&w,p+i,n-i); h=(h^w)*1099511628211ULL; }
    h^=h>>33; h*=0xff51afd7ed558ccdULL; h^=h>>33; return h;
#elif HASHV==2
    uint64_t a=n,b=n; if(n>=8){ memcpy(&a,p,8); memcpy(&b,p+n-8,8);} else { memcpy(&a,p,n); b=a; }
    uint64_t h=(a ^ rotl(b,32) ^ (n*0x9E3779B97F4A7C15ULL)); h*=0xff51afd7ed558ccdULL; h^=h>>29; return h;
#elif HASHV==3   // wyhash-style 128-bit multiply-fold
    const uint64_t P1=0xa0761d6478bd642full, P2=0xe7037ed1a0b428dbull;
    uint64_t h=n^P1; size_t i=0;
    for(; i+16<=n; i+=16){ uint64_t x,y; memcpy(&x,p+i,8); memcpy(&y,p+i+8,8); h=mulfold(x^P1, y^h^P2); }
    uint64_t x=0,y=0; size_t rem=n-i;
    if(rem){ memcpy(&x,p+i, rem<8?rem:8); if(rem>8) memcpy(&y,p+i+8, rem-8); }
    return mulfold(x^P1, y^h^P2);
#else   // HASHV==4: rapidhash (fastest measured per-line)
    return rapidhash(p,n);
#endif
}

struct Slot { uint32_t off, len, id, pad; };   // 16 bytes, inline (off,len,id)
static uint8_t* ctrl; static Slot* slots; static char* arena;
static uint64_t CAP, MASK, ARENA_SZ, acur=0, nextid=1, count=0;
static const uint8_t EMPTY=0x80;

static void* halloc(size_t n){
    size_t HP=2u<<20, r=(n+HP-1)&~(HP-1);
    void* p=mmap(nullptr,r,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(p==MAP_FAILED){ perror("mmap"); exit(1); }
    madvise(p,r,MADV_HUGEPAGE);       // BEFORE first touch -> faults come in as 2MB pages
    return p;
}
static void init(uint64_t cap, uint64_t arena_sz){
    CAP=cap; MASK=cap-1; ARENA_SZ=arena_sz;
    ctrl=(uint8_t*)halloc(CAP+64); memset(ctrl,EMPTY,CAP+16);
    slots=(Slot*)halloc(CAP*sizeof(Slot));         // mmap zero-filled
    arena=(char*)halloc(ARENA_SZ);
}
static inline void set_ctrl(uint64_t s, uint8_t tag){ ctrl[s]=tag; if(s<16) ctrl[s+CAP]=tag; }

static inline uint32_t ins(const char* p, uint32_t n){
    uint64_t h=hashln(p,n); uint8_t tag=(uint8_t)(h&0x7F); uint64_t g=(h>>7)&MASK;
    for(;;){
        __m128i grp=_mm_loadu_si128((const __m128i*)(ctrl+g));
        unsigned mm=(unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(grp,_mm_set1_epi8((char)tag)));
        while(mm){ int b=__builtin_ctz(mm); uint64_t s=(g+b)&MASK;
            if(slots[s].len==n && memcmp(arena+slots[s].off,p,n)==0) return slots[s].id; mm&=mm-1; }
        unsigned me=(unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(grp,_mm_set1_epi8((char)EMPTY)));
        if(me){ int b=__builtin_ctz(me); uint64_t s=(g+b)&MASK;
            uint32_t off=(uint32_t)acur; memcpy(arena+acur,p,n); acur+=n;
            slots[s]={off,n,(uint32_t)nextid,0}; set_ctrl(s,tag); count++; return (uint32_t)nextid++; }
        g=(g+16)&MASK;
    }
}
static inline uint32_t lookup(const char* p, uint32_t n){
    uint64_t h=hashln(p,n); uint8_t tag=(uint8_t)(h&0x7F); uint64_t g=(h>>7)&MASK;
    for(;;){
        __m128i grp=_mm_loadu_si128((const __m128i*)(ctrl+g));
        unsigned mm=(unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(grp,_mm_set1_epi8((char)tag)));
        while(mm){ int b=__builtin_ctz(mm); uint64_t s=(g+b)&MASK;
            if(slots[s].len==n && memcmp(arena+slots[s].off,p,n)==0) return slots[s].id; mm&=mm-1; }
        if(_mm_movemask_epi8(_mm_cmpeq_epi8(grp,_mm_set1_epi8((char)EMPTY)))) return 0;
        g=(g+16)&MASK;
    }
}

int main(int argc,char**argv){
    const char* manifest=nullptr;
    for(int i=1;i<argc;i++) if(!strcmp(argv[i],"--manifest")&&i+1<argc) manifest=argv[++i];
    if(!manifest){ fprintf(stderr,"usage: swisshash2-bench --manifest F\n"); return 2; }
    init(1u<<21, 256u<<20);

    FILE* mf=fopen(manifest,"r"); if(!mf){ perror("manifest"); return 2; }
    char line[8192]; std::vector<std::string> srcs; size_t raw_bytes=0; long ntu=0,skipped=0;
    while(fgets(line,sizeof line,mf)){
        size_t L=strlen(line); while(L&&(line[L-1]=='\n'||line[L-1]=='\r')) line[--L]=0; if(!L)continue;
        FILE* tf=fopen(line,"rb"); if(!tf){ skipped++; continue; }
        std::string s; fseek(tf,0,SEEK_END); long sz=ftell(tf); fseek(tf,0,SEEK_SET);
        if(sz>0){ s.resize(sz); if(fread(&s[0],1,sz,tf)!=(size_t)sz) s.clear(); } fclose(tf);
        if(!s.empty()){ raw_bytes+=s.size(); srcs.push_back(std::move(s)); ntu++; }
    }
    fclose(mf);
    double mb=raw_bytes/1048576.0;

    std::vector<const char*> rp; std::vector<uint32_t> rn; rp.reserve(140000000); rn.reserve(140000000);
    auto tc=clk::now();
    for(auto& s:srcs){ size_t i=0,n=s.size(); while(i<n){ size_t j=s.find('\n',i); size_t end=(j==std::string::npos)?n:j+1;
        rp.push_back(s.data()+i); rn.push_back((uint32_t)(end-i)); ins(s.data()+i,(uint32_t)(end-i)); i=end; } }
    double dtc=s_since(tc); size_t M=rp.size();
    fprintf(stderr,"HASHV=%d WIN=%d  TUs=%ld raw=%.1f MB lines=%zu distinct=%llu load=%.2f arena=%.0fMB\n",
        HASHV,WIN,ntu,mb,M,(unsigned long long)(nextid-1),(double)count/CAP,acur/1048576.0);
    fprintf(stderr,"  COLD serial     : %.2f s  %.0f MB/s (%.2f GB/s)\n", dtc, mb/dtc, mb/dtc/1024.0);

    volatile uint64_t sink=0;
    { auto t0=clk::now(); uint64_t s=0; for(size_t k=0;k<M;k++) s+=lookup(rp[k],rn[k]); double dt=s_since(t0);
      sink+=s; fprintf(stderr,"  HOT serial      : %.2f s  %.0f MB/s (%.2f GB/s)\n", dt, mb/dt, mb/dt/1024.0); }

    // HOT batched, 3-stage pipeline: A hash+find slot idx (ctrl only, L2)+prefetch slot;
    // B read slot -> off, prefetch arena; C memcmp.
    { auto t0=clk::now(); uint64_t s=0; uint64_t sidx[WIN]; uint8_t ok[WIN]; uint32_t off_[WIN], len_[WIN], id_[WIN];
      for(size_t base=0;base<M;base+=WIN){ int w=(int)std::min((size_t)WIN,M-base);
        for(int k=0;k<w;k++){ const char* p=rp[base+k]; uint32_t n=rn[base+k];
            uint64_t h=hashln(p,n); uint8_t tag=(uint8_t)(h&0x7F); uint64_t g=(h>>7)&MASK; uint64_t found=~0ull;
            for(;;){ __m128i grp=_mm_loadu_si128((const __m128i*)(ctrl+g));
                unsigned mm=(unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(grp,_mm_set1_epi8((char)tag)));
                if(mm){ int b=__builtin_ctz(mm); found=(g+b)&MASK; break; }
                if(_mm_movemask_epi8(_mm_cmpeq_epi8(grp,_mm_set1_epi8((char)EMPTY)))) break; g=(g+16)&MASK; }
            sidx[k]=found; if(found!=~0ull) __builtin_prefetch(&slots[found],0,3); }
        for(int k=0;k<w;k++){ if(sidx[k]!=~0ull){ Slot sl=slots[sidx[k]]; off_[k]=sl.off; len_[k]=sl.len; id_[k]=sl.id; ok[k]=1;
              __builtin_prefetch(arena+sl.off,0,3);} else ok[k]=0; }
        for(int k=0;k<w;k++){ const char* p=rp[base+k]; uint32_t n=rn[base+k];
            if(ok[k] && len_[k]==n && memcmp(arena+off_[k],p,n)==0) s+=id_[k]; else s+=lookup(p,n); }
      }
      double dt=s_since(t0); sink+=s;
      fprintf(stderr,"  HOT batched(pf) : %.2f s  %.0f MB/s (%.2f GB/s)\n", dt, mb/dt, mb/dt/1024.0); }
    fprintf(stderr,"  (sink=%llu)\n",(unsigned long long)sink);

    { size_t hp=0; FILE* f=fopen("/proc/self/smaps_rollup","r"); if(f){ char b[256];
        while(fgets(b,sizeof b,f)) if(!strncmp(b,"AnonHugePages:",14)) hp=strtoul(b+14,nullptr,10); fclose(f);
        fprintf(stderr,"  AnonHugePages (this proc): %zu kB\n",hp); } }
    return 0;
}
