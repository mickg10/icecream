// icecream #16 capability harness — cap_main.cpp  (M1 two-process split + M2 survival)
//
// GENUINE two-process C<->F split of codec50-m1's canonical M1 path
// (--mixed-regions --byte-array-lines --direct-ordinals, z3), over cap_transport frames.
// M2 adds public-Line survival: immutable materialization on F, typed recovery of public
// Lines/Regions/Blocks/paths under worker restart, late join, and bounded eviction — while
// the pure-cold 7-category ledger stays byte-identical to M1 (DuckDB 9,590,734 / RocksDB
// 9,825,414).  In cold the recovery machinery is a strict no-op (op7/op8=0, NEED drop=0,
// no Rejoin frame).
//
// Per-TU dialogue: C->Root[rootb|blockRaw] ; F->Need[missReg|drops] ; C->Fill[paths|mixed0..3].
// M2 restart/late-join: F->Rejoin[resumeTU] ; C->Ack[nextPublic|pathTable] inserted before Root.
//
// build: g++ -O3 -std=c++17 -DICE_LINE_CAP_LOG2=23 cap_main.cpp cap_codec.cpp -o cap_main -lzstd
#include "cap_codec.h"
#include "cap_protocol.h"
#include "cap_transport.h"
#include "cap_identity.h"
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <chrono>
using namespace capc;
using Clock = std::chrono::steady_clock;
static double secs(Clock::time_point b){ return std::chrono::duration<double>(Clock::now()-b).count(); }

enum { SC_COLD=0, SC_RESTART=1, SC_LATEJOIN=2 };

