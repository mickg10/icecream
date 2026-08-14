// ========= PRODUCTION_FUSED concurrency: many simultaneous TUs (the real flow) =========
// Single-stream ping-pong hides the truth: production runs many compile jobs at once and
// the daemon forks per job. Model it faithfully: build the WARM dict + WARM F-store once,
// then fork N workers. Copy-on-write makes them SHARE the read-only warm dictionary and F
// store with zero copies and zero races (separate processes) — each worker runs the full
// in-process fused codec (intern -> KEYSET/MISSING/BODY -> zstd -> reconstruct -> verify)
// over its own byte-balanced slice of TUs. Aggregate throughput = raw / wall-time, vs N.
#include <sys/wait.h>
#include <sched.h>
#if defined(__has_include) && __has_include(<zstd.h>)
#include <zstd.h>
#else
extern "C" { size_t ZSTD_compress(void*,size_t,const void*,size_t,int); size_t ZSTD_decompress(void*,size_t,const void*,size_t); size_t ZSTD_compressBound(size_t); unsigned ZSTD_isError(size_t); }
#endif

struct FStore {
    std::vector<uint8_t> has; std::vector<uint32_t> foff, flen; std::vector<char> fb;
    void ensure(uint32_t d){ size_t need=size_t(d)+1; if(has.size()<need){ has.resize(need,0); foff.resize(need,0); flen.resize(need,0);} }
    bool have(uint32_t g) const { return g<has.size() && has[g]; }
    void add(uint32_t g,const char*p,uint32_t n){ ensure(g); if(has[g])return; uint32_t o=(uint32_t)fb.size(); fb.insert(fb.end(),p,p+n); foff[g]=o; flen[g]=n; has[g]=1; }
};
static inline void putv(std::vector<uint8_t>&b,uint64_t v){ while(v>=0x80){ b.push_back(uint8_t(v)|0x80); v>>=7;} b.push_back(uint8_t(v)); }
static inline uint64_t getv(const uint8_t*&p){ uint64_t v=0;int s=0;for(;;){uint8_t c=*p++;v|=uint64_t(c&0x7f)<<s;if(!(c&0x80))break;s+=7;}return v; }

// one worker (a forked process): full fused codec over TU range [lo,hi). Returns 0 on exact PASS.
static int worker(Interner& dict, const Corpus& corpus, const FStore& F, size_t lo, size_t hi, int level, uint32_t distinct, int core){
    if(core>=0){ cpu_set_t s; CPU_ZERO(&s); CPU_SET(core,&s); sched_setaffinity(0,sizeof s,&s); }
    uint32_t ml=0; for(size_t i=lo;i<hi;i++) ml=std::max(ml,corpus.files[i].len);
    std::vector<uint32_t> out(size_t(ml)+1), used_g, local_of(size_t(distinct)+1,0), stamp(size_t(distinct)+1,0);
    std::vector<uint8_t> body,dbody,comp,miss; std::vector<const char*> fptr; std::vector<uint32_t> fln; std::string recon;
    uint32_t epoch=0;
    for(size_t fi=lo; fi<hi; fi++){ const auto&f=corpus.files[fi]; const char* src=corpus.bytes.data()+f.off; uint32_t flen=f.len;
        size_t occ=0; uint64_t h=0; dict.process(src,src+flen,out.data(),occ,h,false);
        ++epoch; used_g.clear();
        for(size_t i=0;i<occ;i++){ uint32_t g=out[i]; if(stamp[g]!=epoch){ stamp[g]=epoch; local_of[g]=(uint32_t)used_g.size(); used_g.push_back(g);} }
        size_t nloc=used_g.size(); miss.assign(nloc,0); uint64_t nmiss=0;
        for(size_t i=0;i<nloc;i++) if(!F.have(used_g[i])){ miss[i]=1; nmiss++; }
        body.clear(); putv(body,nloc); putv(body,nmiss); putv(body,occ); putv(body,flen);
        for(size_t i=0;i<nloc;i++) if(miss[i]){ const LineRef&r=dict.ref(used_g[i]); putv(body,i); putv(body,r.len); const char* lb=dict.line_data(r.off); body.insert(body.end(),(const uint8_t*)lb,(const uint8_t*)lb+r.len);}
        for(size_t i=0;i<occ;i++) putv(body, local_of[out[i]]);
        size_t cap=ZSTD_compressBound(body.size()); if(comp.size()<cap)comp.resize(cap);
        size_t csz=ZSTD_compress(comp.data(),cap,body.data(),body.size(),level); if(ZSTD_isError(csz)) return 2;
        if(dbody.size()<body.size())dbody.resize(body.size());
        size_t dsz=ZSTD_decompress(dbody.data(),dbody.size(),comp.data(),csz); if(ZSTD_isError(dsz)||dsz!=body.size()) return 2;
        const uint8_t* pp=dbody.data(); uint64_t Rn=getv(pp),Rm=getv(pp),Ro=getv(pp),Rl=getv(pp); (void)Rl;
        fptr.assign(Rn,nullptr); fln.assign(Rn,0);
        for(uint64_t m=0;m<Rm;m++){ uint64_t lid=getv(pp),ln=getv(pp); const char* bp=(const char*)pp; pp+=ln; fptr[lid]=bp; fln[lid]=(uint32_t)ln; }
        for(uint64_t i=0;i<Rn;i++) if(!fptr[i]){ uint32_t g=used_g[i]; fptr[i]=F.fb.data()+F.foff[g]; fln[i]=F.flen[g]; }
        recon.clear(); for(uint64_t i=0;i<Ro;i++){ uint64_t lid=getv(pp); recon.append(fptr[lid], fln[lid]); }
        if(recon.size()!=flen || memcmp(recon.data(),src,flen)!=0) return 1;
    }
    return 0;
}

