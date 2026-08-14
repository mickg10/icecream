// ============ PRODUCTION_FUSED placement-2/3: process split ============
// C (parent) = encoder/owner; F (child) = decoder/reconstructor with its OWN per-C store.
// Frames over a socketpair (placement-2) — the exact same bytes go over TCP for placement-3.
//   C->F KEYSET  : [u32 nkeys][u32 global_key ...]
//   F->C MISSING : bitmap, ceil(nkeys/8) bytes (bit set = F lacks that dense id)
//   C->F BODY    : [u32 raw_len][ zstd( nloc,nmiss,occ,flen, {lid,len,bytes}*, {occ u32}* ) ]
//   F->C ACK     : [u32 recon_len][u64 digest]  (C compares to the TU's own digest)
#include <sys/socket.h>
#include <sys/wait.h>
#include <errno.h>
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
    uint64_t bytes() const { return fb.size(); } uint64_t keys() const { uint64_t c=0; for(uint8_t h:has)c+=h; return c; }
};
static inline void putv(std::vector<uint8_t>&b,uint64_t v){ while(v>=0x80){ b.push_back(uint8_t(v)|0x80); v>>=7;} b.push_back(uint8_t(v)); }
static inline uint64_t getv(const uint8_t*&p){ uint64_t v=0;int s=0;for(;;){uint8_t c=*p++;v|=uint64_t(c&0x7f)<<s;if(!(c&0x80))break;s+=7;}return v; }
static inline uint64_t digest(const char*p,size_t n){ uint64_t h=0x100000001b3ULL^n; size_t i=0; for(;i+8<=n;i+=8){uint64_t w;memcpy(&w,p+i,8);h=(h^w)*0x9E3779B97F4A7C15ULL;h^=h>>29;} uint64_t t=0; if(i<n)memcpy(&t,p+i,n-i); h=(h^t)*0x9E3779B97F4A7C15ULL; return h^(h>>31); }

static bool wr(int fd,const void*p,size_t n){ const char*b=(const char*)p; while(n){ ssize_t w=write(fd,b,n); if(w<=0){ if(w<0&&errno==EINTR)continue; return false;} b+=w; n-=size_t(w);} return true; }
static bool rd(int fd,void*p,size_t n){ char*b=(char*)p; while(n){ ssize_t r=read(fd,b,n); if(r<=0){ if(r<0&&errno==EINTR)continue; return false;} b+=r; n-=size_t(r);} return true; }
static bool wr_frame(int fd,const void*p,uint32_t n){ return wr(fd,&n,4)&&wr(fd,p,n); }
static bool rd_frame(int fd,std::vector<uint8_t>&buf){ uint32_t n; if(!rd(fd,&n,4))return false; buf.resize(n); return rd(fd,buf.data(),n); }
static void pin(int core){ if(core<0)return; cpu_set_t s; CPU_ZERO(&s); CPU_SET(core,&s); sched_setaffinity(0,sizeof s,&s); }