// ===================== C role (parent): encoder/authority + ledger =====================
static int run_C(int fd, const Interner& dict, const Corpus& corpus,
                 const std::vector<uint32_t>& tokstream, const std::vector<size_t>& tokoff,
                 const std::vector<size_t>& boff2, const std::vector<uint32_t>& bchild,
                 const std::vector<uint32_t>& bcopy_src, const std::vector<uint8_t>& bcopy_ok,
                 uint32_t NREG, uint32_t NBLK, size_t TUs, int zlevel, const char* manifest,
                 int scenario, size_t scenarioT){
    const double FRAME=4; const size_t mixedPartCount=4;
    ZSTD_CCtx* z=ZSTD_createCCtx(); ZSTD_DCtx* messageD=ZSTD_createDCtx();
    std::array<ZSTD_CCtx*,6> mixedZC{}; std::array<uint8_t,6> mixedZActive{};
    for(size_t i=0;i<mixedPartCount;++i){ mixedZC[i]=ZSTD_createCCtx();
        ZSTD_CCtx_setParameter(mixedZC[i],ZSTD_c_compressionLevel,zlevel);
        ZSTD_CCtx_setParameter(mixedZC[i],ZSTD_c_contentSizeFlag,0); }
    MixedEncoder enc; enc.init(dict.distinct(), NREG);
    std::vector<uint8_t> fknownBlk(NBLK,0);
    std::vector<uint32_t> requiredBlockStamp(NBLK,0); uint32_t requestStamp=0;
    double w_root=0,w_linedef=0,w_regiondef=0,w_pathdef=0,w_blockdef=0,w_missing=0,w_framing=0;
    std::array<double,6> mixedPartWire{}; double mixedSelectorWire=0, mixedMissingRequestWire=0;
    std::vector<uint8_t> rootb, blockRaw, messageEncoded, messageDecoded, dst;
    std::vector<uint32_t> requiredBlocks, manifestBlocks, missReg;
    uint64_t socket_bytes=0, literalBlockExtraRaw=0, dropsCleared=0;
    auto tpass=Clock::now();

    // ---- latejoin warm pass: build C's authority (mixedCLine/nextPublic/paths) for TUs 0..T-1
    // exactly as if a cold worker had been served, with NO socket I/O and NO zstd flushes. ----
    if(scenario==SC_LATEJOIN && scenarioT>0){
        std::vector<uint32_t> warmRegionStamp(NREG,0), warmBlockStamp(NBLK,0); uint32_t warmStamp=0;
        std::vector<uint32_t> req, blocksTU, warmMiss;
        for(size_t t=0;t<scenarioT;++t){
            const uint32_t* tk=&tokstream[tokoff[t]]; size_t tn=tokoff[t+1]-tokoff[t];
            if(++warmStamp==0){ std::fill(warmRegionStamp.begin(),warmRegionStamp.end(),0); std::fill(warmBlockStamp.begin(),warmBlockStamp.end(),0); warmStamp=1; }
            req.clear(); blocksTU.clear();
            auto reqReg=[&](uint32_t r){ if(warmRegionStamp[r]!=warmStamp){ warmRegionStamp[r]=warmStamp; req.push_back(r); } };
            for(size_t i=0;i<tn;++i){ uint32_t tok=tk[i]; if(tok<NREG) reqReg(tok);
                else { uint32_t k=tok-NREG; if(warmBlockStamp[k]!=warmStamp){ warmBlockStamp[k]=warmStamp; blocksTU.push_back(k); } } }
            for(uint32_t k:blocksTU){ fknownBlk[k]=1; for(size_t j=boff2[k];j<boff2[k+1];++j) reqReg(bchild[j]); }
            warmMiss.clear(); for(uint32_t r:req) if(!enc.fknownReg[r]) warmMiss.push_back(r);
            if(!warmMiss.empty()) enc.materialize(dict, warmMiss, t);   // advances authority; mixedRaw discarded
        }
        fprintf(stderr,"C latejoin warm pass: authority built through TU %zu (nextPublic=%u paths=%zu)\n",scenarioT,enc.nextMixedPublic,enc.paths.size());
    }
    size_t startT = (scenario==SC_LATEJOIN)? scenarioT : 0;

    // Hello (extended): 16-byte generation + NREG + NBLK.
    cap::SourceGeneration gen; for(int i=0;i<16;++i) gen[i]=uint8_t(0xA0+i);
    { std::vector<uint8_t> hp(gen.begin(),gen.end()); put_varint(hp,NREG); put_varint(hp,NBLK);
      if(!cap::send_frame(fd,cap::Frame::Hello,hp)){fprintf(stderr,"C: hello send failed\n");return 2;}
      socket_bytes += 4 + hp.size(); }

    for(size_t t=startT;t<TUs;++t){
        // ---- M2 rejoin handshake (restart or late-join): F signals first, before Root ----
        if((scenario==SC_RESTART||scenario==SC_LATEJOIN) && t==scenarioT){
            cap::Frame rf; std::vector<uint8_t> rp;
            if(!cap::recv_frame(fd,rf,rp)||rf!=cap::Frame::Rejoin){fprintf(stderr,"C: expected Rejoin at TU %zu\n",t);return 2;}
            socket_bytes += 4 + rp.size();
            enc.reset_model();                                   // F's cache is empty: fknownReg/Public=0, recovering=1
            std::fill(fknownBlk.begin(),fknownBlk.end(),0);      // re-send needed blocks (literal, recovery mode)
            std::vector<uint8_t> resync=capp::pack_resync(enc.nextMixedPublic, enc.paths);
            if(!cap::send_frame(fd,cap::Frame::Ack,resync)){fprintf(stderr,"C: resync send\n");return 2;}
            socket_bytes += 4 + resync.size();
            fprintf(stderr,"C: %s handshake at TU %zu — resync nextPublic=%u, bulk paths=%zu (%zu B)\n",
                scenario==SC_RESTART?"restart":"late-join", t, enc.nextMixedPublic, enc.paths.size(), resync.size());
        }

        const uint32_t* tk=&tokstream[tokoff[t]]; size_t tn=tokoff[t+1]-tokoff[t];
        rootb.clear(); for(size_t i=0;i<tn;++i) put_varint(rootb, tk[i]);
        if(++requestStamp==0){ std::fill(requiredBlockStamp.begin(),requiredBlockStamp.end(),0); requestStamp=1; }
        requiredBlocks.clear();
        for(size_t i=0;i<tn;++i){ uint32_t tok=tk[i]; if(tok>=NREG){ uint32_t k=tok-NREG;
            if(requiredBlockStamp[k]!=requestStamp){ requiredBlockStamp[k]=requestStamp; requiredBlocks.push_back(k); } } }
        w_root += zstd_message_roundtrip(z,messageD,rootb,zlevel,messageEncoded,messageDecoded); w_framing += FRAME;
        // first-use block manifest (recovery mode: LITERAL children, never COPY — F has no occurrence stream)
        manifestBlocks.clear(); for(uint32_t k:requiredBlocks) if(!fknownBlk[k]) manifestBlocks.push_back(k);
        blockRaw.clear();
        if(!manifestBlocks.empty()){
            put_varint(blockRaw,manifestBlocks.size());
            for(uint32_t k:manifestBlocks){ put_varint(blockRaw,k); size_t L=boff2[k+1]-boff2[k];
                if(bcopy_ok[k] && !enc.recovering){ blockRaw.push_back(1); put_varint(blockRaw,bcopy_src[k]); put_varint(blockRaw,L); }
                else { if(bcopy_ok[k]&&enc.recovering) literalBlockExtraRaw += L;   // extra raw vs the cold COPY form
                    blockRaw.push_back(0); put_varint(blockRaw,L); for(size_t j=boff2[k];j<boff2[k+1];++j) put_varint(blockRaw,bchild[j]); } }
            w_blockdef += zstd_message_roundtrip(z,messageD,blockRaw,zlevel,messageEncoded,messageDecoded) + FRAME;
            for(uint32_t k:manifestBlocks) fknownBlk[k]=1;
        }
        { std::vector<uint8_t> rp=capp::pack_root(rootb,blockRaw);
          if(!cap::send_frame(fd,cap::Frame::Root,rp)){fprintf(stderr,"C: root send\n");return 2;} socket_bytes += 4 + rp.size(); }
        cap::Frame ft; std::vector<uint8_t> needPayload;
        if(!cap::recv_frame(fd,ft,needPayload)||ft!=cap::Frame::Need){fprintf(stderr,"C: need recv\n");return 2;}
        socket_bytes += 4 + needPayload.size();
        missReg.clear();
        { const uint8_t* mp=needPayload.data(),*me=mp+needPayload.size(); uint64_t cnt=get_varint(mp);
          missReg.reserve(cnt); for(uint64_t i=0;i<cnt;++i){ uint64_t r=get_varint(mp); if(r>=NREG){fprintf(stderr,"C: bad missing id\n");return 2;} missReg.push_back(uint32_t(r)); }
          // M2: trailing slot is the public-Line drop list (F evicted these); cold => count 0 => M1-identical.
          uint64_t dropCnt=get_varint(mp);
          for(uint64_t i=0;i<dropCnt;++i){ uint64_t ord=get_varint(mp); enc.forget_public(uint32_t(ord)); ++dropsCleared; }
          if(mp!=me){fprintf(stderr,"C: bad missing tail\n");return 2;} }
        if(!manifestBlocks.empty()||!missReg.empty()){
            double bytes=zstd_message_roundtrip(z,messageD,needPayload,zlevel,messageEncoded,messageDecoded)+FRAME;
            w_missing += bytes; mixedMissingRequestWire += bytes;
        }
        uint32_t nr=0;
        if(!missReg.empty()) nr=enc.materialize(dict,missReg,t);
        else { for(auto&v:enc.mixedRaw)v.clear(); enc.fill_paths.clear(); enc.np=0; }
        std::array<std::vector<uint8_t>,6> mixedEncoded;
        if(nr){
            for(size_t i=0;i<mixedPartCount;++i) if(!enc.mixedRaw[i].empty()){
                mixedEncoded[i]=zstd_stream_encode(mixedZC[i],enc.mixedRaw[i],ZSTD_e_flush); mixedZActive[i]=1;
                double bytes=mixedEncoded[i].size()+FRAME; mixedPartWire[i]+=bytes; (i==0?w_regiondef:w_linedef)+=bytes;
            }
            w_linedef+=1; mixedSelectorWire+=1;
        }
        if(t+1==TUs){
            const std::vector<uint8_t> empty;
            for(size_t i=0;i<mixedPartCount;++i) if(mixedZActive[i]){
                std::vector<uint8_t> tail=zstd_stream_encode(mixedZC[i],empty,ZSTD_e_end);
                double& wire=(i==0?w_regiondef:w_linedef);
                if(mixedEncoded[i].empty()&&!tail.empty()){ wire+=FRAME; mixedPartWire[i]+=FRAME; }
                wire+=tail.size(); mixedPartWire[i]+=tail.size(); mixedEncoded[i].insert(mixedEncoded[i].end(),tail.begin(),tail.end());
            }
        }
        if(enc.np){ w_pathdef += zstd_size(z,enc.fill_paths.data(),enc.fill_paths.size(),zlevel,dst); }
        if(enc.np||nr){ w_framing += FRAME; }
        { std::vector<uint8_t> fp=capp::pack_fill(enc.fill_paths,mixedEncoded,mixedPartCount);
          if(!cap::send_frame(fd,cap::Frame::Fill,fp)){fprintf(stderr,"C: fill send\n");return 2;} socket_bytes += 4 + fp.size(); }
    }
    if(!cap::send_frame(fd,cap::Frame::Done,std::vector<uint8_t>{})){fprintf(stderr,"C: done send\n");return 2;} socket_bytes += 4;
    cap::Frame ft; std::vector<uint8_t> ackb;
    if(!cap::recv_frame(fd,ft,ackb)||ft!=cap::Frame::Ack||ackb.empty()){fprintf(stderr,"C: ack recv\n");return 2;} socket_bytes += 4 + ackb.size();
    bool byteexact = ackb[0]!=0;
    double dur=secs(tpass);
    ZSTD_freeCCtx(z); ZSTD_freeDCtx(messageD); for(size_t i=0;i<mixedPartCount;++i) ZSTD_freeCCtx(mixedZC[i]);

    double MiB=1048576.0;
    double total = w_root+w_linedef+w_regiondef+w_pathdef+w_blockdef+w_missing+w_framing;
    const char* scn = scenario==SC_RESTART?"restart":(scenario==SC_LATEJOIN?"late-join":"cold");
    printf("\n==== CAP-M2 (two-process C<->F; scenario=%s%s) — %s ====\n", scn,
        scenario!=SC_COLD?(std::string(" @TU=")+std::to_string(scenarioT)).c_str():"", manifest);
    printf("byte-exact=%s  TUs=%zu(served %zu..%zu) raw=%.1f MiB regions=%u distinct_lines=%u paths=%zu blocks=%zu (marker_lines=%llu literal_lines=%llu)\n",
        byteexact?"OK":"FAIL",TUs,startT,TUs-1,corpus.raw/MiB,NREG,dict.distinct(),enc.paths.size(),boff2.size()-1,(unsigned long long)enc.n_marker,(unsigned long long)enc.n_literal);
    printf("wire by category (post-z%d, bytes): root=%.0f line_def=%.0f region_def=%.0f block_def=%.0f path_def=%.0f missing=%.0f framing=%.0f  TOTAL=%.0f (%.2f MiB)\n",
        zlevel,w_root,w_linedef,w_regiondef,w_blockdef,w_pathdef,w_missing,w_framing,total,total/MiB);
    printf("mixed components: control=%.0f literal=%.0f array_control=%.0f array_values=%.0f selector=%.0f raw_source_reused=%llu public_lines=%u ops=[literal=%llu publish=%llu ref=%llu array=%llu marker=%llu source=%llu patch=%llu]\n",
        mixedPartWire[0],mixedPartWire[1],mixedPartWire[2],mixedPartWire[3],mixedSelectorWire,(unsigned long long)enc.mixedSourceBytes,enc.nextMixedPublic-1,
        (unsigned long long)enc.mixedOps[0],(unsigned long long)enc.mixedOps[1],(unsigned long long)enc.mixedOps[2],(unsigned long long)enc.mixedOps[3],(unsigned long long)enc.mixedOps[4],(unsigned long long)enc.mixedOps[5],(unsigned long long)enc.mixedOps[6]);
    printf("M2 recovery: op7(reprovide-bytes)=%llu raw=%llu B | op8(reprovide-view)=%llu raw=%llu B | op9(publish-bytes)=%llu raw=%llu B | literal-block-extra=%llu raw B | public-drops-cleared=%llu\n",
        (unsigned long long)enc.op7_count,(unsigned long long)enc.op7_wire,(unsigned long long)enc.op8_count,(unsigned long long)enc.op8_wire,
        (unsigned long long)enc.op9_count,(unsigned long long)enc.op9_wire,(unsigned long long)literalBlockExtraRaw,(unsigned long long)dropsCleared);
    printf("REAL socket bytes (C-observed, incl. 4B headers + Hello/Done/Ack/Rejoin): %llu (%.2f MiB) in %.2fs\n",
        (unsigned long long)socket_bytes, socket_bytes/MiB, dur);
    if(scenario==SC_COLD)
        printf("COLD LEDGER (regression gate): TOTAL=%.0f\n", total);
    fflush(stdout);
    return byteexact?0:1;
}

