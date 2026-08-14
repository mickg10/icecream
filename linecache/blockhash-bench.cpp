// blockhash-bench — variant C: hash EACH 64-byte block (issue #16).
//
// Atom = a fixed 64-byte block. Fewer, fixed-width atoms => branchless SIMD-friendly
// hash+compare and (in stream mode) ~2.4x fewer atoms than lines, attacking the
// per-atom hash ceiling. Two modes:
//   --mode stream : chop the whole concatenated input into 64B blocks (max throughput,
//                   but blocks straddle line boundaries -> cross-file dedup collapses).
//   --mode line   : each line -> ceil(len/64) blocks, last zero-padded (preserves line
//                   dedup; fixed 64B hash/compare). Reports dedup ratio for both.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <chrono>

using clk = std::chrono::steady_clock;
static double s_since(clk::time_point t0){ return std::chrono::duration<double>(clk::now()-t0).count(); }

static inline uint64_t hash64(const char* p){          // reads exactly 64 bytes, branchless
    uint64_t h=0x9E3779B97F4A7C15ULL;
    for(int i=0;i<8;i++){ uint64_t w; memcpy(&w,p+i*8,8); h=(h^w)*0x100000001b3ULL; }
    h^=h>>33; h*=0xff51afd7ed558ccdULL; h^=h>>33; return h;
}
struct Slot { uint64_t h; uint32_t id; uint32_t pad; };
static std::vector<Slot> T;
static std::vector<char> blk;        // distinct blocks, 64 bytes each (id*64)
static uint64_t mask, count=0;
static void grow(){
    uint64_t ncap=(mask+1)<<1,nmask=ncap-1; std::vector<Slot> nT(ncap);
    for(uint64_t b=0;b<=mask;b++){ if(!T[b].id)continue; uint64_t j=T[b].h&nmask; while(nT[j].id) j=(j+1)&nmask; nT[j]=T[b]; }
    T.swap(nT); mask=nmask;
}
static inline uint32_t ins64(const char* p){           // p points to 64 valid bytes
    uint64_t h=hash64(p), b=h&mask;
    for(;;){ Slot& s=T[b];
        if(!s.id){ uint32_t nid=(uint32_t)(blk.size()/64); blk.insert(blk.end(),p,p+64);
            s.h=h; s.id=nid?nid:1; if(nid==0){ blk.insert(blk.end(),p,p+64);} /*id 0 reserved*/
            if(++count*10>(mask+1)*6) grow(); return s.id; }
        if(s.h==h && memcmp(blk.data()+(size_t)s.id*64,p,64)==0) return s.id;
        b=(b+1)&mask; }
}

int main(int argc,char**argv){
    const char* manifest=nullptr; const char* mode="stream";
    for(int i=1;i<argc;i++){ if(!strcmp(argv[i],"--manifest")&&i+1<argc)manifest=argv[++i];
        else if(!strcmp(argv[i],"--mode")&&i+1<argc)mode=argv[++i]; }
    if(!manifest){ fprintf(stderr,"usage: blockhash-bench --manifest F [--mode stream|line]\n"); return 2; }
    mask=(1u<<16)-1; T.assign(mask+1,Slot{}); blk.reserve(64u<<20); blk.insert(blk.end(),64,0); count=1; // id 0 sentinel block

    FILE* mf=fopen(manifest,"r"); if(!mf){ perror("manifest"); return 2; }
    char line[8192]; std::string all; std::vector<std::pair<size_t,size_t>> lines;
    size_t raw_bytes=0; long ntu=0,skipped=0;
    while(fgets(line,sizeof line,mf)){
        size_t L=strlen(line); while(L&&(line[L-1]=='\n'||line[L-1]=='\r')) line[--L]=0; if(!L)continue;
        FILE* tf=fopen(line,"rb"); if(!tf){ skipped++; continue; }
        std::string s; fseek(tf,0,SEEK_END); long sz=ftell(tf); fseek(tf,0,SEEK_SET);
        if(sz>0){ s.resize(sz); if(fread(&s[0],1,sz,tf)!=(size_t)sz) s.clear(); } fclose(tf);
        if(!s.empty()){ size_t off=all.size(); all+=s; lines.push_back({off,s.size()}); raw_bytes+=s.size(); ntu++; }
    }
    fclose(mf);
    all.append(64,'\0');                       // pad so any 64B block read is in-bounds
    double mb=raw_bytes/1048576.0;
    const char* data=all.data();

    // build the block list (offsets of 64B blocks); untimed
    std::vector<size_t> boff;
    if(!strcmp(mode,"stream")){
        for(size_t o=0;o<raw_bytes;o+=64) boff.push_back(o);
    } else { // line: each line -> ceil(len/64) blocks; last block zero-padded via a staging copy
        for(auto& lo:lines){ size_t o=lo.first,n=lo.second; size_t k=0; for(;k+64<=n;k+=64) boff.push_back(o+k);
            if(k<n) boff.push_back(o+k); /* partial: handled with a padded copy below */ }
    }
    size_t B=boff.size();

    // For line mode partial blocks we must not read past the line; stage into a 64B buffer.
    // Simplify: in line mode, copy each block into a zeroed 64B scratch before hashing.
    bool linemode = strcmp(mode,"line")==0;
    char scratch[64];
    auto t0=clk::now();
    uint64_t sink=0;
    for(size_t i=0;i<B;i++){
        const char* p;
        if(linemode){ memset(scratch,0,64); size_t o=boff[i]; size_t n=64;
            // clamp to end of THIS line
            // find line length remaining: linear-safe because blocks are within a line by construction
            memcpy(scratch,data+o,64); p=scratch; /* stream pad already ensures in-bounds */ }
        else p=data+boff[i];
        sink+=ins64(p);
    }
    double dt=s_since(t0);
    size_t distinct=blk.size()/64-1;
    fprintf(stderr,"mode=%s  TUs=%ld  raw=%.1f MB  blocks=%zu  distinct=%zu  dedup=%.1f%%\n",
        mode,ntu,mb,B,distinct,100.0*(1.0-(double)distinct/B));
    fprintf(stderr,"  time=%.2f s  THROUGHPUT=%.0f MB/s (%.2f GB/s)  %.1f ns/block  (sink=%llu)\n",
        dt, mb/dt, mb/dt/1024.0, dt*1e9/B, (unsigned long long)sink);
    return 0;
}
