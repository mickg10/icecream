// hash-micro — pure per-line hash cost, no table. Resolves which hash is the cheapest
// per atom (h0 FNV-chain vs h3 mulfold vs real rapidhash) on the actual corpus atoms.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <chrono>
#include <immintrin.h>
using clk=std::chrono::steady_clock;
static double s_since(clk::time_point t0){ return std::chrono::duration<double>(clk::now()-t0).count(); }

// h0: FNV-style multiply chain + murmur finalize
static inline uint64_t h0(const char* p,size_t n){
    uint64_t h=0x9E3779B97F4A7C15ULL^(n*0xff51afd7ed558ccdULL); size_t i=0;
    for(;i+8<=n;i+=8){ uint64_t w; memcpy(&w,p+i,8); h=(h^w)*1099511628211ULL; }
    if(i<n){ uint64_t w=0; memcpy(&w,p+i,n-i); h=(h^w)*1099511628211ULL; }
    h^=h>>33; h*=0xff51afd7ed558ccdULL; h^=h>>33; return h;
}
// h3: wyhash-ish 16-byte multiply-fold chain
static inline uint64_t mulfold(uint64_t a,uint64_t b){ __uint128_t r=(__uint128_t)a*b; return (uint64_t)r^(uint64_t)(r>>64); }
static inline uint64_t h3(const char* p,size_t n){
    const uint64_t P1=0xa0761d6478bd642full,P2=0xe7037ed1a0b428dbull; uint64_t h=n^P1; size_t i=0;
    for(;i+16<=n;i+=16){ uint64_t x,y; memcpy(&x,p+i,8); memcpy(&y,p+i+8,8); h=mulfold(x^P1,y^h^P2); }
    uint64_t x=0,y=0; size_t rem=n-i; if(rem){ memcpy(&x,p+i,rem<8?rem:8); if(rem>8) memcpy(&y,p+i+8,rem-8);} return mulfold(x^P1,y^h^P2);
}
// rapidhash (Nicoshev), short-key paths
static inline void rmum(uint64_t*A,uint64_t*B){ __uint128_t r=(__uint128_t)*A * *B; *A=(uint64_t)r; *B=(uint64_t)(r>>64); }
static inline uint64_t rmix(uint64_t A,uint64_t B){ rmum(&A,&B); return A^B; }
static inline uint64_t rd64(const uint8_t*p){ uint64_t v; memcpy(&v,p,8); return v; }
static inline uint64_t rd32(const uint8_t*p){ uint32_t v; memcpy(&v,p,4); return v; }
static inline uint64_t rdS(const uint8_t*p,size_t k){ return ((uint64_t)p[0]<<56)|((uint64_t)p[k>>1]<<32)|p[k-1]; }
static const uint64_t SEC[3]={0x2d358dccaa6c78a5ull,0x8bb84b93962eacc9ull,0x4b33a62ed433d4a3ull};
static inline uint64_t rapid(const char* key,size_t len){
    const uint8_t* p=(const uint8_t*)key; uint64_t seed=0xbdd89aa982704029ull; seed^=rmix(seed^SEC[0],SEC[1])^len; uint64_t a,b;
    if(len<=16){ if(len>=4){ const uint8_t* pl=p+len-4; a=(rd32(p)<<32)|rd32(pl); uint64_t d=((len&24)>>(len>>3)); b=(rd32(p+d)<<32)|rd32(pl-d); }
        else if(len>0){ a=rdS(p,len); b=0; } else a=b=0; }
    else { size_t i=len; const uint8_t* pp=p;
        if(i>48){ uint64_t s1=seed,s2=seed; do{ seed=rmix(rd64(pp)^SEC[0],rd64(pp+8)^seed);
            s1=rmix(rd64(pp+16)^SEC[1],rd64(pp+24)^s1); s2=rmix(rd64(pp+32)^SEC[2],rd64(pp+40)^s2); pp+=48; i-=48;}while(i>=48); seed^=s1^s2; }
        if(i>16){ seed=rmix(rd64(pp)^SEC[2],rd64(pp+8)^seed^SEC[1]); if(i>32) seed=rmix(rd64(pp+16)^SEC[2],rd64(pp+24)^seed); }
        a=rd64(p+len-16); b=rd64(p+len-8); }
    a^=SEC[1]; b^=seed; rmum(&a,&b); return rmix(a^SEC[0]^len,b^SEC[1]);
}

