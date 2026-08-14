// batchhash-bench — variant C + software-pipelined prefetched probing (issue #16).
//
// The wall (measured): even hot & fully cache-resident, per-line dedup sits ~0.55 GB/s
// because each line costs TWO dependent random accesses — table slot, then arena bytes.
// That is latency-bound. Fix: process a WINDOW of lines in stages so the CPU has many
// independent misses in flight (software pipelining + __builtin_prefetch):
//   stage 1: hash line, prefetch its table slot
//   stage 2: read slot; if first-slot hash matches, prefetch its arena bytes
//   stage 3: confirm with memcmp (byte-exact); rare collisions fall back to serial probe
// Combined 16-byte slot {hash,id} so one prefetch pulls both. Cold builds serially; we
// then time HOT-serial vs HOT-batched to isolate the pipelining win.
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
#ifndef WIN
#define WIN 16
#endif

using clk = std::chrono::steady_clock;
static double s_since(clk::time_point t0){ return std::chrono::duration<double>(clk::now()-t0).count(); }

static inline uint64_t rotl(uint64_t x,int r){ return (x<<r)|(x>>(64-r)); }
static inline uint64_t hashln(const char* p, size_t n){
#if HASHV==0
    uint64_t h=0x9E3779B97F4A7C15ULL ^ (n*0xff51afd7ed558ccdULL);
    size_t i=0; for(;i+8<=n;i+=8){ uint64_t w; memcpy(&w,p+i,8); h=(h^w)*1099511628211ULL; }
    if(i<n){ uint64_t w=0; memcpy(&w,p+i,n-i); h=(h^w)*1099511628211ULL; }
    h^=h>>33; h*=0xff51afd7ed558ccdULL; h^=h>>33; return h;
#else
    uint64_t a=n,b=n; if(n>=8){ memcpy(&a,p,8); memcpy(&b,p+n-8,8);} else { memcpy(&a,p,n); b=a; }
    uint64_t h=(a ^ rotl(b,32) ^ (n*0x9E3779B97F4A7C15ULL)); h*=0xff51afd7ed558ccdULL; h^=h>>29; return h;
#endif
}

struct Slot { uint64_t h; uint32_t id; uint32_t pad; };   // 16 bytes; 4 per cache line
static std::vector<Slot> T;
static std::vector<char> arena;
static std::vector<uint32_t> aoff, alen;
static uint64_t mask, count=0;

static void grow(){
    uint64_t ncap=(mask+1)<<1, nmask=ncap-1; std::vector<Slot> nT(ncap);
    for(uint64_t b=0;b<=mask;b++){ if(!T[b].id)continue; uint64_t j=T[b].h&nmask; while(nT[j].id) j=(j+1)&nmask; nT[j]=T[b]; }
    T.swap(nT); mask=nmask;
}
static inline uint32_t ins(const char* a, size_t n){        // serial insert-or-lookup (cold build)
    uint64_t h=hashln(a,n), b=h&mask;
    for(;;){ Slot& s=T[b];
        if(!s.id){ uint32_t off=(uint32_t)arena.size(); arena.insert(arena.end(),a,a+n);
            aoff.push_back(off); alen.push_back((uint32_t)n); uint32_t nid=(uint32_t)aoff.size()-1;
            s.h=h; s.id=nid; if(++count*10>(mask+1)*6) grow(); return nid; }
        if(s.h==h && alen[s.id]==n && memcmp(arena.data()+aoff[s.id],a,n)==0) return s.id;
        b=(b+1)&mask; }
}
static inline uint32_t lookup_serial(const char* a, size_t n){   // all-hit probe
    uint64_t h=hashln(a,n), b=h&mask;
    for(;;){ Slot& s=T[b]; if(s.h==h && alen[s.id]==n && memcmp(arena.data()+aoff[s.id],a,n)==0) return s.id; b=(b+1)&mask; }
}

int main(int argc, char** argv){
    const char* manifest=nullptr;
    for(int i=1;i<argc;i++) if(!strcmp(argv[i],"--manifest")&&i+1<argc) manifest=argv[++i];
    if(!manifest){ fprintf(stderr,"usage: batchhash-bench --manifest F\n"); return 2; }
    mask=(1u<<16)-1; T.assign(mask+1,Slot{}); arena.reserve(64u<<20); aoff.push_back(0); alen.push_back(0);

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

    // COLD build (serial) + collect line refs (untimed)
    std::vector<const char*> rp; std::vector<uint32_t> rn; rp.reserve(3000000); rn.reserve(3000000);
    auto tc=clk::now();
    for(auto& s:srcs){ size_t i=0,n=s.size(); while(i<n){ size_t j=s.find('\n',i); size_t end=(j==std::string::npos)?n:j+1;
        rp.push_back(s.data()+i); rn.push_back((uint32_t)(end-i)); ins(s.data()+i,end-i); i=end; } }
    double dtc=s_since(tc);
    size_t M=rp.size();
    fprintf(stderr,"HASHV=%d WIN=%d  TUs=%ld  raw=%.1f MB  lines=%zu  distinct=%zu\n",HASHV,WIN,ntu,mb,M,aoff.size()-1);
    fprintf(stderr,"  COLD serial     : %.2f s  %.0f MB/s  (%.2f GB/s)\n", dtc, mb/dtc, mb/dtc/1024.0);

    volatile uint64_t sink=0;
    // HOT serial
    { auto t0=clk::now(); uint64_t s=0; for(size_t k=0;k<M;k++) s+=lookup_serial(rp[k],rn[k]); double dt=s_since(t0);
      sink+=s; fprintf(stderr,"  HOT serial      : %.2f s  %.0f MB/s  (%.2f GB/s)\n", dt, mb/dt, mb/dt/1024.0); }

    // HOT batched (software-pipelined, prefetched)
    { auto t0=clk::now(); uint64_t s=0;
      uint64_t hv[WIN], bk[WIN]; uint32_t id[WIN]; unsigned char st[WIN];
      for(size_t base=0; base<M; base+=WIN){
        int w=(int)std::min((size_t)WIN, M-base);
        for(int k=0;k<w;k++){ hv[k]=hashln(rp[base+k],rn[base+k]); bk[k]=hv[k]&mask; __builtin_prefetch(&T[bk[k]],0,3); }
        for(int k=0;k<w;k++){ Slot sl=T[bk[k]]; if(sl.h==hv[k]){ id[k]=sl.id; st[k]=1; __builtin_prefetch(arena.data()+aoff[sl.id],0,3);} else st[k]=0; }
        for(int k=0;k<w;k++){
          uint32_t n=rn[base+k]; const char* p=rp[base+k];
          if(st[k] && alen[id[k]]==n && memcmp(arena.data()+aoff[id[k]],p,n)==0){ s+=id[k]; continue; }
          s+=lookup_serial(p,n);   // collision / non-first-slot: correct fallback
        }
      }
      double dt=s_since(t0); sink+=s;
      fprintf(stderr,"  HOT batched(pf) : %.2f s  %.0f MB/s  (%.2f GB/s)\n", dt, mb/dt, mb/dt/1024.0); }
    fprintf(stderr,"  (sink=%llu)\n",(unsigned long long)sink);
    return 0;
}
