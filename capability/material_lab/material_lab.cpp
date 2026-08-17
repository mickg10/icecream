// icecream #16 — material_lab.cpp  (single-process, byte-exact compression-research engine)
//
// NO fork / socket / process split.  Reuses cap_codec (Interner, S1 blocks, the P24/M3
// mixed-region materializer with the reduced grammar RAW_RUN / PUBLIC_LINE / BYTE_ARRAY /
// PP_MARKER).  Phase-1 deliverables:
//   ledger          — reproduce the committed M3 per-component wire ledger EXACTLY (RBASE-M3,
//                     shared cross-TU streaming window).  GATE: DuckDB 9,802,066 / RocksDB 10,111,855.
//   residual-census — decompose the M3 wire by component; characterise the line_def RAW_RUN
//                     literal residual; compute the S0 O0 free-definition (MATERIAL_BLOCK) ceiling.
//   curve           — per-TU curve.tsv (raw / z19 / cold0(=RBASE-M3) / test(=O0 ceiling) + savings).
//   export-events   — per-TU M3 opcode/event stream + per-line source-origin -> events.<corpus>.zst
//
// build: g++ -O3 -std=c++17 -DICE_LINE_CAP_LOG2=23 material_lab.cpp cap_codec.cpp -o material_lab -lzstd
#include "cap_codec.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <thread>
#include <zdict.h>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>
using namespace capc;
using Clock = std::chrono::steady_clock;
static double secs(Clock::time_point b){ return std::chrono::duration<double>(Clock::now()-b).count(); }

// ============================ shared engine setup =====================================
// Verbatim from cap_main.cpp main(): load corpus, intern regions/lines, S1 LZ over the
// region-id stream -> per-TU token streams + flat Blocks.  Identical byte semantics.
struct Engine {
    Corpus corpus; Interner dict;
    std::vector<uint32_t> allreg; std::vector<size_t> roff;
    std::vector<uint32_t> bchild; std::vector<size_t> boff2;
    std::vector<uint32_t> tokstream; std::vector<size_t> tokoff;
    std::vector<uint32_t> bcopy_src; std::vector<uint8_t> bcopy_ok;
    uint32_t NREG=0, NBLK=0; size_t TUs=0;
    void build(const char* manifest, size_t max_files);
};

void Engine::build(const char* manifest, size_t max_files){
    auto t0=Clock::now();
    corpus=load_corpus(manifest,max_files);
    roff.push_back(0);
    { uint32_t maxlen=0; for(auto&f:corpus.files) maxlen=std::max(maxlen,f.len);
      std::vector<uint32_t> out(size_t(maxlen)+1); uint64_t hits=0; std::vector<uint32_t> rs;
      for(auto&f:corpus.files){ size_t oc=0; rs.clear(); const char*p=corpus.bytes.data()+f.off;
          dict.process(p,p+f.len,out.data(),oc,hits,true,&rs);
          allreg.insert(allreg.end(),rs.begin(),rs.end()); roff.push_back(allreg.size()); } }
    NREG=uint32_t(dict.region_count()); TUs=corpus.files.size();
    fprintf(stderr,"[engine] loaded+interned %.1fs TUs=%zu raw=%llu regions=%u region_occ=%zu distinct_lines=%u\n",
        secs(t0),TUs,(unsigned long long)corpus.raw,NREG,allreg.size(),dict.distinct());
    // ---- S1 LZ (VERBATIM from cap_main) ----
    boff2.push_back(0);
    std::unordered_map<uint64_t,uint32_t> bdict;
    tokoff.push_back(0);
    { size_t NS=allreg.size(); uint32_t MINMATCH=3, MAXCHAIN=64, hbits=22;
      std::vector<uint32_t> head(size_t(1)<<hbits, UINT32_MAX), prevp(NS, UINT32_MAX);
      auto kgram=[&](size_t i)->uint64_t{ uint64_t h=1469598103934665603ULL; for(uint32_t j=0;j<MINMATCH;++j){ h^=allreg[i+j]; h*=1099511628211ULL; } return (h*0x9E3779B97F4A7C15ULL)>>(64-hbits); };
      auto block_get=[&](const uint32_t*p,size_t L,uint32_t srcpos,uint8_t copyok)->uint32_t{ uint64_t h=1469598103934665603ULL^(L*0x100000001b3ULL); for(size_t j=0;j<L;++j){h^=p[j];h*=1099511628211ULL;}
          auto it=bdict.find(h); if(it!=bdict.end()){ uint32_t k=it->second; if(boff2[k+1]-boff2[k]==L && memcmp(&bchild[boff2[k]],p,L*4)==0) return NREG+k; }
          uint32_t k=uint32_t(boff2.size()-1); bchild.insert(bchild.end(),p,p+L); boff2.push_back(bchild.size()); bcopy_src.push_back(srcpos); bcopy_ok.push_back(copyok); if(it==bdict.end()) bdict.emplace(h,k); return NREG+k; };
      auto tb=Clock::now();
      for(size_t t=0;t<TUs;++t){ size_t a=roff[t],b=roff[t+1]; size_t i=a;
          while(i<b){ size_t bestL=0,bestP=0;
              if(i+MINMATCH<=b && i+MINMATCH<=NS){ uint32_t cand=head[kgram(i)],chain=0;
                  while(cand!=UINT32_MAX&&chain<MAXCHAIN){ if(cand<i){ size_t L=0,mx=b-i; while(L<mx&&allreg[cand+L]==allreg[i+L])++L; if(L>=MINMATCH&&L>bestL){bestL=L;bestP=cand;if(L==mx)break;} } cand=prevp[cand]; ++chain; } }
              size_t step; (void)bestP;
              if(bestL>=MINMATCH){ tokstream.push_back(block_get(&allreg[i],bestL,uint32_t(bestP),(bestP+bestL<=roff[t])?1:0)); step=bestL; }
              else { tokstream.push_back(allreg[i]); step=1; }
              for(size_t j=i;j<i+step;++j){ if(j+MINMATCH<=NS){ uint64_t g=kgram(j); prevp[j]=head[g]; head[g]=uint32_t(j); } }
              i+=step; }
          tokoff.push_back(tokstream.size()); }
      fprintf(stderr,"[engine] S1 LZ: %.1fs tokens=%zu blocks=%zu (%.4f tok/region)\n",secs(tb),tokstream.size(),boff2.size()-1,double(tokstream.size())/NS);
    }
    NBLK=uint32_t(boff2.size());
}

// ============================ RBASE ledger ============================================
// Reproduces cap_main run_C's cold 7-category accounting SINGLE-PROCESS (F's required-region /
// missReg computation inlined).  The ONLY difference M3 vs M4 is the mixed-lane zstd framing:
//   MIX_M3_STREAM = shared cross-TU streaming window (ZSTD_e_flush/TU + ZSTD_e_end) = RBASE-M3.
//   MIX_M4_FRAMES = independent per-TU frames (session reset) = current cap_main CAP-M4 (validation).
enum MixMode { MIX_M3_STREAM=0, MIX_M4_FRAMES=1 };

struct Ledger {
    double w_root=0,w_linedef=0,w_regiondef=0,w_blockdef=0,w_pathdef=0,w_missing=0,w_framing=0;
    double mixedPartWire[4]={0,0,0,0}; double selector=0;
    uint64_t literalRaw=0, arrayValues=0, publicLines=0, n_marker=0, n_literal=0;
    std::array<uint64_t,7> mixedOps{};
    std::vector<double> cold0_this;    // per-TU wire increment (causal)
    std::vector<double> state_bytes;   // cumulative persistent region-store bytes after TU t
    // throughput / chronological (tournament goals): encode wall time and cumulative-at-TU marks
    double enc_secs=0, enc_secs_100=0; double cum_wire_100=0, cum_wire_200=0; double raw_100=0, raw_200=0;
    double total() const { return w_root+w_linedef+w_regiondef+w_blockdef+w_pathdef+w_missing+w_framing; }
};

// Config-aware stateless zstd size (level + optional LDM + windowLog). Resets session+params.
static size_t zsize_cfg(ZSTD_CCtx*c,const uint8_t*data,size_t n,int level,bool ldm,int wlog,std::vector<uint8_t>&dst){
    ZSTD_CCtx_reset(c,ZSTD_reset_session_and_parameters);
    ZSTD_CCtx_setParameter(c,ZSTD_c_compressionLevel,level);
    if(ldm){ ZSTD_CCtx_setParameter(c,ZSTD_c_enableLongDistanceMatching,1); if(wlog) ZSTD_CCtx_setParameter(c,ZSTD_c_windowLog,wlog); }
    size_t bound=ZSTD_compressBound(n); if(dst.size()<bound) dst.resize(bound);
    size_t r=ZSTD_compress2(c,dst.data(),dst.size(),data?data:(const uint8_t*)"",n);
    if(ZSTD_isError(r)){ fprintf(stderr,"zsize_cfg %s\n",ZSTD_getErrorName(r)); exit(2); } return r;
}

static Ledger run_ledger(Engine& E, int zlevel, MixMode mode,
                         std::vector<MixedEncoder::LiteralOccurrence>* census=nullptr,
                         std::vector<double>* lit_this_out=nullptr,
                         bool ldm=false, int wlog=0,
                         std::array<std::vector<uint8_t>,4>* lanesOut=nullptr){
    Ledger L; const double FRAME=4; const size_t MP=4;
    ZSTD_CCtx* z=ZSTD_createCCtx();
    std::array<ZSTD_CCtx*,4> mixedZC{};
    for(size_t i=0;i<MP;++i){ mixedZC[i]=ZSTD_createCCtx();
        ZSTD_CCtx_setParameter(mixedZC[i],ZSTD_c_compressionLevel,zlevel);
        ZSTD_CCtx_setParameter(mixedZC[i],ZSTD_c_contentSizeFlag,0);
        if(ldm){ ZSTD_CCtx_setParameter(mixedZC[i],ZSTD_c_enableLongDistanceMatching,1);
                 if(wlog) ZSTD_CCtx_setParameter(mixedZC[i],ZSTD_c_windowLog,wlog); } }
    auto _t_enc0=Clock::now();
    std::vector<uint8_t> dst;
    MixedEncoder enc; enc.init(E.dict.distinct(), E.NREG); enc.census_sink=census;
    std::vector<uint8_t> known(E.NREG,0), fknownBlk(E.NBLK,0);
    std::vector<uint32_t> reqBlkStampC(E.NBLK,0); uint32_t stampC=0;
    std::vector<uint32_t> reqRegStamp(E.NREG,0), reqBlkStampF(E.NBLK,0); uint32_t stampF=0;
    std::array<bool,4> active{false,false,false,false};
    std::array<std::vector<uint8_t>,4> encLastTU;   // per-TU-fresh (matches codec50 mixedEncoded scope)
    L.cold0_this.assign(E.TUs,0.0); L.state_bytes.assign(E.TUs,0.0);
    if(lit_this_out) lit_this_out->assign(E.TUs,0.0);
    uint64_t stateAcc=0;
    std::vector<uint8_t> rootb, blockRaw, needPayload;
    std::vector<uint32_t> requiredBlocks, manifestBlocks, missReg, Freq, FreqBlk;

    for(size_t t=0;t<E.TUs;++t){
        double before=L.total();
        for(auto&e:encLastTU) e.clear();   // fresh each TU (only the final TU's flush survives to ZSTD_e_end)
        const uint32_t* tk=&E.tokstream[E.tokoff[t]]; size_t tn=E.tokoff[t+1]-E.tokoff[t];
        rootb.clear(); for(size_t i=0;i<tn;++i) put_varint(rootb, tk[i]);
        // C: required blocks (root scan)
        if(++stampC==0){ std::fill(reqBlkStampC.begin(),reqBlkStampC.end(),0); stampC=1; }
        requiredBlocks.clear();
        for(size_t i=0;i<tn;++i){ uint32_t tok=tk[i]; if(tok>=E.NREG){ uint32_t k=tok-E.NREG;
            if(reqBlkStampC[k]!=stampC){ reqBlkStampC[k]=stampC; requiredBlocks.push_back(k);} } }
        L.w_root += zsize_cfg(z, rootb.data(), rootb.size(), zlevel, ldm, wlog, dst); L.w_framing += FRAME;
        // block manifest (COPY when possible in cold)
        manifestBlocks.clear(); for(uint32_t k:requiredBlocks) if(!fknownBlk[k]) manifestBlocks.push_back(k);
        blockRaw.clear();
        if(!manifestBlocks.empty()){
            put_varint(blockRaw, manifestBlocks.size());
            for(uint32_t k:manifestBlocks){ put_varint(blockRaw,k); size_t Lb=E.boff2[k+1]-E.boff2[k];
                if(E.bcopy_ok[k]){ blockRaw.push_back(1); put_varint(blockRaw,E.bcopy_src[k]); put_varint(blockRaw,Lb); }
                else { blockRaw.push_back(0); put_varint(blockRaw,Lb); for(size_t j=E.boff2[k];j<E.boff2[k+1];++j) put_varint(blockRaw,E.bchild[j]); } }
            L.w_blockdef += zsize_cfg(z, blockRaw.data(), blockRaw.size(), zlevel, ldm, wlog, dst) + FRAME;
            for(uint32_t k:manifestBlocks) fknownBlk[k]=1;
        }
        // F: required-region order then missReg
        if(++stampF==0){ std::fill(reqRegStamp.begin(),reqRegStamp.end(),0); std::fill(reqBlkStampF.begin(),reqBlkStampF.end(),0); stampF=1; }
        Freq.clear(); FreqBlk.clear();
        auto reqReg=[&](uint32_t r){ if(reqRegStamp[r]!=stampF){ reqRegStamp[r]=stampF; Freq.push_back(r);} };
        auto reqBlk=[&](uint32_t k){ if(reqBlkStampF[k]!=stampF){ reqBlkStampF[k]=stampF; FreqBlk.push_back(k);} };
        for(size_t i=0;i<tn;++i){ uint32_t tok=tk[i]; if(tok<E.NREG) reqReg(tok); else reqBlk(tok-E.NREG); }
        for(uint32_t k:FreqBlk){ for(size_t j=E.boff2[k];j<E.boff2[k+1];++j) reqReg(E.bchild[j]); }
        missReg.clear(); for(uint32_t r:Freq) if(!known[r]) missReg.push_back(r);
        needPayload.clear(); put_varint(needPayload, missReg.size());
        for(uint32_t r:missReg){ put_varint(needPayload,r); } put_varint(needPayload,0);
        if(!manifestBlocks.empty()||!missReg.empty())
            L.w_missing += zsize_cfg(z, needPayload.data(), needPayload.size(), zlevel, ldm, wlog, dst) + FRAME;
        // materialize
        uint32_t nr=0;
        if(!missReg.empty()){ nr=enc.materialize(E.dict, missReg, t);
            for(uint32_t r:missReg){ known[r]=1; stateAcc+=E.dict.region_raw_len(r); } }
        else { for(auto&v:enc.mixedRaw) v.clear(); enc.fill_paths.clear(); enc.np=0; }
        if(lanesOut) for(size_t i=0;i<MP;++i) (*lanesOut)[i].insert((*lanesOut)[i].end(), enc.mixedRaw[i].begin(), enc.mixedRaw[i].end());
        // mixed lane
        if(nr){
            for(size_t i=0;i<MP;++i) if(!enc.mixedRaw[i].empty()){
                size_t sz;
                if(mode==MIX_M4_FRAMES){ sz=zsize_cfg(mixedZC[i], enc.mixedRaw[i].data(), enc.mixedRaw[i].size(), zlevel, ldm, wlog, dst); }
                else { active[i]=true; encLastTU[i]=zstd_stream_encode(mixedZC[i], enc.mixedRaw[i], ZSTD_e_flush);
                       sz=encLastTU[i].size(); }
                double bytes=sz+FRAME; L.mixedPartWire[i]+=bytes; (i==0?L.w_regiondef:L.w_linedef)+=bytes;
                if(i==1 && lit_this_out) (*lit_this_out)[t]+=bytes;   // per-TU RAW_RUN literal component
            }
            L.w_linedef += 1; L.selector += 1;
        }
        if(enc.np){ L.w_pathdef += zsize_cfg(z, enc.fill_paths.data(), enc.fill_paths.size(), zlevel, ldm, wlog, dst); }
        if(enc.np||nr){ L.w_framing += FRAME; }
        L.cold0_this[t]=L.total()-before; L.state_bytes[t]=double(stateAcc);
        if(t==99){  L.cum_wire_100=L.total(); L.enc_secs_100=secs(_t_enc0); }
        if(t==199){ L.cum_wire_200=L.total(); }
    }
    L.enc_secs=secs(_t_enc0);
    { double r=0; for(size_t t=0;t<E.TUs && t<100;++t) r+=E.corpus.files[t].len; L.raw_100=r;
      r=0; for(size_t t=0;t<E.TUs && t<200;++t) r+=E.corpus.files[t].len; L.raw_200=r; }
    if(mode==MIX_M3_STREAM){
        double preTail=L.total(); std::vector<uint8_t> empty;
        for(size_t i=0;i<MP;++i) if(active[i]){
            std::vector<uint8_t> tail=zstd_stream_encode(mixedZC[i], empty, ZSTD_e_end);
            double& wire=(i==0?L.w_regiondef:L.w_linedef); double add=0;
            if(encLastTU[i].empty() && !tail.empty()){ add+=FRAME; L.mixedPartWire[i]+=FRAME; }
            add+=tail.size(); wire+=add; L.mixedPartWire[i]+=tail.size();
            if(i==1 && lit_this_out && E.TUs) (*lit_this_out)[E.TUs-1]+=add;
        }
        if(E.TUs) L.cold0_this[E.TUs-1]+=(L.total()-preTail);
    }
    L.literalRaw=enc.mixedLiteralRaw; L.arrayValues=enc.mixedArrayValues; L.publicLines=enc.nextMixedPublic-1;
    L.n_marker=enc.n_marker; L.n_literal=enc.n_literal; L.mixedOps=enc.mixedOps;
    ZSTD_freeCCtx(z); for(size_t i=0;i<MP;++i) ZSTD_freeCCtx(mixedZC[i]);
    return L;
}

