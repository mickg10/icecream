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
    double total() const { return w_root+w_linedef+w_regiondef+w_blockdef+w_pathdef+w_missing+w_framing; }
};

static Ledger run_ledger(Engine& E, int zlevel, MixMode mode,
                         std::vector<MixedEncoder::LiteralOccurrence>* census=nullptr,
                         std::vector<double>* lit_this_out=nullptr){
    Ledger L; const double FRAME=4; const size_t MP=4;
    ZSTD_CCtx* z=ZSTD_createCCtx();
    std::array<ZSTD_CCtx*,4> mixedZC{};
    for(size_t i=0;i<MP;++i){ mixedZC[i]=ZSTD_createCCtx();
        ZSTD_CCtx_setParameter(mixedZC[i],ZSTD_c_compressionLevel,zlevel);
        ZSTD_CCtx_setParameter(mixedZC[i],ZSTD_c_contentSizeFlag,0); }
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
        L.w_root += zstd_size(z, rootb.data(), rootb.size(), zlevel, dst); L.w_framing += FRAME;
        // block manifest (COPY when possible in cold)
        manifestBlocks.clear(); for(uint32_t k:requiredBlocks) if(!fknownBlk[k]) manifestBlocks.push_back(k);
        blockRaw.clear();
        if(!manifestBlocks.empty()){
            put_varint(blockRaw, manifestBlocks.size());
            for(uint32_t k:manifestBlocks){ put_varint(blockRaw,k); size_t Lb=E.boff2[k+1]-E.boff2[k];
                if(E.bcopy_ok[k]){ blockRaw.push_back(1); put_varint(blockRaw,E.bcopy_src[k]); put_varint(blockRaw,Lb); }
                else { blockRaw.push_back(0); put_varint(blockRaw,Lb); for(size_t j=E.boff2[k];j<E.boff2[k+1];++j) put_varint(blockRaw,E.bchild[j]); } }
            L.w_blockdef += zstd_size(z, blockRaw.data(), blockRaw.size(), zlevel, dst) + FRAME;
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
            L.w_missing += zstd_size(z, needPayload.data(), needPayload.size(), zlevel, dst) + FRAME;
        // materialize
        uint32_t nr=0;
        if(!missReg.empty()){ nr=enc.materialize(E.dict, missReg, t);
            for(uint32_t r:missReg){ known[r]=1; stateAcc+=E.dict.region_raw_len(r); } }
        else { for(auto&v:enc.mixedRaw) v.clear(); enc.fill_paths.clear(); enc.np=0; }
        // mixed lane
        if(nr){
            for(size_t i=0;i<MP;++i) if(!enc.mixedRaw[i].empty()){
                size_t sz;
                if(mode==MIX_M4_FRAMES){ sz=zstd_size(mixedZC[i], enc.mixedRaw[i].data(), enc.mixedRaw[i].size(), zlevel, dst); }
                else { active[i]=true; encLastTU[i]=zstd_stream_encode(mixedZC[i], enc.mixedRaw[i], ZSTD_e_flush);
                       sz=encLastTU[i].size(); }
                double bytes=sz+FRAME; L.mixedPartWire[i]+=bytes; (i==0?L.w_regiondef:L.w_linedef)+=bytes;
                if(i==1 && lit_this_out) (*lit_this_out)[t]+=bytes;   // per-TU RAW_RUN literal component
            }
            L.w_linedef += 1; L.selector += 1;
        }
        if(enc.np){ L.w_pathdef += zstd_size(z, enc.fill_paths.data(), enc.fill_paths.size(), zlevel, dst); }
        if(enc.np||nr){ L.w_framing += FRAME; }
        L.cold0_this[t]=L.total()-before; L.state_bytes[t]=double(stateAcc);
    }
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
static size_t zstd_batch(const std::vector<uint8_t>& v, int level, bool ldm){
    if(v.empty()) return 0;
    ZSTD_CCtx* c=ZSTD_createCCtx();
    ZSTD_CCtx_setParameter(c,ZSTD_c_compressionLevel,level);
    if(ldm){ ZSTD_CCtx_setParameter(c,ZSTD_c_enableLongDistanceMatching,1); ZSTD_CCtx_setParameter(c,ZSTD_c_windowLog,27); }
    std::vector<uint8_t> dst(ZSTD_compressBound(v.size())+64);
    size_t r=ZSTD_compress2(c,dst.data(),dst.size(),v.data(),v.size());
    if(ZSTD_isError(r)){ fprintf(stderr,"zstd_batch %s\n",ZSTD_getErrorName(r)); exit(2); }
    ZSTD_freeCCtx(c); return r;
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

int main(int argc,char**argv){
    if(argc<2){ fprintf(stderr,"usage: %s <ledger|residual-census|curve|export-events> --manifest F [--z N] [--mode m3|m4] [--max-files N]\n",argv[0]); return 2; }
    std::string cmd=argv[1];
    const char* manifest=nullptr; int zlevel=3; size_t max_files=SIZE_MAX; MixMode mode=MIX_M3_STREAM;
    bool want_z19=false; const char* out_path=nullptr; const char* test_mode="o1";
    for(int i=2;i<argc;++i){
        if(!strcmp(argv[i],"--manifest")&&i+1<argc) manifest=argv[++i];
        else if(!strcmp(argv[i],"--z")&&i+1<argc) zlevel=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--mode")&&i+1<argc){ mode=!strcmp(argv[i+1],"m4")?MIX_M4_FRAMES:MIX_M3_STREAM; ++i; }
        else if(!strcmp(argv[i],"--max-files")&&i+1<argc){ max_files=strtoull(argv[++i],nullptr,10); }
        else if(!strcmp(argv[i],"--z19")) want_z19=true;
        else if(!strcmp(argv[i],"--out")&&i+1<argc) out_path=argv[++i];
        else if(!strcmp(argv[i],"--test-mode")&&i+1<argc) test_mode=argv[++i];
        else { fprintf(stderr,"unknown arg %s\n",argv[i]); return 2; }
    }
    (void)out_path;
    if(cmd=="rbase-p29"){
        printf("RBASE-P29: pending local-oracle P29 per-TU export (separate codec; not reproduced in material_lab).\n");
        printf("  When available, add columns p29_this_tu / p29_cumulative next to cold0_* using local-oracle's numbers.\n");
        return 0;
    }
    if(!manifest){ fprintf(stderr,"need --manifest\n"); return 2; }

    Engine E; E.build(manifest,max_files);

    if(cmd=="ledger"){
        Ledger L=run_ledger(E, zlevel, mode);
        print_ledger(mode==MIX_M3_STREAM?"RBASE-M3 (shared-window streaming; z3)":"CAP-M4 (per-TU frames; validation)", L);
        printf("GATE TOTAL=%.0f\n", L.total());
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
        fprintf(out,"tu_index\traw_this_tu\traw_cumulative\tz19_this_tu\tz19_cumulative\tcold0_this_tu\tcold0_cumulative\ttest_this_tu\ttest_cumulative\tsaving_vs_z19_cumulative\tsaving_vs_cold0_cumulative\texact\tstate_bytes\n");
        double rawC=0,z19C=0,coldC=0,testC=0;
        std::vector<double> testCum(E.TUs), z19Cum(E.TUs), rawCum(E.TUs), coldCum(E.TUs);
        for(size_t t=0;t<E.TUs;++t){
            double rawT=E.corpus.files[t].len; double testT=L.cold0_this[t]-lit_this[t]+testM[t];
            rawC+=rawT; z19C+=z19_this[t]; coldC+=L.cold0_this[t]; testC+=testT;
            rawCum[t]=rawC; z19Cum[t]=z19C; coldCum[t]=coldC; testCum[t]=testC;
            fprintf(out,"%zu\t%.0f\t%.0f\t%.0f\t%.0f\t%.0f\t%.0f\t%.0f\t%.0f\t%.0f\t%.0f\t%d\t%.0f\n",
                t,rawT,rawC,z19_this[t],z19C,L.cold0_this[t],coldC,testT,testC,
                z19C-testC, coldC-testC, 1, L.state_bytes[t]);
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
