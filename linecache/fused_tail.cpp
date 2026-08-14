// ================= PRODUCTION_FUSED (mickg10/implementer) =================
// Built on local-oracle's accepted trace interner (head of this file, verbatim).
// Placement 1: in-process KEYSET -> MISSING -> BODY cache transaction, per stage timed,
// byte-exact verified. F reconstructs from ITS OWN per-C store + the BODY definitions.
//   C: intern TU -> ordered global IDs -> USED_KEYS (first-appearance) + dense file-local u32
//   F: resolve USED_KEYS against its per-C store -> MISSING bitmap
//   C: BODY = {defs for missing IDs} + {ordered occurrence stream as local u32} + expected len
//      -> zstd(level) -> u32-framed
//   F: unzstd -> add missing defs to store -> expand occurrence stream -> reconstruct TU bytes
#if defined(__has_include) && __has_include(<zstd.h>)
#include <zstd.h>
#else   // dev header absent (e.g. quietbox2) — declare the 4 entry points, link the runtime .so
extern "C" {
size_t ZSTD_compress(void*, size_t, const void*, size_t, int);
size_t ZSTD_decompress(void*, size_t, const void*, size_t);
size_t ZSTD_compressBound(size_t);
unsigned ZSTD_isError(size_t);
}
#endif

struct FStore {                         // F's per-C-GUID resident set (keys it has + their bytes)
    std::vector<uint8_t> has; std::vector<uint32_t> foff, flen; std::vector<char> fb;
    void ensure(uint32_t d){ size_t need=size_t(d)+1; if(has.size()<need){ has.resize(need,0); foff.resize(need,0); flen.resize(need,0);} }
    bool have(uint32_t g) const { return g<has.size() && has[g]; }
    void add(uint32_t g,const char*p,uint32_t n){ ensure(g); if(has[g])return; uint32_t o=(uint32_t)fb.size(); fb.insert(fb.end(),p,p+n); foff[g]=o; flen[g]=n; has[g]=1; }
    uint64_t bytes() const { return fb.size(); }
    uint64_t keys() const { uint64_t c=0; for(uint8_t h:has)c+=h; return c; }
};
static inline void putv(std::vector<uint8_t>&b,uint64_t v){ while(v>=0x80){ b.push_back(uint8_t(v)|0x80); v>>=7;} b.push_back(uint8_t(v)); }
static inline uint64_t getv(const uint8_t*&p){ uint64_t v=0; int s=0; for(;;){ uint8_t c=*p++; v|=uint64_t(c&0x7f)<<s; if(!(c&0x80))break; s+=7;} return v; }

struct Stats { double enc=0,res=0,bod=0,zc=0,zd=0,rec=0; uint64_t body=0,comp=0,keys=0,miss=0,occ=0,rbytes=0; long tus=0; };