static void print_ledger(const char* tag, const Ledger& L){
    printf("==== %s ====\n",tag);
    printf("wire by category (bytes): root=%.0f line_def=%.0f region_def=%.0f block_def=%.0f path_def=%.0f missing=%.0f framing=%.0f  TOTAL=%.0f\n",
        L.w_root,L.w_linedef,L.w_regiondef,L.w_blockdef,L.w_pathdef,L.w_missing,L.w_framing,L.total());
    printf("mixed components: control(region_def)=%.0f literal=%.0f array_control=%.0f array_values=%.0f selector=%.0f\n",
        L.mixedPartWire[0],L.mixedPartWire[1],L.mixedPartWire[2],L.mixedPartWire[3],L.selector);
    printf("raw: literal=%llu array_values=%llu public_lines=%llu marker_lines=%llu literal_lines=%llu ops[RAW_RUN=%llu publish=%llu ref=%llu BYTE_ARRAY=%llu PP_MARKER=%llu]\n",
        (unsigned long long)L.literalRaw,(unsigned long long)L.arrayValues,(unsigned long long)L.publicLines,
        (unsigned long long)L.n_marker,(unsigned long long)L.n_literal,
        (unsigned long long)L.mixedOps[0],(unsigned long long)L.mixedOps[1],(unsigned long long)L.mixedOps[2],
        (unsigned long long)L.mixedOps[3],(unsigned long long)L.mixedOps[4]);
}

// ============================ census / O0 ceiling helpers ============================
// One-shot (batched, whole-corpus) zstd size at a given level; LDM+wlog27 == `zstd -N --long`.
static size_t zstd_batch(const std::vector<uint8_t>& v, int level, bool ldm, int wlog=27){
    if(v.empty()) return 0;
    ZSTD_CCtx* c=ZSTD_createCCtx();
    ZSTD_CCtx_setParameter(c,ZSTD_c_compressionLevel,level);
    if(ldm){ ZSTD_CCtx_setParameter(c,ZSTD_c_enableLongDistanceMatching,1); ZSTD_CCtx_setParameter(c,ZSTD_c_windowLog,wlog); }
    std::vector<uint8_t> dst(ZSTD_compressBound(v.size())+64);
    size_t r=ZSTD_compress2(c,dst.data(),dst.size(),v.data(),v.size());
    if(ZSTD_isError(r)){ fprintf(stderr,"zstd_batch %s\n",ZSTD_getErrorName(r)); exit(2); }
    ZSTD_freeCCtx(c); return r;
}
// ---- residual-coder selector primitives (byte-exact) for the integrated codec ----
static std::vector<uint8_t> z_comp(const std::vector<uint8_t>& in,int level,bool ldm,int wlog){
    if(in.empty()) return {};
    ZSTD_CCtx* c=ZSTD_createCCtx(); ZSTD_CCtx_setParameter(c,ZSTD_c_compressionLevel,level);
    if(ldm){ ZSTD_CCtx_setParameter(c,ZSTD_c_enableLongDistanceMatching,1); ZSTD_CCtx_setParameter(c,ZSTD_c_windowLog,wlog); }
    std::vector<uint8_t> o(ZSTD_compressBound(in.size())+64);
    size_t r=ZSTD_compress2(c,o.data(),o.size(),in.data(),in.size()); if(ZSTD_isError(r)){fprintf(stderr,"z_comp %s\n",ZSTD_getErrorName(r));exit(2);}
    o.resize(r); ZSTD_freeCCtx(c); return o;
}
static std::vector<uint8_t> z_dec(const uint8_t* p,size_t n,size_t rawlen){
    std::vector<uint8_t> out(rawlen); if(!rawlen) return out;
    ZSTD_DCtx* d=ZSTD_createDCtx(); ZSTD_DCtx_setParameter(d,ZSTD_d_windowLogMax,31);
    ZSTD_inBuffer in{p,n,0}; ZSTD_outBuffer ob{out.data(),rawlen,0};
    size_t r=ZSTD_decompressStream(d,&ob,&in); if(ZSTD_isError(r)||ob.pos!=rawlen){fprintf(stderr,"z_dec %s\n",ZSTD_isError(r)?ZSTD_getErrorName(r):"size");exit(2);}
    ZSTD_freeDCtx(d); return out;
}
static const char* g_bsc="/home/ttuser/libbsc/bsc";
static std::vector<uint8_t> bsc_enc(const std::vector<uint8_t>& in){
    char ib[64],ob[64]; snprintf(ib,64,"/tmp/_bi_%d.bin",getpid()); snprintf(ob,64,"/tmp/_bo_%d.bsc",getpid());
    FILE*f=fopen(ib,"wb"); fwrite(in.data(),1,in.size(),f); fclose(f);
    char cmd[256]; snprintf(cmd,256,"%s e %s %s -b1024 -m0 -e2 >/dev/null 2>&1",g_bsc,ib,ob); if(system(cmd)){}
    FILE*g=fopen(ob,"rb"); fseek(g,0,SEEK_END); long sz=ftell(g); fseek(g,0,SEEK_SET);
    std::vector<uint8_t> out(sz); if(fread(out.data(),1,sz,g)!=(size_t)sz){} fclose(g); unlink(ib); unlink(ob); return out;
}
static std::vector<uint8_t> bsc_dec(const std::vector<uint8_t>& coded,size_t rawlen){
    char ib[64],ob[64]; snprintf(ib,64,"/tmp/_bdi_%d.bsc",getpid()); snprintf(ob,64,"/tmp/_bdo_%d.bin",getpid());
    FILE*f=fopen(ib,"wb"); fwrite(coded.data(),1,coded.size(),f); fclose(f);
    char cmd[256]; snprintf(cmd,256,"%s d %s %s >/dev/null 2>&1",g_bsc,ib,ob); if(system(cmd)){}
    FILE*g=fopen(ob,"rb"); std::vector<uint8_t> out(rawlen); if(fread(out.data(),1,rawlen,g)!=rawlen){} fclose(g); unlink(ib); unlink(ob); return out;
}
// Select the min-actual-byte residual coder; returns {tag,coded}. tag 0=z3 1=bsc 2=zstd10 3=zstd19long
static std::pair<uint8_t,std::vector<uint8_t>> select_residual(const std::vector<uint8_t>& r){
    auto z3=z_comp(r,3,false,0); std::pair<uint8_t,std::vector<uint8_t>> best{0,z3};
    auto z10=z_comp(r,10,false,0); if(z10.size()<best.second.size()) best={2,z10};
    if(r.size()>=4096){ auto b=bsc_enc(r); if(!b.empty()&&b.size()<best.second.size()) best={1,b}; }
    return best;
}
static std::vector<uint8_t> decode_residual(uint8_t tag,const uint8_t* p,size_t n,size_t rawlen){
    if(tag==1) return bsc_dec(std::vector<uint8_t>(p,p+n),rawlen);
    return z_dec(p,n,rawlen);
}

