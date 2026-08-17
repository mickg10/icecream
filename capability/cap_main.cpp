// icecream #16 capability harness — cap_main.cpp
// GENUINE two-process C<->F split of codec50-m1's canonical M1 path
// (--mixed-regions --byte-array-lines --direct-ordinals, z3), over cap_transport frames.
//
// Topology: the launcher loads the corpus, interns, and builds the S1 block plane ONCE,
// then forks.  parent = C (encoder/authority, keeps the immutable dict + block store and
// the virtual wire ledger); child = F (decoder/store, rebuilds everything from wire bytes
// and system-header files, reconstructs each .ii, and byte-checks against the corpus).
// They exchange only cap_transport frames — F never reads C's dict/tokstream after fork.
//
// Per-TU dialogue (strict lockstep): C->Root[rootb|blockRaw] ; F->Need[missingRaw] ;
// C->Fill[fill_paths|mixed0..3].  Then C->Done ; F->Ack[byteexact].
//
// The 7-category codec ledger is a VIRTUAL wire accounting kept by C (FRAME=4 per codec
// message, exactly codec50's grouping) — the success gate is TOTAL==9,590,734 byte-exact.
// The REAL socket byte total (incl. Hello/Done/Ack) is reported separately.
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

// ===================== C role (parent): encoder/authority + ledger =====================
static int run_C(int fd, const Interner& dict, const Corpus& corpus,
                 const std::vector<uint32_t>& tokstream, const std::vector<size_t>& tokoff,
                 const std::vector<size_t>& boff2, const std::vector<uint32_t>& bchild,
                 const std::vector<uint32_t>& bcopy_src, const std::vector<uint8_t>& bcopy_ok,
                 uint32_t NREG, uint32_t NBLK, size_t TUs, int zlevel, const char* manifest){
    const double FRAME=4; const size_t mixedPartCount=4;
    ZSTD_CCtx* z=ZSTD_createCCtx(); ZSTD_DCtx* messageD=ZSTD_createDCtx();
    std::array<ZSTD_CCtx*,6> mixedZC{}; std::array<uint8_t,6> mixedZActive{};
    for(size_t i=0;i<mixedPartCount;++i){ mixedZC[i]=ZSTD_createCCtx();
        ZSTD_CCtx_setParameter(mixedZC[i],ZSTD_c_compressionLevel,zlevel);
        ZSTD_CCtx_setParameter(mixedZC[i],ZSTD_c_contentSizeFlag,0); }
    MixedEncoder enc; enc.init(dict.distinct());
    std::vector<uint8_t> fknownBlk(NBLK,0);
    std::vector<uint32_t> requiredBlockStamp(NBLK,0); uint32_t requestStamp=0;
    double w_root=0,w_linedef=0,w_regiondef=0,w_pathdef=0,w_blockdef=0,w_missing=0,w_framing=0;
    std::array<double,6> mixedPartWire{}; double mixedSelectorWire=0, mixedMissingRequestWire=0;
    std::vector<uint8_t> rootb, blockRaw, messageEncoded, messageDecoded, dst;
    std::vector<uint32_t> requiredBlocks, manifestBlocks, missReg;
    uint64_t socket_bytes=0;
    auto tpass=Clock::now();

    // Hello (extended): 16-byte generation + NREG + NBLK.
    cap::SourceGeneration gen; for(int i=0;i<16;++i) gen[i]=uint8_t(0xA0+i);
    { std::vector<uint8_t> hp(gen.begin(),gen.end()); put_varint(hp,NREG); put_varint(hp,NBLK);
      if(!cap::send_frame(fd,cap::Frame::Hello,hp)){fprintf(stderr,"C: hello send failed\n");return 2;}
      socket_bytes += 4 + hp.size(); }

    for(size_t t=0;t<TUs;++t){
        const uint32_t* tk=&tokstream[tokoff[t]]; size_t tn=tokoff[t+1]-tokoff[t];
        rootb.clear(); for(size_t i=0;i<tn;++i) put_varint(rootb, tk[i]);
        if(++requestStamp==0){ std::fill(requiredBlockStamp.begin(),requiredBlockStamp.end(),0); requestStamp=1; }
        requiredBlocks.clear();
        for(size_t i=0;i<tn;++i){ uint32_t tok=tk[i]; if(tok>=NREG){ uint32_t k=tok-NREG;
            if(requiredBlockStamp[k]!=requestStamp){ requiredBlockStamp[k]=requestStamp; requiredBlocks.push_back(k); } } }
        // ROOT ledger
        w_root += zstd_message_roundtrip(z,messageD,rootb,zlevel,messageEncoded,messageDecoded); w_framing += FRAME;
        // first-use block manifest
        manifestBlocks.clear(); for(uint32_t k:requiredBlocks) if(!fknownBlk[k]) manifestBlocks.push_back(k);
        blockRaw.clear();
        if(!manifestBlocks.empty()){
            put_varint(blockRaw,manifestBlocks.size());
            for(uint32_t k:manifestBlocks){ put_varint(blockRaw,k); size_t L=boff2[k+1]-boff2[k];
                if(bcopy_ok[k]){ blockRaw.push_back(1); put_varint(blockRaw,bcopy_src[k]); put_varint(blockRaw,L); }
                else { blockRaw.push_back(0); put_varint(blockRaw,L); for(size_t j=boff2[k];j<boff2[k+1];++j) put_varint(blockRaw,bchild[j]); } }
            w_blockdef += zstd_message_roundtrip(z,messageD,blockRaw,zlevel,messageEncoded,messageDecoded) + FRAME;
            for(uint32_t k:manifestBlocks) fknownBlk[k]=1;
        }
        // C -> F ROOT
        { std::vector<uint8_t> rp=capp::pack_root(rootb,blockRaw);
          if(!cap::send_frame(fd,cap::Frame::Root,rp)){fprintf(stderr,"C: root send\n");return 2;} socket_bytes += 4 + rp.size(); }
        // F -> C NEED (missingRaw), decode missReg IN RECEIVED ORDER
        cap::Frame ft; std::vector<uint8_t> needPayload;
        if(!cap::recv_frame(fd,ft,needPayload)||ft!=cap::Frame::Need){fprintf(stderr,"C: need recv\n");return 2;}
        socket_bytes += 4 + needPayload.size();
        missReg.clear();
        { const uint8_t* mp=needPayload.data(),*me=mp+needPayload.size(); uint64_t cnt=get_varint(mp);
          missReg.reserve(cnt); for(uint64_t i=0;i<cnt;++i){ uint64_t r=get_varint(mp); if(r>=NREG){fprintf(stderr,"C: bad missing id\n");return 2;} missReg.push_back(uint32_t(r)); }
          uint64_t trailing=get_varint(mp); if(trailing!=0||mp!=me){fprintf(stderr,"C: bad missing tail\n");return 2;} }
        // MISSING ledger (charge the received bytes, same condition as the reference)
        if(!manifestBlocks.empty()||!missReg.empty()){
            double bytes=zstd_message_roundtrip(z,messageD,needPayload,zlevel,messageEncoded,messageDecoded)+FRAME;
            w_missing += bytes; mixedMissingRequestWire += bytes;
        }
        // materialize the mixed FILL over missReg (received order)
        uint32_t nr=0;
        if(!missReg.empty()) nr=enc.materialize(dict,missReg,t);
        else { for(auto&v:enc.mixedRaw)v.clear(); enc.fill_paths.clear(); enc.np=0; }
        // FILL stateful encode + ledger
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
        // PATH ledger + FILL framing
        if(enc.np){ w_pathdef += zstd_size(z,enc.fill_paths.data(),enc.fill_paths.size(),zlevel,dst); }
        if(enc.np||nr){ w_framing += FRAME; }
        // C -> F FILL (always, even empty; the last-TU e_end tails ride here)
        { std::vector<uint8_t> fp=capp::pack_fill(enc.fill_paths,mixedEncoded,mixedPartCount);
          if(!cap::send_frame(fd,cap::Frame::Fill,fp)){fprintf(stderr,"C: fill send\n");return 2;} socket_bytes += 4 + fp.size(); }
    }
    // Done / Ack
    if(!cap::send_frame(fd,cap::Frame::Done,std::vector<uint8_t>{})){fprintf(stderr,"C: done send\n");return 2;} socket_bytes += 4;
    cap::Frame ft; std::vector<uint8_t> ackb;
    if(!cap::recv_frame(fd,ft,ackb)||ft!=cap::Frame::Ack||ackb.empty()){fprintf(stderr,"C: ack recv\n");return 2;} socket_bytes += 4 + ackb.size();
    bool byteexact = ackb[0]!=0;
    double dur=secs(tpass);

    ZSTD_freeCCtx(z); ZSTD_freeDCtx(messageD); for(size_t i=0;i<mixedPartCount;++i) ZSTD_freeCCtx(mixedZC[i]);

    double MiB=1048576.0;
    double total = w_root+w_linedef+w_regiondef+w_pathdef+w_blockdef+w_missing+w_framing;
    printf("\n==== CAP-M1 (two-process C<->F over cap_transport; V1+S1+P24 mixed-regions+BYTE_ARRAY+DIRECT_ORDINALS, z%d) — %s ====\n", zlevel, manifest);
    printf("byte-exact=%s  TUs=%zu raw=%.1f MiB regions=%u distinct_lines=%u paths=%zu blocks=%zu (marker_lines=%llu literal_lines=%llu)\n",
        byteexact?"OK":"FAIL",TUs,corpus.raw/MiB,NREG,dict.distinct(),enc.paths.size(),boff2.size()-1,(unsigned long long)enc.n_marker,(unsigned long long)enc.n_literal);
    printf("wire by category (post-z%d, bytes): root=%.0f line_def=%.0f region_def=%.0f block_def=%.0f path_def=%.0f missing=%.0f framing=%.0f  TOTAL=%.0f (%.2f MiB)\n",
        zlevel,w_root,w_linedef,w_regiondef,w_blockdef,w_pathdef,w_missing,w_framing,total,total/MiB);
    printf("FinalRatio (raw / total wire, one cold pass) = %.1fx\n", corpus.raw/total);
    printf("mixed components: control=%.0f literal=%.0f array_control=%.0f array_values=%.0f source_control=%.0f source_files=%.0f selector=%.0f raw_literal=%llu raw_array_values=%llu raw_source_reused=%llu source_package_raw=%llu source_package_files=%llu public_lines=%u ops=[literal=%llu publish=%llu ref=%llu array=%llu marker=%llu source=%llu patch=%llu]\n",
        mixedPartWire[0],mixedPartWire[1],mixedPartWire[2],mixedPartWire[3],mixedPartWire[4],mixedPartWire[5],mixedSelectorWire,
        (unsigned long long)enc.mixedLiteralRaw,(unsigned long long)enc.mixedArrayValues,(unsigned long long)enc.mixedSourceBytes,(unsigned long long)enc.mixedSourcePackageRaw,(unsigned long long)enc.mixedSourcePackageFiles,enc.nextMixedPublic-1,
        (unsigned long long)enc.mixedOps[0],(unsigned long long)enc.mixedOps[1],(unsigned long long)enc.mixedOps[2],(unsigned long long)enc.mixedOps[3],(unsigned long long)enc.mixedOps[4],(unsigned long long)enc.mixedOps[5],(unsigned long long)enc.mixedOps[6]);
    printf("key map: half_cold_bit=-1 preloaded_regions=0 preloaded_raw_bytes=0 associated_regions=0 association_wire=0 missing_reply_wire=%.0f\n", mixedMissingRequestWire);
    printf("direct ordinals: generation_latched=1 region_namespaces=1 block_lifetime=generation\n");
    printf("REAL socket bytes (C-observed, incl. 4B headers + Hello/Done/Ack): %llu (%.2f MiB) in %.2fs\n",
        (unsigned long long)socket_bytes, socket_bytes/MiB, dur);
    fflush(stdout);
    return byteexact?0:1;
}

