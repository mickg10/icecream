// icecream #16 capability harness — cap_codec.cpp
// Out-of-line codec helper definitions, EXTRACTED VERBATIM from codec50-m1.cpp.
#include "cap_codec.h"

namespace capc {

Corpus load_corpus(const char*manifest,size_t max_files){
    FILE*mf=fopen(manifest,"r"); if(!mf){perror(manifest);exit(2);} std::vector<std::string> paths; char path[8192]; uint64_t total=0;
    while(fgets(path,sizeof path,mf)){ size_t n=strlen(path); while(n&&(path[n-1]=='\n'||path[n-1]=='\r'))path[--n]=0; if(!n)continue; struct stat st{}; if(stat(path,&st)!=0){perror(path);exit(2);} paths.emplace_back(path); total+=uint64_t(st.st_size); if(paths.size()==max_files)break; }
    fclose(mf); Corpus c; c.bytes.resize(size_t(total)+64); c.files.reserve(paths.size()); uint64_t off=0;
    for(auto&p:paths){ FILE*f=fopen(p.c_str(),"rb"); if(!f){perror(p.c_str());exit(2);} struct stat st{}; fstat(fileno(f),&st); size_t n=size_t(st.st_size); if(n&&fread(c.bytes.data()+off,1,n,f)!=n){fprintf(stderr,"short read\n");exit(2);} fclose(f); c.files.push_back({off,uint32_t(n)}); off+=n; }
    c.raw=off; return c;
}

const PublishedRegion* PublishedDictionarySnapshot::pin(uint32_t id) const{
    if(id>=region_count_)return nullptr;
    const size_t chunk=id/CHUNK_REGIONS,index=id%CHUNK_REGIONS;
    if(chunk>=chunks_.size()||!chunks_[chunk]||index>=chunks_[chunk]->regions.size())return nullptr;
    return chunks_[chunk]->regions[index].get();
}

std::shared_ptr<const PublishedDictionarySnapshot>
PublishedDictionaryPublisher::publish(const Interner&dict){
    const uint64_t available=dict.region_count();
    if(available>UINT32_MAX||published_>available)return {};
    while(published_<available){
        const uint32_t id=published_;
        auto region=std::make_shared<PublishedRegion>();
        const uint32_t rawLength=dict.region_raw_len(id);
        const char*raw=dict.region_data(id);
        region->raw.assign(raw,raw+rawLength);
        const uint32_t count=dict.region_ids_count(id);
        const uint32_t*ids=dict.region_ids_ptr(id);
        region->lines.reserve(count);
        uint64_t offset=0;
        for(uint32_t index=0;index<count;++index){
            const LineRef&line=dict.ref(ids[index]);
            if(offset+line.len>rawLength)return {};
            region->lines.push_back({ids[index],uint32_t(offset),line.len});
            offset+=line.len;
        }
        if(offset!=rawLength)return {};
        raw_bytes_+=rawLength;line_entries_+=count;
        partial_.push_back(std::move(region));
        ++published_;
        if(partial_.size()==PublishedDictionarySnapshot::CHUNK_REGIONS){
            auto chunk=std::make_shared<PublishedRegionChunk>();
            chunk->regions=std::move(partial_);
            complete_.push_back(std::move(chunk));
            partial_.clear();
            partial_.reserve(PublishedDictionarySnapshot::CHUNK_REGIONS);
        }
    }
    auto snapshot=std::make_shared<PublishedDictionarySnapshot>();
    snapshot->chunks_=complete_;
    if(!partial_.empty()){
        auto tail=std::make_shared<PublishedRegionChunk>();
        tail->regions=partial_;
        snapshot->chunks_.push_back(std::move(tail));
    }
    snapshot->region_count_=published_;
    snapshot->raw_bytes_=raw_bytes_;
    snapshot->line_entries_=line_entries_;
    return snapshot;
}

bool parse_marker(const char* s, uint32_t len, Marker& m){
    if(len<4 || s[0]!='#' || s[1]!=' ') return false;
    const char* e=s+len; const char* p=s+2;
    if(p>=e || *p<'0'||*p>'9') return false;
    uint64_t n=0; while(p<e && *p>='0'&&*p<='9'){ n=n*10+(*p-'0'); ++p; } m.lineno=n;
    if(p+2>e || p[0]!=' '||p[1]!='"') return false;
    p+=2; const char* q=p; while(q<e && *q!='"') ++q; if(q>=e) return false; m.path.assign(p,q); p=q+1;
    m.flags.clear(); while(p<e && *p==' '){ ++p; if(p>=e||*p<'0'||*p>'9') return false; uint8_t fl=0; while(p<e&&*p>='0'&&*p<='9'){ fl=fl*10+(*p-'0'); ++p; } m.flags.push_back(fl); }
    if(p>=e || *p!='\n' || p+1!=e) return false;   // must end exactly with newline
    return true;
}
void emit_marker(const Marker& m, std::vector<uint8_t>& out){ char buf[32]; int l=snprintf(buf,sizeof buf,"# %llu \"",(unsigned long long)m.lineno); out.insert(out.end(),buf,buf+l); out.insert(out.end(),m.path.begin(),m.path.end()); out.push_back('"'); for(uint8_t f:m.flags){ out.push_back(' '); l=snprintf(buf,sizeof buf,"%u",f); out.insert(out.end(),buf,buf+l);} out.push_back('\n'); }

// M3: the system-header read path is DISABLED and abort-guarded (see cap_codec.h). A
// conformant M3 run never calls this; if a future edit reintroduces a header read it aborts
// loudly, and system_header_reads() (asserted 0 at end of run) makes the invariant testable.
static uint64_t g_system_header_reads=0;
uint64_t system_header_reads(){ return g_system_header_reads; }
void SourceTextStore::get(const std::string& path){
    ++g_system_header_reads;
    fprintf(stderr,"M3 VIOLATION: system-header read attempted for '%s' — the reduced grammar must be self-describing (F needs no headers)\n",path.c_str());
    abort();
}

static bool equal_tail(const char*p,const char*end,const char*value,size_t n){ return size_t(end-p)==n&&!memcmp(p,value,n); }
static int hex_value(uint8_t c){ if(c>='0'&&c<='9')return c-'0'; if(c>='a'&&c<='f')return c-'a'+10; if(c>='A'&&c<='F')return c-'A'+10; return -1; }
bool parse_byte_array(const char*data,uint32_t length,GeneratedByteArray&out){
    const char*begin=data,*end=data+length; if(length<8||end[-1]!='\n')return false;
    const char*p=begin; while(p<end&&(*p==' '||*p=='\t'))++p; const char*token=p;
    if(p<end&&*p==','){ ++p;while(p<end&&(*p==' '||*p=='\t'))++p;token=p; }
    else if(p>=end || !( (*p>='0'&&*p<='9') || (p+2<=end&&p[0]=='0'&&(p[1]=='x'||p[1]=='X')) )){
        const char*brace=(const char*)memchr(p,'{',size_t(end-p)); if(!brace)return false;
        std::string declaration(p,brace);
        if(declaration.find("uint8_t")==std::string::npos&&declaration.find("unsigned char")==std::string::npos)return false;
        p=brace+1;while(p<end&&(*p==' '||*p=='\t'))++p;token=p;
    }
    out={};out.prefix.assign(begin,token); bool haveSeparator=false,haveFormat=false,prefixUpper=false,havePrefix=false,sawLower=false,sawUpper=false; int numberFormat=-2;
    while(p<end){
        int value=0,currentFormat;
        if(p+2<=end&&p[0]=='0'&&(p[1]=='x'||p[1]=='X')){
            bool pu=p[1]=='X'; if(p+4>end)return false; int a=hex_value(uint8_t(p[2])),b=hex_value(uint8_t(p[3])); if(a<0||b<0)return false;
            if(!havePrefix){prefixUpper=pu;havePrefix=true;}else if(prefixUpper!=pu)return false;
            for(int i=2;i<4;++i){sawLower|=p[i]>='a'&&p[i]<='f';sawUpper|=p[i]>='A'&&p[i]<='F';}
            if(sawLower&&sawUpper)return false;
            value=(a<<4)|b;currentFormat=-1;p+=4;
        } else {
            const char*d=p;while(p<end&&*p>='0'&&*p<='9')++p;if(d==p)return false;
            if(p-d>1&&*d=='0')return false;
            for(const char*q=d;q<p;++q){value=value*10+(*q-'0');if(value>255)return false;}
            currentFormat=DECIMAL;
        }
        if(!haveFormat){numberFormat=currentFormat;haveFormat=true;}else if(numberFormat!=currentFormat)return false;
        out.values.push_back(uint8_t(value));
        if(equal_tail(p,end,"\n",1)||equal_tail(p,end,"}\n",2)||equal_tail(p,end,"};\n",3)){out.suffix.assign(p,end);break;}
        if(p>=end||*p!=',')return false;
        const char*separatorBegin=p++;
        if(equal_tail(p,end,"\n",1)||equal_tail(p,end,"}\n",2)||equal_tail(p,end,"};\n",3)){out.suffix.assign(separatorBegin,end);break;}
        while(p<end&&(*p==' '||*p=='\t'))++p;
        std::string current(separatorBegin,p);
        if(!haveSeparator){out.separator=current;haveSeparator=true;}else if(out.separator!=current)return false;
    }
    if(out.suffix.empty()||out.values.size()<4||!haveFormat)return false;
    if(!haveSeparator)out.separator=",";
    if(numberFormat==DECIMAL)out.format=DECIMAL;
    else if(prefixUpper)out.format=sawLower?HEX_UL:HEX_UU;
    else out.format=sawLower?HEX_LL:HEX_LU;
    return true;
}
void append_rendered_byte_array(const ByteArrayStyle&style,const uint8_t*values,size_t count,std::vector<uint8_t>&out){
    out.insert(out.end(),style.prefix.begin(),style.prefix.end());
    static const char*lo="0123456789abcdef",*up="0123456789ABCDEF";
    for(size_t i=0;i<count;++i){ if(i)out.insert(out.end(),style.separator.begin(),style.separator.end()); uint8_t v=values[i];
        if(style.format==DECIMAL){
            if(v>=100){out.push_back(uint8_t('0'+v/100));v%=100;out.push_back(uint8_t('0'+v/10));out.push_back(uint8_t('0'+v%10));}
            else if(v>=10){out.push_back(uint8_t('0'+v/10));out.push_back(uint8_t('0'+v%10));}
            else out.push_back(uint8_t('0'+v));
        }
        else { bool prefixUpper=style.format>=HEX_UL, lower=style.format==HEX_LL||style.format==HEX_UL;const char*digits=lower?lo:up;out.push_back('0');out.push_back(prefixUpper?'X':'x');out.push_back(digits[v>>4]);out.push_back(digits[v&15]); }
    }
    out.insert(out.end(),style.suffix.begin(),style.suffix.end());
}

// =====================================================================================
// C: mixed-region materializer — M3 REDUCED GRAMMAR.  The control lane emits ONLY the four
// terminals RAW_RUN(op0) / PUBLIC_LINE {publish op1/op9, ref op2, recover op7/op8} /
// BYTE_ARRAY(op3) / PP_MARKER(op4).  The system-header SOURCE_COPY/PATCH path (op5/op6) and
// all project-source/package machinery are REMOVED so the codec is self-describing and F
// reads zero system headers.  EMBEDDED_OBJECT(op10, P26-P29 material lane) is a reserved
// stub — never emitted here.
// =====================================================================================
void MixedEncoder::begin_authority_transaction(AuthorityTransaction&tx){
    if(++authorityJournalSerial==0){std::fill(authorityJournalStamp.begin(),authorityJournalStamp.end(),0);authorityJournalSerial=1;}
    tx=AuthorityTransaction{};tx.path_size=paths.size();tx.next_public=nextMixedPublic;
    tx.mixed_ops=mixedOps;tx.op7_count=op7_count;tx.op8_count=op8_count;tx.op9_count=op9_count;
    tx.op7_wire=op7_wire;tx.op8_wire=op8_wire;tx.op9_wire=op9_wire;
    tx.literal_raw=mixedLiteralRaw;tx.array_values=mixedArrayValues;tx.markers=n_marker;tx.literals=n_literal;
    tx.had_census=census_sink!=nullptr;tx.census_size=census_sink?census_sink->size():0;tx.active=true;
}

void MixedEncoder::commit_authority_transaction(AuthorityTransaction&tx){
    tx.active=false;tx.line_before.clear();tx.receiver_region_flips.clear();tx.receiver_public_flips.clear();
}

void MixedEncoder::rollback_authority_transaction(AuthorityTransaction&tx){
    if(!tx.active)return;
    for(const auto&entry:tx.line_before)mixedCLine[entry.first]=entry.second;
    for(size_t i=tx.path_size;i<paths.size();++i)pathid.erase(paths[i]);
    paths.resize(tx.path_size);nextMixedPublic=tx.next_public;mixedOps=tx.mixed_ops;
    op7_count=tx.op7_count;op8_count=tx.op8_count;op9_count=tx.op9_count;
    op7_wire=tx.op7_wire;op8_wire=tx.op8_wire;op9_wire=tx.op9_wire;
    mixedLiteralRaw=tx.literal_raw;mixedArrayValues=tx.array_values;n_marker=tx.markers;n_literal=tx.literals;
    if(tx.had_census&&census_sink)census_sink->resize(tx.census_size);
    if(tx.receiver_started){
        for(uint32_t id:tx.receiver_region_flips)
            if(id<tx.receiver_region_size)fknownReg[id]=0;
        for(uint32_t id:tx.receiver_public_flips)
            if(id<tx.receiver_public_size)fknownPublic[id]=0;
        fknownReg.resize(tx.receiver_region_size);
        fknownPublic.resize(tx.receiver_public_size);
    }
    tx.active=false;tx.line_before.clear();tx.receiver_region_flips.clear();tx.receiver_public_flips.clear();
}

struct MaterialLineView { uint32_t id=0,len=0; const char*data=nullptr; };
struct InternerMaterialAccess {
    using Region=uint32_t;
    const Interner&dict;
    Region pin(uint32_t id) const { if(id>=dict.region_count()){fprintf(stderr,"materialize: bad Region\n");exit(2);}return id; }
    uint32_t line_count(Region region) const { return dict.region_ids_count(region); }
    MaterialLineView line(Region region,uint32_t index) const {
        uint32_t id=dict.region_ids_ptr(region)[index];const LineRef&ref=dict.ref(id);
        return {id,ref.len,dict.line_data(ref.off)};
    }
    uint32_t raw_len(Region region) const { return dict.region_raw_len(region); }
    const char*raw_data(Region region) const { return dict.region_data(region); }
};
struct PublishedMaterialAccess {
    using Region=const PublishedRegion*;
    const PublishedDictionarySnapshot&dict;
    Region pin(uint32_t id) const { auto*region=dict.pin(id);if(!region){fprintf(stderr,"materialize: unpublished Region\n");exit(2);}return region; }
    uint32_t line_count(Region region) const { return uint32_t(region->lines.size()); }
    MaterialLineView line(Region region,uint32_t index) const {
        const auto&line=region->lines[index];
        return {line.id,line.length,reinterpret_cast<const char*>(region->raw.data()+line.offset)};
    }
    uint32_t raw_len(Region region) const { return uint32_t(region->raw.size()); }
    const char*raw_data(Region region) const { return reinterpret_cast<const char*>(region->raw.data()); }
};

template<class Dictionary>
static uint32_t materialize_impl(MixedEncoder&encoder,const Dictionary&dict,
                                 const std::vector<uint32_t>&missReg,size_t t,
                                 MixedEncoder::AuthorityTransaction*tx){
    auto&authorityJournalStamp=encoder.authorityJournalStamp;
    const uint32_t authorityJournalSerial=encoder.authorityJournalSerial;
    auto&mixedRaw=encoder.mixedRaw;auto&fill_paths=encoder.fill_paths;auto&np=encoder.np;
    auto&fill_public_base=encoder.fill_public_base;auto&nextMixedPublic=encoder.nextMixedPublic;
    auto&fill_path_base=encoder.fill_path_base;auto&paths=encoder.paths;auto&pathid=encoder.pathid;
    auto&mixedCLine=encoder.mixedCLine;
    auto&mixedOps=encoder.mixedOps;auto&fknownPublic=encoder.fknownPublic;auto&fknownReg=encoder.fknownReg;
    auto&op7_count=encoder.op7_count;auto&op8_count=encoder.op8_count;auto&op9_count=encoder.op9_count;
    auto&op7_wire=encoder.op7_wire;auto&op8_wire=encoder.op8_wire;auto&op9_wire=encoder.op9_wire;
    auto&mixedLiteralRaw=encoder.mixedLiteralRaw;auto&mixedArrayValues=encoder.mixedArrayValues;
    auto&n_marker=encoder.n_marker;auto&n_literal=encoder.n_literal;auto&census_sink=encoder.census_sink;
    if(tx&&!tx->active){fprintf(stderr,"materialize: inactive authority transaction\n");exit(2);}
    if(tx&&!tx->receiver_started){
        tx->receiver_region_size=fknownReg.size();tx->receiver_public_size=fknownPublic.size();
        tx->receiver_started=true;
    }
    auto journalLine=[&](uint32_t lineId,const MixedCLineState&state){
        if(tx&&authorityJournalStamp[lineId]!=authorityJournalSerial){
            authorityJournalStamp[lineId]=authorityJournalSerial;tx->line_before.emplace_back(lineId,state);
        }
    };
    auto markPublicKnown=[&](uint32_t ord){
        if(fknownPublic.size()<=ord)fknownPublic.resize(size_t(ord)+1,0);
        if(!fknownPublic[ord]){
            if(tx&&ord<tx->receiver_public_size)tx->receiver_public_flips.push_back(ord);
            fknownPublic[ord]=1;
        }
    };
    auto markRegionKnown=[&](uint32_t ord){
        if(ord>=fknownReg.size())return;
        if(!fknownReg[ord]){
            if(tx&&ord<tx->receiver_region_size)tx->receiver_region_flips.push_back(ord);
            fknownReg[ord]=1;
        }
    };
    (void)t;
    for(auto& v:mixedRaw) v.clear();
    fill_paths.clear(); np=0;
    fill_public_base=nextMixedPublic;
    fill_path_base=uint32_t(paths.size());
    std::vector<GeneratedByteArray> mixedArrayEntries;
    uint32_t nr=0;

    put_varint(mixedRaw[0],missReg.size());
    for(uint32_t r:missReg){
        auto region=dict.pin(r);uint32_t count=dict.line_count(region),offset=0,literalLength=0;
        auto ensurePathId=[&](const std::string&path){auto found=pathid.find(path);if(found!=pathid.end())return found->second;
            uint32_t id=uint32_t(paths.size());pathid.emplace(path,id);paths.push_back(path);put_varint(fill_paths,path.size());fill_paths.insert(fill_paths.end(),path.begin(),path.end());++np;return id;};
        put_varint(mixedRaw[0],dict.raw_len(region));
        auto flushLiteral=[&](){ if(!literalLength)return; mixedRaw[0].push_back(0);put_varint(mixedRaw[0],literalLength);++mixedOps[0];literalLength=0; };
        for(uint32_t j=0;j<count;++j){
            MaterialLineView line=dict.line(region,j);uint32_t lineId=line.id;const char*text=line.data;MixedCLineState&state=mixedCLine[lineId];
            if(state.public_id){
                uint32_t ord=state.public_id; flushLiteral();
                bool haveIt = ord<fknownPublic.size() && fknownPublic[ord];
                if(haveIt){
                    // INVARIANT (bigoracle): C must never op2 an ordinal it believes F lacks.
                    if(!(ord<fknownPublic.size()&&fknownPublic[ord])){fprintf(stderr,"INVARIANT: op2 for ordinal F lacks (%u)\n",ord);exit(2);}
                    mixedRaw[0].push_back(2);put_varint(mixedRaw[0],ord);++mixedOps[2];
                } else if(state.source_region<fknownReg.size() && fknownReg[state.source_region]){
                    // op8 DEFINE_FROM_VIEW: F still holds the source Region; re-bind ordinal via a cheap view.
                    mixedRaw[0].push_back(8);put_varint(mixedRaw[0],ord);
                    put_zigzag(mixedRaw[0],int64_t(state.source_region)-int64_t(r));
                    put_varint(mixedRaw[0],state.source_offset);put_varint(mixedRaw[0],line.len);
                    ++op8_count; op8_wire+=line.len;
                    markPublicKnown(ord);
                } else {
                    // op7 DEFINE_FROM_BYTES: re-bind ordinal with exact immutable bytes (literal lane).
                    mixedRaw[0].push_back(7);put_varint(mixedRaw[0],ord);put_varint(mixedRaw[0],line.len);
                    mixedRaw[1].insert(mixedRaw[1].end(),text,text+line.len);
                    ++op7_count; op7_wire+=line.len;
                    markPublicKnown(ord);
                }
            } else if(state.source_region!=UINT32_MAX&&state.source_region!=r){
                // Region ordinals are identities, not observation order. Reverse/shuffled
                // schedules may discover the matching source at a larger ordinal.
                auto source=dict.pin(state.source_region);
                if(state.source_offset+line.len>dict.raw_len(source)||
                   memcmp(dict.raw_data(source)+state.source_offset,text,line.len)){
                    fprintf(stderr,"bad mixed source Line\n");exit(2);
                }
                flushLiteral();
                journalLine(lineId,state);
                uint32_t ord=nextMixedPublic++; state.public_id=ord;
                markPublicKnown(ord);   // F materializes it now
                if(state.source_region<fknownReg.size() && fknownReg[state.source_region]){
                    // op1 PUBLISH_VIEW (cold + recovery-with-source): implicit ordinal from a view into F's source Region.
                    mixedRaw[0].push_back(1);put_zigzag(mixedRaw[0],int64_t(state.source_region)-int64_t(r));
                    put_varint(mixedRaw[0],state.source_offset);put_varint(mixedRaw[0],line.len);++mixedOps[1];
                } else {
                    // op9 PUBLISH_BYTES (recovery: F lacks the source Region): implicit ordinal from exact bytes.
                    mixedRaw[0].push_back(9);put_varint(mixedRaw[0],line.len);
                    mixedRaw[1].insert(mixedRaw[1].end(),text,text+line.len);++op9_count;op9_wire+=line.len;
                }
            } else {
                if(state.source_region==UINT32_MAX){journalLine(lineId,state);state.source_region=r;state.source_offset=offset;}
                GeneratedByteArray parsed;Marker lineMarker;
                bool arrayLine=parse_byte_array(text,line.len,parsed);
                bool markerLine=parse_marker(text,line.len,lineMarker);
                // --- EMBEDDED_OBJECT (op10, P26-P29 material lane) hook point: reserved, not implemented in M3. ---
                if(arrayLine){                                                      // BYTE_ARRAY
                    mixedArrayValues+=parsed.values.size();flushLiteral();mixedRaw[0].push_back(3);mixedArrayEntries.push_back(std::move(parsed));++mixedOps[3];++n_literal;
                } else if(markerLine){                                              // PP_MARKER
                    uint32_t pathId=ensurePathId(lineMarker.path);
                    flushLiteral();mixedRaw[0].push_back(4);put_varint(mixedRaw[0],pathId);put_varint(mixedRaw[0],lineMarker.lineno);
                    put_varint(mixedRaw[0],lineMarker.flags.size());for(uint8_t flag:lineMarker.flags)mixedRaw[0].push_back(flag);++mixedOps[4];++n_marker;
                } else {                                                            // RAW_RUN (self-describing literal — replaces op5/op6)
                    mixedRaw[1].insert(mixedRaw[1].end(),text,text+line.len);literalLength+=line.len;mixedLiteralRaw+=line.len;++n_literal;
                    if(census_sink) census_sink->push_back({uint32_t(t),r,lineId,offset,line.len});   // OPTIONAL observation (null=no-op)
                }
            }
            offset+=line.len;
        }
        flushLiteral();
        if(offset!=dict.raw_len(region)){fprintf(stderr,"mixed Region length differs\n");exit(2);}
        markRegionKnown(r);   // C now believes F holds region r (F materializes it from this Fill)
        ++nr;
    }
    if(!mixedArrayEntries.empty()){
        std::vector<ByteArrayStyle> styles;styles.reserve(mixedArrayEntries.size());
        for(const auto&value:mixedArrayEntries)styles.push_back({value.prefix,value.separator,value.suffix,value.format});
        std::sort(styles.begin(),styles.end());styles.erase(std::unique(styles.begin(),styles.end()),styles.end());
        put_varint(mixedRaw[2],styles.size());
        for(const auto&style:styles){put_varint(mixedRaw[2],style.format);
            for(const std::string*field:{&style.prefix,&style.separator,&style.suffix}){put_varint(mixedRaw[2],field->size());mixedRaw[2].insert(mixedRaw[2].end(),field->begin(),field->end());}}
        put_varint(mixedRaw[2],mixedArrayEntries.size());
        for(const auto&value:mixedArrayEntries){ByteArrayStyle style{value.prefix,value.separator,value.suffix,value.format};
            size_t styleId=std::lower_bound(styles.begin(),styles.end(),style)-styles.begin();put_varint(mixedRaw[2],styleId);put_varint(mixedRaw[2],value.values.size());
            mixedRaw[3].insert(mixedRaw[3].end(),value.values.begin(),value.values.end());}
    }
    return nr;
}

uint32_t MixedEncoder::materialize(const Interner&dict,const std::vector<uint32_t>&missReg,size_t t,
                                   AuthorityTransaction*tx){
    return materialize_impl(*this,InternerMaterialAccess{dict},missReg,t,tx);
}

uint32_t MixedEncoder::materialize(const PublishedDictionarySnapshot&dict,
                                   const std::vector<uint32_t>&missReg,size_t t,
                                   AuthorityTransaction*tx){
    return materialize_impl(*this,PublishedMaterialAccess{dict},missReg,t,tx);
}

// =====================================================================================
// F: install a first-use Block manifest (codec50-m1.cpp 678-686), F-side only.  Blocks are
// stored SPARSELY by id (a restart re-installs an arbitrary subset, not a dense 0..N run).
// COPY blocks resolve against Freg_stream (prior TUs); under recovery C sends LITERAL only.
// =====================================================================================
bool FStore::stage_blocks(const std::vector<uint8_t>&blockRaw,BlockTransaction&tx) const{
    tx=BlockTransaction{};tx.active=true;
    if(blockRaw.empty())return true;
    auto reject=[&](const char*why){fprintf(stderr,"stage_blocks: %s (payload=%zu)\n",why,blockRaw.size());tx.active=false;return false;};
    const uint8_t*bp=blockRaw.data(),*be=bp+blockRaw.size();uint64_t count=0;
    if(!get_varint_bounded(bp,be,count)||count>NBLK)return reject("count");
    tx.bindings.reserve(size_t(count));
    for(uint64_t i=0;i<count;++i){
        uint32_t id=0;if(!get_u32_bounded(bp,be,id)||id>=NBLK||bp==be)return reject("identity");
        uint8_t kind=*bp++;std::vector<uint32_t> children;uint64_t length=0;
        if(kind==1){
            uint64_t source=0;
            if(!get_varint_bounded(bp,be,source)||!get_varint_bounded(bp,be,length) ||
               source>Freg_stream.size()||length>Freg_stream.size()-size_t(source))return reject("copy range");
            children.insert(children.end(),Freg_stream.begin()+size_t(source),Freg_stream.begin()+size_t(source+length));
        }else if(kind==0){
            if(!get_varint_bounded(bp,be,length)||length>size_t(be-bp))return reject("literal length");
            children.reserve(size_t(length));
            for(uint64_t j=0;j<length;++j){uint32_t child=0;if(!get_u32_bounded(bp,be,child)||child>=NREG)return reject("literal child");children.push_back(child);}
        }else return reject("kind");

        const BlockBinding* prior=nullptr;
        for(const auto&binding:tx.bindings)if(binding.id==id){prior=&binding;break;}
        if(prior){if(prior->children!=children)return reject("unequal repeated binding");continue;}
        if(FknownBlk[id]){
            if(FblkChildren[id]!=children)return reject("unequal existing binding"); // unequal duplicate binding
            tx.bindings.push_back({id,std::move(children),false});  // equal replay is idempotent
        }else tx.bindings.push_back({id,std::move(children),true});
    }
    return bp==be?true:reject("trailing bytes");
}

void FStore::commit_blocks(BlockTransaction&tx){
    if(!tx.active)return;
    for(auto&binding:tx.bindings)if(binding.install){
        FblkChildren[binding.id]=std::move(binding.children);FknownBlk[binding.id]=1;
    }
    tx.active=false;
}

const std::vector<uint32_t>* FStore::block_children(uint32_t id,const BlockTransaction*tx) const{
    if(id>=NBLK)return nullptr;
    if(tx)for(const auto&binding:tx->bindings)if(binding.id==id)return &binding.children;
    return FknownBlk[id]?&FblkChildren[id]:nullptr;
}

void FStore::install_blocks(const std::vector<uint8_t>&blockRaw){
    BlockTransaction tx;
    if(!stage_blocks(blockRaw,tx)){fprintf(stderr,"bad direct Block manifest\n");exit(2);}
    commit_blocks(tx);
}

// =====================================================================================
// F: decode the mixed FILL streams into the region store (codec50-m1.cpp 982-1038 + M2).
// TRANSACTIONAL: every mutation is checkpointed; any validation failure truncates back and
// returns false with the store byte-for-byte unchanged (scenario 5).  op1 additionally
// MATERIALIZES immutable public-Line bytes (indexed by ordinal); op2 resolves from those
// bytes (so an evicted source Region can't dangle a view); op7/op8 re-establish a public
// ordinal under recovery (bytes / view); an unequal rebind of a held ordinal is rejected.
// =====================================================================================
void FStore::rollback_fill(FillTransaction&tx){
    if(!tx.active)return;
    FmixedRegionData.resize(tx.region_data_size);Fpaths.resize(tx.path_size);Fpublic_next=tx.public_next;
    for(auto undo=tx.regions.rbegin();undo!=tx.regions.rend();++undo)FmixedRegions[undo->first]=undo->second;
    for(uint32_t ord:tx.public_flips)if(ord<FpublicPresent.size()){
        FpublicPresent[ord]=0;std::vector<uint8_t>().swap(FpublicBytes[ord]);
    }
    FpublicBytes.resize(tx.public_size);FpublicPresent.resize(tx.public_size);FpublicLastUse.resize(tx.public_size);
    publicHeld=tx.public_held;tx.active=false;
}

void FStore::commit_fill(FillTransaction&tx){
    if(!tx.active)return;
    for(uint32_t ord:tx.public_touches)FpublicLastUse[ord]=tx.tu;
    tx.active=false;
}

bool FStore::stage_fill(const std::array<std::vector<uint8_t>,6>&recovered,
                        const std::vector<uint32_t>&missReg,
                        const std::vector<uint8_t>&fill_paths,
                        uint32_t pathBase,uint32_t publicBase,uint32_t t,FillTransaction&tx){
    tx=FillTransaction{};tx.region_data_size=FmixedRegionData.size();tx.path_size=Fpaths.size();
    tx.public_size=FpublicBytes.size();tx.public_next=Fpublic_next;tx.public_held=publicHeld;tx.tu=t;tx.active=true;
    auto reject=[&](const char*why)->bool{fprintf(stderr,"stage_fill: rollback (%s) — nothing committed\n",why);rollback_fill(tx);return false;};
    auto ensurePublic=[&](uint32_t ord){if(FpublicBytes.size()<=ord){FpublicBytes.resize(size_t(ord)+1);FpublicPresent.resize(size_t(ord)+1,0);FpublicLastUse.resize(size_t(ord)+1,0);}};
    auto bindPublic=[&](uint32_t ord,const uint8_t*p,size_t n)->bool{
        if(!ord)return false;
        ensurePublic(ord);
        if(FpublicPresent[ord]){
            if(FpublicBytes[ord].size()!=n||(n&&memcmp(FpublicBytes[ord].data(),p,n)))return false;
        }else{FpublicBytes[ord].assign(p,p+n);FpublicPresent[ord]=1;++publicHeld;tx.public_flips.push_back(ord);}
        tx.public_touches.push_back(ord);return true;
    };

    // Path definitions carry a base ordinal at the outer M4 Fill boundary.  Replaying an
    // equal definition is a no-op; a gap or unequal duplicate is rejected.
    if(pathBase>Fpaths.size())return reject("path base gap");
    {const uint8_t*pp=fill_paths.data(),*pe=pp+fill_paths.size();uint64_t ordinal=pathBase;
     while(pp<pe){uint64_t length=0;if(!get_varint_bounded(pp,pe,length)||length>size_t(pe-pp))return reject("truncated path");
        const uint8_t*begin=pp;pp+=size_t(length);
        if(ordinal<Fpaths.size()){
            if(Fpaths[size_t(ordinal)].size()!=length||(length&&memcmp(Fpaths[size_t(ordinal)].data(),begin,size_t(length))))return reject("unequal path replay");
        }else if(ordinal==Fpaths.size())Fpaths.emplace_back((const char*)begin,size_t(length));
        else return reject("path ordinal gap");
        ++ordinal;
     }}

    if(!recovered[4].empty()||!recovered[5].empty())return reject("source lanes removed in M3");
    const uint8_t*ap=recovered[2].data(),*ae=ap+recovered[2].size();
    const uint8_t*vp=recovered[3].data(),*ve=vp+recovered[3].size();
    std::vector<ByteArrayStyle> styles;uint64_t arrayCount=0,arraysUsed=0;
    if(!recovered[2].empty()){
        uint64_t styleCount=0;if(!get_varint_bounded(ap,ae,styleCount)||styleCount>recovered[2].size())return reject("bad style count");
        styles.reserve(size_t(styleCount));
        for(uint64_t k=0;k<styleCount;++k){
            uint64_t format=0;if(!get_varint_bounded(ap,ae,format)||format>HEX_UU)return reject("bad array format");
            ByteArrayStyle style;style.format=uint8_t(format);
            for(std::string*field:{&style.prefix,&style.separator,&style.suffix}){
                uint64_t length=0;if(!get_varint_bounded(ap,ae,length)||length>size_t(ae-ap))return reject("bad array style");
                field->assign((const char*)ap,size_t(length));ap+=size_t(length);
            }
            styles.push_back(std::move(style));
        }
        if(!get_varint_bounded(ap,ae,arrayCount))return reject("missing array count");
    }else if(!recovered[3].empty())return reject("partial array");

    uint64_t publicCursor=publicBase;
    // Public ordinals are allocated once by C across all receiver mirrors. A receiver
    // may first observe a sparse later ordinal after jobs ran on other F instances.
    if(!publicBase)return reject("zero public base");
    if(!recovered[0].empty()){
        const uint8_t*cp=recovered[0].data(),*ce=cp+recovered[0].size();
        const uint8_t*lp=recovered[1].data(),*le=lp+recovered[1].size();
        uint64_t regionCount=0;if(!get_varint_bounded(cp,ce,regionCount)||regionCount!=missReg.size())return reject("region count");
        for(uint64_t k=0;k<regionCount;++k){
            uint32_t regionId=missReg[size_t(k)];if(regionId>=FmixedRegions.size())return reject("region identity");
            uint64_t rawLength=0;if(!get_varint_bounded(cp,ce,rawLength)||rawLength>0x1fffffffull)return reject("region length");
            bool replay=FmixedRegions[regionId].known;size_t begin=FmixedRegionData.size();
            while(FmixedRegionData.size()-begin<rawLength){
                if(cp==ce)return reject("trunc control");
                uint8_t op=*cp++;
                if(op==0){
                    uint64_t length=0;if(!get_varint_bounded(cp,ce,length)||length>size_t(le-lp))return reject("trunc literal");
                    FmixedRegionData.insert(FmixedRegionData.end(),lp,lp+size_t(length));lp+=size_t(length);
                }else if(op==1||op==8){
                    uint32_t explicitOrd=0;if(op==8&&!get_u32_bounded(cp,ce,explicitOrd))return reject("bad op8 ordinal");
                    int64_t delta=0;uint64_t offset=0,length=0;
                    if(!get_zigzag_bounded(cp,ce,delta)||!get_varint_bounded(cp,ce,offset)||!get_varint_bounded(cp,ce,length))return reject("trunc public view");
                    int64_t source=int64_t(regionId)+delta;
                    if(source<0||uint64_t(source)>=FmixedRegions.size()||!FmixedRegions[size_t(source)].known||
                       offset>FmixedRegions[size_t(source)].length||length>FmixedRegions[size_t(source)].length-size_t(offset))return reject("bad public view");
                    size_t sourceBegin=FmixedRegions[size_t(source)].offset+size_t(offset),destination=FmixedRegionData.size();
                    if(length>SIZE_MAX-destination)return reject("public view overflow");
                    FmixedRegionData.resize(destination+size_t(length));
                    memmove(FmixedRegionData.data()+destination,FmixedRegionData.data()+sourceBegin,size_t(length));
                    uint32_t ord=explicitOrd;
                    if(op==1){if(publicCursor>UINT32_MAX)return reject("public ordinal overflow");ord=uint32_t(publicCursor++);}
                    if(!bindPublic(ord,FmixedRegionData.data()+destination,size_t(length)))return reject(op==1?"op1 rebind":"op8 rebind");
                }else if(op==2){
                    uint32_t ordinal=0;if(!get_u32_bounded(cp,ce,ordinal)||!ordinal||ordinal>=FpublicBytes.size()||!FpublicPresent[ordinal])return reject("bad public ref");
                    const auto&bytes=FpublicBytes[ordinal];FmixedRegionData.insert(FmixedRegionData.end(),bytes.begin(),bytes.end());tx.public_touches.push_back(ordinal);
                }else if(op==3){
                    uint64_t styleId=0,count=0;if(arraysUsed>=arrayCount||!get_varint_bounded(ap,ae,styleId)||!get_varint_bounded(ap,ae,count)||styleId>=styles.size()||count>size_t(ve-vp))return reject("bad array rec");
                    append_rendered_byte_array(styles[size_t(styleId)],vp,size_t(count),FmixedRegionData);vp+=size_t(count);++arraysUsed;
                }else if(op==4){
                    uint64_t pathId=0,lineNumber=0,flagCount=0;
                    if(!get_varint_bounded(cp,ce,pathId)||!get_varint_bounded(cp,ce,lineNumber)||!get_varint_bounded(cp,ce,flagCount)||pathId>=Fpaths.size()||flagCount>size_t(ce-cp))return reject("bad marker rec");
                    Marker marker;marker.path=Fpaths[size_t(pathId)];marker.lineno=lineNumber;marker.flags.assign(cp,cp+size_t(flagCount));cp+=size_t(flagCount);
                    std::vector<uint8_t>tmp;emit_marker(marker,tmp);FmixedRegionData.insert(FmixedRegionData.end(),tmp.begin(),tmp.end());
                }else if(op==7){
                    uint32_t ordinal=0;uint64_t length=0;if(!get_u32_bounded(cp,ce,ordinal)||!get_varint_bounded(cp,ce,length)||length>size_t(le-lp))return reject("trunc op7 bytes");
                    const uint8_t*bytes=lp;lp+=size_t(length);FmixedRegionData.insert(FmixedRegionData.end(),bytes,bytes+size_t(length));
                    if(!bindPublic(ordinal,bytes,size_t(length)))return reject("op7 unequal rebind");
                }else if(op==9){
                    uint64_t length=0;if(!get_varint_bounded(cp,ce,length)||length>size_t(le-lp)||publicCursor>UINT32_MAX)return reject("trunc op9 bytes");
                    const uint8_t*bytes=lp;lp+=size_t(length);FmixedRegionData.insert(FmixedRegionData.end(),bytes,bytes+size_t(length));
                    if(!bindPublic(uint32_t(publicCursor++),bytes,size_t(length)))return reject("op9 rebind");
                }else if(op==10)return reject("EMBEDDED_OBJECT reserved in M3");
                else return reject("bad opcode");
                if(FmixedRegionData.size()-begin>rawLength)return reject("region overrun");
            }
            if(replay){
                const auto&old=FmixedRegions[regionId];
                if(old.length!=rawLength||(rawLength&&memcmp(FmixedRegionData.data()+old.offset,FmixedRegionData.data()+begin,size_t(rawLength))))return reject("unequal Region replay");
                FmixedRegionData.resize(begin);
            }else{
                tx.regions.emplace_back(regionId,FmixedRegions[regionId]);
                FmixedRegions[regionId]={begin,uint32_t(rawLength),true};
            }
        }
        if(cp!=ce||lp!=le||ap!=ae||vp!=ve||arraysUsed!=arrayCount)return reject("stream trailing");
    }else if(!missReg.empty()||!recovered[1].empty()||!recovered[2].empty()||!recovered[3].empty())return reject("partial region streams");
    if(publicCursor>UINT32_MAX)return reject("public cursor overflow");
    Fpublic_next=std::max(Fpublic_next,uint32_t(publicCursor));
    return true;
}

bool FStore::decode_fill(const std::array<std::vector<uint8_t>,6>&recovered,
                         const std::vector<uint32_t>&missReg,
                         const std::vector<uint8_t>&fill_paths,uint32_t t){
    FillTransaction tx;
    if(!stage_fill(recovered,missReg,fill_paths,uint32_t(Fpaths.size()),Fpublic_next,t,tx))return false;
    commit_fill(tx);return true;
}

// =====================================================================================
// F: reconstruct one TU (.ii bytes) from decoded Root tokens (codec50-m1.cpp 1107-1127,
// mixed path) and grow Freg_stream in strict order.
// =====================================================================================
bool FStore::reconstruct_staged(const std::vector<uint8_t>&rootb,const BlockTransaction*blocks,
                                std::vector<uint8_t>&recon,std::vector<uint32_t>&occurrences) const{
    recon.clear();occurrences.clear();
    auto emitRegion=[&](uint32_t region)->bool{
        if(region>=FmixedRegions.size()||!FmixedRegions[region].known)return false;
        const auto&view=FmixedRegions[region];
        if(view.offset>FmixedRegionData.size()||view.length>FmixedRegionData.size()-view.offset)return false;
        if(view.length>SIZE_MAX-recon.size())return false;
        occurrences.push_back(region);
        recon.insert(recon.end(),FmixedRegionData.begin()+view.offset,FmixedRegionData.begin()+view.offset+view.length);
        return true;
    };
    const uint8_t*rp=rootb.data(),*re=rp+rootb.size();
    while(rp<re){
        uint32_t token=0;if(!get_u32_bounded(rp,re,token))return false;
        if(token<NREG){if(!emitRegion(token))return false;}
        else{
            uint32_t block=token-NREG;const auto*children=block_children(block,blocks);if(!children)return false;
            for(uint32_t child:*children)if(!emitRegion(child))return false;
        }
    }
    return true;
}

bool FStore::typed_requirements(const std::vector<uint8_t>&rootb,const BlockTransaction*blocks,
                                std::vector<uint32_t>&regions,
                                std::vector<uint32_t>&requiredBlocks) const{
    regions.clear();requiredBlocks.clear();
    const uint8_t*rp=rootb.data(),*re=rp+rootb.size();
    while(rp<re){
        uint64_t token=0;if(!get_varint_bounded(rp,re,token)||(token>>1)>UINT32_MAX)return false;
        uint32_t id=uint32_t(token>>1);
        if(token&1){if(id>=NBLK)return false;requiredBlocks.push_back(id);}
        else{if(id>=NREG)return false;regions.push_back(id);}
    }
    std::sort(requiredBlocks.begin(),requiredBlocks.end());
    requiredBlocks.erase(std::unique(requiredBlocks.begin(),requiredBlocks.end()),requiredBlocks.end());
    for(uint32_t block:requiredBlocks){
        const auto*children=block_children(block,blocks);if(!children)return false;
        for(uint32_t child:*children){if(child>=NREG)return false;regions.push_back(child);}
    }
    std::sort(regions.begin(),regions.end());
    regions.erase(std::unique(regions.begin(),regions.end()),regions.end());
    return true;
}

bool FStore::reconstruct_typed_staged(const std::vector<uint8_t>&rootb,
                                      const BlockTransaction*blocks,
                                      std::vector<uint8_t>&recon,
                                      std::vector<uint32_t>&occurrences) const{
    recon.clear();occurrences.clear();
    auto emitRegion=[&](uint32_t region)->bool{
        if(region>=FmixedRegions.size()||!FmixedRegions[region].known)return false;
        const auto&view=FmixedRegions[region];
        if(view.offset>FmixedRegionData.size()||view.length>FmixedRegionData.size()-view.offset)return false;
        if(view.length>SIZE_MAX-recon.size())return false;
        occurrences.push_back(region);
        recon.insert(recon.end(),FmixedRegionData.begin()+view.offset,
                     FmixedRegionData.begin()+view.offset+view.length);
        return true;
    };
    const uint8_t*rp=rootb.data(),*re=rp+rootb.size();
    while(rp<re){
        uint64_t token=0;if(!get_varint_bounded(rp,re,token)||(token>>1)>UINT32_MAX)return false;
        uint32_t id=uint32_t(token>>1);
        if(!(token&1)){if(!emitRegion(id))return false;continue;}
        const auto*children=block_children(id,blocks);if(!children)return false;
        for(uint32_t child:*children)if(!emitRegion(child))return false;
    }
    return true;
}

void FStore::reconstruct(const std::vector<uint8_t>&rootb,std::vector<uint8_t>&recon){
    std::vector<uint32_t>occurrences;
    if(!reconstruct_staged(rootb,nullptr,recon,occurrences)){fprintf(stderr,"bad Root expansion\n");exit(2);}
    Freg_stream.insert(Freg_stream.end(),occurrences.begin(),occurrences.end());
}

}  // namespace capc
