// corpus-stats — how many UNIQUE lines, size-class split, hot-set concentration.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <algorithm>

static inline uint64_t mulfold(uint64_t a,uint64_t b){ __uint128_t r=(__uint128_t)a*b; return (uint64_t)r ^ (uint64_t)(r>>64); }
static inline uint64_t hashln(const char* p,size_t n){
    const uint64_t P1=0xa0761d6478bd642full,P2=0xe7037ed1a0b428dbull; uint64_t h=n^P1; size_t i=0;
    for(;i+16<=n;i+=16){ uint64_t x,y; memcpy(&x,p+i,8); memcpy(&y,p+i+8,8); h=mulfold(x^P1,y^h^P2); }
    uint64_t x=0,y=0; size_t rem=n-i; if(rem){ memcpy(&x,p+i,rem<8?rem:8); if(rem>8) memcpy(&y,p+i+8,rem-8);} return mulfold(x^P1,y^h^P2);
}
static std::vector<uint64_t> H; static std::vector<uint32_t> ID; static uint64_t MASK;
static std::vector<char> arena; static std::vector<uint32_t> aoff,alen,cnt;
static inline uint32_t intern(const char* p,uint32_t n){
    uint64_t h=hashln(p,n),b=h&MASK;
    for(;;){ uint32_t id=ID[b];
        if(!id){ uint32_t off=(uint32_t)arena.size(); arena.insert(arena.end(),p,p+n);
            aoff.push_back(off); alen.push_back(n); cnt.push_back(0); uint32_t nid=(uint32_t)aoff.size()-1;
            H[b]=h; ID[b]=nid; return nid; }
        if(H[b]==h&&alen[id]==n&&memcmp(arena.data()+aoff[id],p,n)==0) return id; b=(b+1)&MASK; }
}
static int cls(uint32_t n){ return n<=4?0 : n<=16?1 : n<=48?2 : 3; }

int main(int argc,char**argv){
    const char* manifest=nullptr; for(int i=1;i<argc;i++) if(!strcmp(argv[i],"--manifest")&&i+1<argc) manifest=argv[++i];
    if(!manifest){ fprintf(stderr,"usage: corpus-stats --manifest F\n"); return 2; }
    MASK=(1u<<21)-1; H.assign(MASK+1,0); ID.assign(MASK+1,0); arena.reserve(64u<<20);
    aoff.push_back(0); alen.push_back(0); cnt.push_back(0);

    FILE* mf=fopen(manifest,"r"); if(!mf){ perror("manifest"); return 2; }
    char line[8192]; std::vector<std::string> srcs; size_t raw=0; long ntu=0;
    while(fgets(line,sizeof line,mf)){ size_t L=strlen(line); while(L&&(line[L-1]=='\n'||line[L-1]=='\r'))line[--L]=0; if(!L)continue;
        FILE* tf=fopen(line,"rb"); if(!tf)continue; std::string s; fseek(tf,0,SEEK_END); long sz=ftell(tf); fseek(tf,0,SEEK_SET);
        if(sz>0){ s.resize(sz); if(fread(&s[0],1,sz,tf)!=(size_t)sz) s.clear(); } fclose(tf);
        if(!s.empty()){ raw+=s.size(); srcs.push_back(std::move(s)); ntu++; } }
    fclose(mf);

    uint64_t occ=0, occ_cls[4]={0}, blank_occ=0;
    for(auto& s:srcs){ size_t i=0,n=s.size(); while(i<n){ size_t j=s.find('\n',i); size_t end=(j==std::string::npos)?n:j+1;
        uint32_t len=(uint32_t)(end-i); uint32_t id=intern(s.data()+i,len); cnt[id]++; occ++; occ_cls[cls(len)]++;
        if(len==1) blank_occ++; i=end; } }

    uint64_t dist=aoff.size()-1, dist_cls[4]={0}, blank_dist=0;
    for(uint32_t id=1;id<=dist;id++){ dist_cls[cls(alen[id])]++; if(alen[id]==1) blank_dist++; }

    printf("=== CORPUS ===\n");
    printf("TUs=%ld  raw=%.1f MB  total line-occurrences=%llu  UNIQUE lines=%llu  redundancy=%.2f%%  avg reuse=%.1fx\n",
        ntu, raw/1048576.0, (unsigned long long)occ, (unsigned long long)dist,
        100.0*(1.0-(double)dist/occ), (double)occ/dist);
    const char* nm[4]={"tiny <=4","small 5-16","medium 17-48","large >48"};
    printf("\nby size class      occurrences (%% of all)      distinct (%% of unique)\n");
    for(int c=0;c<4;c++) printf("  %-14s %14llu (%5.1f%%)     %10llu (%5.1f%%)\n",
        nm[c],(unsigned long long)occ_cls[c],100.0*occ_cls[c]/occ,(unsigned long long)dist_cls[c],100.0*dist_cls[c]/dist);
    printf("  blank ('\\n' len1) %14llu (%5.1f%%)     %10llu (%5.1f%%)\n",
        (unsigned long long)blank_occ,100.0*blank_occ/occ,(unsigned long long)blank_dist,100.0*blank_dist/dist);

    // hot-set concentration: what % of occurrences do the top-K most frequent unique lines cover?
    std::vector<uint32_t> c2(cnt.begin()+1,cnt.end()); std::sort(c2.begin(),c2.end(),std::greater<uint32_t>());
    uint64_t acc=0; size_t K[5]={100,1000,10000,100000,(size_t)dist}; int ki=0;
    printf("\nhot-set concentration (top-K unique lines -> %% of ALL occurrences):\n");
    for(size_t i=0;i<c2.size();i++){ acc+=c2[i];
        if(ki<5 && i+1==K[ki]){ printf("  top %-7zu : %5.2f%% of occurrences\n",K[ki],100.0*acc/occ); ki++; } }
    return 0;
}