// aes1: single-lane AES-NI hash (portable: AES-NI on Skylake AND Zen4). aesenc = ~4cyc lat, 1/cyc tput.
static inline uint64_t aes1(const char* p,size_t n){
    const __m128i K=_mm_set_epi64x(0x9E3779B97F4A7C15LL,0x2545F4914F6CDD1DLL);
    __m128i h=_mm_set_epi64x((long long)n,0xa0761d6478bd642fLL);
    size_t i=0; for(; i+16<=n; i+=16){ __m128i x=_mm_loadu_si128((const __m128i*)(p+i)); h=_mm_aesenc_si128(_mm_xor_si128(h,x),K); }
    if(n>i){ char buf[16]={0}; memcpy(buf,p+i,n-i); __m128i t=_mm_loadu_si128((const __m128i*)buf); h=_mm_aesenc_si128(_mm_xor_si128(h,t),K); }
    h=_mm_aesenc_si128(h,K); h=_mm_aesenc_si128(h,K); return (uint64_t)_mm_cvtsi128_si64(h);
}
// aes4: 4 independent AES chains interleaved (ILP → ~1 aesenc/cyc; the portable stand-in for VAES 4-lane)
static inline void aes4(const char*p0,uint32_t n0,const char*p1,uint32_t n1,const char*p2,uint32_t n2,const char*p3,uint32_t n3,uint64_t*o){
    const __m128i K=_mm_set_epi64x(0x9E3779B97F4A7C15LL,0x2545F4914F6CDD1DLL);
    __m128i h0=_mm_set_epi64x(n0,0xa0761d6478bd642fLL),h1=_mm_set_epi64x(n1,0xa0761d6478bd642fLL),
            h2=_mm_set_epi64x(n2,0xa0761d6478bd642fLL),h3=_mm_set_epi64x(n3,0xa0761d6478bd642fLL);
    uint32_t m=n0; if(n1>m)m=n1; if(n2>m)m=n2; if(n3>m)m=n3;
    for(uint32_t i=0;i+16<=m;i+=16){
        if(i+16<=n0){ __m128i x=_mm_loadu_si128((const __m128i*)(p0+i)); h0=_mm_aesenc_si128(_mm_xor_si128(h0,x),K);}
        if(i+16<=n1){ __m128i x=_mm_loadu_si128((const __m128i*)(p1+i)); h1=_mm_aesenc_si128(_mm_xor_si128(h1,x),K);}
        if(i+16<=n2){ __m128i x=_mm_loadu_si128((const __m128i*)(p2+i)); h2=_mm_aesenc_si128(_mm_xor_si128(h2,x),K);}
        if(i+16<=n3){ __m128i x=_mm_loadu_si128((const __m128i*)(p3+i)); h3=_mm_aesenc_si128(_mm_xor_si128(h3,x),K);}
    }
    auto tail=[&](const char*p,uint32_t n,__m128i&h){ uint32_t i=n&~15u; if(n>i){ char b[16]={0}; memcpy(b,p+i,n-i); __m128i t=_mm_loadu_si128((const __m128i*)b); h=_mm_aesenc_si128(_mm_xor_si128(h,t),K);} h=_mm_aesenc_si128(h,K); h=_mm_aesenc_si128(h,K); };
    tail(p0,n0,h0); tail(p1,n1,h1); tail(p2,n2,h2); tail(p3,n3,h3);
    o[0]=_mm_cvtsi128_si64(h0); o[1]=_mm_cvtsi128_si64(h1); o[2]=_mm_cvtsi128_si64(h2); o[3]=_mm_cvtsi128_si64(h3);
}

