// linehash-bench — variant C(line): flat open-addressed hash of whole lines (issue #16).
//
// No tree, no descents: hash each line once, flat open-addressed probe (contiguous =
// cache-friendly), stored 64-bit hash rejects before the (SIMD) memcmp. Atom bytes in
// an append-only arena; decode = arena[off..off+len].
//
// Build 3 hash variants:  -DHASHV=0 murmur-finalize (strong) | 1 FNV no-finalize |
// 2 first8^rot(last8)^len (one multiply, position-aware to dodge indentation collisions).
// --passes N times the loop: pass 1 = COLD (all inserts), pass 2+ = HOT (all hits).
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <chrono>

#ifndef HASHV
#define HASHV 0
#endif

using clk = std::chrono::steady_clock;
static double s_since(clk::time_point t0){ return std::chrono::duration<double>(clk::now()-t0).count(); }

static inline uint64_t rotl(uint64_t x,int r){ return (x<<r)|(x>>(64-r)); }
static inline uint64_t hashln(const char* p, size_t n){
#if HASHV==0
    uint64_t h = 0x9E3779B97F4A7C15ULL ^ (n*0xff51afd7ed558ccdULL);
    size_t i=0; for (; i+8<=n; i+=8){ uint64_t w; memcpy(&w,p+i,8); h=(h^w)*1099511628211ULL; }
    if (i<n){ uint64_t w=0; memcpy(&w,p+i,n-i); h=(h^w)*1099511628211ULL; }
    h^=h>>33; h*=0xff51afd7ed558ccdULL; h^=h>>33; return h;
#elif HASHV==1
    uint64_t h = 1469598103934665603ULL ^ (n*1099511628211ULL);
    size_t i=0; for (; i+8<=n; i+=8){ uint64_t w; memcpy(&w,p+i,8); h=(h^w)*1099511628211ULL; }
    if (i<n){ uint64_t w=0; memcpy(&w,p+i,n-i); h=(h^w)*1099511628211ULL; }
    return h;   // no finalize
#else
    // cheap: mix first 8 and last 8 bytes + length, one multiply. Position-aware so
    // "        foo" vs "        bar" (shared indentation prefix) differ via the tail.
    uint64_t a=n, b=n;
    if (n>=8){ memcpy(&a,p,8); memcpy(&b,p+n-8,8); }
    else { memcpy(&a,p,n); b=a; }
    uint64_t h=(a ^ rotl(b,32) ^ (n*0x9E3779B97F4A7C15ULL));
    h*=0xff51afd7ed558ccdULL; h^=h>>29; return h;
#endif
}

static std::vector<char>     arena;
static std::vector<uint32_t> aoff, alen;
static std::vector<uint64_t> H;
static std::vector<uint32_t> ID;
static uint64_t mask, count=0, g_probes=0;

static void grow(){
    uint64_t ncap=(mask+1)<<1, nmask=ncap-1;
    std::vector<uint64_t> nH(ncap,0); std::vector<uint32_t> nID(ncap,0);
    for (uint64_t b=0;b<=mask;b++){ uint32_t id=ID[b]; if(!id)continue; uint64_t h=H[b],j=h&nmask;
        while(nID[j]) j=(j+1)&nmask; nH[j]=h; nID[j]=id; }
    H.swap(nH); ID.swap(nID); mask=nmask;
}
static inline uint32_t ins(const char* a, size_t n){
    uint64_t h=hashln(a,n), b=h&mask;
    for (;;){
        uint32_t id=ID[b];
        if(!id){ uint32_t off=(uint32_t)arena.size(); arena.insert(arena.end(),a,a+n);
            aoff.push_back(off); alen.push_back((uint32_t)n); uint32_t nid=(uint32_t)aoff.size()-1;
            H[b]=h; ID[b]=nid; if(++count*10 > (mask+1)*6) grow(); return nid; }
        g_probes++;
        if(H[b]==h && alen[id]==n && memcmp(arena.data()+aoff[id],a,n)==0) return id;
        b=(b+1)&mask;
    }
}

int main(int argc, char** argv){
    const char* manifest=nullptr; int passes=1; bool verify=false;
    for(int i=1;i<argc;i++){
        if(!strcmp(argv[i],"--manifest")&&i+1<argc) manifest=argv[++i];
        else if(!strcmp(argv[i],"--passes")&&i+1<argc) passes=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--verify")) verify=true;
    }
    if(!manifest){ fprintf(stderr,"usage: linehash-bench --manifest F [--passes N] [--verify]\n"); return 2; }
    mask=(1u<<16)-1; H.assign(mask+1,0); ID.assign(mask+1,0);
    arena.reserve(64u<<20); aoff.push_back(0); alen.push_back(0);

    FILE* mf=fopen(manifest,"r"); if(!mf){ perror("manifest"); return 2; }
    char line[8192]; std::vector<std::string> srcs; size_t raw_bytes=0,occ=0; long ntu=0,skipped=0;
    while(fgets(line,sizeof line,mf)){
        size_t L=strlen(line); while(L&&(line[L-1]=='\n'||line[L-1]=='\r')) line[--L]=0; if(!L)continue;
        FILE* tf=fopen(line,"rb"); if(!tf){ skipped++; continue; }
        std::string s; fseek(tf,0,SEEK_END); long sz=ftell(tf); fseek(tf,0,SEEK_SET);
        if(sz>0){ s.resize(sz); if(fread(&s[0],1,sz,tf)!=(size_t)sz) s.clear(); } fclose(tf);
        if(!s.empty()){ raw_bytes+=s.size(); srcs.push_back(std::move(s)); ntu++; }
    }
    fclose(mf);
    double mb=raw_bytes/1048576.0;
    fprintf(stderr,"HASHV=%d  TUs=%ld skipped=%ld  raw=%.1f MB\n",HASHV,ntu,skipped,mb);

    long verify_fail=0; std::string chk;
    for(int p=0;p<passes;p++){
        uint64_t p0=g_probes; occ=0;
        auto t0=clk::now();
        for(auto& s:srcs){ size_t i=0,n=s.size();
            while(i<n){ size_t j=s.find('\n',i); size_t end=(j==std::string::npos)?n:j+1;
                uint32_t id=ins(s.data()+i,end-i); occ++;
                if(verify&&p==0){ chk.assign(arena.data()+aoff[id],alen[id]);
                    if(chk.size()!=end-i||memcmp(chk.data(),s.data()+i,end-i)!=0) verify_fail++; }
                i=end; } }
        double dt=s_since(t0);
        fprintf(stderr,"  pass %d (%s): %.2f s  %.0f MB/s  (%.2f GB/s)  probes/lookup=%.3f  distinct=%zu\n",
            p, p==0?"COLD":"HOT", dt, mb/dt, mb/dt/1024.0, (g_probes-p0)/(double)occ, aoff.size()-1);
    }
    if(verify) fprintf(stderr,"  verify_fail=%ld\n",verify_fail);
    return verify_fail?1:0;
}
