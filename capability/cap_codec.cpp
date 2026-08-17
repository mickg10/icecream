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

bool system_source_path(const std::string&path){
    return path.rfind("/usr/include/",0)==0||path.rfind("/usr/lib/gcc/",0)==0||path.rfind("/usr/local/include/",0)==0;
}
const SourceText& SourceTextStore::get(const std::string&path){
    SourceText&source=files_[path];if(source.attempted)return source;source.attempted=true;source.offsets.push_back(0);
    struct stat st{};if((!allowProject_&&!system_source_path(path))||stat(path.c_str(),&st)||st.st_size<0||uint64_t(st.st_size)>UINT32_MAX)return source;
    FILE*file=fopen(path.c_str(),"rb");if(!file)return source;source.bytes.resize(size_t(st.st_size));
    if(!source.bytes.empty()&&fread(source.bytes.data(),1,source.bytes.size(),file)!=source.bytes.size()){fclose(file);source.bytes.clear();return source;}fclose(file);
    finish(source);return source;
}
bool SourceTextStore::install(const std::string&path,const uint8_t*data,size_t size){
    if(size>UINT32_MAX)return false;
    SourceText&source=files_[path];
    if(source.available)return source.bytes.size()==size&&(!size||!memcmp(source.bytes.data(),data,size));
    source={};source.attempted=true;source.offsets.push_back(0);
    if(size)source.bytes.assign(data,data+size);
    finish(source);return true;
}