// ===================== F role (child): decoder/store from wire only =====================
static int run_F(int fd, const Corpus& corpus, size_t TUs, int zlevel){
    const size_t mixedPartCount=4; (void)zlevel;
    cap::Frame ft; std::vector<uint8_t> hp;
    if(!cap::recv_frame(fd,ft,hp)||ft!=cap::Frame::Hello||hp.size()<16){fprintf(stderr,"F: hello recv\n");return 2;}
    cap::SourceGeneration gen; memcpy(gen.data(),hp.data(),16); (void)gen;
    const uint8_t* hpp=hp.data()+16; uint32_t NREG=uint32_t(get_varint(hpp)); uint32_t NBLK=uint32_t(get_varint(hpp));
    FStore F; F.init(NREG,NBLK);
    std::array<ZSTD_DCtx*,6> mixedZD{}; for(size_t i=0;i<mixedPartCount;++i) mixedZD[i]=ZSTD_createDCtx();
    std::vector<uint32_t> FrequiredRegions, FrequiredBlocks, missReg;
    std::vector<uint8_t> rootb, blockRaw, fill_paths, missingRaw, recon;
    bool byteexact=true;

    for(size_t t=0;t<TUs;++t){
        std::vector<uint8_t> rootPayload;
        if(!cap::recv_frame(fd,ft,rootPayload)||ft!=cap::Frame::Root){fprintf(stderr,"F: root recv\n");return 2;}
        capp::unpack_root(rootPayload,rootb,blockRaw);     // rootb == Frootb (F's decoded Root)
        if(++F.requestStamp==0){ std::fill(F.FrequiredRegionStamp.begin(),F.FrequiredRegionStamp.end(),0);
            std::fill(F.FrequiredBlockStamp.begin(),F.FrequiredBlockStamp.end(),0); F.requestStamp=1; }
        FrequiredRegions.clear(); FrequiredBlocks.clear();
        auto FrequireRegion=[&](uint32_t r){ if(F.FrequiredRegionStamp[r]!=F.requestStamp){ F.FrequiredRegionStamp[r]=F.requestStamp; FrequiredRegions.push_back(r); } };
        auto FrequireBlock=[&](uint32_t k){ if(F.FrequiredBlockStamp[k]!=F.requestStamp){ F.FrequiredBlockStamp[k]=F.requestStamp; FrequiredBlocks.push_back(k); } };
        { const uint8_t* rp=rootb.data(),*re=rp+rootb.size();
          while(rp<re){ uint64_t tok=get_varint(rp); if(tok<NREG) FrequireRegion(uint32_t(tok));
              else if(tok-NREG<NBLK) FrequireBlock(uint32_t(tok-NREG)); else {fprintf(stderr,"F: bad Root token\n");return 2;} } }
        if(!blockRaw.empty()) F.install_blocks(blockRaw);
        for(uint32_t k:FrequiredBlocks){ if(!F.FknownBlk[k]){fprintf(stderr,"F: missing direct Block\n");return 2;}
            for(size_t j=F.Fblk_off[k];j<F.Fblk_off[k+1];++j) FrequireRegion(F.Fblk_child[j]); }
        missReg.clear(); for(uint32_t r:FrequiredRegions) if(!F.FmixedRegions[r].known) missReg.push_back(r);
        missingRaw.clear(); put_varint(missingRaw,missReg.size()); for(uint32_t r:missReg) put_varint(missingRaw,r); put_varint(missingRaw,0);
        if(!cap::send_frame(fd,cap::Frame::Need,missingRaw)){fprintf(stderr,"F: need send\n");return 2;}

        std::vector<uint8_t> fillPayload;
        if(!cap::recv_frame(fd,ft,fillPayload)||ft!=cap::Frame::Fill){fprintf(stderr,"F: fill recv\n");return 2;}
        std::array<std::vector<uint8_t>,6> mixedEncoded;
        capp::unpack_fill(fillPayload,fill_paths,mixedEncoded,mixedPartCount);
        { const uint8_t* pp=fill_paths.data(),*pe=pp+fill_paths.size(); while(pp<pe){ uint64_t L=get_varint(pp); F.Fpaths.emplace_back((const char*)pp,(size_t)L); pp+=L; } }
        std::array<std::vector<uint8_t>,6> recovered;
        for(size_t i=0;i<mixedPartCount;++i) if(!mixedEncoded[i].empty()){
            size_t remaining=1; recovered[i]=zstd_stream_decode(mixedZD[i],mixedEncoded[i],remaining);
            if(t+1==TUs&&remaining!=0){fprintf(stderr,"F: stream not ended part=%zu\n",i);return 2;}
        }
        F.decode_fill(recovered,missReg);
        recon.clear(); F.reconstruct(rootb,recon);
        const char* orig=corpus.bytes.data()+corpus.files[t].off; uint32_t olen=corpus.files[t].len;
        if(recon.size()!=olen||memcmp(recon.data(),orig,olen)!=0){ byteexact=false; if(t<5||TUs<10) fprintf(stderr,"F: BYTE-EXACT FAIL TU %zu (%zu vs %u)\n",t,recon.size(),olen); }
    }
    std::vector<uint8_t> doneb;
    if(!cap::recv_frame(fd,ft,doneb)||ft!=cap::Frame::Done){fprintf(stderr,"F: done recv\n");return 2;}
    std::vector<uint8_t> ack{ uint8_t(byteexact?1:0) };
    if(!cap::send_frame(fd,cap::Frame::Ack,ack)){fprintf(stderr,"F: ack send\n");return 2;}
    for(size_t i=0;i<mixedPartCount;++i) ZSTD_freeDCtx(mixedZD[i]);
    fprintf(stderr,"F: reconstruct byte-exact=%s (%zu TUs)\n", byteexact?"OK":"FAIL", TUs);
    return byteexact?0:1;
}