// ===================== F role (child): decoder/store from wire only =====================
static int run_F(int fd, const Corpus& corpus, size_t TUs, int zlevel,
                 int scenario, size_t scenarioT, uint32_t evictBudget){
    const size_t mixedPartCount=4; (void)zlevel;
    cap::Frame ft; std::vector<uint8_t> hp;
    if(!cap::recv_frame(fd,ft,hp)||ft!=cap::Frame::Hello||hp.size()<16){fprintf(stderr,"F: hello recv\n");return 2;}
    cap::SourceGeneration gen; memcpy(gen.data(),hp.data(),16); (void)gen;
    const uint8_t* hpp=hp.data()+16; uint32_t NREG=uint32_t(get_varint(hpp)); uint32_t NBLK=uint32_t(get_varint(hpp));
    FStore F; F.init(NREG,NBLK); F.publicBudget=evictBudget;
    std::array<ZSTD_DCtx*,6> mixedZD{}; for(size_t i=0;i<mixedPartCount;++i) mixedZD[i]=ZSTD_createDCtx();
    std::vector<uint32_t> FrequiredRegions, FrequiredBlocks, missReg;
    std::vector<uint8_t> rootb, blockRaw, fill_paths, missingRaw, recon;
    bool byteexact=true; size_t startT=(scenario==SC_LATEJOIN)?scenarioT:0;
    uint64_t evictedTotal=0, verified=0;

    for(size_t t=startT;t<TUs;++t){
        // ---- M2 rejoin: signal restart/late-join before receiving this TU's Root ----
        if((scenario==SC_RESTART||scenario==SC_LATEJOIN) && t==scenarioT){
            std::vector<uint8_t> rj=capp::pack_rejoin(uint32_t(t));
            if(!cap::send_frame(fd,cap::Frame::Rejoin,rj)){fprintf(stderr,"F: rejoin send\n");return 2;}
            cap::Frame af; std::vector<uint8_t> resync;
            if(!cap::recv_frame(fd,af,resync)||af!=cap::Frame::Ack){fprintf(stderr,"F: resync recv\n");return 2;}
            uint32_t nextPublic; std::vector<std::string> paths; capp::unpack_resync(resync,nextPublic,paths);
            F.reset_store(nextPublic); F.Fpaths=paths;
            fprintf(stderr,"F: %s at TU %zu — store dropped, resynced nextPublic=%u, paths=%zu\n",
                scenario==SC_RESTART?"RESTART":"LATE-JOIN", t, nextPublic, paths.size());
        }

        std::vector<uint8_t> rootPayload;
        if(!cap::recv_frame(fd,ft,rootPayload)||ft!=cap::Frame::Root){fprintf(stderr,"F: root recv\n");return 2;}
        capp::unpack_root(rootPayload,rootb,blockRaw);     // rootb == Frootb
        if(++F.requestStamp==0){ std::fill(F.FrequiredRegionStamp.begin(),F.FrequiredRegionStamp.end(),0);
            std::fill(F.FrequiredBlockStamp.begin(),F.FrequiredBlockStamp.end(),0); F.requestStamp=1; }
        FrequiredRegions.clear(); FrequiredBlocks.clear();
        auto FrequireRegion=[&](uint32_t r){ if(F.FrequiredRegionStamp[r]!=F.requestStamp){ F.FrequiredRegionStamp[r]=F.requestStamp; FrequiredRegions.push_back(r); } };
        auto FrequireBlock=[&](uint32_t k){ if(F.FrequiredBlockStamp[k]!=F.requestStamp){ F.FrequiredBlockStamp[k]=F.requestStamp; FrequiredBlocks.push_back(k); } };
        { const uint8_t* rp=rootb.data(),*re=rp+rootb.size();
          while(rp<re){ uint64_t tok=get_varint(rp); if(tok<NREG) FrequireRegion(uint32_t(tok));
              else if(tok-NREG<NBLK) FrequireBlock(uint32_t(tok-NREG)); else {fprintf(stderr,"F: bad Root token\n");return 2;} } }
        if(!blockRaw.empty()) F.install_blocks(blockRaw);
        for(uint32_t k:FrequiredBlocks){ if(!F.FknownBlk[k]){fprintf(stderr,"F: missing direct Block %u at TU %zu\n",k,t);return 2;}
            for(uint32_t child:F.FblkChildren[k]) FrequireRegion(child); }
        missReg.clear(); for(uint32_t r:FrequiredRegions) if(!F.FmixedRegions[r].known) missReg.push_back(r);
        missingRaw.clear(); put_varint(missingRaw,missReg.size()); for(uint32_t r:missReg) put_varint(missingRaw,r);
        put_varint(missingRaw,F.pendingDrops.size()); for(uint32_t o:F.pendingDrops) put_varint(missingRaw,o); F.pendingDrops.clear();
        if(!cap::send_frame(fd,cap::Frame::Need,missingRaw)){fprintf(stderr,"F: need send\n");return 2;}

        std::vector<uint8_t> fillPayload;
        if(!cap::recv_frame(fd,ft,fillPayload)||ft!=cap::Frame::Fill){fprintf(stderr,"F: fill recv\n");return 2;}
        std::array<std::vector<uint8_t>,6> mixedEncoded;
        capp::unpack_fill(fillPayload,fill_paths,mixedEncoded,mixedPartCount);
        std::array<std::vector<uint8_t>,6> recovered;
        for(size_t i=0;i<mixedPartCount;++i) if(!mixedEncoded[i].empty()){
            size_t remaining=1; recovered[i]=zstd_stream_decode(mixedZD[i],mixedEncoded[i],remaining);
            if(t+1==TUs&&remaining!=0){fprintf(stderr,"F: stream not ended part=%zu\n",i);return 2;}
        }
        if(!F.decode_fill(recovered,missReg,fill_paths,uint32_t(t))){fprintf(stderr,"F: decode_fill failed TU %zu\n",t);return 2;}
        recon.clear(); F.reconstruct(rootb,recon);
        const char* orig=corpus.bytes.data()+corpus.files[t].off; uint32_t olen=corpus.files[t].len;
        if(recon.size()!=olen||memcmp(recon.data(),orig,olen)!=0){ byteexact=false; if(verified<5) fprintf(stderr,"F: BYTE-EXACT FAIL TU %zu (%zu vs %u)\n",t,recon.size(),olen); }
        ++verified;
        // bounded eviction (scenario 3): drop LRU public Lines beyond the budget; report next NEED.
        if(evictBudget!=UINT32_MAX){ size_t before=F.publicHeld; F.evict_to_budget(uint32_t(t)); evictedTotal += (before-F.publicHeld); }
    }
    std::vector<uint8_t> doneb;
    if(!cap::recv_frame(fd,ft,doneb)||ft!=cap::Frame::Done){fprintf(stderr,"F: done recv\n");return 2;}
    std::vector<uint8_t> ack{ uint8_t(byteexact?1:0) };
    if(!cap::send_frame(fd,cap::Frame::Ack,ack)){fprintf(stderr,"F: ack send\n");return 2;}
    for(size_t i=0;i<mixedPartCount;++i) ZSTD_freeDCtx(mixedZD[i]);
    fprintf(stderr,"F: reconstruct byte-exact=%s over %llu TUs (served %zu..%zu); public held=%u evicted-total=%llu budget=%s\n",
        byteexact?"OK":"FAIL",(unsigned long long)verified,startT,TUs-1,F.publicHeld,(unsigned long long)evictedTotal,
        evictBudget==UINT32_MAX?"inf":std::to_string(evictBudget).c_str());
    return byteexact?0:1;
}

