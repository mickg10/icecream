// ===== PRODUCTION_FUSED: pipelined C->F, no MISSING round-trip (warm path) =====
// Owner insight: don't wait for MISSING. F derives the miss-set from the KEYSET; when F has
// everything (steady state) MISSING is empty, so C sends KEYSET+occurrences in ONE frame and
// F reconstructs from its store immediately. The def round-trip is paid only for actual cold
// misses (answered after BODY). Warm = a one-way stream ⇒ latency hidden ⇒ link/CPU-bound.
// Frame (C->F): zstd([u32 nk][u32 keys...][u32 nocc][u32 occ_localids...]).  F->C: [u32 len][u64 digest].
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <sys/wait.h>
#include <errno.h>
#include <csignal>
#include <thread>
#if defined(__has_include) && __has_include(<zstd.h>)
#include <zstd.h>
#else
extern "C" { size_t ZSTD_compress(void*,size_t,const void*,size_t,int); size_t ZSTD_decompress(void*,size_t,const void*,size_t); size_t ZSTD_compressBound(size_t); unsigned ZSTD_isError(size_t); }
#endif
struct FStore { std::vector<uint8_t> has; std::vector<uint32_t> foff,flen; std::vector<char> fb;
    void ensure(uint32_t d){ size_t n=size_t(d)+1; if(has.size()<n){has.resize(n,0);foff.resize(n,0);flen.resize(n,0);} }
    void add(uint32_t g,const char*p,uint32_t n){ ensure(g); if(has[g])return; uint32_t o=(uint32_t)fb.size(); fb.insert(fb.end(),p,p+n); foff[g]=o; flen[g]=n; has[g]=1; } };
static inline uint64_t digest(const char*p,size_t n){ uint64_t h=0x100000001b3ULL^n; size_t i=0; for(;i+8<=n;i+=8){uint64_t w;memcpy(&w,p+i,8);h=(h^w)*0x9E3779B97F4A7C15ULL;h^=h>>29;} uint64_t t=0; if(i<n)memcpy(&t,p+i,n-i); h=(h^t)*0x9E3779B97F4A7C15ULL; return h^(h>>31); }
static bool wr(int fd,const void*p,size_t n){ const char*b=(const char*)p; while(n){ ssize_t w=write(fd,b,n); if(w<=0){ if(w<0&&errno==EINTR)continue; return false;} b+=w; n-=size_t(w);} return true; }
static bool rd(int fd,void*p,size_t n){ char*b=(char*)p; while(n){ ssize_t r=read(fd,b,n); if(r<=0){ if(r<0&&errno==EINTR)continue; return false;} b+=r; n-=size_t(r);} return true; }
static bool wrf(int fd,const void*p,uint32_t n){ return wr(fd,&n,4)&&wr(fd,p,n); }
static bool rdf(int fd,std::vector<uint8_t>&b){ uint32_t n; if(!rd(fd,&n,4))return false; b.resize(n); return rd(fd,b.data(),n); }
static Interner* build_dict(const Corpus& c){ Interner* d=new Interner(); uint32_t ml=0; for(auto&f:c.files) ml=std::max(ml,f.len);
    std::vector<uint32_t> out(size_t(ml)+1); for(auto&f:c.files){ size_t oc=0; uint64_t h=0; d->process(c.bytes.data()+f.off,c.bytes.data()+f.off+f.len,out.data(),oc,h,true); expose_output(out.data(),oc);} return d; }

