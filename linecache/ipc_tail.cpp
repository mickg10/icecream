// ============ PRODUCTION_FUSED: real C-daemon / F-cache-server topology ============
// F = a line/cache SERVER with per-GUID datastructures (key->text store). It accept()s C
// connections and FORKS one handler per C-request; all handlers share the warm per-GUID
// store read-only via COW (separate processes -> no locks, no races). Symmetric to the C
// side, which shares its per-GUID interner/trace state across its concurrent jobs.
// Both sides deterministically intern the same corpus in manifest order -> IDENTICAL global
// key namespace, so C's KEYSET global ids resolve against F's store.  Run C-clients on one
// machine, F-server on another -> each gets its own full memory bandwidth for its half.
//   role fserver  --port P
//   role client   --host H --port P --clients N
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sched.h>
#include <errno.h>
#include <csignal>
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
static inline uint64_t digest(const char*p,size_t n){ uint64_t h=0x100000001b3ULL^n; size_t i=0; for(;i+8<=n;i+=8){uint64_t w;memcpy(&w,p+i,8);h=(h^w)*0x9E3779B97F4A7C15ULL;h^=h>>29;} uint64_t t=0; if(i<n)memcpy(&t,p+i,n-i); h=(h^t)*0x9E3779B97F4A7C15ULL; return h^(h>>31); }
static bool wr(int fd,const void*p,size_t n){ const char*b=(const char*)p; while(n){ ssize_t w=write(fd,b,n); if(w<=0){ if(w<0&&errno==EINTR)continue; return false;} b+=w; n-=size_t(w);} return true; }
static bool rd(int fd,void*p,size_t n){ char*b=(char*)p; while(n){ ssize_t r=read(fd,b,n); if(r<=0){ if(r<0&&errno==EINTR)continue; return false;} b+=r; n-=size_t(r);} return true; }
static bool wrf(int fd,const void*p,uint32_t n){ return wr(fd,&n,4)&&wr(fd,p,n); }
static bool rdf(int fd,std::vector<uint8_t>&b){ uint32_t n; if(!rd(fd,&n,4))return false; b.resize(n); return rd(fd,b.data(),n); }

static Interner* build_dict(const Corpus& corpus){
    Interner* d=new Interner(); uint32_t ml=0; for(auto&f:corpus.files) ml=std::max(ml,f.len);
    std::vector<uint32_t> out(size_t(ml)+1);
    for(auto&f:corpus.files){ size_t oc=0; uint64_t h=0; d->process(corpus.bytes.data()+f.off,corpus.bytes.data()+f.off+f.len,out.data(),oc,h,true); expose_output(out.data(),oc);}
    return d;
}

// F handler for one connection (a forked child): shared warm store, read-only. Frames:
//   C->F KEYSET(zstd) : [u32 raw][zstd([u32 nk][u32 keys])]
//   F->C MISSING      : bitmap
//   C->F BODY(zstd)   : [u32 raw][zstd(nloc,nmiss,occ,flen,{lid,len,bytes}*,{occ}*)]
//   F->C ACK          : [u32 recon_len][u64 digest]
static int f_handler(int cfd, const FStore& F){
    std::vector<uint8_t> ks, ksraw, bodyf, dbody, miss; std::vector<uint32_t> used_g;
    std::vector<const char*> fptr; std::vector<uint32_t> fln; std::string recon;
    for(;;){
        if(!rdf(cfd,ks)) break;
        uint32_t krl; memcpy(&krl,ks.data(),4); if(ksraw.size()<krl)ksraw.resize(krl);
        if(ZSTD_isError(ZSTD_decompress(ksraw.data(),ksraw.size(),ks.data()+4,ks.size()-4))) return 3;
        const uint8_t* p=ksraw.data(); uint32_t nk; memcpy(&nk,p,4); p+=4; used_g.resize(nk); if(nk)memcpy(used_g.data(),p,size_t(nk)*4);
        uint32_t nb=(nk+7)/8; miss.assign(nb,0);
        for(uint32_t i=0;i<nk;i++) if(!F.have(used_g[i])) miss[i>>3]|=uint8_t(1u<<(i&7));
        if(!wrf(cfd,miss.data(),nb)) break;
        if(!rdf(cfd,bodyf)) break;
        uint32_t brl; memcpy(&brl,bodyf.data(),4); if(dbody.size()<brl)dbody.resize(brl);
        if(ZSTD_isError(ZSTD_decompress(dbody.data(),dbody.size(),bodyf.data()+4,bodyf.size()-4))) return 3;
        const uint8_t* pp=dbody.data(); uint64_t nloc=getv(pp),nmiss=getv(pp),nocc=getv(pp),flen=getv(pp);(void)flen;
        fptr.assign(nloc,nullptr); fln.assign(nloc,0);
        for(uint64_t m=0;m<nmiss;m++){ uint64_t lid=getv(pp),ln=getv(pp); const char* bp=(const char*)pp; pp+=ln; fptr[lid]=bp; fln[lid]=(uint32_t)ln; }
        for(uint64_t i=0;i<nloc;i++) if(!fptr[i]){ uint32_t g=used_g[i]; fptr[i]=F.fb.data()+F.foff[g]; fln[i]=F.flen[g]; }
        recon.clear(); for(uint64_t i=0;i<nocc;i++){ uint64_t lid=getv(pp); recon.append(fptr[lid],fln[lid]); }
        uint32_t rl=(uint32_t)recon.size(); uint64_t dg=digest(recon.data(),recon.size());
        uint8_t ack[12]; memcpy(ack,&rl,4); memcpy(ack+4,&dg,8); if(!wrf(cfd,ack,12)) break;
    }
    return 0;
}