int main(int argc,char**argv){
    const char* manifest=nullptr; int zlevel=3; size_t max_files=SIZE_MAX;
    int scenario=SC_COLD; size_t scenarioT=0; uint32_t evictBudget=UINT32_MAX;
    for(int i=1;i<argc;++i){
        if(!strcmp(argv[i],"--manifest")&&i+1<argc) manifest=argv[++i];
        else if(!strcmp(argv[i],"--z")&&i+1<argc) zlevel=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--max-files")&&i+1<argc){ char*t=nullptr; unsigned long long v=strtoull(argv[++i],&t,10); if(!t||*t||!v){fprintf(stderr,"bad max-files\n");return 2;} max_files=size_t(v); }
        else if(!strcmp(argv[i],"--restart")&&i+1<argc){ scenario=SC_RESTART; scenarioT=strtoull(argv[++i],nullptr,10); }
        else if(!strcmp(argv[i],"--latejoin")&&i+1<argc){ scenario=SC_LATEJOIN; scenarioT=strtoull(argv[++i],nullptr,10); }
        else if(!strcmp(argv[i],"--evict")&&i+1<argc){ evictBudget=uint32_t(strtoul(argv[++i],nullptr,10)); }
        else { fprintf(stderr,"unknown %s\n",argv[i]); return 2; }
    }
    if(!manifest){ fprintf(stderr,"usage: %s --manifest F [--z 0|1|3] [--max-files N] [--restart T|--latejoin T] [--evict BUDGET]\n",argv[0]); return 2; }

    auto t0=Clock::now();
    Corpus corpus=load_corpus(manifest,max_files); Interner dict;
    std::vector<uint32_t> allreg; std::vector<size_t> roff; roff.push_back(0);
    { uint32_t maxlen=0; for(auto&f:corpus.files) maxlen=std::max(maxlen,f.len); std::vector<uint32_t> out(size_t(maxlen)+1); uint64_t hits=0; std::vector<uint32_t> rs;
      for(auto&f:corpus.files){ size_t oc=0; rs.clear(); const char*p=corpus.bytes.data()+f.off; dict.process(p,p+f.len,out.data(),oc,hits,true,&rs); allreg.insert(allreg.end(),rs.begin(),rs.end()); roff.push_back(allreg.size()); } }
    uint32_t NREG=uint32_t(dict.region_count()); size_t TUs=corpus.files.size();
    if(scenarioT>=TUs && scenario!=SC_COLD){ fprintf(stderr,"scenario TU %zu >= TUs %zu\n",scenarioT,TUs); return 2; }
    fprintf(stderr,"loaded+interned %.1fs TUs=%zu raw=%llu regions=%u region_occ=%zu distinct_lines=%u\n",secs(t0),TUs,(unsigned long long)corpus.raw,NREG,allreg.size(),dict.distinct());

    // ===== S1: LZ over the region-id stream -> per-TU token streams + flat Blocks (VERBATIM) =====
    std::vector<uint32_t> bchild; std::vector<size_t> boff2; boff2.push_back(0);
    std::unordered_map<uint64_t,uint32_t> bdict;
    std::vector<uint32_t> tokstream; std::vector<size_t> tokoff; tokoff.push_back(0);
    std::vector<uint32_t> bcopy_src; std::vector<uint8_t> bcopy_ok;
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
      fprintf(stderr,"S1 LZ: %.1fs tokens=%zu blocks=%zu (%.4f tok/region)\n",secs(tb),tokstream.size(),boff2.size()-1,double(tokstream.size())/NS);
    }
    uint32_t NBLK=uint32_t(boff2.size());

    int sv[2];
    if(socketpair(AF_UNIX, SOCK_STREAM, 0, sv)!=0){ perror("socketpair"); return 2; }
    pid_t pid=fork();
    if(pid<0){ perror("fork"); return 2; }
    if(pid==0){   // ===== F (child) =====
        close(sv[0]); int rc=run_F(sv[1], corpus, TUs, zlevel, scenario, scenarioT, evictBudget); close(sv[1]); _exit(rc);
    }
    close(sv[1]);
    int rc=run_C(sv[0], dict, corpus, tokstream, tokoff, boff2, bchild, bcopy_src, bcopy_ok, NREG, NBLK, TUs, zlevel, manifest, scenario, scenarioT);
    close(sv[0]);
    int st=0; waitpid(pid,&st,0);
    bool fok = WIFEXITED(st) && WEXITSTATUS(st)==0;
    struct rusage ru{}; getrusage(RUSAGE_SELF,&ru); fprintf(stderr,"C peak RSS=%.1f MiB total=%.1fs (F exit ok=%d)\n",ru.ru_maxrss/1024.0,secs(t0),int(fok));
    return (rc==0 && fok)?0:1;
}