uint32_t common_prefix(const uint8_t*a,uint32_t an,const uint8_t*b,uint32_t bn){
    uint32_t n=std::min(an,bn),i=0;while(i<n&&a[i]==b[i])++i;return i;
}
uint32_t common_suffix(const uint8_t*a,uint32_t an,const uint8_t*b,uint32_t bn,uint32_t prefix){
    uint32_t n=std::min(an-std::min(an,prefix),bn-std::min(bn,prefix)),i=0;while(i<n&&a[an-1-i]==b[bn-1-i])++i;return i;
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
// C: mixed-region materializer (codec50-m1.cpp 778-888), byte-for-byte for the canonical
// path.  useByteArrayLines=true, useProjectSource=false (the project-source/admission
// branches are dead for these flags but kept structurally so the live op5/op6 system-
// header SOURCE_COPY/PATCH branch ordering is identical to the reference).
// =====================================================================================
uint32_t MixedEncoder::materialize(const Interner& dict, const std::vector<uint32_t>& missReg, size_t t){
    const bool useByteArrayLines=true, useProjectSource=false;
    for(auto& v:mixedRaw) v.clear();
    fill_paths.clear(); np=0;
    std::vector<GeneratedByteArray> mixedArrayEntries;
    std::vector<std::pair<uint32_t,const SourceText*>> mixedSourceDefinitions;
    uint32_t nr=0;
    std::vector<uint8_t> sourceCostDst;

    put_varint(mixedRaw[0],missReg.size());
    for(uint32_t r:missReg){
        const uint32_t* lids=dict.region_ids_ptr(r); uint32_t count=dict.region_ids_count(r),offset=0,literalLength=0;
        Marker regionMarker;bool regionMarkerOk=false;
        if(count){const LineRef&first=dict.ref(lids[0]);regionMarkerOk=parse_marker(dict.line_data(first.off),first.len,regionMarker);}
        const SourceText*regionSource=regionMarkerOk?&mixedCSource.get(regionMarker.path):nullptr;
        auto ensurePathId=[&](const std::string&path){auto found=pathid.find(path);if(found!=pathid.end())return found->second;
            uint32_t id=uint32_t(paths.size());pathid.emplace(path,id);paths.push_back(path);put_varint(fill_paths,path.size());fill_paths.insert(fill_paths.end(),path.begin(),path.end());++np;return id;};
        auto ensureSourceDefinition=[&](const std::string&path,uint32_t pathId,const SourceText&source){
            if(!useProjectSource||system_source_path(path))return;
            if(mixedSourceSent.size()<=pathId)mixedSourceSent.resize(size_t(pathId)+1);
            if(mixedSourceSent[pathId])return;
            mixedSourceSent[pathId]=1;mixedSourceDefinitions.emplace_back(pathId,&source);
        };
        put_varint(mixedRaw[0],dict.region_raw_len(r));
        auto flushLiteral=[&](){ if(!literalLength)return; mixedRaw[0].push_back(0);put_varint(mixedRaw[0],literalLength);++mixedOps[0];literalLength=0; };
        for(uint32_t j=0;j<count;++j){
            uint32_t lineId=lids[j];const LineRef&line=dict.ref(lineId);const char*text=dict.line_data(line.off);MixedCLineState&state=mixedCLine[lineId];
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
                    if(fknownPublic.size()<=ord)fknownPublic.resize(size_t(ord)+1,0); fknownPublic[ord]=1;
                } else {
                    // op7 DEFINE_FROM_BYTES: re-bind ordinal with exact immutable bytes (literal lane).
                    mixedRaw[0].push_back(7);put_varint(mixedRaw[0],ord);put_varint(mixedRaw[0],line.len);
                    mixedRaw[1].insert(mixedRaw[1].end(),text,text+line.len);
                    ++op7_count; op7_wire+=line.len;
                    if(fknownPublic.size()<=ord)fknownPublic.resize(size_t(ord)+1,0); fknownPublic[ord]=1;
                }
            } else if(state.source_region!=UINT32_MAX&&state.source_region!=r){
                if(state.source_region>=r||state.source_offset+line.len>dict.region_raw_len(state.source_region)||
                   memcmp(dict.region_data(state.source_region)+state.source_offset,text,line.len)){
                    fprintf(stderr,"bad mixed source Line\n");exit(2);
                }
                flushLiteral();
                uint32_t ord=nextMixedPublic++; state.public_id=ord;
                if(fknownPublic.size()<=ord)fknownPublic.resize(size_t(ord)+1,0); fknownPublic[ord]=1;   // F materializes it now
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
                if(state.source_region==UINT32_MAX){state.source_region=r;state.source_offset=offset;}
                GeneratedByteArray parsed;Marker lineMarker;
                bool arrayLine=useByteArrayLines&&parse_byte_array(text,line.len,parsed),markerLine=parse_marker(text,line.len,lineMarker);
                size_t arrayValueCount=parsed.values.size();
                bool sourceCopy=false,sourcePatch=false;uint32_t sourceLine=0,patchPrefix=0,patchSuffix=0,patchMiddle=0;
                if(regionSource&&j>0){uint64_t candidate=uint64_t(regionMarker.lineno)+j-1;
                    if(candidate&&candidate<regionSource->offsets.size()){uint32_t index=uint32_t(candidate-1),begin=regionSource->offsets[index],end=regionSource->offsets[index+1];
                        if(regionSource->available){sourceLine=index;const uint8_t*base=regionSource->bytes.data()+begin;uint32_t baseLength=end-begin;
                            if(baseLength==line.len&&!memcmp(base,text,line.len))sourceCopy=true;
                            else {patchPrefix=common_prefix((const uint8_t*)text,line.len,base,baseLength);patchSuffix=common_suffix((const uint8_t*)text,line.len,base,baseLength,patchPrefix);patchMiddle=line.len-patchPrefix-patchSuffix;
                                size_t cost=1+varint_size(paths.size())+varint_size(sourceLine)+varint_size(patchPrefix)+varint_size(patchSuffix)+varint_size(patchMiddle)+patchMiddle;
                                sourcePatch=cost<line.len;}}
                    }
                }
                bool systemSource=regionSource&&system_source_path(regionMarker.path),sourceAdmitted=systemSource;
                uint32_t sourcePath=UINT32_MAX;SourceAdmission*admission=nullptr;
                if(regionSource&&(sourceCopy||sourcePatch)&&!systemSource&&useProjectSource){
                    sourcePath=ensurePathId(regionMarker.path);
                    if(mixedSourceAdmission.size()<=sourcePath)mixedSourceAdmission.resize(size_t(sourcePath)+1);
                    admission=&mixedSourceAdmission[sourcePath];
                    if(!admission->cost_known){
                        admission->package_cost=zstd_size(sourceCostZ,regionSource->bytes.data(),regionSource->bytes.size(),3,sourceCostDst)
                            +varint_size(sourcePath)+varint_size(regionSource->bytes.size())+16;
                        mixedSourceEstimatedCost+=admission->package_cost;++mixedSourceConsidered;admission->cost_known=true;
                    }
                    if(!admission->admitted&&admission->last_observed_tu!=SIZE_MAX&&admission->last_observed_tu<t&&
                       admission->observed_benefit>=uint64_t(sourceAdmitRatio)*admission->package_cost){
                        admission->admitted=true;++mixedSourceAdmitted;
                    }
                    sourceAdmitted=admission->admitted;
                }
                if(sourceCopy&&sourceAdmitted){
                    if(sourcePath==UINT32_MAX)sourcePath=ensurePathId(regionMarker.path);
                    ensureSourceDefinition(regionMarker.path,sourcePath,*regionSource);
                    flushLiteral();mixedRaw[0].push_back(5);put_varint(mixedRaw[0],sourcePath);put_varint(mixedRaw[0],sourceLine);++mixedOps[5];mixedSourceBytes+=line.len;
                } else if(arrayLine){
                    mixedArrayValues+=parsed.values.size();flushLiteral();mixedRaw[0].push_back(3);mixedArrayEntries.push_back(std::move(parsed));++mixedOps[3];++n_literal;
                } else if(markerLine){
                    uint32_t pathId=ensurePathId(lineMarker.path);
                    flushLiteral();mixedRaw[0].push_back(4);put_varint(mixedRaw[0],pathId);put_varint(mixedRaw[0],lineMarker.lineno);
                    put_varint(mixedRaw[0],lineMarker.flags.size());for(uint8_t flag:lineMarker.flags)mixedRaw[0].push_back(flag);++mixedOps[4];++n_marker;
                } else if(sourcePatch&&sourceAdmitted){
                    if(sourcePath==UINT32_MAX)sourcePath=ensurePathId(regionMarker.path);
                    ensureSourceDefinition(regionMarker.path,sourcePath,*regionSource);
                    flushLiteral();mixedRaw[0].push_back(6);put_varint(mixedRaw[0],sourcePath);put_varint(mixedRaw[0],sourceLine);
                    put_varint(mixedRaw[0],patchPrefix);put_varint(mixedRaw[0],patchSuffix);put_varint(mixedRaw[0],patchMiddle);mixedRaw[1].insert(mixedRaw[1].end(),text+patchPrefix,text+patchPrefix+patchMiddle);
                    ++mixedOps[6];mixedLiteralRaw+=patchMiddle;mixedSourceBytes+=patchPrefix+patchSuffix;++n_literal;
                } else {mixedRaw[1].insert(mixedRaw[1].end(),text,text+line.len);literalLength+=line.len;mixedLiteralRaw+=line.len;++n_literal;}
                if(admission&&!admission->admitted){
                    uint64_t benefit=sourceCopy?(arrayLine?arrayValueCount:(markerLine?0:line.len)):
                        ((!arrayLine&&!markerLine&&sourcePatch)?uint64_t(patchPrefix)+patchSuffix:0);
                    admission->observed_benefit+=benefit;admission->last_observed_tu=t;mixedSourcePotentialRaw+=benefit;
                }
            }
            offset+=line.len;
        }
        flushLiteral();
        if(offset!=dict.region_raw_len(r)){fprintf(stderr,"mixed Region length differs\n");exit(2);}
        if(r<fknownReg.size()) fknownReg[r]=1;   // C now believes F holds region r (F materializes it from this Fill)
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
    if(!mixedSourceDefinitions.empty()){
        put_varint(mixedRaw[4],mixedSourceDefinitions.size());
        for(const auto&definition:mixedSourceDefinitions){
            put_varint(mixedRaw[4],definition.first);put_varint(mixedRaw[4],definition.second->bytes.size());
            mixedRaw[5].insert(mixedRaw[5].end(),definition.second->bytes.begin(),definition.second->bytes.end());
            mixedSourcePackageRaw+=definition.second->bytes.size();++mixedSourcePackageFiles;
        }
    }
    return nr;
}