int main(int argc,char**argv){
    const char* manifest=nullptr; int zlevel=3; size_t max_files=SIZE_MAX;
    for(int i=1;i<argc;++i){
        if(!strcmp(argv[i],"--manifest")&&i+1<argc) manifest=argv[++i];
        else if(!strcmp(argv[i],"--z")&&i+1<argc) zlevel=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--max-files")&&i+1<argc){ char*t=nullptr; unsigned long long v=strtoull(argv[++i],&t,10); if(!t||*t||!v){fprintf(stderr,"bad max-files\n");return 2;} max_files=size_t(v); }
        else { fprintf(stderr,"unknown %s\n",argv[i]); return 2; }
    }
    if(!manifest){ fprintf(stderr,"usage: %s --manifest F [--z 0|1|3] [--max-files N]\n",argv[0]); return 2; }

    auto t0=Clock::now();
    Corpus corpus=load_corpus(manifest,max_files); Interner dict;
    std::vector<uint32_t> allreg; std::vector<size_t> roff; roff.push_back(0);
    { uint32_t maxlen=0; for(auto&f:corpus.files) maxlen=std::max(maxlen,f.len); std::vector<uint32_t> out(size_t(maxlen)+1); uint64_t hits=0; std::vector<uint32_t> rs;
      for(auto&f:corpus.files){ size_t oc=0; rs.clear(); const char*p=corpus.bytes.data()+f.off; dict.process(p,p+f.len,out.data(),oc,hits,true,&rs); allreg.insert(allreg.end(),rs.begin(),rs.end()); roff.push_back(allreg.size()); } }
    uint32_t NREG=uint32_t(dict.region_count()); size_t TUs=corpus.files.size();
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
    uint32_t NBLK=uint32_t(boff2.size());   // fknownBlk.size() in the reference

    // ===== fork: parent = C, child = F, connected by an AF_UNIX socketpair =====
    int sv[2];
    if(socketpair(AF_UNIX, SOCK_STREAM, 0, sv)!=0){ perror("socketpair"); return 2; }
    pid_t pid=fork();
    if(pid<0){ perror("fork"); return 2; }
    if(pid==0){   // ===== F (child) — drops/ignores the C-only structures =====
        close(sv[0]); int rc=run_F(sv[1], corpus, TUs, zlevel); close(sv[1]); _exit(rc);
    }
    // ===== C (parent) =====
    close(sv[1]);
    int rc=run_C(sv[0], dict, corpus, tokstream, tokoff, boff2, bchild, bcopy_src, bcopy_ok, NREG, NBLK, TUs, zlevel, manifest);
    close(sv[0]);
    int st=0; waitpid(pid,&st,0);
    bool fok = WIFEXITED(st) && WEXITSTATUS(st)==0;
    struct rusage ru{}; getrusage(RUSAGE_SELF,&ru); fprintf(stderr,"C peak RSS=%.1f MiB total=%.1fs (F exit ok=%d)\n",ru.ru_maxrss/1024.0,secs(t0),int(fok));
    return (rc==0 && fok)?0:1;
}