int main(int argc,char**argv){
    const char* manifest=nullptr; const char* role=nullptr; const char* host="127.0.0.1"; int port=8899; int clients=8; int level=3; size_t maxf=SIZE_MAX;
    for(int i=1;i<argc;i++){ if(!strcmp(argv[i],"--manifest")&&i+1<argc)manifest=argv[++i];
        else if(!strcmp(argv[i],"--role")&&i+1<argc)role=argv[++i];
        else if(!strcmp(argv[i],"--host")&&i+1<argc)host=argv[++i];
        else if(!strcmp(argv[i],"--port")&&i+1<argc)port=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--clients")&&i+1<argc)clients=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--level")&&i+1<argc)level=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--max-files")&&i+1<argc)maxf=strtoull(argv[++i],0,10); }
    if(!manifest||!role){ fprintf(stderr,"usage: ipc --role fserver|client --manifest F [--host H --port P --clients N --level L]\n"); return 2; }
    Corpus corpus=load_corpus(manifest,maxf);
    Interner* dict=build_dict(corpus); uint32_t distinct=dict->distinct();

    if(!strcmp(role,"fserver")){
        FStore F; F.ensure(distinct); for(uint32_t g=1;g<=distinct;g++){ const LineRef&r=dict->ref(g); F.add(g,dict->line_data(r.off),r.len);} // warm per-GUID store
        int s=socket(AF_INET,SOCK_STREAM,0); int one=1; setsockopt(s,SOL_SOCKET,SO_REUSEADDR,&one,sizeof one);
        sockaddr_in a{}; a.sin_family=AF_INET; a.sin_addr.s_addr=INADDR_ANY; a.sin_port=htons(port);
        if(bind(s,(sockaddr*)&a,sizeof a)){ perror("bind"); return 2; } listen(s,64);
        fprintf(stderr,"F cache-server ready: port=%d distinct=%u store=%.1fMB (forks per C-request, shared warm per-GUID store)\n",
            port,distinct,(F.foff.size()?F.fb.size():0)/1048576.0);
        signal(SIGCHLD,SIG_IGN);
        for(;;){ int c=accept(s,nullptr,nullptr); if(c<0){ if(errno==EINTR)continue; break; }
            int one2=1; setsockopt(c,IPPROTO_TCP,TCP_NODELAY,&one2,sizeof one2);
            pid_t pid=fork(); if(pid==0){ close(s); _exit(f_handler(c,F)); } close(c); }
        return 0;
    }
    // client driver: fork N C-clients (share warm dict via COW); each streams its TU partition.
    std::vector<uint64_t> cum(corpus.files.size()+1,0); for(size_t i=0;i<corpus.files.size();i++) cum[i+1]=cum[i]+corpus.files[i].len;
    uint32_t ml=0; for(auto&f:corpus.files) ml=std::max(ml,f.len);
    fprintf(stderr,"C clients=%d -> F %s:%d level=%d distinct=%u\n",clients,host,port,level,distinct);
    auto t0=Clock::now(); std::vector<pid_t> pids;
    for(int w=0; w<clients; w++){ uint64_t a=corpus.raw*w/clients,b=corpus.raw*(w+1)/clients;
        size_t lo=std::lower_bound(cum.begin(),cum.end(),a)-cum.begin(), hi=std::lower_bound(cum.begin(),cum.end(),b)-cum.begin();
        pid_t pid=fork(); if(pid){ pids.push_back(pid); continue; }
        // child C-client
        int s=socket(AF_INET,SOCK_STREAM,0); sockaddr_in sa{}; sa.sin_family=AF_INET; sa.sin_port=htons(port); inet_pton(AF_INET,host,&sa.sin_addr);
        int one=1; setsockopt(s,IPPROTO_TCP,TCP_NODELAY,&one,sizeof one);
        for(int tries=0; connect(s,(sockaddr*)&sa,sizeof sa) && tries<6000; ++tries) usleep(5000);
        std::vector<uint32_t> out(size_t(ml)+1), used_g, local_of(size_t(distinct)+1,0), stamp(size_t(distinct)+1,0);
        std::vector<uint8_t> ksbuf,kscomp,body,comp,missf; uint32_t epoch=0; int rc=0;
        for(size_t fi=lo; fi<hi; fi++){ const auto&f=corpus.files[fi]; const char* src=corpus.bytes.data()+f.off; uint32_t flen=f.len;
            size_t occ=0; uint64_t h=0; dict->process(src,src+flen,out.data(),occ,h,false);
            ++epoch; used_g.clear(); for(size_t i=0;i<occ;i++){ uint32_t g=out[i]; if(stamp[g]!=epoch){ stamp[g]=epoch; local_of[g]=(uint32_t)used_g.size(); used_g.push_back(g);} }
            uint32_t nk=(uint32_t)used_g.size(); ksbuf.resize(4+size_t(nk)*4); memcpy(ksbuf.data(),&nk,4); if(nk)memcpy(ksbuf.data()+4,used_g.data(),size_t(nk)*4);
            size_t kc=ZSTD_compressBound(ksbuf.size()); if(kscomp.size()<kc+4)kscomp.resize(kc+4); uint32_t krl=(uint32_t)ksbuf.size(); memcpy(kscomp.data(),&krl,4);
            size_t kz=ZSTD_compress(kscomp.data()+4,kc,ksbuf.data(),ksbuf.size(),level); if(!wrf(s,kscomp.data(),(uint32_t)(kz+4))){rc=4;break;}
            if(!rdf(s,missf)){rc=4;break;} body.clear(); uint64_t nmiss=0; for(uint32_t i=0;i<nk;i++) if(missf[i>>3]&(1u<<(i&7))) nmiss++;
            putv(body,nk);putv(body,nmiss);putv(body,occ);putv(body,flen);
            for(uint32_t i=0;i<nk;i++) if(missf[i>>3]&(1u<<(i&7))){ const LineRef&r=dict->ref(used_g[i]); putv(body,i);putv(body,r.len); const char* lb=dict->line_data(r.off); body.insert(body.end(),(const uint8_t*)lb,(const uint8_t*)lb+r.len);}
            for(size_t i=0;i<occ;i++) putv(body,local_of[out[i]]);
            size_t cap=ZSTD_compressBound(body.size()); if(comp.size()<cap+4)comp.resize(cap+4); uint32_t bl=(uint32_t)body.size(); memcpy(comp.data(),&bl,4);
            size_t cz=ZSTD_compress(comp.data()+4,cap,body.data(),body.size(),level); if(!wrf(s,comp.data(),(uint32_t)(cz+4))){rc=4;break;}
            std::vector<uint8_t> ack; if(!rdf(s,ack)||ack.size()!=12){rc=4;break;} uint32_t rl; uint64_t dg; memcpy(&rl,ack.data(),4); memcpy(&dg,ack.data()+4,8);
            if(rl!=flen||dg!=digest(src,flen)){ rc=1; break; }
        }
        shutdown(s,SHUT_WR); close(s); _exit(rc);
    }
    bool ok=true; for(pid_t p:pids){ int st=0; waitpid(p,&st,0); if(!(WIFEXITED(st)&&WEXITSTATUS(st)==0)) ok=false; }
    double dt=seconds_since(t0), raw=corpus.raw/1e9;
    fprintf(stderr,"IPC clients=%d: %.3f s  aggregate %.2f GB/s  verify=%s\n", clients, dt, raw/dt, ok?"PASS":"FAIL");
    return ok?0:1;
}