int main(int argc,char**argv){
    const char* manifest=nullptr; size_t maxf=SIZE_MAX; int level=3; const char* nlist="1,2,4,8,16";
    for(int i=1;i<argc;i++){ if(!strcmp(argv[i],"--manifest")&&i+1<argc)manifest=argv[++i];
        else if(!strcmp(argv[i],"--max-files")&&i+1<argc)maxf=strtoull(argv[++i],0,10);
        else if(!strcmp(argv[i],"--level")&&i+1<argc)level=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--workers")&&i+1<argc)nlist=argv[++i]; }
    if(!manifest){ fprintf(stderr,"usage: fused-concurrent --manifest F [--level L] [--workers 1,2,4,8,16]\n"); return 2; }
    Corpus corpus=load_corpus(manifest,maxf);
    Interner dict; uint32_t ml=0; for(auto&f:corpus.files) ml=std::max(ml,f.len);
    { std::vector<uint32_t> out(size_t(ml)+1); for(auto&f:corpus.files){ size_t oc=0; uint64_t h=0; dict.process(corpus.bytes.data()+f.off,corpus.bytes.data()+f.off+f.len,out.data(),oc,h,true); expose_output(out.data(),oc);} }
    uint32_t distinct=dict.distinct();
    FStore F; F.ensure(distinct); for(uint32_t g=1;g<=distinct;g++){ const LineRef&r=dict.ref(g); F.add(g,dict.line_data(r.off),r.len);}   // warm F
    fprintf(stderr,"FUSED concurrent (warm dict+F, fork-per-worker COW) TUs=%zu raw=%llu distinct=%u level=%d\n",
        corpus.files.size(),(unsigned long long)corpus.raw,distinct,level);
    // cumulative byte prefix for balanced partition
    std::vector<uint64_t> cum(corpus.files.size()+1,0); for(size_t i=0;i<corpus.files.size();i++) cum[i+1]=cum[i]+corpus.files[i].len;
    double raw=corpus.raw/1e9;
    for(const char* q=nlist; q && *q; ){ int N=atoi(q); const char* c=strchr(q,','); q=c?c+1:nullptr; if(N<1)continue;
        std::vector<pid_t> pids; auto t0=Clock::now();
        for(int w=0; w<N; w++){ size_t lo=0,hi=corpus.files.size();
            uint64_t a=corpus.raw*w/N, b=corpus.raw*(w+1)/N;
            lo=std::lower_bound(cum.begin(),cum.end(),a)-cum.begin(); hi=std::lower_bound(cum.begin(),cum.end(),b)-cum.begin();
            pid_t pid=fork(); if(pid==0){ _exit(worker(dict,corpus,F,lo,hi,level,distinct, w)); } pids.push_back(pid); }
        bool ok=true; for(pid_t p:pids){ int st=0; waitpid(p,&st,0); if(!(WIFEXITED(st)&&WEXITSTATUS(st)==0)) ok=false; }
        double dt=seconds_since(t0);
        fprintf(stderr,"  N=%-2d workers: %.3f s  aggregate %.2f GB/s  (%.2f GB/s/worker)  verify=%s\n",
            N, dt, raw/dt, raw/dt/N, ok?"PASS":"FAIL");
    }
    return 0;
}