static int f_handler(int c, const FStore& F){
    std::vector<uint8_t> fr, raw; std::string recon;
    for(;;){ if(!rdf(c,fr)) break; uint32_t rl; memcpy(&rl,fr.data(),4); if(raw.size()<rl)raw.resize(rl);
        if(ZSTD_isError(ZSTD_decompress(raw.data(),raw.size(),fr.data()+4,fr.size()-4))) return 3;
        const uint8_t* p=raw.data(); uint32_t nk; memcpy(&nk,p,4); p+=4; const uint32_t* keys=reinterpret_cast<const uint32_t*>(p); p+=size_t(nk)*4;
        uint32_t nocc; memcpy(&nocc,p,4); p+=4; const uint32_t* occ=reinterpret_cast<const uint32_t*>(p);
        recon.clear(); for(uint32_t i=0;i<nocc;i++){ uint32_t g=keys[occ[i]]; recon.append(F.fb.data()+F.foff[g], F.flen[g]); }
        uint32_t rlen=(uint32_t)recon.size(); uint64_t dg=digest(recon.data(),recon.size());
        uint8_t ack[12]; memcpy(ack,&rlen,4); memcpy(ack+4,&dg,8); if(!wr(c,ack,12)) break; }
    return 0;
}
int main(int argc,char**argv){
    const char* manifest=nullptr; const char* role=nullptr; const char* host="127.0.0.1"; int port=9200,clients=8,level=3; size_t maxf=SIZE_MAX;
    for(int i=1;i<argc;i++){ if(!strcmp(argv[i],"--manifest")&&i+1<argc)manifest=argv[++i];
        else if(!strcmp(argv[i],"--role")&&i+1<argc)role=argv[++i]; else if(!strcmp(argv[i],"--host")&&i+1<argc)host=argv[++i];
        else if(!strcmp(argv[i],"--port")&&i+1<argc)port=atoi(argv[++i]); else if(!strcmp(argv[i],"--clients")&&i+1<argc)clients=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--level")&&i+1<argc)level=atoi(argv[++i]); else if(!strcmp(argv[i],"--max-files")&&i+1<argc)maxf=strtoull(argv[++i],0,10); }
    if(!manifest||!role){ fprintf(stderr,"usage: ipc2 --role fserver|client --manifest F [--host H --port P --clients N]\n"); return 2; }
    Corpus corpus=load_corpus(manifest,maxf); Interner* dict=build_dict(corpus); uint32_t distinct=dict->distinct();
    if(!strcmp(role,"fserver")){
        FStore F; F.ensure(distinct); for(uint32_t g=1;g<=distinct;g++){ const LineRef&r=dict->ref(g); F.add(g,dict->line_data(r.off),r.len);}
        int s=socket(AF_INET,SOCK_STREAM,0); int one=1; setsockopt(s,SOL_SOCKET,SO_REUSEADDR,&one,4);
        sockaddr_in a{}; a.sin_family=AF_INET; a.sin_addr.s_addr=INADDR_ANY; a.sin_port=htons(port);
        if(bind(s,(sockaddr*)&a,sizeof a)){ perror("bind"); return 2;} listen(s,128);
        fprintf(stderr,"F-server(pipelined) ready port=%d distinct=%u\n",port,distinct); signal(SIGCHLD,SIG_IGN);
        for(;;){ int c=accept(s,nullptr,nullptr); if(c<0){if(errno==EINTR)continue;break;} int o=1; setsockopt(c,IPPROTO_TCP,TCP_NODELAY,&o,4);
            pid_t pid=fork(); if(pid==0){ close(s); _exit(f_handler(c,F)); } close(c); }
        return 0;
    }
    // client: precompute per-TU (keys, occ). Fork N; each streams frames (NO round-trip), then reads acks.
    std::vector<uint32_t> ks_flat, occ_flat; std::vector<size_t> ks_off(corpus.files.size()+1,0), occ_off(corpus.files.size()+1,0);
    uint32_t ml=0; for(auto&f:corpus.files) ml=std::max(ml,f.len);
    { std::vector<uint32_t> out(size_t(ml)+1), used_g, local_of(size_t(distinct)+1,0), stamp(size_t(distinct)+1,0); uint32_t ep=0;
      for(size_t fi=0;fi<corpus.files.size();++fi){ const auto&f=corpus.files[fi]; size_t occ=0; uint64_t h=0;
        dict->process(corpus.bytes.data()+f.off,corpus.bytes.data()+f.off+f.len,out.data(),occ,h,false);
        ++ep; used_g.clear(); for(size_t i=0;i<occ;i++){ uint32_t g=out[i]; if(stamp[g]!=ep){stamp[g]=ep; local_of[g]=(uint32_t)used_g.size(); used_g.push_back(g);} }
        ks_flat.insert(ks_flat.end(),used_g.begin(),used_g.end()); ks_off[fi+1]=ks_flat.size();
        for(size_t i=0;i<occ;i++) occ_flat.push_back(local_of[out[i]]); occ_off[fi+1]=occ_flat.size(); } }
    std::vector<uint64_t> tudig(corpus.files.size()); for(size_t fi=0;fi<corpus.files.size();++fi){ const auto&f=corpus.files[fi]; tudig[fi]=digest(corpus.bytes.data()+f.off,f.len); }
    std::vector<uint64_t> cum(corpus.files.size()+1,0); for(size_t i=0;i<corpus.files.size();i++) cum[i+1]=cum[i]+corpus.files[i].len;
    fprintf(stderr,"C-clients=%d -> F %s:%d distinct=%u (pipelined, no MISSING wait)\n",clients,host,port,distinct);
    auto client=[&](size_t lo,size_t hi,bool measure)->int{
        int s=socket(AF_INET,SOCK_STREAM,0); int o=1; setsockopt(s,IPPROTO_TCP,TCP_NODELAY,&o,4);
        sockaddr_in a{}; a.sin_family=AF_INET; a.sin_port=htons(port); inet_pton(AF_INET,host,&a.sin_addr);
        for(int t=0; connect(s,(sockaddr*)&a,sizeof a)&&t<6000;++t) usleep(2000);
        std::vector<uint8_t> raw,comp; double t_z=0,t_s=0,t_a=0; uint64_t praw=0,wire=0;
        for(size_t fi=lo; fi<hi; ++fi) praw+=corpus.files[fi].len;
        for(size_t fi=lo; fi<hi; ++fi){ uint32_t nk=(uint32_t)(ks_off[fi+1]-ks_off[fi]); uint32_t no=(uint32_t)(occ_off[fi+1]-occ_off[fi]);
            raw.resize(8+size_t(nk)*4+size_t(no)*4); uint8_t* p=raw.data(); memcpy(p,&nk,4); p+=4; memcpy(p,ks_flat.data()+ks_off[fi],size_t(nk)*4); p+=size_t(nk)*4;
            memcpy(p,&no,4); p+=4; memcpy(p,occ_flat.data()+occ_off[fi],size_t(no)*4);
            size_t cap=ZSTD_compressBound(raw.size()); if(comp.size()<cap+4)comp.resize(cap+4); uint32_t rl=(uint32_t)raw.size(); memcpy(comp.data(),&rl,4);
            auto z0=Clock::now(); size_t cz=ZSTD_compress(comp.data()+4,cap,raw.data(),raw.size(),level); t_z+=seconds_since(z0); if(ZSTD_isError(cz)){close(s);return 2;}
            auto s0=Clock::now(); if(!wrf(s,comp.data(),(uint32_t)(cz+4))){ close(s); return 4; } t_s+=seconds_since(s0); wire+=cz+4; }
        auto a0=Clock::now();
        for(size_t fi=lo; fi<hi; ++fi){ uint8_t ack[12]; if(!rd(s,ack,12)){ close(s); return 4; } uint32_t rl; uint64_t dg; memcpy(&rl,ack,4); memcpy(&dg,ack+4,8);
            if(rl!=corpus.files[fi].len || dg!=tudig[fi]){ fprintf(stderr,"FAIL fi=%zu rl=%u/%u dg=%llx/%llx\n",fi,rl,corpus.files[fi].len,(unsigned long long)dg,(unsigned long long)tudig[fi]); close(s); return 1; } }
        t_a+=seconds_since(a0);
        if(measure) fprintf(stderr,"  1-client breakdown: zstd %.2f GB/s | send %.2f GB/s | ack-wait %.2fs | wire=%.0fMB ratio=%.1fx (praw=%.0fMB)\n",
            praw/1e9/t_z, t_s>0?praw/1e9/t_s:0, t_a, wire/1e6, praw/double(wire), praw/1e6);
        close(s); return 0;
    };
    if(clients==1){ int rc=client(0,corpus.files.size(),true);
        fprintf(stderr,"IPC2 clients=1: verify=%s\n", rc==0?"PASS":"FAIL"); return rc; }
    auto t0=Clock::now(); std::vector<pid_t> pids;
    for(int w=0; w<clients; w++){ uint64_t a=corpus.raw*uint64_t(w)/clients,b=corpus.raw*uint64_t(w+1)/clients;
        size_t lo=std::lower_bound(cum.begin(),cum.end(),a)-cum.begin(), hi=std::lower_bound(cum.begin(),cum.end(),b)-cum.begin();
        pid_t pid=fork(); if(pid==0){ _exit(client(lo,hi,false)); } pids.push_back(pid); }
    bool ok=true; for(pid_t p:pids){ int st=0; waitpid(p,&st,0); if(!(WIFEXITED(st)&&WEXITSTATUS(st)==0)) ok=false; }
    double dt=seconds_since(t0), raw=corpus.raw/1e9;
    fprintf(stderr,"IPC2 clients=%d: %.3f s  aggregate %.2f GB/s  (pipelined, no round-trip)  verify=%s\n",clients,dt,raw/dt,ok?"PASS":"FAIL");
    return ok?0:1;
}
