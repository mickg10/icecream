// lean.cpp — lean Swiss: 64-wide AVX-512 tag scan + minimal-branch first-group probe +
// rapidhash + inline slot (issue #16). Goal: close the 2.7x gap between the full pipeline
// (~1.04 GB/s Zen4) and the pure rapidhash throughput ceiling (2.82 GB/s Zen4) by cutting
// instructions and branch-mispredicts. 64-byte control groups => 4x fewer probe iterations
// than SSE2-16. Both target boxes have AVX-512BW; SSE2 path kept for portability elsewhere.
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
#ifndef WIN
#define WIN 24
#endif
using clk=std::chrono::steady_clock;
static double s_since(clk::time_point t0){ return std::chrono::duration<double>(clk::now()-t0).count(); }
// rapidhash
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
static inline bool eqbytes(const char* a,const char* b,uint32_t n){
    if(n<=64){ __mmask64 m=(n==64)?~0ull:((1ull<<n)-1);
        __m512i va=_mm512_maskz_loadu_epi8(m,a), vb=_mm512_maskz_loadu_epi8(m,b);
        return _mm512_cmpeq_epi8_mask(va,vb)==(__mmask64)~0ull; }
    return memcmp(a,b,n)==0;
}
struct Slot{ uint32_t id,len; char b[24]; };
static uint8_t* ctrl; static Slot* slots; static char* arena;
static uint64_t CAP,MASK,acur=0,nextid=1; static const uint8_t EMPTY=0x80;
static void* halloc(size_t n){ size_t HP=2u<<20,r=(n+HP-1)&~(HP-1);
    void* p=mmap(nullptr,r,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0); if(p==MAP_FAILED){perror("mmap");exit(1);} madvise(p,r,MADV_HUGEPAGE); return p; }