int main(int argc,char**argv){
    const char* manifest=nullptr; size_t maxf=SIZE_MAX; int only_level=-1;
    for(int i=1;i<argc;i++){ if(!strcmp(argv[i],"--manifest")&&i+1<argc)manifest=argv[++i];
        else if(!strcmp(argv[i],"--max-files")&&i+1<argc)maxf=strtoull(argv[++i],0,10);
        else if(!strcmp(argv[i],"--level")&&i+1<argc)only_level=atoi(argv[++i]); }
    if(!manifest){ fprintf(stderr,"usage: production-fused --manifest F [--max-files N] [--level L]\n"); return 2; }
    Corpus corpus=load_corpus(manifest,maxf);
    Interner dict;
    uint32_t ml=0; for(auto&f:corpus.files) ml=std::max(ml,f.len);
    { std::vector<uint32_t> out(size_t(ml)+1);   // build C dictionary (cold, trains predictor)
      for(auto&f:corpus.files){ size_t oc=0; uint64_t h=0; dict.process(corpus.bytes.data()+f.off, corpus.bytes.data()+f.off+f.len, out.data(), oc, h, true); expose_output(out.data(),oc);} }
    uint32_t distinct=dict.distinct();
    fprintf(stderr,"FUSED placement=in-process TUs=%zu raw=%llu distinct=%u\n",corpus.files.size(),(unsigned long long)corpus.raw,distinct);

    std::vector<uint32_t> out(size_t(ml)+1), used_g, local_of(size_t(distinct)+1,0), stamp(size_t(distinct)+1,0);
    std::vector<const char*> f_ptr; std::vector<uint32_t> f_len;
    uint32_t epoch=0; std::vector<uint8_t> body, dbody; std::vector<char> comp; std::string reconbuf;
    int levels[3]={1,3,6};
    for(int li=0; li<3; li++){ int L=levels[li]; if(only_level>=0 && L!=only_level) continue;
      for(int warm=0; warm<2; warm++){
        FStore F;
        if(warm){ F.ensure(distinct); for(uint32_t g=1;g<=distinct;g++){ const LineRef&r=dict.ref(g); F.add(g,dict.line_data(r.off),r.len);} }
        Stats st{}; bool ok=true;
        for(auto&f:corpus.files){ const char* src=corpus.bytes.data()+f.off; uint32_t flen=f.len;
            auto t0=Clock::now(); size_t occ=0; uint64_t h=0; dict.process(src,src+flen,out.data(),occ,h,false);
            ++epoch; used_g.clear();
            for(size_t i=0;i<occ;i++){ uint32_t g=out[i]; if(stamp[g]!=epoch){ stamp[g]=epoch; local_of[g]=(uint32_t)used_g.size(); used_g.push_back(g);} }
            st.enc+=seconds_since(t0);
            size_t nloc=used_g.size();
            auto t1=Clock::now(); static std::vector<uint8_t> miss; miss.assign(nloc,0); uint64_t nmiss=0;
            for(size_t i=0;i<nloc;i++) if(!F.have(used_g[i])){ miss[i]=1; nmiss++; }
            st.res+=seconds_since(t1);
            auto t2=Clock::now(); body.clear();
            putv(body,nloc); putv(body,nmiss); putv(body,occ); putv(body,flen);
            for(size_t i=0;i<nloc;i++) if(miss[i]){ const LineRef&r=dict.ref(used_g[i]); putv(body,i); putv(body,r.len); const char* lb=dict.line_data(r.off); body.insert(body.end(),(const uint8_t*)lb,(const uint8_t*)lb+r.len);}
            for(size_t i=0;i<occ;i++) putv(body, local_of[out[i]]);
            st.bod+=seconds_since(t2); st.body+=body.size();
            auto t3=Clock::now(); size_t cap=ZSTD_compressBound(body.size()); if(comp.size()<cap)comp.resize(cap);
            size_t csz=ZSTD_compress(comp.data(),cap,body.data(),body.size(),L); if(ZSTD_isError(csz)){fprintf(stderr,"zc err\n");return 2;}
            st.zc+=seconds_since(t3); st.comp+=csz;
            auto t4=Clock::now(); if(dbody.size()<body.size())dbody.resize(body.size());
            size_t dsz=ZSTD_decompress(dbody.data(),dbody.size(),comp.data(),csz); if(ZSTD_isError(dsz)||dsz!=body.size()){fprintf(stderr,"zd err\n");return 2;}
            st.zd+=seconds_since(t4);
            auto t5=Clock::now(); const uint8_t* pp=dbody.data();
            uint64_t Rnloc=getv(pp),Rnmiss=getv(pp),Rocc=getv(pp),Rlen=getv(pp); (void)Rlen;
            f_ptr.assign(Rnloc,nullptr); f_len.assign(Rnloc,0);
            for(uint64_t m=0;m<Rnmiss;m++){ uint64_t lid=getv(pp); uint64_t ln=getv(pp); const char* bp=(const char*)pp; pp+=ln; f_ptr[lid]=bp; f_len[lid]=(uint32_t)ln; F.add(used_g[lid],bp,(uint32_t)ln); }
            for(uint64_t i=0;i<Rnloc;i++) if(!f_ptr[i]){ uint32_t g=used_g[i]; f_ptr[i]=F.fb.data()+F.foff[g]; f_len[i]=F.flen[g]; }
            reconbuf.clear();
            for(uint64_t i=0;i<Rocc;i++){ uint64_t lid=getv(pp); reconbuf.append(f_ptr[lid], f_len[lid]); }
            st.rec+=seconds_since(t5); st.rbytes+=reconbuf.size(); st.keys+=nloc; st.miss+=nmiss; st.occ+=occ; st.tus++;
            if(reconbuf.size()!=flen || memcmp(reconbuf.data(),src,flen)!=0){ fprintf(stderr,"RECON MISMATCH TU off=%llu len=%u got=%zu\n",(unsigned long long)f.off,flen,reconbuf.size()); ok=false; break; }
        }
        if(!ok) return 1;
        double raw=corpus.raw/1e9; auto gb=[&](double t){ return t>0? raw/t:0; };
        double ct=st.enc+st.bod+st.zc, ft=st.res+st.zd+st.rec, e2e=std::max(ct,ft);
        fprintf(stderr,"L=%d %-7s: [C] enc %.2f body %.2f zstd %.2f =%.2f | [F] resolve %.2f unzstd %.2f recon %.2f =%.2f | pipelined e2e=%.2f GB/s\n",
            L, warm?"F-warm":"F-empty", gb(st.enc),gb(st.bod),gb(st.zc),gb(ct), gb(st.res),gb(st.zd),gb(st.rec),gb(ft), gb(e2e));
        fprintf(stderr,"   body=%.1fMB comp=%.1fMB overall-ratio=%.2fx miss=%llu/%llu(keys) occ=%llu Fstore=%.1fMB/%lluk recon=%.0fMB verify=PASS\n",
            st.body/1048576.0, st.comp/1048576.0, corpus.raw/(double)st.comp, (unsigned long long)st.miss,(unsigned long long)st.keys,
            (unsigned long long)st.occ, F.bytes()/1048576.0,(unsigned long long)(F.keys()/1000), st.rbytes/1048576.0);
      }
    }
    return 0;
}
