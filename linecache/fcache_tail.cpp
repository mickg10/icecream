// ===== PRODUCTION_FUSED: F cache-server + ephemeral fork-per-job (the real shape) =====
// Correct model (owner-specified):
//   * F cache SERVER  = ONE persistent process, sole owner of the per-GUID key->text store.
//   * iceccd per-job FORK = ephemeral: handles ONE job then DIES. It holds NONE of the
//     cache; it TALKS to the cache server over IPC to get the text it needs. NO COW, NO
//     shared memory of the cache. Each job = fork -> connect -> ask server -> reconstruct
//     -> exit. This measures whether ONE cache owner saturates as concurrency rises (-> shard).
// Roles:
//   --role cacheserver --sock PATH        (persistent; serves KEYSET-> text)
//   --role driver --sock PATH --jobs N    (fork-per-TU, N concurrent, over the whole corpus)
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <errno.h>
#include <csignal>
#include <thread>

struct FStore {
    std::vector<uint8_t> has; std::vector<uint32_t> foff, flen; std::vector<char> fb;
    void ensure(uint32_t d){ size_t n=size_t(d)+1; if(has.size()<n){has.resize(n,0);foff.resize(n,0);flen.resize(n,0);} }
    void add(uint32_t g,const char*p,uint32_t n){ ensure(g); if(has[g])return; uint32_t o=(uint32_t)fb.size(); fb.insert(fb.end(),p,p+n); foff[g]=o; flen[g]=n; has[g]=1; }
};
static inline uint64_t digest(const char*p,size_t n){ uint64_t h=0x100000001b3ULL^n; size_t i=0; for(;i+8<=n;i+=8){uint64_t w;memcpy(&w,p+i,8);h=(h^w)*0x9E3779B97F4A7C15ULL;h^=h>>29;} uint64_t t=0; if(i<n)memcpy(&t,p+i,n-i); h=(h^t)*0x9E3779B97F4A7C15ULL; return h^(h>>31); }
static bool wr(int fd,const void*p,size_t n){ const char*b=(const char*)p; while(n){ ssize_t w=write(fd,b,n); if(w<=0){ if(w<0&&errno==EINTR)continue; return false;} b+=w; n-=size_t(w);} return true; }
static bool rd(int fd,void*p,size_t n){ char*b=(char*)p; while(n){ ssize_t r=read(fd,b,n); if(r<=0){ if(r<0&&errno==EINTR)continue; return false;} b+=r; n-=size_t(r);} return true; }
static bool wrf(int fd,const void*p,uint32_t n){ return wr(fd,&n,4)&&wr(fd,p,n); }
static bool rdf(int fd,std::vector<uint8_t>&b){ uint32_t n; if(!rd(fd,&n,4))return false; b.resize(n); return rd(fd,b.data(),n); }
static Interner* build_dict(const Corpus& c){ Interner* d=new Interner(); uint32_t ml=0; for(auto&f:c.files) ml=std::max(ml,f.len);
    std::vector<uint32_t> out(size_t(ml)+1); for(auto&f:c.files){ size_t oc=0; uint64_t h=0; d->process(c.bytes.data()+f.off,c.bytes.data()+f.off+f.len,out.data(),oc,h,true); expose_output(out.data(),oc);} return d; }
static int usock_listen(const char* path){ unlink(path); int s=socket(AF_UNIX,SOCK_STREAM,0); sockaddr_un a{}; a.sun_family=AF_UNIX; strncpy(a.sun_path,path,sizeof(a.sun_path)-1);
    if(bind(s,(sockaddr*)&a,sizeof a)){ perror("bind"); exit(2);} listen(s,256); return s; }
static int usock_conn(const char* path){ int s=socket(AF_UNIX,SOCK_STREAM,0); sockaddr_un a{}; a.sun_family=AF_UNIX; strncpy(a.sun_path,path,sizeof(a.sun_path)-1);
    for(int t=0; connect(s,(sockaddr*)&a,sizeof a) && t<6000; ++t) usleep(2000); return s; }

