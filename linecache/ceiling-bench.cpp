// ceiling-bench — variant D + raw ceilings (issue #16).
//
// The goal is 3 GB/s single-thread over ALL llvm input. Before choosing a dedup
// datastructure, establish what one thread can physically do on this box:
//   memcpy       : memory-bandwidth ceiling
//   byte-sum     : compute-bound linear scan ceiling
//   one-shot hash: hash the ENTIRE concatenated input in one call (variant D)
//   per-line hash: hash each line but do NO table work (isolates hashing from lookup)
// Any dedup structure must live under the lowest of these. If one-shot hash << 3 GB/s
// the target is hopeless for any hash scheme and E must find something cheaper.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <chrono>

using clk = std::chrono::steady_clock;
static double s_since(clk::time_point t0){ return std::chrono::duration<double>(clk::now()-t0).count(); }

static inline uint64_t hashln(const char* p, size_t n){
    uint64_t h = 0x9E3779B97F4A7C15ULL ^ (n*0xff51afd7ed558ccdULL);
    size_t i = 0;
    for (; i+8 <= n; i += 8){ uint64_t w; memcpy(&w,p+i,8); h = (h^w)*1099511628211ULL; }
    if (i < n){ uint64_t w=0; memcpy(&w,p+i,n-i); h = (h^w)*1099511628211ULL; }
    h ^= h>>33; h *= 0xff51afd7ed558ccdULL; h ^= h>>33;
    return h;
}

int main(int argc, char** argv){
    const char* manifest = argc>1 && strcmp(argv[1],"--manifest")==0 ? argv[2] : nullptr;
    if (!manifest){ fprintf(stderr,"usage: ceiling-bench --manifest F\n"); return 2; }

    FILE* mf = fopen(manifest,"r"); if(!mf){ perror("manifest"); return 2; }
    char line[8192];
    // concatenate all input into ONE contiguous buffer
    std::string all; std::vector<std::pair<size_t,size_t>> lines;  // (offset,len) per line
    size_t raw_bytes=0; long ntu=0, skipped=0;
    while (fgets(line,sizeof line,mf)){
        size_t L=strlen(line); while(L&&(line[L-1]=='\n'||line[L-1]=='\r')) line[--L]=0; if(!L) continue;
        FILE* tf=fopen(line,"rb"); if(!tf){ skipped++; continue; }
        std::string s; fseek(tf,0,SEEK_END); long sz=ftell(tf); fseek(tf,0,SEEK_SET);
        if(sz>0){ s.resize(sz); if(fread(&s[0],1,sz,tf)!=(size_t)sz) s.clear(); } fclose(tf);
        if(!s.empty()){ all += s; raw_bytes+=s.size(); ntu++; }
    }
    fclose(mf);
    // precompute line boundaries (excluded from timings)
    { size_t i=0,n=all.size(); while(i<n){ size_t j=all.find('\n',i); size_t end=(j==std::string::npos)?n:j+1; lines.push_back({i,end-i}); i=end; } }
    double mb = raw_bytes/1048576.0;
    const char* data = all.data(); size_t N = all.size();
    fprintf(stderr,"TUs=%ld skipped=%ld  raw=%.1f MB  lines=%zu\n", ntu, skipped, mb, lines.size());

    // --- memcpy ceiling ---
    { std::vector<char> dst(N);
      auto t0=clk::now(); memcpy(dst.data(), data, N); double dt=s_since(t0);
      volatile char sink=dst[N/2]; (void)sink;
      fprintf(stderr,"memcpy        : %.2f GB/s\n", mb/1024.0/dt); }
    // --- byte-sum scan ceiling ---
    { auto t0=clk::now(); uint64_t s=0; for(size_t i=0;i<N;i++) s+=(uint8_t)data[i]; double dt=s_since(t0);
      fprintf(stderr,"byte-sum scan : %.2f GB/s  (sink=%llu)\n", mb/1024.0/dt,(unsigned long long)s); }
    // --- D: one-shot hash of the ENTIRE input ---
    { auto t0=clk::now(); uint64_t h=hashln(data,N); double dt=s_since(t0);
      fprintf(stderr,"D one-shot hash: %.2f GB/s  (h=%016llx)\n", mb/1024.0/dt,(unsigned long long)h); }
    // --- per-line hash, NO table (isolate hashing cost from lookup) ---
    { auto t0=clk::now(); uint64_t h=0; for(auto&lo:lines) h^=hashln(data+lo.first,lo.second); double dt=s_since(t0);
      fprintf(stderr,"per-line hash : %.2f GB/s  (%.1f ns/line, h=%016llx)\n",
              mb/1024.0/dt, dt*1e9/lines.size(), (unsigned long long)h); }
    return 0;
}