static inline void set_ctrl(uint64_t s,uint8_t t){ ctrl[s]=t; if(s<64) ctrl[s+CAP]=t; }
static inline bool slot_eq(const Slot& sl,const char* p,uint32_t n){ if(sl.len!=n) return false; if(n<=24) return eqbytes(sl.b,p,n); uint32_t o; memcpy(&o,sl.b,4); return eqbytes(arena+o,p,n); }
static inline uint32_t ins(const char* p,uint32_t n){
    uint64_t h=rapidhash(p,n); uint8_t tag=h&0x7F; uint64_t g=(h>>6)&MASK;
    for(;;){ __m512i grp=_mm512_loadu_si512((const void*)(ctrl+g));
        uint64_t mm=_mm512_cmpeq_epi8_mask(grp,_mm512_set1_epi8((char)tag));
        while(mm){ uint64_t s=(g+__builtin_ctzll(mm))&MASK; if(slot_eq(slots[s],p,n)) return slots[s].id; mm&=mm-1; }
        uint64_t me=_mm512_cmpeq_epi8_mask(grp,_mm512_set1_epi8((char)EMPTY));
        if(me){ uint64_t s=(g+__builtin_ctzll(me))&MASK; slots[s].id=(uint32_t)nextid; slots[s].len=n;
            if(n<=24) memcpy(slots[s].b,p,n); else { uint32_t o=(uint32_t)acur; memcpy(slots[s].b,&o,4); memcpy(arena+acur,p,n); acur+=n; }
            set_ctrl(s,tag); return (uint32_t)nextid++; }
        g=(g+64)&MASK; }
}
static inline uint32_t lookup(const char* p,uint32_t n){
    uint64_t h=rapidhash(p,n); uint8_t tag=h&0x7F; uint64_t g=(h>>6)&MASK;
    for(;;){ __m512i grp=_mm512_loadu_si512((const void*)(ctrl+g));
        uint64_t mm=_mm512_cmpeq_epi8_mask(grp,_mm512_set1_epi8((char)tag));
        while(mm){ uint64_t s=(g+__builtin_ctzll(mm))&MASK; if(slot_eq(slots[s],p,n)) return slots[s].id; mm&=mm-1; }
        if(_mm512_cmpeq_epi8_mask(grp,_mm512_set1_epi8((char)EMPTY))) return 0; g=(g+64)&MASK; }
}
int main(int argc,char**argv){
    const char* manifest=nullptr; for(int i=1;i<argc;i++) if(!strcmp(argv[i],"--manifest")&&i+1<argc) manifest=argv[++i];
    if(!manifest){ fprintf(stderr,"usage: lean --manifest F\n"); return 2; }
    CAP=1u<<21; MASK=CAP-1; ctrl=(uint8_t*)halloc(CAP+128); memset(ctrl,EMPTY,CAP+64);
    slots=(Slot*)halloc(CAP*sizeof(Slot)); arena=(char*)halloc(256u<<20);
    FILE* mf=fopen(manifest,"r"); if(!mf){perror("m");return 2;} char line[8192]; std::vector<std::string> srcs; size_t raw=0; long ntu=0;
    while(fgets(line,sizeof line,mf)){ size_t L=strlen(line); while(L&&(line[L-1]=='\n'||line[L-1]=='\r'))line[--L]=0; if(!L)continue;
        FILE* tf=fopen(line,"rb"); if(!tf)continue; std::string s; fseek(tf,0,SEEK_END); long sz=ftell(tf); fseek(tf,0,SEEK_SET);
        if(sz>0){ s.resize(sz); if(fread(&s[0],1,sz,tf)!=(size_t)sz)s.clear();} fclose(tf); if(!s.empty()){raw+=s.size();srcs.push_back(std::move(s));ntu++;} }
    fclose(mf);
    double mb=raw/1048576.0;
    std::vector<const char*> rp; std::vector<uint32_t> rn; rp.reserve(140000000); rn.reserve(140000000);
    auto tc=clk::now();
    for(auto& s:srcs){ size_t i=0,n=s.size(); while(i<n){ size_t j=s.find('\n',i); size_t e=(j==std::string::npos)?n:j+1; uint32_t len=(uint32_t)(e-i); const char* p=s.data()+i; ins(p,len); rp.push_back(p); rn.push_back(len); i=e; } }
    double dtc=s_since(tc); size_t M=rp.size();
    fprintf(stderr,"TUs=%ld raw=%.1f MB lines=%zu distinct=%llu arena=%.0fMB\n",ntu,mb,M,(unsigned long long)(nextid-1),acur/1048576.0);
    fprintf(stderr,"  COLD : %.2f s  %.2f GB/s\n",dtc,mb/dtc/1024.0);
    volatile uint64_t sink=0;
    for(int r=0;r<3;r++){ auto t0=clk::now(); uint64_t s=0; for(size_t k=0;k<M;k++) s+=lookup(rp[k],rn[k]); double dt=s_since(t0); sink+=s;
        fprintf(stderr,"  HOT serial r%d : %.2f s  %.2f GB/s\n",r,dt,mb/dt/1024.0); }
    // batched: 64-wide, 3-stage
    for(int r=0;r<3;r++){ auto t0=clk::now(); uint64_t s=0; uint64_t idx[WIN]; uint32_t off_[WIN]; uint8_t ar[WIN];
        for(size_t base=0;base<M;base+=WIN){ int w=(int)std::min((size_t)WIN,M-base);
            for(int k=0;k<w;k++){ const char* p=rp[base+k]; uint32_t nn=rn[base+k]; uint64_t h=rapidhash(p,nn); uint8_t tag=h&0x7F; uint64_t g=(h>>6)&MASK; uint64_t found=~0ull;
                for(;;){ __m512i grp=_mm512_loadu_si512((const void*)(ctrl+g)); uint64_t mm=_mm512_cmpeq_epi8_mask(grp,_mm512_set1_epi8((char)tag));
                    if(mm){ found=(g+__builtin_ctzll(mm))&MASK; break; } if(_mm512_cmpeq_epi8_mask(grp,_mm512_set1_epi8((char)EMPTY))) break; g=(g+64)&MASK; }
                idx[k]=found; if(found!=~0ull) __builtin_prefetch(&slots[found],0,3); }
            for(int k=0;k<w;k++){ if(idx[k]==~0ull){ar[k]=0;continue;} Slot& sl=slots[idx[k]]; if(sl.len>24){ uint32_t o; memcpy(&o,sl.b,4); off_[k]=o; __builtin_prefetch(arena+o,0,3); ar[k]=1;} else ar[k]=2; }
            for(int k=0;k<w;k++){ const char* p=rp[base+k]; uint32_t nn=rn[base+k];
                if(idx[k]!=~0ull){ Slot& sl=slots[idx[k]]; bool ok=sl.len==nn && (ar[k]==2?eqbytes(sl.b,p,nn):eqbytes(arena+off_[k],p,nn)); if(ok){s+=sl.id;continue;} }
                s+=lookup(p,nn); } }
        double dt=s_since(t0); sink+=s; fprintf(stderr,"  HOT batched r%d: %.2f s  %.2f GB/s\n",r,dt,mb/dt/1024.0); }
    fprintf(stderr,"  (sink=%llu)\n",(unsigned long long)sink);
    return 0;
}