// Overlap-parallel z19+LDM(wlog27): K chunks, each refPrefix'ing the preceding `overlap` tail
// (skip-on-output => byte-exact). Parallel encode; sequential-but-fast decode. Returns {bytes,sec,rt}.
struct OZ { size_t bytes; double sec; bool rt; };
static OZ overlap_z19(const uint8_t* data, size_t N, int K, size_t overlap){
    if(N==0) return {0,0,true};
    size_t chunk=(N+K-1)/K; std::vector<size_t> sizes(K,0); std::vector<std::vector<uint8_t>> encs(K);
    auto tt=Clock::now(); std::vector<std::thread> th;
    for(int k=0;k<K;k++) th.emplace_back([&,k](){
        size_t a=size_t(k)*chunk, b=std::min(N,a+chunk); if(a>=b){ return; }
        ZSTD_CCtx* c=ZSTD_createCCtx(); ZSTD_CCtx_setParameter(c,ZSTD_c_compressionLevel,19);
        ZSTD_CCtx_setParameter(c,ZSTD_c_enableLongDistanceMatching,1); ZSTD_CCtx_setParameter(c,ZSTD_c_windowLog,27);
        if(a>0){ size_t o=std::min(overlap,a); ZSTD_CCtx_refPrefix(c,data+a-o,o); }
        std::vector<uint8_t> dst(ZSTD_compressBound(b-a)+64);
        size_t z=ZSTD_compress2(c,dst.data(),dst.size(),data+a,b-a); dst.resize(z); encs[k]=std::move(dst); sizes[k]=z; ZSTD_freeCCtx(c); });
    for(auto&t:th){ t.join(); } double s=secs(tt);
    size_t tot=0; for(auto z:sizes) tot+=z;
    bool rt=true;
    for(int k=0;k<K&&rt;k++){ size_t a=size_t(k)*chunk, b=std::min(N,a+chunk); if(a>=b) continue;
        ZSTD_DCtx* d=ZSTD_createDCtx(); ZSTD_DCtx_setParameter(d,ZSTD_d_windowLogMax,30);
        if(a>0){ size_t o=std::min(overlap,a); ZSTD_DCtx_refPrefix(d,data+a-o,o); }
        std::vector<uint8_t> out(b-a); ZSTD_inBuffer in{encs[k].data(),encs[k].size(),0}; ZSTD_outBuffer ob{out.data(),out.size(),0};
        size_t r=ZSTD_decompressStream(d,&ob,&in);
        if(ZSTD_isError(r)||ob.pos!=(b-a)||memcmp(out.data(),data+a,b-a)!=0){ rt=false; } ZSTD_freeDCtx(d); }
    return {tot,s,rt};
}
// Whole-program zstd of a raw byte span (streaming, memory-bounded) — the tournament baseline.
static size_t zstd_wholeprog(const uint8_t* p, size_t n, int level, bool ldm, int wlog){
    ZSTD_CCtx* c=ZSTD_createCCtx();
    ZSTD_CCtx_setParameter(c,ZSTD_c_compressionLevel,level);
    if(ldm){ ZSTD_CCtx_setParameter(c,ZSTD_c_enableLongDistanceMatching,1); ZSTD_CCtx_setParameter(c,ZSTD_c_windowLog,wlog); }
    std::vector<uint8_t> outbuf(ZSTD_CStreamOutSize()); size_t total=0;
    ZSTD_inBuffer in{p,n,0};
    for(;;){ ZSTD_outBuffer out{outbuf.data(),outbuf.size(),0};
        size_t rem=ZSTD_compressStream2(c,&out,&in,ZSTD_e_end);
        if(ZSTD_isError(rem)){ fprintf(stderr,"wholeprog %s\n",ZSTD_getErrorName(rem)); exit(2); }
        total+=out.pos; if(rem==0 && in.pos>=in.size) break; }
    ZSTD_freeCCtx(c); return total;
}
// Deterministic tokenizer: partition a line into a skeleton (fixed literals + 1-byte typed
// placeholders) and an ordered list of typed slots.  Slot types: STRING "..", CHAR '..',
// NUMBER (dec or 0x hex), IDENT [A-Za-z_]\w*.  Every source byte lands in exactly one of
// {skeleton literal, slot value}, so skeleton+slots reconstruct the line exactly.
enum SlotType { SLOT_STR=0, SLOT_CHR=1, SLOT_NUM=2, SLOT_ID=3 };
struct Slot { uint8_t type; uint32_t off; uint32_t len; };   // off/len into the line text
static inline void tokenize(const char* p, uint32_t n, std::string& skel, std::vector<Slot>& slots){
    skel.clear(); slots.clear();
    auto isIdStart=[](unsigned char c){ return (c>='A'&&c<='Z')||(c>='a'&&c<='z')||c=='_'; };
    auto isId     =[](unsigned char c){ return (c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_'; };
    auto isDig    =[](unsigned char c){ return c>='0'&&c<='9'; };
    auto isHex    =[](unsigned char c){ return (c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F'); };
    uint32_t i=0;
    while(i<n){ unsigned char c=(unsigned char)p[i];
        if(c=='"'){ uint32_t j=i+1; while(j<n){ if(p[j]=='\\'&&j+1<n){j+=2;continue;} if(p[j]=='"'){++j;break;} ++j; }
            slots.push_back({SLOT_STR,i,j-i}); skel.push_back('\x01'); i=j; }
        else if(c=='\''){ uint32_t j=i+1; while(j<n){ if(p[j]=='\\'&&j+1<n){j+=2;continue;} if(p[j]=='\''){++j;break;} ++j; }
            slots.push_back({SLOT_CHR,i,j-i}); skel.push_back('\x02'); i=j; }
        else if(isDig(c)){ uint32_t j=i;
            if(c=='0'&&i+1<n&&(p[i+1]=='x'||p[i+1]=='X')){ j=i+2; while(j<n&&isHex((unsigned char)p[j]))++j; }
            else { while(j<n&&isDig((unsigned char)p[j]))++j; }
            slots.push_back({SLOT_NUM,i,j-i}); skel.push_back('\x03'); i=j; }
        else if(isIdStart(c)){ uint32_t j=i+1; while(j<n&&isId((unsigned char)p[j]))++j;
            slots.push_back({SLOT_ID,i,j-i}); skel.push_back('\x04'); i=j; }
        else { skel.push_back((char)c); ++i; }
    }
}
// First-occurrence source-origin per distinct line: 1=generated(<..>/pre-marker), 2=system(/usr/), 3=project.
static void compute_origin(Engine& E, std::vector<uint8_t>& origin){
    origin.assign(size_t(E.dict.distinct())+1, 0);
    Marker mk; std::string cur;
    auto classify=[&](const std::string& path)->uint8_t{
        if(path.empty()||path[0]=='<') return 1;
        if(path.rfind("/usr/",0)==0)  return 2;
        return 3; };
    auto walkReg=[&](uint32_t r){ const uint32_t* lids=E.dict.region_ids_ptr(r); uint32_t c=E.dict.region_ids_count(r);
        for(uint32_t j=0;j<c;++j){ uint32_t ln=lids[j]; const LineRef& lr=E.dict.ref(ln); const char* txt=E.dict.line_data(lr.off);
            if(parse_marker(txt,lr.len,mk)){ cur=mk.path; }
            else if(!origin[ln]){ origin[ln]=classify(cur); } } };
    for(size_t t=0;t<E.TUs;++t){ const uint32_t* tk=&E.tokstream[E.tokoff[t]]; size_t tn=E.tokoff[t+1]-E.tokoff[t];
        for(size_t i=0;i<tn;++i){ uint32_t tok=tk[i]; if(tok<E.NREG) walkReg(tok);
            else { uint32_t k=tok-E.NREG; for(size_t j=E.boff2[k];j<E.boff2[k+1];++j) walkReg(E.bchild[j]); } } }
}
// Aggregate z19 baseline (sum of independent whole-TU `zstd -19 --long` frames; NO cross-TU history).
static uint64_t z19_total(Engine& E){
    uint64_t s=0;
    for(auto&f:E.corpus.files){ std::vector<uint8_t> v((const uint8_t*)E.corpus.bytes.data()+f.off,(const uint8_t*)E.corpus.bytes.data()+f.off+f.len);
        s+=zstd_batch(v,19,true); }
    return s;
}

// ================= adaptive range coder + order-N byte model (rANS-class entropy stage) =========
// Subbotin carryless range coder (total freq < 2^16) + Fenwick adaptive frequency model.
// Byte-exact by construction (encoder/decoder adapt identically). Order 0/1/2 (context = prev bytes).
namespace rc {
  static const uint32_t TOP=1u<<24, BOT=1u<<16;
  struct Enc { std::vector<uint8_t>* o; uint32_t low=0,range=0xFFFFFFFFu;
    inline void encode(uint32_t cum,uint32_t f,uint32_t tot){
      low += cum*(range/=tot); range*=f;
      while((low^(low+range))<TOP || (range<BOT && ((range=(0u-low)&(BOT-1)),true))){ o->push_back(uint8_t(low>>24)); low<<=8; range<<=8; } }
    inline void flush(){ for(int i=0;i<4;i++){ o->push_back(uint8_t(low>>24)); low<<=8; } } };
  struct Dec { const uint8_t* p; const uint8_t* e; uint32_t low=0,range=0xFFFFFFFFu,code=0;
    inline uint8_t g(){ return p<e?*p++:0; }
    inline void init(){ for(int i=0;i<4;i++) code=(code<<8)|g(); }
    inline uint32_t getcum(uint32_t tot){ return (code-low)/(range/=tot); }
    inline void decode(uint32_t cum,uint32_t f){ low += cum*range; range*=f;
      while((low^(low+range))<TOP || (range<BOT && ((range=(0u-low)&(BOT-1)),true))){ code=(code<<8)|g(); low<<=8; range<<=8; } } };
  struct Model {
    std::vector<uint16_t> t; uint32_t nctx=0; std::vector<uint32_t> tot;
    static const uint32_t INC=24, LIMIT=BOT-300;
    void init(uint32_t n){ nctx=n; t.assign(size_t(n)*257,0); tot.assign(n,0);
      for(uint32_t c=0;c<n;c++) for(int s=0;s<256;s++) add(c,s,1); }
    inline void add(uint32_t c,int sym,int d){ uint16_t* T=&t[size_t(c)*257]; for(int i=sym+1;i<=256;i+=i&-i) T[i]=uint16_t(T[i]+d); tot[c]+=d; }
    inline uint32_t cum(uint32_t c,int sym){ uint16_t* T=&t[size_t(c)*257]; uint32_t s=0; for(int i=sym;i>0;i-=i&-i) s+=T[i]; return s; }
    inline int find(uint32_t c,uint32_t f,uint32_t& cumOut){ uint16_t* T=&t[size_t(c)*257]; int pos=0; uint32_t acc=0;
      for(int pw=256; pw; pw>>=1){ int np=pos+pw; if(np<=256 && acc+T[np]<=f){ pos=np; acc+=T[np]; } } cumOut=acc; return pos; }
    void rescale(uint32_t c){ uint32_t f[256],prev=0; for(int s=0;s<256;s++){ uint32_t cs=cum(c,s+1); f[s]=cs-prev; prev=cs; }
      uint16_t* T=&t[size_t(c)*257]; for(int i=0;i<=256;i++) T[i]=0; tot[c]=0;
      for(int s=0;s<256;s++) add(c,s,int((f[s]>>1)|1)); }
    inline void update(uint32_t c,int sym){ add(c,sym,INC); if(tot[c]>=LIMIT) rescale(c); } };
}
static std::vector<uint8_t> rans_compress(int order,const uint8_t* d,size_t n){
    uint32_t nctx = order<=0?1u : order==1?256u : 65536u, mask=nctx-1;
    rc::Model m; m.init(nctx); std::vector<uint8_t> out; out.reserve(n/2+16); rc::Enc e; e.o=&out;
    uint32_t ctx=0;
    for(size_t i=0;i<n;i++){ uint8_t sym=d[i]; uint32_t cf=m.cum(ctx,sym), f=m.cum(ctx,sym+1)-cf;
        e.encode(cf,f,m.tot[ctx]); m.update(ctx,sym); ctx = order<=0?0u : ((ctx<<8)|sym)&mask; }
    e.flush(); return out;
}
static bool rans_roundtrip_ok(int order,const uint8_t* d,size_t n,const std::vector<uint8_t>& enc){
    uint32_t nctx = order<=0?1u : order==1?256u : 65536u, mask=nctx-1;
    rc::Model m; m.init(nctx); rc::Dec dec; dec.p=enc.data(); dec.e=enc.data()+enc.size(); dec.init();
    uint32_t ctx=0;
    for(size_t i=0;i<n;i++){ uint32_t tot=m.tot[ctx]; uint32_t dc=dec.getcum(tot); uint32_t cumOut; int sym=m.find(ctx,dc,cumOut);
        uint32_t f=m.cum(ctx,sym+1)-cumOut; dec.decode(cumOut,f); m.update(ctx,sym);
        if(uint8_t(sym)!=d[i]){ return false; } ctx = order<=0?0u : ((ctx<<8)|uint32_t(sym))&mask; }
    return true;
}

// ---- RBASE-P29 consumption (local-oracle's exact fixed-16 per-TU export; sha256
// 0673e735...a32a).  P29 is STATEFUL/chronological (cross-TU flushes) — a valid online
// comparison, NOT an independently-framed result (do NOT call it M4).  Consumed verbatim;
// never regenerated or approximated here. TU order = native-manifest order (P29 tu is 1-based). ----
static std::string p29_name_for_manifest(const std::string& manifest){
    static const std::pair<const char*,const char*> M[]={
        {"/corpus16/","cereal"},{"/corpus15/","simdjson"},{"/corpus14/","leveldb"},{"/corpus13/","re2"},
        {"/corpus12/","eigen"},{"/corpus11/","range-v3"},{"/corpus10/","nlohmann-json"},{"/corpus9/","catch2"},
        {"/corpus8/","spdlog"},{"/corpus7/","fmt"},{"/corpus6/","godot"},{"/corpus5/","opencv"},
        {"/corpus4/","abseil"},{"/corpus3/","duckdb"},{"/corpus2/","rocksdb"},{"/corpus/","llvm"}};
    for(auto&kv:M) if(manifest.find(kv.first)!=std::string::npos) return kv.second;
    return "";
}
struct P29 { std::vector<double> wire_this, wire_cum; bool ok=false; double total=0; std::string name; };
static P29 load_p29(const char* path, const std::string& corpus, size_t expectTUs){
    P29 p; p.name=corpus; if(corpus.empty()){ fprintf(stderr,"P29: could not map manifest to a P29 corpus name\n"); return p; }
    FILE* f=fopen(path,"r"); if(!f){ perror(path); return p; }
    p.wire_this.assign(expectTUs,0.0); p.wire_cum.assign(expectTUs,0.0);
    char line[8192]; bool hdr=true; size_t got=0;
    while(fgets(line,sizeof line,f)){ if(hdr){hdr=false;continue;}
        char* sv=nullptr; char* f1=strtok_r(line,"\t",&sv); if(!f1||corpus!=f1) continue;
        char* f2=strtok_r(nullptr,"\t",&sv); strtok_r(nullptr,"\t",&sv);           // tu, raw
        char* f4=strtok_r(nullptr,"\t",&sv); strtok_r(nullptr,"\t",&sv);           // wire, cumraw
        char* f6=strtok_r(nullptr,"\t",&sv);                                        // cumwire
        if(!f2||!f4||!f6){ continue; } long tu=strtol(f2,nullptr,10);
        if(tu<1||size_t(tu)>expectTUs){ fprintf(stderr,"P29: tu %ld out of range for %s\n",tu,corpus.c_str()); fclose(f); return p; }
        p.wire_this[tu-1]=strtod(f4,nullptr); p.wire_cum[tu-1]=strtod(f6,nullptr); ++got;
    }
    fclose(f);
    if(got!=expectTUs){ fprintf(stderr,"P29: got %zu rows for %s, expected %zu\n",got,corpus.c_str(),expectTUs); return p; }
    p.total=p.wire_cum[expectTUs-1]; p.ok=true; return p;
}

int main(int argc,char**argv){
    if(argc<2){ fprintf(stderr,"usage: %s <ledger|residual-census|curve|export-events> --manifest F [--z N] [--mode m3|m4] [--max-files N]\n",argv[0]); return 2; }
    std::string cmd=argv[1];
    const char* manifest=nullptr; int zlevel=3; size_t max_files=SIZE_MAX; MixMode mode=MIX_M3_STREAM;
    bool want_z19=false; const char* out_path=nullptr; const char* test_mode="o1";
    const char* p29_path=nullptr; const char* p29_corpus=nullptr;
    bool ldm=false; int wlog=0; size_t prefixN=0; int cornerK=24; size_t cornerOvl=1u<<20; double wp19arg=0;
    for(int i=2;i<argc;++i){
        if(!strcmp(argv[i],"--manifest")&&i+1<argc) manifest=argv[++i];
        else if(!strcmp(argv[i],"--z")&&i+1<argc) zlevel=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--mode")&&i+1<argc){ mode=!strcmp(argv[i+1],"m4")?MIX_M4_FRAMES:MIX_M3_STREAM; ++i; }
        else if(!strcmp(argv[i],"--max-files")&&i+1<argc){ max_files=strtoull(argv[++i],nullptr,10); }
        else if(!strcmp(argv[i],"--z19")) want_z19=true;
        else if(!strcmp(argv[i],"--out")&&i+1<argc) out_path=argv[++i];
        else if(!strcmp(argv[i],"--test-mode")&&i+1<argc) test_mode=argv[++i];
        else if(!strcmp(argv[i],"--p29")&&i+1<argc) p29_path=argv[++i];
        else if(!strcmp(argv[i],"--p29-corpus")&&i+1<argc) p29_corpus=argv[++i];
        else if(!strcmp(argv[i],"--ldm")) ldm=true;
        else if(!strcmp(argv[i],"--wlog")&&i+1<argc) wlog=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--prefix")&&i+1<argc) prefixN=(size_t)strtoull(argv[++i],nullptr,10);
        else if(!strcmp(argv[i],"--K")&&i+1<argc) cornerK=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--overlap")&&i+1<argc) cornerOvl=(size_t)strtoull(argv[++i],nullptr,10);
        else if(!strcmp(argv[i],"--wp19")&&i+1<argc) wp19arg=strtod(argv[++i],nullptr);
        else { fprintf(stderr,"unknown arg %s\n",argv[i]); return 2; }
    }
    (void)out_path;
    if(cmd=="rbase-p29"){
        if(!p29_path){ fprintf(stderr,"rbase-p29: pass --p29 <rbase-p29-fixed16-per-tu.tsv> (local-oracle's exact export)\n"); return 2; }
        FILE* f=fopen(p29_path,"r"); if(!f){ perror(p29_path); return 2; }
        std::map<std::string,std::pair<double,long>> perc; std::vector<std::string> order;
        char line[8192]; bool hdr=true; long nonexact=0;
        while(fgets(line,sizeof line,f)){ if(hdr){hdr=false;continue;}
            char* sv=nullptr; char* f1=strtok_r(line,"\t",&sv); if(!f1)continue; std::string nm=f1;
            for(int s=0;s<4;++s) strtok_r(nullptr,"\t",&sv);                 // tu,raw,wire,cumraw
            char* f6=strtok_r(nullptr,"\t",&sv); char* f7=strtok_r(nullptr,"\t",&sv);
            if(!f6){ continue; } double cw=strtod(f6,nullptr);
            if(f7 && strncmp(f7,"true",4)!=0) ++nonexact;
            if(perc.find(nm)==perc.end()) order.push_back(nm);
            if(cw>perc[nm].first){ perc[nm].first=cw; } perc[nm].second++;
        }
        fclose(f);
        double agg=0; long tot=0;
        printf("RBASE-P29 (local-oracle exact fixed-16 export; STATEFUL/chronological, cross-TU flushes; NOT independently-framed, NOT M4):\n");
        for(auto&nm:order){ printf("  %-14s TUs=%-5ld P29_wire=%.0f\n",nm.c_str(),perc[nm].second,perc[nm].first); agg+=perc[nm].first; tot+=perc[nm].second; }
        printf("  AGGREGATE: corpora=%zu TUs=%ld P29_wire=%.0f  (nonexact rows=%ld)\n",order.size(),tot,agg,nonexact);
        return 0;
    }
    if(cmd=="rawsrc"){
        // Part B: raw-source front. Load a manifest of RAW SOURCE files (concatenation), compare
        // monolithic-z19 and overlap-parallel-z19 to wp_z19_src (z19 --long=31). SIZE gate only.
        if(!manifest){ fprintf(stderr,"rawsrc needs --manifest <source-file-manifest>\n"); return 2; }
        Corpus c=load_corpus(manifest,max_files); double GB=1e9, raw=double(c.raw);
        const uint8_t* p=(const uint8_t*)c.bytes.data();
        auto tm=Clock::now(); size_t wp=zstd_wholeprog(p,c.raw,19,true,31); double mono_s=secs(tm);   // monolithic = baseline
        int K = cornerK>0? cornerK : std::max(2,(int)std::min<size_t>(32,c.raw/(4u<<20)+1));
        OZ o=overlap_z19(p,c.raw,K,cornerOvl); double ov_veff=raw/GB/o.sec, mono_veff=raw/GB/mono_s;
        bool ov_sz=double(o.bytes)<=1.10*double(wp);
        printf("RAWSRC_ROW\t%s\t%zu\t%.0f\t%zu\t%zu\t%.3f\t%.3f\t%d\t%d\n",
            manifest,c.files.size(),raw,wp,o.bytes,double(o.bytes)/double(wp),ov_veff,o.rt?1:0,ov_sz?1:0);
        printf("  %-24s files=%zu raw=%.1fMB | wp_z19_src(mono)=%zu (1.000x, %.3f GBps) | ovl-par-z19 K=%d=%zu (%.3fx, %.3f GBps, rt=%s) => size %s\n",
            manifest,c.files.size(),raw/1e6,wp,mono_veff,K,o.bytes,double(o.bytes)/double(wp),ov_veff,o.rt?"OK":"FAIL",ov_sz?"OK":"OVER");
        return 0;
    }

    if(cmd=="rans-selftest"){
        srand(12345);
        for(int order=0;order<=2;order++) for(int trial=0;trial<3;trial++){
            size_t n = trial==0?1000: trial==1?100000: 500000; std::vector<uint8_t> d(n);
            if(trial==0) for(auto&b:d) b=uint8_t(rand()&0xff);
            else if(trial==1){ const char* al="abcdefg .;\n{}()"; for(size_t i=0;i<n;i++) d[i]=uint8_t(al[rand()%15]); }
            else for(size_t i=0;i<n;i++) d[i]= (i%7<5)?'x':uint8_t(rand()&0xff);
            auto enc=rans_compress(order,d.data(),n); bool ok=rans_roundtrip_ok(order,d.data(),n,enc);
            printf("rans-selftest order=%d trial=%d n=%zu enc=%zu ratio=%.2f roundtrip=%s\n",order,trial,n,enc.size(),double(n)/enc.size(),ok?"OK":"*** FAIL ***");
            if(!ok) return 1;
        }
        printf("rans-selftest: ALL ROUNDTRIPS OK\n"); return 0;
    }
    if(!manifest){ fprintf(stderr,"need --manifest\n"); return 2; }

    Engine E; E.build(manifest,max_files);

    if(cmd=="rans"){
        std::array<std::vector<uint8_t>,4> lanes;
        Ledger L=run_ledger(E, 3, MIX_M3_STREAM, nullptr, nullptr, false, 0, &lanes);
        const char* nm[4]={"region_control","literal_RAWRUN","array_control","array_values"};
        int dense[3]={1,0,3};                       // literal, region-control, array-values
        double streamed3 = L.mixedPartWire[0]+L.mixedPartWire[1]+L.mixedPartWire[3];
        size_t z3sum=0,z19sum=0,bestSum=0; double ransSecs=0; size_t denseRaw=0; bool allrt=true;
        for(int di=0;di<3;++di){ int i=dense[di]; auto& v=lanes[i]; if(v.empty()) continue; denseRaw+=v.size();
            size_t z3=zstd_batch(v,3,false), z19=zstd_batch(v,19,true,31);
            size_t best=SIZE_MAX; int bestOrd=-1;
            for(int ord=0;ord<=2;ord++){ auto tt=Clock::now(); auto enc=rans_compress(ord,v.data(),v.size()); double s=secs(tt);
                bool ok=rans_roundtrip_ok(ord,v.data(),v.size(),enc); allrt=allrt&&ok;
                printf("  %-15s ord%d rANS=%9zu (%.2fx vs raw, %.0f MB/s, rt=%s)\n",nm[i],ord,enc.size(),double(v.size())/enc.size(),v.size()/1e6/(s>0?s:1),ok?"OK":"FAIL");
                if(enc.size()<best){ best=enc.size(); bestOrd=ord; ransSecs+=s; } }
            printf("  => %-15s raw=%9zu  z3=%9zu  z19=%9zu  bestRANS=ord%d %9zu  [rANS/z3=%.1f%% rANS/z19=%.1f%%]\n",
                nm[i],v.size(),z3,z19,bestOrd,best,100.0*best/z3,100.0*best/z19);
            z3sum+=z3; z19sum+=z19; bestSum+=best;
        }
        double GB=1e9, rawTot=double(E.corpus.raw);
        double proj = L.total() - streamed3 + double(bestSum);           // z3 elsewhere, rANS on the 3 dense lanes
        // 1/veff: base vb over full pipeline z3; heavy vm = denseRaw/ransSecs; f = denseRaw/rawTot
        double vb = rawTot/GB/(L.enc_secs>0?L.enc_secs:1);
        double vm = double(denseRaw)/GB/(ransSecs>0?ransSecs:1);
        double f  = double(denseRaw)/rawTot;
        double veff = 1.0/(1.0/vb + f/vm);
        printf("DENSE-3: raw=%zu  z3=%zu  z19=%zu  bestRANS=%zu (%.1f%% of z3, %.1f%% of z19)  roundtrip=%s\n",
            denseRaw,z3sum,z19sum,bestSum,100.0*bestSum/z3sum,100.0*bestSum/z19sum,allrt?"ALL OK":"*** FAIL ***");
        printf("PROJECTED cold total (z3 base + rANS on 3 lanes) = %.0f  (codec z3=%.0f)\n",proj,L.total());
        printf("THROUGHPUT 1/veff: vb=%.3f GBps  f=%.4f (denseRaw/rawTot)  vm=%.3f GBps (rANS)  => veff=%.3f GBps\n",vb,f,vm,veff);
        return 0;
    }

    if(cmd=="mtbench"){
        // The gap is LZ (match-finding), which is slow single-thread. Test z19 (LZ) on the dense-3
        // lanes with zstd multithreading — does it reach z19 ratio at >=1 GB/s raw-equivalent?
        std::array<std::vector<uint8_t>,4> lanes;
        Ledger L=run_ledger(E,3,MIX_M3_STREAM,nullptr,nullptr,false,0,&lanes);
        std::vector<uint8_t> dense; for(int i:{0,1,3}) dense.insert(dense.end(),lanes[i].begin(),lanes[i].end());
        double streamed3=L.mixedPartWire[0]+L.mixedPartWire[1]+L.mixedPartWire[3];
        double GB=1e9, rawTot=double(E.corpus.raw);
        // z3 time on the dense-3 (to subtract from the base pass)
        std::vector<uint8_t> dtmp; auto tz=Clock::now(); ZSTD_CCtx* zc=ZSTD_createCCtx(); (void)zstd_batch(dense,3,false); ZSTD_freeCCtx(zc); double z3dense=secs(tz);
        double nonDenseZ3 = (L.enc_secs>z3dense? L.enc_secs-z3dense : L.enc_secs);
        for(int nw : {0,8,16,32,48}){
            ZSTD_CCtx* c=ZSTD_createCCtx();
            ZSTD_CCtx_setParameter(c,ZSTD_c_compressionLevel,19);
            ZSTD_CCtx_setParameter(c,ZSTD_c_enableLongDistanceMatching,1);
            ZSTD_CCtx_setParameter(c,ZSTD_c_windowLog,27);
            size_t setr = ZSTD_CCtx_setParameter(c,ZSTD_c_nbWorkers,nw);
            std::vector<uint8_t> dst(ZSTD_compressBound(dense.size())+64);
            auto tt=Clock::now(); size_t z=ZSTD_compress2(c,dst.data(),dst.size(),dense.data(),dense.size()); double s=secs(tt);
            ZSTD_freeCCtx(c);
            double proj=L.total()-streamed3+double(z);
            double veff=rawTot/GB/(nonDenseZ3+s);
            printf("mtbench nbWorkers=%2d%s: dense_z19=%zu (%.2fs, %.0f MB/s)  proj_cold=%.0f (%.2fx wp_z19-ref)  veff=%.3f GBps%s\n",
                nw, ZSTD_isError(setr)?"(unsupported)":"", z,s,dense.size()/1e6/s, proj, proj/7189449.0, veff, veff>=1.0?"  [>=1 OK]":"");
        }
        printf("  (base non-dense z3 pass ~%.2fs; dense-3 raw=%zu; codec z3 total=%.0f)\n",nonDenseZ3,dense.size(),L.total());
        return 0;
    }

    if(cmd=="cornersweep"){
        // Recover cross-chunk LZ at parallel speed: (A) trained zstd dictionary loaded into each chunk,
        // (B) overlap = refPrefix the immediately-preceding tail. Byte-exact roundtrip-verified per chunk.
        std::array<std::vector<uint8_t>,4> lanes;
        Ledger L=run_ledger(E,3,MIX_M3_STREAM,nullptr,nullptr,false,0,&lanes);
        std::vector<uint8_t> dense; for(int i:{0,1,3}) dense.insert(dense.end(),lanes[i].begin(),lanes[i].end());
        double streamed3=L.mixedPartWire[0]+L.mixedPartWire[1]+L.mixedPartWire[3];
        double GB=1e9, rawTot=double(E.corpus.raw), WP19=7150874.0, nonDense=L.total()-streamed3;
        auto tz=Clock::now(); (void)zstd_batch(dense,3,false); double z3dense=secs(tz);
        double nonDenseZ3=(L.enc_secs>z3dense?L.enc_secs-z3dense:L.enc_secs);
        double CAP=7908394.0;
        printf("target: dense<=%.0f, dense_time<=%.2fs (proj<=%.0f @ >=1GBps); non-dense=%.0f base=%.2fs\n",
            CAP-nonDense, rawTot/GB-nonDenseZ3, CAP, nonDense, nonDenseZ3);
        // train dictionaries once
        auto trainDict=[&](size_t cap)->std::vector<uint8_t>{
            size_t ss=16384, ns=dense.size()/ss; std::vector<size_t> szs(ns,ss);
            std::vector<uint8_t> dict(cap);
            size_t r=ZDICT_trainFromBuffer(dict.data(),cap,dense.data(),szs.data(),unsigned(ns));
            if(ZDICT_isError(r)){ fprintf(stderr,"ZDICT: %s\n",ZDICT_getErrorName(r)); dict.clear(); return dict; }
            dict.resize(r); return dict; };
        // one config: mode 0=plain 1=dict 2=overlap
        auto run=[&](const char* tag,int K,int mode,const std::vector<uint8_t>& dict,size_t overlap){
            size_t N=dense.size(), chunk=(N+K-1)/K; std::vector<size_t> sizes(K,0); std::vector<std::vector<uint8_t>> encs(K);
            auto tt=Clock::now(); std::vector<std::thread> th;
            for(int k=0;k<K;k++) th.emplace_back([&,k](){
                size_t a=size_t(k)*chunk, b=std::min(N,a+chunk); if(a>=b){ return; }
                ZSTD_CCtx* c=ZSTD_createCCtx();
                ZSTD_CCtx_setParameter(c,ZSTD_c_compressionLevel,19);
                ZSTD_CCtx_setParameter(c,ZSTD_c_enableLongDistanceMatching,1);
                ZSTD_CCtx_setParameter(c,ZSTD_c_windowLog,27);
                if(mode==1&&!dict.empty()) ZSTD_CCtx_loadDictionary(c,dict.data(),dict.size());
                else if(mode==2&&a>0){ size_t o=std::min(overlap,a); ZSTD_CCtx_refPrefix(c,dense.data()+a-o,o); }
                std::vector<uint8_t> dst(ZSTD_compressBound(b-a)+64);
                size_t z=ZSTD_compress2(c,dst.data(),dst.size(),dense.data()+a,b-a); dst.resize(z);
                encs[k]=std::move(dst); sizes[k]=z; ZSTD_freeCCtx(c); });
            for(auto&t:th){ t.join(); } double s=secs(tt);
            size_t tot=0; for(auto z:sizes) tot+=z;
            size_t dictCost = (mode==1&&!dict.empty())? zstd_batch(dict,19,true,27) : 0; tot+=dictCost;
            bool rt=true;                                    // byte-exact roundtrip (sequential)
            for(int k=0;k<K&&rt;k++){ size_t a=size_t(k)*chunk, b=std::min(N,a+chunk); if(a>=b) continue;
                ZSTD_DCtx* d=ZSTD_createDCtx(); ZSTD_DCtx_setParameter(d,ZSTD_d_windowLogMax,30);
                if(mode==1&&!dict.empty()) ZSTD_DCtx_loadDictionary(d,dict.data(),dict.size());
                else if(mode==2&&a>0){ size_t o=std::min(overlap,a); ZSTD_DCtx_refPrefix(d,dense.data()+a-o,o); }
                std::vector<uint8_t> out(b-a); ZSTD_inBuffer in{encs[k].data(),encs[k].size(),0}; ZSTD_outBuffer ob{out.data(),out.size(),0};
                size_t r=ZSTD_decompressStream(d,&ob,&in);
                if(ZSTD_isError(r)||ob.pos!=(b-a)||memcmp(out.data(),dense.data()+a,b-a)!=0) rt=false;
                ZSTD_freeDCtx(d); }
            double proj=nonDense+double(tot), veff=rawTot/GB/(nonDenseZ3+s);
            printf("%-22s dense=%9zu proj=%9.0f %.3fx %s  %.2fs veff=%.3f %s  rt=%s%s\n",
                tag,tot,proj,proj/WP19, proj<=CAP?"OK":"  ", s,veff, veff>=1.0?"OK":"  ", rt?"OK":"FAIL",
                (proj<=CAP&&veff>=1.0&&rt)?"   <<< CLEARS BOTH":"");
        };
        printf("config                 dense       proj  xz19 sz   time  veff  sp  rt\n");
        run("single K=1",1,0,{},0);
        for(int K:{8,12,16,24,32}) run((std::string("plain K=")+std::to_string(K)).c_str(),K,0,{},0);
        for(size_t cap:{65536u,262144u,1048576u}){ auto dict=trainDict(cap);
            for(int K:{16,24,32}) run((std::string("dict")+std::to_string(cap/1024)+"K K="+std::to_string(K)).c_str(),K,1,dict,0); }
        for(size_t ov:{262144u,524288u,1048576u}) for(int K:{16,24,32})
            run((std::string("ovl")+std::to_string(ov/1024)+"K K="+std::to_string(K)).c_str(),K,2,{},ov);
        return 0;
    }

    if(cmd=="select"){
        // Part A best-of per-program .ii selector. Candidates (all byte-exact):
        //  cc = codec-corner (z19 overlap-parallel on dense lanes + z3 base)  [structured, fast]
        //  wi = whole-.ii overlap-parallel z19 (monolithic content, chunked)  [~1.0x size, z19-class speed]
        //  P29= local-oracle's z3 structured codec (size from file; speed z3-class, proxied by our z3 throughput)
        double GB=1e9, rawTot=double(E.corpus.raw);
        double wp19 = wp19arg>0? wp19arg : double(zstd_wholeprog((const uint8_t*)E.corpus.bytes.data(),E.corpus.raw,19,true,31));
        std::array<std::vector<uint8_t>,4> lanes;
        Ledger L=run_ledger(E,3,MIX_M3_STREAM,nullptr,nullptr,false,0,&lanes);
        std::vector<uint8_t> dense; for(int i:{0,1,3}) dense.insert(dense.end(),lanes[i].begin(),lanes[i].end());
        double streamed3=L.mixedPartWire[0]+L.mixedPartWire[1]+L.mixedPartWire[3], nonDense=L.total()-streamed3;
        auto tz=Clock::now(); (void)zstd_batch(dense,3,false); double z3dense=secs(tz);
        double nonDenseZ3=(L.enc_secs>z3dense?L.enc_secs-z3dense:L.enc_secs);
        // codec-corner: sweep K, pick min-bytes that holds veff>=1 (per-corpus best); overlap 2MB.
        OZ cc{0,0,true}; double cc_bytes=1e18, cc_veff=0;
        for(int K : {16,24,32,48,64}){ OZ o=overlap_z19(dense.data(),dense.size(),K,cornerOvl);
            double b=nonDense+double(o.bytes), v=rawTot/GB/(nonDenseZ3+o.sec);
            if(o.rt && v>=1.0 && b<cc_bytes){ cc_bytes=b; cc_veff=v; cc=o; } }
        if(cc_bytes>1e17){ cc=overlap_z19(dense.data(),dense.size(),16,cornerOvl); cc_bytes=nonDense+double(cc.bytes); cc_veff=rawTot/GB/(nonDenseZ3+cc.sec); }
        // whole-.ii chunked z19 (guard against huge raw: z19 is ~2.6MB/s)
        double wi_bytes=0, wi_veff=0; bool wi_rt=true, wi_done=false;
        if(rawTot <= 800e6){ int Kii=std::max(2,(int)std::min<size_t>(32,E.corpus.raw/(8u<<20)+1));
            OZ wi=overlap_z19((const uint8_t*)E.corpus.bytes.data(),E.corpus.raw,Kii,8u<<20);
            wi_bytes=double(wi.bytes); wi_veff=rawTot/GB/wi.sec; wi_rt=wi.rt; wi_done=true; }
        double p29_bytes=0, p29_veff=rawTot/GB/(L.enc_secs>0?L.enc_secs:1);   // z3-class proxy = our z3 codec throughput
        if(p29_path){ P29 p=load_p29(p29_path,p29_corpus?std::string(p29_corpus):p29_name_for_manifest(manifest),E.TUs); if(p.ok) p29_bytes=p.total; }
        // select: min bytes among speed-clearing (veff>=1) byte-exact candidates
        double best=1e18; const char* bmode="none"; double bveff=0;
        if(cc.rt && cc_veff>=1.0 && cc_bytes<best){ best=cc_bytes; bmode="codec-corner"; bveff=cc_veff; }
        if(wi_done && wi_rt && wi_veff>=1.0 && wi_bytes<best){ best=wi_bytes; bmode="whole-ii-z19"; bveff=wi_veff; }
        if(p29_bytes>0 && p29_veff>=1.0 && p29_bytes<best){ best=p29_bytes; bmode="P29-z3"; bveff=p29_veff; }
        bool sz = best<=1.10*wp19, sp = bveff>=1.0, both = (best<1e17)&&sz&&sp;
        printf("SELECT_ROW\t%s\t%zu\t%.0f\t%.0f\t%.0f\t%.3f\t%.0f\t%.3f\t%.0f\t%.0f\t%s\t%.0f\t%.3f\t%d\n",
            manifest,E.TUs,rawTot,wp19, cc_bytes,cc_veff, wi_bytes,wi_veff, p29_bytes,p29_veff, bmode,best,bveff,both?1:0);
        printf("  %-13s wp_z19=%.0f | cc=%.0f(%.3fx,%.2fG,rt%d) wi=%.0f(%.3fx,%.2fG%s) P29=%.0f(%.3fx,z3~%.2fG) => BEST=%s %.0f (%.3fx @ %.2f GBps) %s\n",
            p29_name_for_manifest(manifest).c_str(),wp19,
            cc_bytes,cc_bytes/wp19,cc_veff,cc.rt?1:0, wi_bytes,wi_done?wi_bytes/wp19:0,wi_veff,wi_done?"":"(skip)",
            p29_bytes,p29_bytes>0?p29_bytes/wp19:0,p29_veff, bmode,best<1e17?best:0,best<1e17?best/wp19:0,bveff,
            both?"<<< PASS BOTH":(best<1e17?"(fails size or speed)":"NO STATIC MODE CLEARS"));
        return 0;
    }

    if(cmd=="integrate"){
        // CAPSTONE: real end-to-end byte-exact codec = RBASE-M3 structure + residual-coder SELECTOR on
        // the literal lane. Encode -> serialize a COMPLETE self-contained stream -> INDEPENDENT decode
        // (fresh FStore, reads only the stream) -> reconstruct .ii byte-exact. z3 is the always-available
        // fallback in the selector. (Structure is RBASE-M3, the codec I own & can reproduce; the P29
        // projections used local-oracle's better structure — noted.)
        MixedEncoder enc; enc.init(E.dict.distinct(), E.NREG);
        std::vector<uint8_t> known(E.NREG,0), fknownBlk(E.NBLK,0);
        std::vector<uint32_t> reqBlkStampC(E.NBLK,0); uint32_t stampC=0;
        std::vector<uint32_t> reqRegStamp(E.NREG,0), reqBlkStampF(E.NBLK,0); uint32_t stampF=0;
        std::vector<uint8_t> rootbCat, blockRawCat, L0,L1,L2,L3, pathsCat, LT;
        std::vector<uint8_t> rootb, blockRaw; std::vector<uint32_t> requiredBlocks, manifestBlocks, missReg, Freq, FreqBlk;
        auto tE=Clock::now();
        for(size_t t=0;t<E.TUs;++t){
            const uint32_t* tk=&E.tokstream[E.tokoff[t]]; size_t tn=E.tokoff[t+1]-E.tokoff[t];
            rootb.clear(); for(size_t i=0;i<tn;++i) put_varint(rootb, tk[i]);
            if(++stampC==0){ std::fill(reqBlkStampC.begin(),reqBlkStampC.end(),0); stampC=1; }
            requiredBlocks.clear();
            for(size_t i=0;i<tn;++i){ uint32_t tok=tk[i]; if(tok>=E.NREG){ uint32_t k=tok-E.NREG; if(reqBlkStampC[k]!=stampC){ reqBlkStampC[k]=stampC; requiredBlocks.push_back(k);} } }
            manifestBlocks.clear(); for(uint32_t k:requiredBlocks) if(!fknownBlk[k]) manifestBlocks.push_back(k);
            blockRaw.clear();
            if(!manifestBlocks.empty()){ put_varint(blockRaw, manifestBlocks.size());
                for(uint32_t k:manifestBlocks){ put_varint(blockRaw,k); size_t Lb=E.boff2[k+1]-E.boff2[k];
                    if(E.bcopy_ok[k]){ blockRaw.push_back(1); put_varint(blockRaw,E.bcopy_src[k]); put_varint(blockRaw,Lb); }
                    else { blockRaw.push_back(0); put_varint(blockRaw,Lb); for(size_t j=E.boff2[k];j<E.boff2[k+1];++j) put_varint(blockRaw,E.bchild[j]); } }
                for(uint32_t k:manifestBlocks) fknownBlk[k]=1; }
            if(++stampF==0){ std::fill(reqRegStamp.begin(),reqRegStamp.end(),0); std::fill(reqBlkStampF.begin(),reqBlkStampF.end(),0); stampF=1; }
            Freq.clear(); FreqBlk.clear();
            auto reqReg=[&](uint32_t r){ if(reqRegStamp[r]!=stampF){ reqRegStamp[r]=stampF; Freq.push_back(r);} };
            auto reqBlk=[&](uint32_t k){ if(reqBlkStampF[k]!=stampF){ reqBlkStampF[k]=stampF; FreqBlk.push_back(k);} };
            for(size_t i=0;i<tn;++i){ uint32_t tok=tk[i]; if(tok<E.NREG) reqReg(tok); else reqBlk(tok-E.NREG); }
            for(uint32_t k:FreqBlk){ for(size_t j=E.boff2[k];j<E.boff2[k+1];++j) reqReg(E.bchild[j]); }
            missReg.clear(); for(uint32_t r:Freq) if(!known[r]) missReg.push_back(r);
            uint32_t nr=0; if(!missReg.empty()){ nr=enc.materialize(E.dict, missReg, t); for(uint32_t r:missReg) known[r]=1; }
            else { for(auto&v:enc.mixedRaw) v.clear(); enc.fill_paths.clear(); enc.np=0; }
            (void)nr;
            put_varint(LT,rootb.size()); rootbCat.insert(rootbCat.end(),rootb.begin(),rootb.end());
            put_varint(LT,blockRaw.size()); blockRawCat.insert(blockRawCat.end(),blockRaw.begin(),blockRaw.end());
            for(int i=0;i<4;++i){ auto&v=enc.mixedRaw[i]; put_varint(LT,v.size()); std::vector<uint8_t>&D=(i==0?L0:i==1?L1:i==2?L2:L3); D.insert(D.end(),v.begin(),v.end()); }
            put_varint(LT,enc.fill_paths.size()); pathsCat.insert(pathsCat.end(),enc.fill_paths.begin(),enc.fill_paths.end());
        }
        double encSecs=secs(tE);
        // residual-coder SELECTOR on the literal lane (z3 always-available fallback); structure lanes
        // are z3-optimal (verified: bsc/zstd never beat z3 there). tag per block; only L1 is non-z3.
        auto z3c=[&](const std::vector<uint8_t>&v)->std::pair<uint8_t,std::vector<uint8_t>>{ return {0,z_comp(v,3,false,0)}; };
        auto S1=z3c(LT), S2=z3c(rootbCat), S3=z3c(blockRawCat), S4=z3c(L0), S6=z3c(L2), S7=z3c(L3), S8=z3c(pathsCat);
        auto S5=select_residual(L1); uint8_t rtag=S5.first;
        std::vector<uint8_t> S; put_varint(S,E.NREG); put_varint(S,E.NBLK); put_varint(S,E.TUs);
        auto emit=[&](std::pair<uint8_t,std::vector<uint8_t>>&s,size_t rawlen){ S.push_back(s.first); put_varint(S,rawlen); put_varint(S,s.second.size()); S.insert(S.end(),s.second.begin(),s.second.end()); };
        emit(S1,LT.size()); emit(S2,rootbCat.size()); emit(S3,blockRawCat.size());
        emit(S4,L0.size()); emit(S5,L1.size()); emit(S6,L2.size()); emit(S7,L3.size()); emit(S8,pathsCat.size());
        double completeSize=double(S.size());
        double resid_z3=double(z_comp(L1,3,false,0).size());   // baseline for the selector's real delta
        std::vector<uint8_t> c1=S5.second;
        // INDEPENDENT DECODE — fresh FStore, reads only S
        const uint8_t* sp=S.data();
        uint32_t dNREG=uint32_t(get_varint(sp)), dNBLK=uint32_t(get_varint(sp)), dTUs=uint32_t(get_varint(sp));
        auto readbuf=[&]()->std::vector<uint8_t>{ uint8_t tag=*sp++; size_t rawlen=get_varint(sp); size_t clen=get_varint(sp);
            std::vector<uint8_t> out=decode_residual(tag,sp,clen,rawlen); sp+=clen; return out; };
        std::vector<uint8_t> LTd=readbuf(), rootbCatD=readbuf(), blockRawCatD=readbuf(), L0d=readbuf(), L1d=readbuf(), L2d=readbuf(), L3d=readbuf(), pathsD=readbuf();
        FStore F; F.init(dNREG,dNBLK);
        const uint8_t* lp=LTd.data();
        size_t oR=0,oB=0,o0=0,o1=0,o2=0,o3=0,oP=0; bool allok=true; size_t firstbad=SIZE_MAX;
        std::vector<uint32_t> FReq, FReqBlk;
        for(size_t t=0;t<dTUs;++t){
            size_t lr=get_varint(lp), lb=get_varint(lp), l0=get_varint(lp), l1=get_varint(lp), l2=get_varint(lp), l3=get_varint(lp), lpn=get_varint(lp);
            std::vector<uint8_t> rootbT(rootbCatD.begin()+oR, rootbCatD.begin()+oR+lr); oR+=lr;
            std::vector<uint8_t> blockRawT(blockRawCatD.begin()+oB, blockRawCatD.begin()+oB+lb); oB+=lb;
            std::array<std::vector<uint8_t>,6> rec;
            rec[0].assign(L0d.begin()+o0,L0d.begin()+o0+l0); o0+=l0;
            rec[1].assign(L1d.begin()+o1,L1d.begin()+o1+l1); o1+=l1;
            rec[2].assign(L2d.begin()+o2,L2d.begin()+o2+l2); o2+=l2;
            rec[3].assign(L3d.begin()+o3,L3d.begin()+o3+l3); o3+=l3;
            std::vector<uint8_t> fpT(pathsD.begin()+oP,pathsD.begin()+oP+lpn); oP+=lpn;
            if(++F.requestStamp==0){ std::fill(F.FrequiredRegionStamp.begin(),F.FrequiredRegionStamp.end(),0); std::fill(F.FrequiredBlockStamp.begin(),F.FrequiredBlockStamp.end(),0); F.requestStamp=1; }
            FReq.clear(); FReqBlk.clear();
            auto FrReg=[&](uint32_t r){ if(F.FrequiredRegionStamp[r]!=F.requestStamp){ F.FrequiredRegionStamp[r]=F.requestStamp; FReq.push_back(r);} };
            auto FrBlk=[&](uint32_t k){ if(F.FrequiredBlockStamp[k]!=F.requestStamp){ F.FrequiredBlockStamp[k]=F.requestStamp; FReqBlk.push_back(k);} };
            { const uint8_t* rp=rootbT.data(),*re=rp+rootbT.size(); while(rp<re){ uint64_t tok=get_varint(rp); if(tok<dNREG) FrReg(uint32_t(tok)); else FrBlk(uint32_t(tok-dNREG)); } }
            if(!blockRawT.empty()) F.install_blocks(blockRawT);
            for(uint32_t k:FReqBlk){ for(uint32_t child:F.FblkChildren[k]) FrReg(child); }
            std::vector<uint32_t> mr; for(uint32_t r:FReq) if(!F.FmixedRegions[r].known) mr.push_back(r);
            if(!F.decode_fill(rec,mr,fpT,uint32_t(t))){ allok=false; if(firstbad==SIZE_MAX)firstbad=t; }
            std::vector<uint8_t> recon; F.reconstruct(rootbT,recon);
            const char* orig=E.corpus.bytes.data()+E.corpus.files[t].off; uint32_t olen=E.corpus.files[t].len;
            if(recon.size()!=olen||memcmp(recon.data(),orig,olen)){ allok=false; if(firstbad==SIZE_MAX)firstbad=t; }
        }
        double wp19 = wp19arg>0?wp19arg:double(zstd_wholeprog((const uint8_t*)E.corpus.bytes.data(),E.corpus.raw,19,true,31));
        const char* tagn[4]={"z3","bsc","zstd10","zstd19"};
        printf("INTEGRATE\t%s\t%zu\t%.0f\t%.0f\t%s\t%zu\t%.0f\t%.3f\t%d\n",manifest,E.TUs,double(E.corpus.raw),completeSize,tagn[rtag],c1.size(),wp19,completeSize/wp19,allok?1:0);
        printf("  %-13s COMPLETE stream=%.0f B (%.3fx wp_z19)  residual coder=%s (%zu B, z3 was %.0f)  BYTE-EXACT=%s%s  enc=%.1fs\n",
            p29_name_for_manifest(manifest).c_str(),completeSize,completeSize/wp19,tagn[rtag],c1.size(),resid_z3,
            allok?"YES (independent decode of all TUs)":"*** NO ***", allok?"":(std::string(" first-bad-TU=")+std::to_string(firstbad)).c_str(), encSecs);
        return (allok?0:1);
    }

    if(cmd=="corner"){
        // LOCKED goal-achiever: z19+LDM on the 3 dense lanes, K parallel chunks each refPrefix'ing the
        // preceding `overlap` tail (skip-on-output => byte-exact). z3 on all other lanes. Full goal check.
        std::array<std::vector<uint8_t>,4> lanes;
        Ledger L=run_ledger(E,3,MIX_M3_STREAM,nullptr,nullptr,false,0,&lanes);
        std::vector<uint8_t> dense; for(int i:{0,1,3}) dense.insert(dense.end(),lanes[i].begin(),lanes[i].end());
        double streamed3=L.mixedPartWire[0]+L.mixedPartWire[1]+L.mixedPartWire[3];
        double GB=1e9, rawTot=double(E.corpus.raw), nonDense=L.total()-streamed3;
        auto tz=Clock::now(); (void)zstd_batch(dense,3,false); double z3dense=secs(tz);
        double nonDenseZ3=(L.enc_secs>z3dense?L.enc_secs-z3dense:L.enc_secs);
        int K=cornerK; size_t overlap=cornerOvl, N=dense.size(), chunk=(N+K-1)/K;
        std::vector<size_t> sizes(K,0); std::vector<std::vector<uint8_t>> encs(K);
        auto tt=Clock::now(); std::vector<std::thread> th;
        for(int k=0;k<K;k++) th.emplace_back([&,k](){
            size_t a=size_t(k)*chunk, b=std::min(N,a+chunk); if(a>=b){ return; }
            ZSTD_CCtx* c=ZSTD_createCCtx(); ZSTD_CCtx_setParameter(c,ZSTD_c_compressionLevel,19);
            ZSTD_CCtx_setParameter(c,ZSTD_c_enableLongDistanceMatching,1); ZSTD_CCtx_setParameter(c,ZSTD_c_windowLog,27);
            if(a>0){ size_t o=std::min(overlap,a); ZSTD_CCtx_refPrefix(c,dense.data()+a-o,o); }
            std::vector<uint8_t> dst(ZSTD_compressBound(b-a)+64);
            size_t z=ZSTD_compress2(c,dst.data(),dst.size(),dense.data()+a,b-a); dst.resize(z);
            encs[k]=std::move(dst); sizes[k]=z; ZSTD_freeCCtx(c); });
        for(auto&t:th){ t.join(); } double s=secs(tt);
        size_t tot=0; for(auto z:sizes) tot+=z;
        bool rt=true;
        for(int k=0;k<K&&rt;k++){ size_t a=size_t(k)*chunk, b=std::min(N,a+chunk); if(a>=b) continue;
            ZSTD_DCtx* d=ZSTD_createDCtx(); ZSTD_DCtx_setParameter(d,ZSTD_d_windowLogMax,30);
            if(a>0){ size_t o=std::min(overlap,a); ZSTD_DCtx_refPrefix(d,dense.data()+a-o,o); }
            std::vector<uint8_t> out(b-a); ZSTD_inBuffer in{encs[k].data(),encs[k].size(),0}; ZSTD_outBuffer ob{out.data(),out.size(),0};
            size_t r=ZSTD_decompressStream(d,&ob,&in);
            if(ZSTD_isError(r)||ob.pos!=(b-a)||memcmp(out.data(),dense.data()+a,b-a)!=0) rt=false;
            ZSTD_freeDCtx(d); }
        size_t wp19 = wp19arg>0 ? size_t(wp19arg) : zstd_wholeprog((const uint8_t*)E.corpus.bytes.data(),E.corpus.raw,19,true,31);
        double proj=nonDense+double(tot), veff=rawTot/GB/(nonDenseZ3+s);
        bool szOK=proj<=1.10*double(wp19), spOK=veff>=1.0;
        printf("CORNER_ROW\t%s\t%zu\t%.0f\t%d\t%zu\t%zu\t%.0f\t%zu\t%.3f\t%.3f\t%d\t%d\t%d\n",
            manifest,E.TUs,rawTot,K,overlap,tot,proj,wp19,proj/double(wp19),veff,szOK?1:0,spOK?1:0,rt?1:0);
        printf("  %-13s K=%d ovl=%zuK: dense=%zu proj_cold=%.0f (%.3fx wp_z19=%zu) veff=%.3f GBps rt=%s => size %s speed %s %s\n",
            p29_name_for_manifest(manifest).c_str(),K,overlap/1024,tot,proj,proj/double(wp19),wp19,veff,rt?"OK":"FAIL",
            szOK?"OK":"OVER",spOK?"OK":"SLOW",(szOK&&spOK&&rt)?"<<< CLEARS BOTH":"");
        return 0;
    }

    if(cmd=="dumpresidual"){
        // Export the exact RAW_RUN residual (mixedRaw[1] = first-occurrence/unique content after
        // line+region dedup, BEFORE zstd coding) — the P29 residual plane — for external coders (libbsc).
        std::array<std::vector<uint8_t>,4> lanes;
        Ledger L=run_ledger(E,3,MIX_M3_STREAM,nullptr,nullptr,false,0,&lanes);
        std::vector<uint8_t>& resid = lanes[1];
        const char* op = out_path? out_path : "/tmp/residual.bin";
        FILE* f=fopen(op,"wb"); if(!f){perror(op);return 2;} if(!resid.empty()) fwrite(resid.data(),1,resid.size(),f); fclose(f);
        size_t z3=zstd_batch(resid,3,false), z19=zstd_batch(resid,19,true,31);
        printf("RESID\t%s\t%zu\t%.0f\t%zu\t%.5f\t%zu\t%zu\n",manifest,E.TUs,double(E.corpus.raw),resid.size(),
            double(resid.size())/double(E.corpus.raw),z3,z19);
        printf("  %-13s residual_raw=%zu (f=%.4f of raw_ii)  z3=%zu z19=%zu  wrote %s\n",
            p29_name_for_manifest(manifest).c_str(),resid.size(),double(resid.size())/double(E.corpus.raw),z3,z19,op);
        return 0;
    }

    if(cmd=="dumpdense"){
        std::array<std::vector<uint8_t>,4> lanes;
        Ledger L=run_ledger(E,3,MIX_M3_STREAM,nullptr,nullptr,false,0,&lanes);
        std::vector<uint8_t> dense; for(int i:{0,1,3}) dense.insert(dense.end(),lanes[i].begin(),lanes[i].end());
        double streamed3=L.mixedPartWire[0]+L.mixedPartWire[1]+L.mixedPartWire[3];
        const char* op = out_path? out_path : "/tmp/dense.bin";
        FILE* f=fopen(op,"wb"); if(!f){perror(op);return 2;} fwrite(dense.data(),1,dense.size(),f); fclose(f);
        printf("dumpdense: wrote %s (%zu bytes) | codec z3 total=%.0f  non-dense(z3)=%.0f  (proj_cold = %.0f + MT_z19(dense))\n",
            op,dense.size(),L.total(),L.total()-streamed3,L.total()-streamed3);
        return 0;
    }

    if(cmd=="parbench"){
        // Manual chunked-parallel z19 on the dense-3 lanes (libzstd here lacks built-in MT).
        // Each chunk = independent z19+LDM frame; trades a little cross-chunk LZ for parallelism.
        std::array<std::vector<uint8_t>,4> lanes;
        Ledger L=run_ledger(E,3,MIX_M3_STREAM,nullptr,nullptr,false,0,&lanes);
        std::vector<uint8_t> dense; for(int i:{0,1,3}) dense.insert(dense.end(),lanes[i].begin(),lanes[i].end());
        double streamed3=L.mixedPartWire[0]+L.mixedPartWire[1]+L.mixedPartWire[3];
        double GB=1e9, rawTot=double(E.corpus.raw), WP19=7189449.0;
        auto tz=Clock::now(); (void)zstd_batch(dense,3,false); double z3dense=secs(tz);
        double nonDenseZ3=(L.enc_secs>z3dense?L.enc_secs-z3dense:L.enc_secs);
        for(int K : {1,4,8,16,32,64}){
            size_t N=dense.size(); size_t chunk=(N+K-1)/K; std::vector<size_t> sizes(K,0);
            auto tt=Clock::now(); std::vector<std::thread> th;
            for(int k=0;k<K;k++) th.emplace_back([&,k](){
                size_t a=size_t(k)*chunk, b=std::min(N,a+chunk); if(a>=b)return;
                ZSTD_CCtx* c=ZSTD_createCCtx(); ZSTD_CCtx_setParameter(c,ZSTD_c_compressionLevel,19);
                ZSTD_CCtx_setParameter(c,ZSTD_c_enableLongDistanceMatching,1); ZSTD_CCtx_setParameter(c,ZSTD_c_windowLog,27);
                std::vector<uint8_t> dst(ZSTD_compressBound(b-a)+64);
                sizes[k]=ZSTD_compress2(c,dst.data(),dst.size(),dense.data()+a,b-a); ZSTD_freeCCtx(c); });
            for(auto&t:th){ t.join(); } double s=secs(tt);
            size_t tot=0; for(auto z:sizes)tot+=z;
            double proj=L.total()-streamed3+double(tot); double veff=rawTot/GB/(nonDenseZ3+s);
            printf("parbench K=%2d: dense_z19=%zu (%.2fs, %.0f MB/s)  proj_cold=%.0f (%.3fx wp_z19)  %s vs 1.10x  veff=%.3f GBps %s\n",
                K,tot,s,dense.size()/1e6/s,proj,proj/WP19, proj<=1.10*WP19?"OK":"OVER", veff, veff>=1.0?"[speed OK]":"[speed FAIL]");
        }
        // shared-dictionary variant: all chunks refPrefix a fixed shared sample (recovers cross-chunk LZ, stays parallel)
        size_t DICT=2u<<20;                                 // 2 MB shared dictionary (from the literal lane head)
        printf("  -- shared-dictionary (refPrefix a %zu-byte shared sample; dict transmitted once) --\n", DICT);
        for(int K : {16,32,64}){
            size_t N=dense.size(); size_t chunk=(N+K-1)/K; std::vector<size_t> sizes(K,0);
            size_t dlen=std::min(DICT,N);
            auto tt=Clock::now(); std::vector<std::thread> th;
            for(int k=0;k<K;k++) th.emplace_back([&,k](){
                size_t a=size_t(k)*chunk, b=std::min(N,a+chunk); if(a>=b)return;
                ZSTD_CCtx* c=ZSTD_createCCtx(); ZSTD_CCtx_setParameter(c,ZSTD_c_compressionLevel,19);
                ZSTD_CCtx_setParameter(c,ZSTD_c_enableLongDistanceMatching,1); ZSTD_CCtx_setParameter(c,ZSTD_c_windowLog,27);
                ZSTD_CCtx_refPrefix(c,dense.data(),dlen);      // shared sample as preceding context
                std::vector<uint8_t> dst(ZSTD_compressBound(b-a)+64);
                sizes[k]=ZSTD_compress2(c,dst.data(),dst.size(),dense.data()+a,b-a); ZSTD_freeCCtx(c); });
            for(auto&t:th){ t.join(); } double s=secs(tt);
            size_t tot=0; for(auto z:sizes)tot+=z; tot+=zstd_batch(std::vector<uint8_t>(dense.begin(),dense.begin()+dlen),19,true,27); // dict transmitted once
            double proj=L.total()-streamed3+double(tot); double veff=rawTot/GB/(nonDenseZ3+s);
            printf("parbench+dict K=%2d: dense_z19+dict=%zu (%.2fs)  proj_cold=%.0f (%.3fx wp_z19)  %s vs 1.10x  veff=%.3f GBps %s\n",
                K,tot,s,proj,proj/WP19, proj<=1.10*WP19?"OK":"OVER", veff, veff>=1.0?"[speed OK]":"[speed FAIL]");
        }
        return 0;
    }

    if(cmd=="ledger"){
        Ledger L=run_ledger(E, zlevel, mode, nullptr, nullptr, ldm, wlog);
        char tag[128]; snprintf(tag,sizeof tag,"%s z%d%s%s", mode==MIX_M3_STREAM?"RBASE-M3 stream":"CAP-M4 frames",
            zlevel, ldm?" +LDM":"", wlog?(std::string(" wlog")+std::to_string(wlog)).c_str():"");
        print_ledger(tag, L);
        double GB=1e9;
        printf("GATE TOTAL=%.0f\n", L.total());
        printf("TOURNEY corpus=%s z=%d ldm=%d wlog=%d  total=%.0f  enc=%.2fs  raw=%.0f  GBps=%.3f (raw/enc)\n",
            manifest,zlevel,ldm?1:0,wlog,L.total(),L.enc_secs,double(E.corpus.raw),double(E.corpus.raw)/GB/(L.enc_secs>0?L.enc_secs:1));
        printf("TOURNEY chrono: cum@TU100=%.0f (raw100=%.0f, enc100=%.2fs, GBps100=%.3f)  cum@TU200=%.0f (raw200=%.0f)\n",
            L.cum_wire_100,L.raw_100,L.enc_secs_100,L.raw_100/GB/(L.enc_secs_100>0?L.enc_secs_100:1),L.cum_wire_200,L.raw_200);
        return 0;
    }

    if(cmd=="wholeprog"){
        // Whole-program (all TUs concatenated in manifest order) zstd baselines. corpus.bytes IS the
        // concatenation (load_corpus lays files end-to-end). z19+long and z6+long at windowLog W.
        const uint8_t* p=(const uint8_t*)E.corpus.bytes.data(); size_t n=E.corpus.raw;
        if(prefixN && prefixN<E.TUs) n=E.corpus.files[prefixN].off;   // whole-prog of first prefixN TUs
        int W = wlog? wlog : 31; double GB=1e9;
        auto t=Clock::now(); size_t z19=zstd_wholeprog(p,n,19,true,W); double s19=secs(t);
        t=Clock::now();      size_t z6 =zstd_wholeprog(p,n,6, true,W); double s6 =secs(t);
        printf("WHOLEPROG corpus=%s raw=%.0f wlog=%d  z19+long=%zu (%.1fs %.3f GBps)  z6+long=%zu (%.1fs %.3f GBps)\n",
            manifest,double(n),W,z19,s19,double(n)/GB/s19,z6,s6,double(n)/GB/s6);
        printf("WHOLEPROG_TSV\t%s\t%.0f\t%d\t%zu\t%zu\t%.0f\n",manifest,double(n),W,z19,z6,1.10*double(z19));
        return 0;
    }

    if(cmd=="tourney"){
        // One build; measure our codec (z6 fast-config + z19 size-config) AND whole-program
        // baselines (z19+long, z6+long @wlog31) AND z6 prefixes for the chronological cap.
        double GB=1e9;
        Ledger l6 = run_ledger(E, 6, MIX_M3_STREAM, nullptr, nullptr, ldm, wlog);
        Ledger l19= run_ledger(E, 19, MIX_M3_STREAM, nullptr, nullptr, ldm, wlog);
        const uint8_t* p=(const uint8_t*)E.corpus.bytes.data();
        size_t wp19=zstd_wholeprog(p,E.corpus.raw,19,true,31);
        size_t wp6 =zstd_wholeprog(p,E.corpus.raw,6, true,31);
        size_t pfx100 = (E.TUs>100)? zstd_wholeprog(p,E.corpus.files[100].off,6,true,31) : 0;
        size_t pfx200 = (E.TUs>200)? zstd_wholeprog(p,E.corpus.files[200].off,6,true,31) : 0;
        double gbps6=double(E.corpus.raw)/GB/(l6.enc_secs>0?l6.enc_secs:1);
        printf("TOURNEY_ROW\t%s\t%zu\t%.0f\t%.0f\t%.3f\t%.0f\t%.0f\t%.0f\t%zu\t%zu\t%zu\t%zu\n",
            manifest,E.TUs,double(E.corpus.raw), l6.total(),gbps6,l6.cum_wire_100,l6.cum_wire_200,
            l19.total(),wp19,wp6,pfx100,pfx200);
        printf("  %-13s raw=%.2fGB  wp_z19@31=%zu wp_z6@31=%zu | codec z6=%.0f (%.1f%%ofz19, %.2fGBps) z19=%.0f (%.1f%%) | 1.10*z19=%.0f -> z6 %s z19 %s | speed z6 %s\n",
            p29_name_for_manifest(manifest).c_str(),double(E.corpus.raw)/GB,wp19,wp6,
            l6.total(),100.0*l6.total()/wp19,gbps6, l19.total(),100.0*l19.total()/wp19,
            1.10*double(wp19), l6.total()<=1.10*wp19?"OK":"FAIL", l19.total()<=1.10*wp19?"OK":"FAIL", gbps6>=1.0?"OK":"FAIL");
        if(pfx100) printf("  chrono: cum@100=%.0f vs z6pfx100=%zu -> %s | cum@200=%.0f vs z6pfx200=%zu -> %s\n",
            l6.cum_wire_100,pfx100,l6.cum_wire_100<=pfx100?"OK":"FAIL", l6.cum_wire_200,pfx200,(pfx200&&l6.cum_wire_200<=pfx200)?"OK":"FAIL");
        return 0;
    }
    if(cmd=="residual-census"){
        auto tc=Clock::now();
        std::vector<MixedEncoder::LiteralOccurrence> occ; occ.reserve(1u<<20);
        Ledger L=run_ledger(E, zlevel, MIX_M3_STREAM, &occ);
        // cross-check: census sees EXACTLY the RAW_RUN literal bytes the codec emitted.
        uint64_t occBytes=0; for(auto&o:occ) occBytes+=o.len;
        std::vector<uint8_t> origin; compute_origin(E, origin);
        // ---- tokenize every literal occurrence; build baseline + O0 streams causally ----
        std::vector<uint8_t> litcat;                 // baseline: raw literal lane bytes (== mixedRaw[1])
        std::vector<uint8_t> refstream;              // O0: skeleton id per line (defs FREE, ref charged)
        std::array<std::vector<uint8_t>,4> ordS, litS; // O0: per-type value ordinal + new-value literal streams
        std::vector<uint8_t> skelDict;               // FREE skeleton bodies (measured, not charged)
        std::unordered_map<std::string,uint32_t> skelMap; skelMap.reserve(1u<<20);
        std::vector<uint32_t> skelCount; std::vector<uint64_t> skelLineBytes;
        std::array<std::unordered_map<std::string,uint32_t>,4> valMap;
        std::vector<uint32_t> occSkel; occSkel.reserve(occ.size());
        uint64_t slotBytes[4]={0,0,0,0}; uint64_t slotCount[4]={0,0,0,0};
        uint64_t totSkelLen=0, totSlots=0;
        uint64_t originOcc[4]={0,0,0,0}, originBytes[4]={0,0,0,0};
        uint64_t lenHist[8]={0,0,0,0,0,0,0,0};       // <16,32,64,128,256,512,1024,>=1024
        std::string skel; std::vector<Slot> slots;
        litcat.reserve(occBytes+16);
        for(auto&o:occ){
            const LineRef& lr=E.dict.ref(o.line_id); const char* txt=E.dict.line_data(lr.off); uint32_t n=lr.len;
            litcat.insert(litcat.end(), (const uint8_t*)txt, (const uint8_t*)txt+n);
            tokenize(txt,n,skel,slots);
            // skeleton id (causal)
            auto it=skelMap.find(skel); uint32_t sid;
            if(it==skelMap.end()){ sid=uint32_t(skelMap.size()); skelMap.emplace(skel,sid);
                skelCount.push_back(0); skelLineBytes.push_back(0);
                put_varint(skelDict,skel.size()); skelDict.insert(skelDict.end(),skel.begin(),skel.end()); }
            else sid=it->second;
            put_varint(refstream,sid); occSkel.push_back(sid);
            skelCount[sid]++; skelLineBytes[sid]+=n; totSkelLen+=skel.size(); totSlots+=slots.size();
            // typed slots -> reintern (equality-linked) ordinal + new-value literal
            for(auto&s:slots){ uint8_t ty=s.type; std::string v(txt+s.off, txt+s.off+s.len);
                slotBytes[ty]+=s.len; slotCount[ty]++;
                auto vit=valMap[ty].find(v); uint32_t ord;
                if(vit==valMap[ty].end()){ ord=uint32_t(valMap[ty].size()); valMap[ty].emplace(v,ord);
                    put_varint(litS[ty],v.size()); litS[ty].insert(litS[ty].end(),v.begin(),v.end()); }
                else ord=vit->second;
                put_varint(ordS[ty],ord);
            }
            uint8_t og=origin[o.line_id]; if(og>=1&&og<=3){ originOcc[og]++; originBytes[og]+=n; }
            uint32_t b = n<16?0:n<32?1:n<64?2:n<128?3:n<256?4:n<512?5:n<1024?6:7; lenHist[b]++;
        }
        // ---- skeleton-sharing (templatable) split ----
        uint64_t nearOcc=0,nearBytes=0,uniqOcc=0,uniqBytes=0; uint32_t reusedSkel=0;
        for(size_t i=0;i<occ.size();++i){ uint32_t sid=occSkel[i]; uint32_t n=occ[i].len;
            if(skelCount[sid]>=2){ nearOcc++; nearBytes+=n; } else { uniqOcc++; uniqBytes+=n; } }
        for(size_t s=0;s<skelCount.size();++s) if(skelCount[s]>=2) reusedSkel++;
        uint64_t fixedLit = totSkelLen - totSlots;                       // skeleton literal bytes (free structure)
        uint64_t slotTot  = slotBytes[0]+slotBytes[1]+slotBytes[2]+slotBytes[3];
        // ---- compression measurements (batched: global window, framing-free) ----
        double MiB=1048576.0;
        size_t cold_lit_z3  = zstd_batch(litcat,3,false);
        size_t cold_lit_z19 = zstd_batch(litcat,19,true);
        auto o0=[&](int lv,bool ldm)->size_t{ size_t s=zstd_batch(refstream,lv,ldm);
            for(int ty=0;ty<4;++ty){ s+=zstd_batch(ordS[ty],lv,ldm)+zstd_batch(litS[ty],lv,ldm); } return s; };
        size_t o0_z3=o0(3,false), o0_z19=o0(19,true);
        size_t ref_z3=zstd_batch(refstream,3,false); size_t skelDict_z3=zstd_batch(skelDict,3,false);
        size_t skelDict_z19=zstd_batch(skelDict,19,true);
        // O1 = FULLY CHARGED (decisive gate): O0 + rule-definition bytes (skeleton dict).  ref=rule refs,
        // ord=slot-equality metadata, lit=slot values already charged in O0; residuals=0 (full coverage);
        // child-rule refs not modelled (would only add cost).  Batched framing ~0 (conservative for O1).
        size_t o1_z3 = o0_z3 + skelDict_z3, o1_z19 = o0_z19 + skelDict_z19;
        size_t ordz3[4],litz3[4]; for(int ty=0;ty<4;++ty){ ordz3[ty]=zstd_batch(ordS[ty],3,false); litz3[ty]=zstd_batch(litS[ty],3,false); }
        // ---- gates ----
        double C=L.total();
        uint64_t Z = want_z19 ? z19_total(E) : 0;   // per-TU z19 is the slow part; gate it behind --z19
        double head_z3 = double(cold_lit_z3) - double(o0_z3);            // free-def O0 headroom vs plain zstd (L3, matched)
        double head_z19= double(cold_lit_z19)- double(o0_z19);
        double test_z3 = C - head_z3;                                    // O0-ceiling total wire (substitute literal component)
        double test_z19= C - head_z19;
        // structure-FULLY-free ceiling: also charge ZERO for skeleton SELECTION (ref) — an oracle on
        // structure.  Remaining charge = slot VALUES only (typed ordinal+literal).  If THIS still
        // exceeds 0.80*C the residual is pure content entropy and the representation is dead.
        double o0_slotsonly_z3 = double(o0_z3) - double(ref_z3);
        double test_free_z3    = C - (double(cold_lit_z3) - o0_slotsonly_z3);
        double test_o1_z3      = C - (double(cold_lit_z3) - double(o1_z3));   // decisive fully-charged gate
        double test_o1_z19     = C - (double(cold_lit_z19)- double(o1_z19));
        double raw=double(E.corpus.raw); double target400=raw/400.0;
        const char* tag = manifest;

        printf("\n################ RESIDUAL CENSUS + S0 O0 FREE-DEFINITION CEILING ################\n");
        printf("corpus=%s TUs=%zu raw=%.1f MiB regions=%u distinct_lines=%u\n",tag,E.TUs,raw/MiB,E.NREG,E.dict.distinct());
        printf("-- RBASE-M3 component breakdown (bytes) --\n");
        printf("  root=%.0f region_def=%.0f line_def=%.0f block_def=%.0f path_def=%.0f missing=%.0f framing=%.0f  TOTAL(C=cold0)=%.0f\n",
            L.w_root,L.w_regiondef,L.w_linedef,L.w_blockdef,L.w_pathdef,L.w_missing,L.w_framing,C);
        printf("  line_def split: literal(RAW_RUN)=%.0f array_control=%.0f array_values=%.0f selector=%.0f\n",
            L.mixedPartWire[1],L.mixedPartWire[2],L.mixedPartWire[3],L.selector);
        printf("-- RAW_RUN literal residual (the census target = line_def literal component) --\n");
        printf("  occurrences=%zu (raw=%llu B  cross-check vs codec mixedLiteralRaw=%llu : %s)\n",
            occ.size(),(unsigned long long)occBytes,(unsigned long long)L.literalRaw, occBytes==L.literalRaw?"MATCH":"*** MISMATCH ***");
        printf("  compressed (batched global window): z3=%zu (%.2f MiB)  z19+long=%zu (%.2f MiB)\n",
            cold_lit_z3,cold_lit_z3/MiB,cold_lit_z19,cold_lit_z19/MiB);
        printf("  streamed literal component in actual wire (mixedPartWire[1])=%.0f  [batched z3 within %.1f%%]\n",
            L.mixedPartWire[1], 100.0*(L.mixedPartWire[1]-double(cold_lit_z3))/L.mixedPartWire[1]);
        printf("-- source-origin of literal residual (first-occurrence) --\n");
        const char* on[4]={"?","generated","system-hdr","project"};
        for(int g=1;g<=3;++g) printf("  %-10s occ=%llu raw=%llu B (%.1f%% of residual)\n",
            on[g],(unsigned long long)originOcc[g],(unsigned long long)originBytes[g],100.0*originBytes[g]/double(occBytes?occBytes:1));
        printf("-- tokenization: fixed skeleton (FREE in O0) vs typed slots (charged content) --\n");
        printf("  fixed-literal skeleton bytes=%llu (%.1f%% of raw)   slot bytes=%llu (%.1f%% of raw)   [slots: str=%llu chr=%llu num=%llu id=%llu]\n",
            (unsigned long long)fixedLit,100.0*fixedLit/double(occBytes?occBytes:1),(unsigned long long)slotTot,100.0*slotTot/double(occBytes?occBytes:1),
            (unsigned long long)slotBytes[0],(unsigned long long)slotBytes[1],(unsigned long long)slotBytes[2],(unsigned long long)slotBytes[3]);
        printf("  slot occurrences: str=%llu chr=%llu num=%llu id=%llu   distinct values: str=%zu chr=%zu num=%zu id=%zu\n",
            (unsigned long long)slotCount[0],(unsigned long long)slotCount[1],(unsigned long long)slotCount[2],(unsigned long long)slotCount[3],
            valMap[0].size(),valMap[1].size(),valMap[2].size(),valMap[3].size());
        printf("-- skeleton (template) structure --\n");
        printf("  distinct skeletons=%zu  reused(>=2 lines)=%u (%.1f%%)  skeleton-dict raw=%zu B z3=%zu B (FREE)\n",
            skelMap.size(),reusedSkel,100.0*reusedSkel/double(skelMap.size()?skelMap.size():1),skelDict.size(),skelDict_z3);
        printf("  near-repeat lines (shared skeleton>=2): occ=%llu raw=%llu B (%.1f%%)   idiosyncratic (unique skeleton): occ=%llu raw=%llu B (%.1f%%)\n",
            (unsigned long long)nearOcc,(unsigned long long)nearBytes,100.0*nearBytes/double(occBytes?occBytes:1),
            (unsigned long long)uniqOcc,(unsigned long long)uniqBytes,100.0*uniqBytes/double(occBytes?occBytes:1));
        printf("-- line length histogram (occurrences) <16 <32 <64 <128 <256 <512 <1024 >=1024 --\n  ");
        for(int b=0;b<8;++b){ printf("%llu ",(unsigned long long)lenHist[b]); } printf("\n");
        printf("-- MATERIAL_BLOCK ceilings on the literal residual --\n");
        printf("  O0 [oracle_free_definitions] (skeleton dict FREE; charged= ref + typed ordinal + new-value literal) -- IMPOSSIBILITY SCREEN:\n");
        printf("  O0 literal wire: z3=%zu (%.2f MiB)  z19+long=%zu (%.2f MiB)\n",o0_z3,o0_z3/MiB,o0_z19,o0_z19/MiB);
        printf("  O1 [fully_charged] (= O0 + rule-definition/skeleton-dict bytes; residuals=0; child-rules n/a) -- DECISIVE GATE:\n");
        printf("  O1 literal wire: z3=%zu (%.2f MiB)  z19+long=%zu (%.2f MiB)   [skeleton-dict charge z3=%zu z19=%zu]\n",
            o1_z3,o1_z3/MiB,o1_z19,o1_z19/MiB,skelDict_z3,skelDict_z19);
        printf("  O0 z3 breakdown: ref=%zu  str[ord=%zu lit=%zu] chr[ord=%zu lit=%zu] num[ord=%zu lit=%zu] id[ord=%zu lit=%zu]\n",
            ref_z3,ordz3[0],litz3[0],ordz3[1],litz3[1],ordz3[2],litz3[2],ordz3[3],litz3[3]);
        printf("  headroom vs plain-zstd literal (matched level): z3=%.0f B (%.1f%% of literal)  z19=%.0f B (%.1f%% of literal)\n",
            head_z3,100.0*head_z3/double(cold_lit_z3), head_z19,100.0*head_z19/double(cold_lit_z19));
        printf("-- GO / NO-GO (headline) --\n");
        printf("  C (cold0=RBASE-M3) = %.0f\n",C);
        if(Z) printf("  Z (z19 whole-build, sum indep per-TU frames) = %llu\n",(unsigned long long)Z);
        else  printf("  Z (z19 whole-build) = [skipped; pass --z19]\n");
        printf("  0.80*C (>=20%% fewer gate) = %.0f    raw/400 (cold-400x target) = %.0f\n",0.80*C,target400);
        auto pz=[&](double p){ return Z? (p<double(Z)?"YES":"no") : "n/a"; };
        printf("  test_total (O0 ceiling, z3 headroom)  = %.0f   [vs C: %.1f%%]  P<Z? %s   P<=0.80C? %s   P<=raw/400? %s\n",
            test_z3,100.0*test_z3/C, pz(test_z3), test_z3<=0.80*C?"YES":"NO", test_z3<=target400?"YES":"no");
        printf("  test_total (O0 ceiling, z19 headroom) = %.0f   [vs C: %.1f%%]  P<Z? %s   P<=0.80C? %s   P<=raw/400? %s\n",
            test_z19,100.0*test_z19/C, pz(test_z19), test_z19<=0.80*C?"YES":"NO", test_z19<=target400?"YES":"no");
        printf("  test_total (structure FULLY free: skeleton bodies+selection free; slot VALUES only, z3) = %.0f   [vs C: %.1f%%]  P<=0.80C? %s\n",
            test_free_z3,100.0*test_free_z3/C, test_free_z3<=0.80*C?"YES":"NO");
        printf("  >> O1 DECISIVE (fully charged) z3  = %.0f   [vs C: %.1f%%]  P<Z? %s   P<=0.80C? %s   P<=raw/400? %s\n",
            test_o1_z3,100.0*test_o1_z3/C, pz(test_o1_z3), test_o1_z3<=0.80*C?"YES":"NO", test_o1_z3<=target400?"YES":"no");
        printf("  >> O1 DECISIVE (fully charged) z19 = %.0f   [vs C: %.1f%%]  P<Z? %s   P<=0.80C? %s   P<=raw/400? %s\n",
            test_o1_z19,100.0*test_o1_z19/C, pz(test_o1_z19), test_o1_z19<=0.80*C?"YES":"NO", test_o1_z19<=target400?"YES":"no");
        printf("  VERDICT: O0 impossibility screen %s (O0 z3 %.1f%% of C, gate<=80%%).  Representation %s.\n",
            test_z3<=0.80*C?"PASSED (continue to O1)":"FAILED", 100.0*test_z3/C,
            test_z3<=0.80*C?"not screened out":"IMPOSSIBLE -> STOP/redesign (O1 only confirms; it is >= O0)");
        // machine-readable row for cross-corpus aggregation
        printf("CENSUS_TSV\t%s\t%zu\t%.0f\t%llu\t%llu\t%zu\t%zu\t%zu\t%zu\t%.0f\t%.0f\t%.0f\t%.0f\t%llu\t%llu\t%llu\t%.0f\t%zu\t%.0f\t%.0f\t%zu\t%zu\n",
            tag,E.TUs,raw,(unsigned long long)C,(unsigned long long)Z,cold_lit_z3,o0_z3,cold_lit_z19,o0_z19,
            test_z3,test_z19,0.80*C,target400,(unsigned long long)occBytes,(unsigned long long)fixedLit,(unsigned long long)slotTot,
            test_free_z3,ref_z3,test_o1_z3,test_o1_z19,o1_z3,skelDict_z3);
        fprintf(stderr,"[census] done in %.1fs\n",secs(tc));
        return 0;
    }

    if(cmd=="curve"){
        std::vector<MixedEncoder::LiteralOccurrence> occ; std::vector<double> lit_this;
        Ledger L=run_ledger(E, zlevel, MIX_M3_STREAM, &occ, &lit_this);
        // RBASE-P29 frontier (local-oracle's exact export; stateful/chronological, not M4).
        P29 p29; std::string p29name = p29_corpus? std::string(p29_corpus) : p29_name_for_manifest(manifest);
        if(p29_path){ p29=load_p29(p29_path,p29name,E.TUs); if(!p29.ok) fprintf(stderr,"[curve] WARNING: P29 not wired for this corpus\n"); }
        // per-TU offsets into occ (occ is emitted in strict TU order)
        std::vector<size_t> tuStart(E.TUs+1,occ.size());
        { size_t k=0; for(size_t t=0;t<E.TUs;++t){ tuStart[t]=k; while(k<occ.size()&&occ[k].tu==t) ++k; } tuStart[E.TUs]=k; }
        // per-TU MATERIAL_BLOCK streams (shared-window, causal).  O0 charges ref+typed ord+lit
        // (skeleton dict FREE).  O1 additionally charges the skeleton-def bytes (fully charged).
        const double FRAME=4;
        std::vector<double> o0_this(E.TUs,0.0), skel_this(E.TUs,0.0), o1_this(E.TUs,0.0);
        { ZSTD_CCtx* ref_z=ZSTD_createCCtx(); ZSTD_CCtx* skel_z=ZSTD_createCCtx(); std::array<ZSTD_CCtx*,4> ordz{},litz{};
          auto mk=[&](ZSTD_CCtx*c){ ZSTD_CCtx_setParameter(c,ZSTD_c_compressionLevel,zlevel); ZSTD_CCtx_setParameter(c,ZSTD_c_contentSizeFlag,0); };
          mk(ref_z); mk(skel_z); for(int i=0;i<4;++i){ ordz[i]=ZSTD_createCCtx(); litz[i]=ZSTD_createCCtx(); mk(ordz[i]); mk(litz[i]); }
          std::unordered_map<std::string,uint32_t> skelMap; std::array<std::unordered_map<std::string,uint32_t>,4> valMap;
          std::string skel; std::vector<Slot> slots;
          bool refActive=false,skelActive=false; std::array<bool,4> ordActive{},litActive{};
          for(size_t t=0;t<E.TUs;++t){
            std::vector<uint8_t> ref,skelDefB; std::array<std::vector<uint8_t>,4> ordB,litB;
            for(size_t k=tuStart[t];k<tuStart[t+1];++k){ auto&o=occ[k]; const LineRef& lr=E.dict.ref(o.line_id);
              const char* txt=E.dict.line_data(lr.off); uint32_t n=lr.len; tokenize(txt,n,skel,slots);
              auto it=skelMap.find(skel); uint32_t sid; if(it==skelMap.end()){ sid=uint32_t(skelMap.size()); skelMap.emplace(skel,sid);
                  put_varint(skelDefB,skel.size()); skelDefB.insert(skelDefB.end(),skel.begin(),skel.end()); } else sid=it->second;
              put_varint(ref,sid);
              for(auto&s:slots){ uint8_t ty=s.type; std::string v(txt+s.off,txt+s.off+s.len);
                auto vit=valMap[ty].find(v); uint32_t ord; if(vit==valMap[ty].end()){ ord=uint32_t(valMap[ty].size()); valMap[ty].emplace(v,ord);
                  put_varint(litB[ty],v.size()); litB[ty].insert(litB[ty].end(),v.begin(),v.end()); } else ord=vit->second;
                put_varint(ordB[ty],ord); } }
            double tuw=0;
            if(!ref.empty()){ refActive=true; tuw+=zstd_stream_encode(ref_z,ref,ZSTD_e_flush).size()+FRAME; }
            for(int i=0;i<4;++i){ if(!ordB[i].empty()){ ordActive[i]=true; tuw+=zstd_stream_encode(ordz[i],ordB[i],ZSTD_e_flush).size()+FRAME; }
                                  if(!litB[i].empty()){ litActive[i]=true; tuw+=zstd_stream_encode(litz[i],litB[i],ZSTD_e_flush).size()+FRAME; } }
            o0_this[t]=tuw;
            if(!skelDefB.empty()){ skelActive=true; skel_this[t]=zstd_stream_encode(skel_z,skelDefB,ZSTD_e_flush).size()+FRAME; }
          }
          std::vector<uint8_t> emp;
          if(refActive) o0_this[E.TUs-1]+=zstd_stream_encode(ref_z,emp,ZSTD_e_end).size();
          for(int i=0;i<4;++i){ if(ordActive[i]) o0_this[E.TUs-1]+=zstd_stream_encode(ordz[i],emp,ZSTD_e_end).size();
                                if(litActive[i]) o0_this[E.TUs-1]+=zstd_stream_encode(litz[i],emp,ZSTD_e_end).size(); }
          if(skelActive) skel_this[E.TUs-1]+=zstd_stream_encode(skel_z,emp,ZSTD_e_end).size();
          for(size_t t=0;t<E.TUs;++t) o1_this[t]=o0_this[t]+skel_this[t];
          ZSTD_freeCCtx(ref_z); ZSTD_freeCCtx(skel_z); for(int i=0;i<4;++i){ ZSTD_freeCCtx(ordz[i]); ZSTD_freeCCtx(litz[i]); }
        }
        bool useO1 = strcmp(test_mode,"o0")!=0;   // default o1 (decisive fully-charged)
        std::vector<double>& testM = useO1? o1_this : o0_this;
        // per-TU z19 (independent whole-TU frame; optional)
        std::vector<double> z19_this(E.TUs,0.0);
        if(want_z19) for(size_t t=0;t<E.TUs;++t){ auto&f=E.corpus.files[t];
            std::vector<uint8_t> v((const uint8_t*)E.corpus.bytes.data()+f.off,(const uint8_t*)E.corpus.bytes.data()+f.off+f.len);
            z19_this[t]=zstd_batch(v,19,true); }
        // write curve.tsv
        FILE* out = out_path?fopen(out_path,"w"):stdout; if(!out){perror(out_path);return 2;}
        fprintf(out,"tu_index\traw_this_tu\traw_cumulative\tz19_this_tu\tz19_cumulative\tcold0_this_tu\tcold0_cumulative\tp29_this_tu\tp29_cumulative\ttest_this_tu\ttest_cumulative\tsaving_vs_z19_cumulative\tsaving_vs_cold0_cumulative\tsaving_vs_p29_cumulative\texact\tstate_bytes\n");
        double rawC=0,z19C=0,coldC=0,testC=0;
        std::vector<double> testCum(E.TUs), z19Cum(E.TUs), rawCum(E.TUs), coldCum(E.TUs);
        for(size_t t=0;t<E.TUs;++t){
            double rawT=E.corpus.files[t].len; double testT=L.cold0_this[t]-lit_this[t]+testM[t];
            double p29t = p29.ok? p29.wire_this[t] : 0.0;
            double p29c = p29.ok? p29.wire_cum[t]  : 0.0;
            rawC+=rawT; z19C+=z19_this[t]; coldC+=L.cold0_this[t]; testC+=testT;
            rawCum[t]=rawC; z19Cum[t]=z19C; coldCum[t]=coldC; testCum[t]=testC;
            fprintf(out,"%zu\t%.0f\t%.0f\t%.0f\t%.0f\t%.0f\t%.0f\t%.0f\t%.0f\t%.0f\t%.0f\t%.0f\t%.0f\t%.0f\t%d\t%.0f\n",
                t,rawT,rawC,z19_this[t],z19C,L.cold0_this[t],coldC, p29t, p29c, testT,testC,
                z19C-testC, coldC-testC, (p29.ok? p29c-testC : 0.0), 1, L.state_bytes[t]);
        }
        if(out!=stdout) fclose(out);
        // break-even: earliest TU after which inequality holds for ALL later TUs
        auto breakeven=[&](std::function<bool(size_t)> ok)->long{ long be=-1; for(long t=long(E.TUs)-1;t>=0;--t){ if(!ok(size_t(t))){ be=t+1; break; } } if(be==-1) be=0; if(size_t(be)>=E.TUs) return -1; return be; };
        double C=coldC;
        printf("[curve] corpus=%s TUs=%zu  raw=%.0f  cold0(C)=%.0f  test(MATERIAL_BLOCK %s stream)=%.0f (%.1f%% of C)  z19=%.0f%s\n",
            manifest,E.TUs,rawC,C,useO1?"O1 fully-charged":"O0 free-def",testC,100.0*testC/C, z19C, want_z19?"":" [z19 skipped]");
        printf("[curve] full-run gates: P<=0.80C? %s (0.80C=%.0f)   P<raw/400? %s (raw/400=%.0f)   %s\n",
            testC<=0.80*C?"YES":"NO",0.80*C, testC<double(E.corpus.raw)/400.0?"YES":"no",double(E.corpus.raw)/400.0,
            want_z19?(testC<z19C?"P<Z19? YES":"P<Z19? no"):"P<Z19? n/a");
        if(p29.ok) printf("[curve] frontier: P29(%s, stateful/chronological, NOT M4)=%.0f (%.1f%% of C0)  |  test(%s)=%.0f = %.1f%% of P29 -> %s frontier\n",
            p29name.c_str(),p29.total,100*p29.total/C, useO1?"O1":"O0", testC, 100*testC/p29.total, testC<=p29.total?"beats":"ABOVE (worse than)");
        if(want_z19){ long beZ=breakeven([&](size_t t){ return testCum[t]<z19Cum[t]; });
            long be20=breakeven([&](size_t t){ return testCum[t]<=0.80*coldCum[t]; });
            auto rep=[&](const char* nm,long be){ if(be<0){printf("[curve] %s: never (or from TU0)\n",nm);} else
                printf("[curve] %s: TU=%ld cumraw=%.0f raw-fraction=%.3f\n",nm,be,rawCum[be],rawCum[be]/rawC); };
            rep("BE_Z19",beZ); rep("BE_20",be20);
        }
        return 0;
    }

    if(cmd=="export-events"){
        // Per-TU M3 opcode/event stream + per-line source-origin -> events.<basename>.zst
        std::vector<MixedEncoder::LiteralOccurrence> occ;
        Ledger L=run_ledger(E, zlevel, MIX_M3_STREAM, &occ);
        std::vector<uint8_t> origin; compute_origin(E, origin);
        // Build a compact textual event log then zstd it.
        std::vector<uint8_t> log;
        auto put=[&](const char* s){ log.insert(log.end(),(const uint8_t*)s,(const uint8_t*)s+strlen(s)); };
        char buf[256];
        // header + per-TU op-count + literal-origin rollup (kept compact; full per-op stream = mixedPartWire[0] control lane)
        snprintf(buf,sizeof buf,"# material_lab export-events corpus=%s TUs=%zu NREG=%u distinct_lines=%u\n",manifest,E.TUs,E.NREG,E.dict.distinct()); put(buf);
        put("# columns: TU\tregions_materialized\tliteral_occ\tlit_raw_bytes\torigin[gen/sys/proj]\n");
        // group occ by TU
        std::vector<size_t> tuStart(E.TUs+1,occ.size());
        { size_t k=0; for(size_t t=0;t<E.TUs;++t){ tuStart[t]=k; while(k<occ.size()&&occ[k].tu==t) ++k; } tuStart[E.TUs]=k; }
        for(size_t t=0;t<E.TUs;++t){ uint64_t lb=0,og[4]={0,0,0,0};
            for(size_t k=tuStart[t];k<tuStart[t+1];++k){ lb+=occ[k].len; uint8_t g=origin[occ[k].line_id]; if(g<4)og[g]++; }
            snprintf(buf,sizeof buf,"%zu\t?\t%zu\t%llu\t%llu/%llu/%llu\n",t,tuStart[t+1]-tuStart[t],(unsigned long long)lb,
                (unsigned long long)og[1],(unsigned long long)og[2],(unsigned long long)og[3]); put(buf); }
        // opcode totals footer
        snprintf(buf,sizeof buf,"# ops RAW_RUN=%llu publish=%llu ref=%llu BYTE_ARRAY=%llu PP_MARKER=%llu  literal_raw=%llu\n",
            (unsigned long long)L.mixedOps[0],(unsigned long long)L.mixedOps[1],(unsigned long long)L.mixedOps[2],
            (unsigned long long)L.mixedOps[3],(unsigned long long)L.mixedOps[4],(unsigned long long)L.literalRaw); put(buf);
        std::vector<uint8_t> dst(ZSTD_compressBound(log.size())+64);
        ZSTD_CCtx* c=ZSTD_createCCtx(); ZSTD_CCtx_setParameter(c,ZSTD_c_compressionLevel,19);
        size_t z=ZSTD_compress2(c,dst.data(),dst.size(),log.data(),log.size()); ZSTD_freeCCtx(c);
        std::string op = out_path?out_path:"events.out.zst";
        FILE* f=fopen(op.c_str(),"wb"); if(!f){perror(op.c_str());return 2;} fwrite(dst.data(),1,z,f); fclose(f);
        printf("[export-events] wrote %s (%zu B log -> %zu B zst) TUs=%zu literal_occ=%zu\n",op.c_str(),log.size(),z,E.TUs,occ.size());
        return 0;
    }

    fprintf(stderr,"subcommand %s not yet implemented in this build\n",cmd.c_str());
    return 2;
}