// =====================================================================================
// F: install a first-use Block manifest (codec50-m1.cpp 678-686), F-side only.  Blocks are
// stored SPARSELY by id (a restart re-installs an arbitrary subset, not a dense 0..N run).
// COPY blocks resolve against Freg_stream (prior TUs); under recovery C sends LITERAL only.
// =====================================================================================
void FStore::install_blocks(const std::vector<uint8_t>& blockRaw){
    const uint8_t*bp=blockRaw.data(),*be=bp+blockRaw.size();uint64_t count=get_varint(bp);
    for(uint64_t i=0;i<count;++i){uint64_t id=get_varint(bp);
        if(id>=FknownBlk.size()||FknownBlk[id]){fprintf(stderr,"bad direct Block identity\n");exit(2);}uint8_t kind=*bp++;
        std::vector<uint32_t>& children=FblkChildren[id]; children.clear();
        if(kind==1){uint64_t source=get_varint(bp),length=get_varint(bp);if(source+length>Freg_stream.size()){fprintf(stderr,"bad direct Block copy\n");exit(2);}for(uint64_t j=0;j<length;++j)children.push_back(Freg_stream[source+j]);}
        else if(kind==0){uint64_t length=get_varint(bp);for(uint64_t j=0;j<length;++j){uint64_t child=get_varint(bp);if(child>=NREG){fprintf(stderr,"bad direct Block child\n");exit(2);}children.push_back(uint32_t(child));}}
        else{fprintf(stderr,"bad direct Block kind\n");exit(2);}FknownBlk[id]=1;
    }
    if(bp!=be){fprintf(stderr,"direct Block manifest has trailing bytes\n");exit(2);}
}