// F child: reconstruct from its own store + BODY defs; verify via digest back to C.
static void run_F(int fd, int core){
    pin(core);
    FStore F; std::vector<uint8_t> ks, ksraw, bodyf, dbody; std::vector<uint32_t> used_g; std::string recon;
    std::vector<const char*> fptr; std::vector<uint32_t> fln; std::vector<uint8_t> miss;
    for(;;){
        if(!rd_frame(fd, ks)) break;
        uint32_t ksrl; memcpy(&ksrl,ks.data(),4); if(ksraw.size()<ksrl) ksraw.resize(ksrl);
        size_t kd=ZSTD_decompress(ksraw.data(),ksraw.size(),ks.data()+4,ks.size()-4);
        if(ZSTD_isError(kd)||kd!=ksrl){ fprintf(stderr,"F ks zd err\n"); _exit(3);}
        const uint8_t* p=ksraw.data(); uint32_t nk; memcpy(&nk,p,4); p+=4;
        used_g.resize(nk); if(nk) memcpy(used_g.data(), p, size_t(nk)*4);
        uint32_t nb=(nk+7)/8; miss.assign(nb,0);
        for(uint32_t i=0;i<nk;i++) if(!F.have(used_g[i])) miss[i>>3]|=uint8_t(1u<<(i&7));
        if(!wr_frame(fd, miss.data(), nb)) break;
        if(!rd_frame(fd, bodyf)) break;
        uint32_t rawlen; memcpy(&rawlen,bodyf.data(),4);
        if(dbody.size()<rawlen) dbody.resize(rawlen);
        size_t dsz=ZSTD_decompress(dbody.data(),dbody.size(),bodyf.data()+4,bodyf.size()-4);
        if(ZSTD_isError(dsz)||dsz!=rawlen){ fprintf(stderr,"F zd err\n"); _exit(3); }
        const uint8_t* pp=dbody.data();
        uint64_t nloc=getv(pp),nmiss=getv(pp),nocc=getv(pp),flen=getv(pp); (void)flen;
        fptr.assign(nloc,nullptr); fln.assign(nloc,0);
        for(uint64_t m=0;m<nmiss;m++){ uint64_t lid=getv(pp),ln=getv(pp); const char* bp=(const char*)pp; pp+=ln; fptr[lid]=bp; fln[lid]=(uint32_t)ln; F.add(used_g[lid],bp,(uint32_t)ln); }
        for(uint64_t i=0;i<nloc;i++) if(!fptr[i]){ uint32_t g=used_g[i]; fptr[i]=F.fb.data()+F.foff[g]; fln[i]=F.flen[g]; }
        recon.clear();
        for(uint64_t i=0;i<nocc;i++){ uint64_t lid=getv(pp); recon.append(fptr[lid], fln[lid]); }
        uint32_t rl=(uint32_t)recon.size(); uint64_t dg=digest(recon.data(),recon.size());
        uint8_t ack[12]; memcpy(ack,&rl,4); memcpy(ack+4,&dg,8);
        if(!wr_frame(fd, ack, 12)) break;
    }
    // report F memory to stderr on exit
    fprintf(stderr,"F(child): resident_keys=%lluk resident_bytes=%.1fMB\n",(unsigned long long)(F.keys()/1000),F.bytes()/1048576.0);
    _exit(0);
}