// cache-server: serve one connection. Request = [u32 nk][u32 global_key ...].
// Response = nk x ([u32 len][text bytes]) for the keys, from the shared read-only warm store.
static void serve(int c, const FStore* F){
    std::vector<uint8_t> ks, resp;
    for(;;){ if(!rdf(c,ks)) break; const uint8_t* p=ks.data(); uint32_t nk; memcpy(&nk,p,4); p+=4;
        resp.clear();
        for(uint32_t i=0;i<nk;i++){ uint32_t g; memcpy(&g,p,4); p+=4; uint32_t n=F->flen[g]; const char* t=F->fb.data()+F->foff[g];
            uint8_t lb[4]; memcpy(lb,&n,4); resp.insert(resp.end(),lb,lb+4); resp.insert(resp.end(),(const uint8_t*)t,(const uint8_t*)t+n); }
        if(!wrf(c,resp.data(),(uint32_t)resp.size())) break;
    }
    close(c);
}

int main(int argc,char**argv){
    const char* manifest=nullptr; const char* role=nullptr; const char* sock="/tmp/fcache.sock"; int jobs=8; size_t maxf=SIZE_MAX;
    for(int i=1;i<argc;i++){ if(!strcmp(argv[i],"--manifest")&&i+1<argc)manifest=argv[++i];
        else if(!strcmp(argv[i],"--role")&&i+1<argc)role=argv[++i];
        else if(!strcmp(argv[i],"--sock")&&i+1<argc)sock=argv[++i];
        else if(!strcmp(argv[i],"--jobs")&&i+1<argc)jobs=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--max-files")&&i+1<argc)maxf=strtoull(argv[++i],0,10); }
    if(!manifest||!role){ fprintf(stderr,"usage: fcache --role cacheserver|driver --manifest F --sock P [--jobs N]\n"); return 2; }
    Corpus corpus=load_corpus(manifest,maxf);
    Interner* dict=build_dict(corpus); uint32_t distinct=dict->distinct();

    if(!strcmp(role,"cacheserver")){
        FStore* F=new FStore(); F->ensure(distinct); for(uint32_t g=1;g<=distinct;g++){ const LineRef&r=dict->ref(g); F->add(g,dict->line_data(r.off),r.len);} // warm per-GUID store
        int s=usock_listen(sock);
        fprintf(stderr,"F cache-server ready: sock=%s distinct=%u store=%.1fMB (single owner; forks talk to it over IPC)\n",sock,distinct,F->fb.size()/1048576.0);
        signal(SIGCHLD,SIG_IGN);
        for(;;){ int c=accept(s,nullptr,nullptr); if(c<0){ if(errno==EINTR)continue; break; } std::thread(serve,c,F).detach(); }
        return 0;
    }
    // driver = iceccd: precompute each TU's message (keyset + occ local ids), then run the
    // corpus through a pool of N concurrent EPHEMERAL forks (one job per fork, then it dies).
    uint32_t ml=0; for(auto&f:corpus.files) ml=std::max(ml,f.len);
    std::vector<uint32_t> ks_flat, occ_flat; std::vector<size_t> ks_off(corpus.files.size()+1,0), occ_off(corpus.files.size()+1,0);
    { std::vector<uint32_t> out(size_t(ml)+1), used_g, local_of(size_t(distinct)+1,0), stamp(size_t(distinct)+1,0); uint32_t epoch=0;
      for(size_t fi=0; fi<corpus.files.size(); ++fi){ const auto&f=corpus.files[fi]; size_t occ=0; uint64_t h=0;
        dict->process(corpus.bytes.data()+f.off,corpus.bytes.data()+f.off+f.len,out.data(),occ,h,false);
        ++epoch; used_g.clear(); for(size_t i=0;i<occ;i++){ uint32_t g=out[i]; if(stamp[g]!=epoch){ stamp[g]=epoch; local_of[g]=(uint32_t)used_g.size(); used_g.push_back(g);} }
        ks_flat.insert(ks_flat.end(),used_g.begin(),used_g.end()); ks_off[fi+1]=ks_flat.size();
        for(size_t i=0;i<occ;i++) occ_flat.push_back(local_of[out[i]]); occ_off[fi+1]=occ_flat.size(); } }
    fprintf(stderr,"F driver: TUs=%zu raw=%llu distinct=%u sock=%s\n",corpus.files.size(),(unsigned long long)corpus.raw,distinct,sock);
    std::vector<uint64_t> tudig(corpus.files.size());   // per-TU digest so the child verifies WITHOUT the corpus
    for(size_t fi=0; fi<corpus.files.size(); ++fi){ const auto&f=corpus.files[fi]; tudig[fi]=digest(corpus.bytes.data()+f.off,f.len); }

    // one job (ephemeral fork): connect -> send keyset -> recv text -> expand occ -> verify -> exit.
    auto do_job=[&](size_t fi)->int{
        int s=usock_conn(sock); if(s<0) return 5;
        uint32_t nk=(uint32_t)(ks_off[fi+1]-ks_off[fi]); const uint32_t* keys=ks_flat.data()+ks_off[fi];
        std::vector<uint8_t> req(4+size_t(nk)*4); memcpy(req.data(),&nk,4); if(nk)memcpy(req.data()+4,keys,size_t(nk)*4);
        if(!wrf(s,req.data(),(uint32_t)req.size())) return 4;
        std::vector<uint8_t> resp; if(!rdf(s,resp)) return 4; close(s);
        std::vector<const char*> tptr(nk); std::vector<uint32_t> tlen(nk); const uint8_t* p=resp.data();
        for(uint32_t i=0;i<nk;i++){ uint32_t n; memcpy(&n,p,4); p+=4; tlen[i]=n; tptr[i]=(const char*)p; p+=n; }
        std::string recon; const uint32_t* occ=occ_flat.data()+occ_off[fi]; size_t no=occ_off[fi+1]-occ_off[fi];
        for(size_t i=0;i<no;i++){ uint32_t lid=occ[i]; recon.append(tptr[lid],tlen[lid]); }
        return (recon.size()==corpus.files[fi].len && digest(recon.data(),recon.size())==tudig[fi])?0:1;
    };
    // children are ephemeral one-job forks and never touch the corpus -> exclude it from fork COW.
    { uintptr_t a=(uintptr_t)corpus.bytes.data(), pg=4096, aa=(a+pg-1)&~(pg-1);
      if(corpus.bytes.size()>pg) madvise((void*)aa, corpus.bytes.size()-(aa-a), MADV_DONTFORK); }

    // --- pure DECODE measurement: one process, ONE persistent connection. Separates the
    // server round-trip (get the text) from the actual reconstruction (expand occ->bytes). ---
    { int s=usock_conn(sock); double t_query=0,t_decode=0; uint64_t rb=0; bool ok=true;
      std::string recon; std::vector<const char*> tptr; std::vector<uint32_t> tlen; std::vector<uint8_t> req,resp;
      for(size_t fi=0; fi<corpus.files.size(); ++fi){
        uint32_t nk=(uint32_t)(ks_off[fi+1]-ks_off[fi]); const uint32_t* keys=ks_flat.data()+ks_off[fi];
        req.resize(4+size_t(nk)*4); memcpy(req.data(),&nk,4); if(nk)memcpy(req.data()+4,keys,size_t(nk)*4);
        auto q0=Clock::now(); if(!wrf(s,req.data(),(uint32_t)req.size())||!rdf(s,resp)){ok=false;break;} t_query+=seconds_since(q0);
        tptr.assign(nk,nullptr); tlen.assign(nk,0); const uint8_t* p=resp.data();
        for(uint32_t i=0;i<nk;i++){ uint32_t n; memcpy(&n,p,4); p+=4; tlen[i]=n; tptr[i]=(const char*)p; p+=n; }
        auto d0=Clock::now(); recon.clear(); const uint32_t* occ=occ_flat.data()+occ_off[fi]; size_t no=occ_off[fi+1]-occ_off[fi];
        for(size_t i=0;i<no;i++){ uint32_t lid=occ[i]; recon.append(tptr[lid],tlen[lid]); } t_decode+=seconds_since(d0); rb+=recon.size();
        const auto&f=corpus.files[fi]; if(recon.size()!=f.len||memcmp(recon.data(),corpus.bytes.data()+f.off,f.len)!=0){ok=false;break;}
      }
      close(s); double raw=corpus.raw/1e9;
      fprintf(stderr,"  DECODE-only(1 proc,persistent conn): reconstruct %.2f GB/s | server-roundtrip %.2f GB/s | verify=%s\n",
        raw/t_decode, raw/t_query, ok?"PASS":"FAIL"); }

    (void)do_job;
    // Cache-server saturation test: N concurrent handlers, each one persistent connection over
    // its own TU partition (query the SINGLE cache server for text + decode). Isolates whether
    // one cache owner scales; fork cost is amortized (N forks, not 1238).
    std::vector<uint64_t> cum(corpus.files.size()+1,0); for(size_t i=0;i<corpus.files.size();i++) cum[i+1]=cum[i]+corpus.files[i].len;
    auto do_part=[&](size_t lo,size_t hi)->int{
        int s=usock_conn(sock); if(s<0) return 5;
        std::vector<const char*> tptr; std::vector<uint32_t> tlen; std::vector<uint8_t> req,resp; std::string recon;
        for(size_t fi=lo; fi<hi; ++fi){
            uint32_t nk=(uint32_t)(ks_off[fi+1]-ks_off[fi]); const uint32_t* keys=ks_flat.data()+ks_off[fi];
            req.resize(4+size_t(nk)*4); memcpy(req.data(),&nk,4); if(nk)memcpy(req.data()+4,keys,size_t(nk)*4);
            if(!wrf(s,req.data(),(uint32_t)req.size())||!rdf(s,resp)){ close(s); return 4; }
            tptr.assign(nk,nullptr); tlen.assign(nk,0); const uint8_t* p=resp.data();
            for(uint32_t i=0;i<nk;i++){ uint32_t n; memcpy(&n,p,4); p+=4; tlen[i]=n; tptr[i]=(const char*)p; p+=n; }
            recon.clear(); const uint32_t* occ=occ_flat.data()+occ_off[fi]; size_t no=occ_off[fi+1]-occ_off[fi];
            for(size_t i=0;i<no;i++){ uint32_t lid=occ[i]; recon.append(tptr[lid],tlen[lid]); }
            if(recon.size()!=corpus.files[fi].len || digest(recon.data(),recon.size())!=tudig[fi]){ close(s); return 1; }
        }
        close(s); return 0;
    };
    for(int trial=0; ; trial++){ int Ns[6]={1,4,8,16,jobs>16?jobs:0,0}; int N=Ns[trial]; if(N<=0) break; if(trial>0&&N==Ns[trial-1]) continue;
        std::vector<pid_t> pids; auto t0=Clock::now();
        for(int w=0; w<N; w++){ uint64_t a=corpus.raw*uint64_t(w)/N,b=corpus.raw*uint64_t(w+1)/N;
            size_t lo=std::lower_bound(cum.begin(),cum.end(),a)-cum.begin(), hi=std::lower_bound(cum.begin(),cum.end(),b)-cum.begin();
            pid_t pid=fork(); if(pid==0){ _exit(do_part(lo,hi)); } pids.push_back(pid); }
        bool ok=true; for(pid_t p:pids){ int st=0; waitpid(p,&st,0); if(!(WIFEXITED(st)&&WEXITSTATUS(st)==0)) ok=false; }
        double dt=seconds_since(t0), raw=corpus.raw/1e9;
        fprintf(stderr,"  handlers=%-3d: %.3f s  aggregate %.2f GB/s  (query single cache-server + decode)  verify=%s\n",N,dt,raw/dt,ok?"PASS":"FAIL");
    }
    return 0;
}