// =====================================================================================
// F: decode the mixed FILL streams into the region store (codec50-m1.cpp 982-1038 + M2).
// TRANSACTIONAL: every mutation is checkpointed; any validation failure truncates back and
// returns false with the store byte-for-byte unchanged (scenario 5).  op1 additionally
// MATERIALIZES immutable public-Line bytes (indexed by ordinal); op2 resolves from those
// bytes (so an evicted source Region can't dangle a view); op7/op8 re-establish a public
// ordinal under recovery (bytes / view); an unequal rebind of a held ordinal is rejected.
// =====================================================================================
bool FStore::decode_fill(const std::array<std::vector<uint8_t>,6>& recovered,
                         const std::vector<uint32_t>& missReg,
                         const std::vector<uint8_t>& fill_paths, uint32_t t){
    const size_t ck_rd=FmixedRegionData.size();
    const size_t ck_paths=Fpaths.size();
    const uint32_t ck_pubnext=Fpublic_next;
    std::vector<uint32_t> committedRegions;      // regions marked .known this call
    std::vector<uint32_t> publicFlips;           // ordinals flipped absent->present this call
    auto rollback=[&](const char* why)->bool{
        fprintf(stderr,"decode_fill: rollback (%s) — nothing committed\n",why);
        FmixedRegionData.resize(ck_rd); Fpaths.resize(ck_paths); Fpublic_next=ck_pubnext;
        for(uint32_t r:committedRegions) FmixedRegions[r].known=false;
        for(uint32_t o:publicFlips){ FpublicPresent[o]=0; std::vector<uint8_t>().swap(FpublicBytes[o]); if(publicHeld) --publicHeld; }
        return false;
    };
    auto ensurePublic=[&](uint32_t ord){ if(FpublicBytes.size()<=ord){ FpublicBytes.resize(size_t(ord)+1); FpublicPresent.resize(size_t(ord)+1,0); FpublicLastUse.resize(size_t(ord)+1,0); } };
    // Materialize an immutable public Line; reject an unequal rebind of a held ordinal.
    auto bindPublic=[&](uint32_t ord,const uint8_t* p,size_t n)->bool{
        ensurePublic(ord);
        if(FpublicPresent[ord]){
            if(FpublicBytes[ord].size()!=n || (n && memcmp(FpublicBytes[ord].data(),p,n))) return false;   // UNEQUAL REBIND
            FpublicLastUse[ord]=t; return true;                                                            // idempotent
        }
        FpublicBytes[ord].assign(p,p+n); FpublicPresent[ord]=1; FpublicLastUse[ord]=t; ++publicHeld; publicFlips.push_back(ord); return true;
    };

    // ---- install Fpaths (checkpointed) ----
    { const uint8_t* pp=fill_paths.data(),*pe=pp+fill_paths.size();
      while(pp<pe){ uint64_t L=get_varint(pp); if(uint64_t(pe-pp)<L) return rollback("truncated path"); Fpaths.emplace_back((const char*)pp,(size_t)L); pp+=L; } }

    // ---- source packages (inert in canonical path) ----
    if(!recovered[4].empty()){
        const uint8_t*sp=recovered[4].data(),*se=sp+recovered[4].size(),*bp=recovered[5].data(),*be=bp+recovered[5].size();
        uint64_t sourceCount=get_varint(sp);
        for(uint64_t k=0;k<sourceCount;++k){ uint64_t pathId=get_varint(sp),length=get_varint(sp);
            if(pathId>=Fpaths.size()||length>uint64_t(be-bp)||!mixedFSource.install(Fpaths[pathId],bp,length)) return rollback("bad source def"); bp+=length; }
        if(sp!=se||bp!=be) return rollback("source def trailing");
    } else if(!recovered[5].empty()) return rollback("partial source def");

    const uint8_t*ap=recovered[2].data(),*ae=ap+recovered[2].size(),*vp=recovered[3].data(),*ve=vp+recovered[3].size();
    std::vector<ByteArrayStyle> mixedStyles;uint64_t arrayCount=0,arraysUsed=0;
    if(!recovered[2].empty()){
        uint64_t styleCount=get_varint(ap);mixedStyles.reserve(styleCount);
        for(uint64_t k=0;k<styleCount;++k){ByteArrayStyle style;uint64_t format=get_varint(ap);if(format>HEX_UU)return rollback("bad array format");style.format=uint8_t(format);
            for(std::string*field:{&style.prefix,&style.separator,&style.suffix}){uint64_t size=get_varint(ap);if(size>uint64_t(ae-ap))return rollback("bad array style");field->assign((const char*)ap,size);ap+=size;}mixedStyles.push_back(std::move(style));}
        arrayCount=get_varint(ap);
    } else if(!recovered[3].empty()) return rollback("partial array");

    if(!recovered[0].empty()){
        const uint8_t*cp=recovered[0].data(),*ce=cp+recovered[0].size();const uint8_t*lp=recovered[1].data(),*le=lp+recovered[1].size();
        uint64_t regionCount=get_varint(cp);if(regionCount!=missReg.size())return rollback("region count");
        for(uint64_t k=0;k<regionCount;++k){
            uint32_t regionId=missReg[k];if(regionId>=FmixedRegions.size()||FmixedRegions[regionId].known)return rollback("region identity");
            uint64_t rawLength=get_varint(cp);size_t begin=FmixedRegionData.size();
            while(FmixedRegionData.size()-begin<rawLength){
                if(cp>=ce)return rollback("trunc control");uint8_t op=*cp++;
                if(op==0){uint64_t length=get_varint(cp);if(length>uint64_t(le-lp))return rollback("trunc literal");FmixedRegionData.insert(FmixedRegionData.end(),lp,lp+length);lp+=length;}
                else if(op==1){int64_t source=int64_t(regionId)+get_zigzag(cp);uint64_t offset=get_varint(cp),length=get_varint(cp);
                  if(source<0||uint64_t(source)>=FmixedRegions.size()||!FmixedRegions[source].known||offset+length>FmixedRegions[source].length)return rollback("bad publish view");
                  size_t sourceBegin=FmixedRegions[source].offset+offset,destination=FmixedRegionData.size();FmixedRegionData.resize(destination+length);memcpy(FmixedRegionData.data()+destination,FmixedRegionData.data()+sourceBegin,length);
                  uint32_t ord=Fpublic_next++;                                                     // implicit ordinal (== C's nextMixedPublic)
                  if(!bindPublic(ord,FmixedRegionData.data()+destination,length))return rollback("op1 rebind");   // MATERIALIZE immutable bytes
                } else if(op==2){uint64_t publicId=get_varint(cp);if(!publicId||publicId>=FpublicBytes.size()||!FpublicPresent[publicId])return rollback("bad public ref");
                  const std::vector<uint8_t>&bytes=FpublicBytes[publicId];FpublicLastUse[publicId]=t;
                  FmixedRegionData.insert(FmixedRegionData.end(),bytes.begin(),bytes.end());        // resolve from IMMUTABLE bytes
                } else if(op==3){if(arraysUsed>=arrayCount)return rollback("missing array rec");uint64_t styleId=get_varint(ap),count=get_varint(ap);
                  if(styleId>=mixedStyles.size()||count>uint64_t(ve-vp))return rollback("bad array rec");
                  append_rendered_byte_array(mixedStyles[styleId],vp,count,FmixedRegionData);vp+=count;++arraysUsed;
                } else if(op==4){uint64_t pathId=get_varint(cp),lineNumber=get_varint(cp),flagCount=get_varint(cp);
                  if(pathId>=Fpaths.size()||flagCount>uint64_t(ce-cp))return rollback("bad marker rec");Marker marker;marker.path=Fpaths[pathId];marker.lineno=lineNumber;
                  marker.flags.assign(cp,cp+flagCount);cp+=flagCount;std::vector<uint8_t> tmp;emit_marker(marker,tmp);FmixedRegionData.insert(FmixedRegionData.end(),tmp.begin(),tmp.end());
                } else if(op==5){uint64_t pathId=get_varint(cp),lineIndex=get_varint(cp);if(pathId>=Fpaths.size())return rollback("bad source path");
                  const SourceText&source=mixedFSource.get(Fpaths[pathId]);if(!source.available||lineIndex+1>=source.offsets.size())return rollback("bad source line");
                  uint32_t begin2=source.offsets[lineIndex],end2=source.offsets[lineIndex+1];FmixedRegionData.insert(FmixedRegionData.end(),source.bytes.begin()+begin2,source.bytes.begin()+end2);
                } else if(op==6){uint64_t pathId=get_varint(cp),lineIndex=get_varint(cp),prefix=get_varint(cp),suffix=get_varint(cp),middle=get_varint(cp);
                  if(pathId>=Fpaths.size()||middle>uint64_t(le-lp))return rollback("bad source patch");const SourceText&source=mixedFSource.get(Fpaths[pathId]);
                  if(!source.available||lineIndex+1>=source.offsets.size())return rollback("bad source patch line");uint32_t begin2=source.offsets[lineIndex],end2=source.offsets[lineIndex+1];
                  if(prefix+suffix>end2-begin2)return rollback("bad source patch extent");FmixedRegionData.insert(FmixedRegionData.end(),source.bytes.begin()+begin2,source.bytes.begin()+begin2+prefix);
                  FmixedRegionData.insert(FmixedRegionData.end(),lp,lp+middle);lp+=middle;FmixedRegionData.insert(FmixedRegionData.end(),source.bytes.begin()+end2-suffix,source.bytes.begin()+end2);
                } else if(op==7){uint64_t ord=get_varint(cp),len=get_varint(cp);if(len>uint64_t(le-lp))return rollback("trunc op7 bytes");
                  const uint8_t* b7=lp; lp+=len; FmixedRegionData.insert(FmixedRegionData.end(),b7,b7+len);             // emit into region
                  if(!bindPublic(uint32_t(ord),b7,len))return rollback("op7 unequal rebind");                          // (re)define immutable
                } else if(op==8){uint64_t ord=get_varint(cp);int64_t source=int64_t(regionId)+get_zigzag(cp);uint64_t offset=get_varint(cp),length=get_varint(cp);
                  if(source<0||uint64_t(source)>=FmixedRegions.size()||!FmixedRegions[source].known||offset+length>FmixedRegions[source].length)return rollback("bad op8 view");
                  size_t sourceBegin=FmixedRegions[source].offset+offset,destination=FmixedRegionData.size();FmixedRegionData.resize(destination+length);memcpy(FmixedRegionData.data()+destination,FmixedRegionData.data()+sourceBegin,length);
                  if(!bindPublic(uint32_t(ord),FmixedRegionData.data()+destination,length))return rollback("op8 unequal rebind");
                } else if(op==9){uint64_t len=get_varint(cp);if(len>uint64_t(le-lp))return rollback("trunc op9 bytes");
                  const uint8_t* b9=lp; lp+=len; FmixedRegionData.insert(FmixedRegionData.end(),b9,b9+len);            // emit into region
                  uint32_t ord=Fpublic_next++;                                                                        // implicit ordinal (== op1)
                  if(!bindPublic(ord,b9,len))return rollback("op9 rebind");                                           // MATERIALIZE from bytes
                } else return rollback("bad opcode");
                if(FmixedRegionData.size()-begin>rawLength)return rollback("region overrun");
            }
            FmixedRegions[regionId]={begin,uint32_t(rawLength),true}; committedRegions.push_back(regionId);
        }
        if(cp!=ce||lp!=le||ap!=ae||vp!=ve||arraysUsed!=arrayCount)return rollback("stream trailing");
    } else if(!missReg.empty()||!recovered[1].empty()) return rollback("partial region streams");
    return true;
}

// =====================================================================================
// F: reconstruct one TU (.ii bytes) from decoded Root tokens (codec50-m1.cpp 1107-1127,
// mixed path) and grow Freg_stream in strict order.
// =====================================================================================
void FStore::reconstruct(const std::vector<uint8_t>& Frootb, std::vector<uint8_t>& recon){
    auto emitRegionF=[&](uint32_t r){ Freg_stream.push_back(r);
        if(r>=FmixedRegions.size()||!FmixedRegions[r].known){fprintf(stderr,"unknown mixed Root Region\n");exit(2);}
        const MixedFRegionView&view=FmixedRegions[r];recon.insert(recon.end(),FmixedRegionData.begin()+view.offset,FmixedRegionData.begin()+view.offset+view.length); };
    const uint8_t* pp=Frootb.data(), *pe=pp+Frootb.size();
    while(pp<pe){ uint32_t tok=uint32_t(get_varint(pp));
        if(tok<NREG) emitRegionF(tok);
        else { uint32_t k=tok-NREG; for(uint32_t child:FblkChildren[k]) emitRegionF(child); } }
}

}  // namespace capc