int main(int argc,char**argv){
    const char* manifest=nullptr; size_t maxf=SIZE_MAX; int level=3; int ccore=2, fcore=3; int passes=2;
    for(int i=1;i<argc;i++){ if(!strcmp(argv[i],"--manifest")&&i+1<argc)manifest=argv[++i];
        else if(!strcmp(argv[i],"--max-files")&&i+1<argc)maxf=strtoull(argv[++i],0,10);
        else if(!strcmp(argv[i],"--level")&&i+1<argc)level=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--ccore")&&i+1<argc)ccore=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--fcore")&&i+1<argc)fcore=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--passes")&&i+1<argc)passes=atoi(argv[++i]); }
    if(!manifest){ fprintf(stderr,"usage: fused-socket --manifest F [--level L] [--ccore N --fcore N] [--warm] [--max-files N]\n"); return 2; }
    Corpus corpus=load_corpus(manifest,maxf);
    Interner dict; uint32_t ml=0; for(auto&f:corpus.files) ml=std::max(ml,f.len);
    { std::vector<uint32_t> out(size_t(ml)+1); for(auto&f:corpus.files){ size_t oc=0; uint64_t h=0; dict.process(corpus.bytes.data()+f.off,corpus.bytes.data()+f.off+f.len,out.data(),oc,h,true); expose_output(out.data(),oc);} }
    uint32_t distinct=dict.distinct();

    int sv[2]; if(socketpair(AF_UNIX,SOCK_STREAM,0,sv)){ perror("socketpair"); return 2; }
    pid_t pid=fork(); if(pid<0){ perror("fork"); return 2; }
    if(pid==0){ close(sv[0]); run_F(sv[1], fcore); return 0; }
    close(sv[1]); pin(ccore);
    int fd=sv[0];
    std::vector<uint32_t> out(size_t(ml)+1), used_g, local_of(size_t(distinct)+1,0), stamp(size_t(distinct)+1,0);
    std::vector<uint8_t> ksbuf, kscomp, body, comp, missf; uint32_t epoch=0;
    double raw=corpus.raw/1e9; bool fail=false;
    fprintf(stderr,"FUSED placement=socketpair TUs=%zu raw=%llu distinct=%u level=%d cores C=%d F=%d passes=%d\n",
        corpus.files.size(),(unsigned long long)corpus.raw,distinct,level,ccore,fcore,passes);
    for(int pass=0; pass<passes && !fail; pass++){
      uint64_t sent_keyset=0,sent_body=0,occ_total=0,miss_total=0; auto t0=Clock::now();
      for(auto&f:corpus.files){ const char* src=corpus.bytes.data()+f.off; uint32_t flen=f.len;
        size_t occ=0; uint64_t h=0; dict.process(src,src+flen,out.data(),occ,h,false);
        ++epoch; used_g.clear();
        for(size_t i=0;i<occ;i++){ uint32_t g=out[i]; if(stamp[g]!=epoch){ stamp[g]=epoch; local_of[g]=(uint32_t)used_g.size(); used_g.push_back(g);} }
        uint32_t nk=(uint32_t)used_g.size();
        ksbuf.resize(4+size_t(nk)*4); memcpy(ksbuf.data(),&nk,4); if(nk) memcpy(ksbuf.data()+4,used_g.data(),size_t(nk)*4);
        { size_t kc=ZSTD_compressBound(ksbuf.size()); if(kscomp.size()<kc+4)kscomp.resize(kc+4); uint32_t krl=(uint32_t)ksbuf.size(); memcpy(kscomp.data(),&krl,4);
          size_t kz=ZSTD_compress(kscomp.data()+4,kc,ksbuf.data(),ksbuf.size(),level); if(ZSTD_isError(kz)){fail=true;break;}
          if(!wr_frame(fd,kscomp.data(),(uint32_t)(kz+4))){fail=true;break;} sent_keyset+=kz+4; }
        if(!rd_frame(fd,missf)){fail=true;break;}
        body.clear(); uint64_t nmiss=0;
        for(uint32_t i=0;i<nk;i++) if(missf[i>>3]&(1u<<(i&7))) nmiss++;
        putv(body,nk); putv(body,nmiss); putv(body,occ); putv(body,flen);
        for(uint32_t i=0;i<nk;i++) if(missf[i>>3]&(1u<<(i&7))){ const LineRef&r=dict.ref(used_g[i]); putv(body,i); putv(body,r.len); const char* lb=dict.line_data(r.off); body.insert(body.end(),(const uint8_t*)lb,(const uint8_t*)lb+r.len);}
        for(size_t i=0;i<occ;i++) putv(body, local_of[out[i]]);
        size_t cap=ZSTD_compressBound(body.size()); if(comp.size()<cap+4)comp.resize(cap+4);
        uint32_t rawlen=(uint32_t)body.size(); memcpy(comp.data(),&rawlen,4);
        size_t csz=ZSTD_compress(comp.data()+4,cap,body.data(),body.size(),level); if(ZSTD_isError(csz)){fail=true;break;}
        if(!wr_frame(fd,comp.data(),(uint32_t)(csz+4))){fail=true;break;} sent_body+=csz+4;
        std::vector<uint8_t> ack; if(!rd_frame(fd,ack)||ack.size()!=12){fail=true;break;}
        uint32_t rl; uint64_t dg; memcpy(&rl,ack.data(),4); memcpy(&dg,ack.data()+4,8);
        if(rl!=flen || dg!=digest(src,flen)){ fprintf(stderr,"VERIFY FAIL TU off=%llu\n",(unsigned long long)f.off); fail=true; break; }
        occ_total+=occ; miss_total+=nmiss;
      }
      double dt=seconds_since(t0);
      fprintf(stderr,"  pass%d(%-6s): %.3f s %.2f GB/s | wire keyset=%.1fMB body=%.1fMB total=%.1fMB ratio=%.2fx | occ=%llu miss=%llu\n",
        pass, pass==0?"F-cold":"F-warm", dt, raw/dt, sent_keyset/1048576.0, sent_body/1048576.0,
        (sent_keyset+sent_body)/1048576.0, corpus.raw/double(sent_keyset+sent_body),
        (unsigned long long)occ_total,(unsigned long long)miss_total);
    }
    shutdown(fd,SHUT_WR); close(fd); int wst=0; waitpid(pid,&wst,0);
    fprintf(stderr,"  verify=%s\n", (!fail && WIFEXITED(wst)&&WEXITSTATUS(wst)==0)?"PASS":"FAIL");
    return fail?1:0;
}