int main(int argc,char**argv){
    const char* manifest=nullptr; for(int i=1;i<argc;i++) if(!strcmp(argv[i],"--manifest")&&i+1<argc) manifest=argv[++i];
    if(!manifest){ fprintf(stderr,"usage: hash-micro --manifest F\n"); return 2; }
    FILE* mf=fopen(manifest,"r"); if(!mf){perror("m");return 2;} char line[8192]; std::vector<std::string> srcs; size_t raw=0;
    while(fgets(line,sizeof line,mf)){ size_t L=strlen(line); while(L&&(line[L-1]=='\n'||line[L-1]=='\r'))line[--L]=0; if(!L)continue;
        FILE* tf=fopen(line,"rb"); if(!tf)continue; std::string s; fseek(tf,0,SEEK_END); long sz=ftell(tf); fseek(tf,0,SEEK_SET);
        if(sz>0){ s.resize(sz); if(fread(&s[0],1,sz,tf)!=(size_t)sz)s.clear();} fclose(tf); if(!s.empty()){raw+=s.size();srcs.push_back(std::move(s));} }
    fclose(mf);
    std::vector<const char*> rp; std::vector<uint32_t> rn; rp.reserve(140000000); rn.reserve(140000000);
    for(auto&s:srcs){ size_t i=0,n=s.size(); while(i<n){ size_t j=s.find('\n',i); size_t e=(j==std::string::npos)?n:j+1; rp.push_back(s.data()+i); rn.push_back((uint32_t)(e-i)); i=e; } }
    size_t M=rp.size(); double mb=raw/1048576.0;
    fprintf(stderr,"lines=%zu raw=%.1f MB\n",M,mb);
    for(int rep=0;rep<3;rep++){
      { auto t0=clk::now(); uint64_t s=0; for(size_t k=0;k<M;k++) s^=h0(rp[k],rn[k]); double dt=s_since(t0);
        fprintf(stderr,"  h0 FNV    : %.2f GB/s  %.1f ns/line  (s=%llx)\n",mb/1024.0/dt,dt*1e9/M,(unsigned long long)s); }
      { auto t0=clk::now(); uint64_t s=0; for(size_t k=0;k<M;k++) s^=h3(rp[k],rn[k]); double dt=s_since(t0);
        fprintf(stderr,"  h3 mulfold: %.2f GB/s  %.1f ns/line  (s=%llx)\n",mb/1024.0/dt,dt*1e9/M,(unsigned long long)s); }
      { auto t0=clk::now(); uint64_t s=0; for(size_t k=0;k<M;k++) s^=rapid(rp[k],rn[k]); double dt=s_since(t0);
        fprintf(stderr,"  rapidhash : %.2f GB/s  %.1f ns/line  (s=%llx)\n",mb/1024.0/dt,dt*1e9/M,(unsigned long long)s); }
      { auto t0=clk::now(); uint64_t s=0; for(size_t k=0;k<M;k++) s^=aes1(rp[k],rn[k]); double dt=s_since(t0);
        fprintf(stderr,"  aes1 AESNI: %.2f GB/s  %.1f ns/line  (s=%llx)\n",mb/1024.0/dt,dt*1e9/M,(unsigned long long)s); }
      { auto t0=clk::now(); uint64_t s=0,o[4]; size_t k=0; for(;k+4<=M;k+=4){ aes4(rp[k],rn[k],rp[k+1],rn[k+1],rp[k+2],rn[k+2],rp[k+3],rn[k+3],o); s^=o[0]^o[1]^o[2]^o[3]; } for(;k<M;k++) s^=aes1(rp[k],rn[k]); double dt=s_since(t0);
        fprintf(stderr,"  aes4 4-lane: %.2f GB/s  %.1f ns/line  (s=%llx)\n",mb/1024.0/dt,dt*1e9/M,(unsigned long long)s); }
      fprintf(stderr,"  ---\n");
    }
    return 0;
}
