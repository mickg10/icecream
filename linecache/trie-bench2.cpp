// trie-bench2 — flat 64-byte trie with ADAPTIVE fat-node child index (issue #16).
//
// v1 diagnosis (measured): the child search was O(K) — a node with K children was
// scanned sibling-by-sibling to find the one matching the next byte, and K reached
// ~150 at structural prefix nodes (indentation, "#include ", ...). 150 hops/line ×
// 2.47M lines = 372M node reads = the whole runtime.
//
// Fix (ART-adaptive, on the same flat store): a node stays a cheap linear sibling
// chain while THIN. The instant a lookup walks >= PROMOTE_AT of its children, it is
// PROMOTED to a 256-entry direct byte->child table (one indirection, O(1)), so the
// giant child-search vanishes at exactly the fat nodes that caused it. Thin nodes
// (the vast majority) pay nothing. Node stays 64 bytes; fat tables live in a side
// arena; the high bit of `child` flags fat and its low bits index the table.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <chrono>
#include <algorithm>

using clk = std::chrono::steady_clock;
static double s_since(clk::time_point t0){ return std::chrono::duration<double>(clk::now()-t0).count(); }

struct Node {                 // exactly 64 bytes
    uint32_t key;             // terminal key, else 0
    uint32_t parent;          // parent node index (for reconstruction)
    uint32_t child;           // THIN: first-child index (0=none). FAT: FATBIT | table-id.
    uint32_t sibling;         // next sibling index (thin only; unused once parent is fat)
    uint8_t  span_len;        // edge label length (0..47)
    char     span[47];
};
static_assert(sizeof(Node)==64, "node must be one cache line");

static const uint32_t FATBIT = 0x80000000u;   // set in Node.child when node is fat
static const int      PROMOTE_AT = 8;         // walk this many siblings -> promote to fat table

static std::vector<Node> N;               // N[0] = root sentinel
static std::vector<uint32_t> key_node;    // key -> terminal node (key_node[0] unused)
static std::vector<uint32_t> fat;         // fat tables: 256 zeroed slots each; table t at fat[t*256]
static uint64_t next_key = 1;
static uint64_t g_sib = 0, g_desc = 0, g_fat_nodes = 0;   // instrumentation

static inline bool     is_fat(uint32_t u){ return (N[u].child & FATBIT) != 0; }
static inline uint32_t fid(uint32_t u){ return N[u].child & ~FATBIT; }

static inline uint32_t new_node(uint32_t parent){
    uint32_t i = (uint32_t)N.size(); N.push_back(Node{}); N[i].parent = parent; return i;
}
// promote a THIN node to a fat 256-way byte->child table (moves its existing children in)
static void promote(uint32_t u){
    uint32_t t = (uint32_t)(fat.size()/256);          // new table id
    fat.resize(fat.size()+256, 0);                    // 256 zeroed slots
    for (uint32_t c = N[u].child; c; c = N[c].sibling) // u is thin here: walk its chain
        fat[t*256 + (uint8_t)N[c].span[0]] = c;
    N[u].child = FATBIT | t;
    g_fat_nodes++;
}
// link `child` under `parent`, respecting thin/fat layout (span[0] distinct among siblings)
static inline void link_child(uint32_t parent, uint32_t child){
    if (is_fat(parent)){ fat[fid(parent)*256 + (uint8_t)N[child].span[0]] = child; N[child].sibling = 0; }
    else { N[child].sibling = N[parent].child; N[parent].child = child; }
}
// append a chain of nodes covering a[pos..len) under `last`; return the terminal node
static uint32_t append_chain(uint32_t last, const char* a, size_t pos, size_t len){
    while (pos < len){
        uint32_t ni = new_node(last);
        uint8_t sl = (uint8_t)std::min<size_t>(47, len-pos);
        N[ni].span_len = sl; memcpy(N[ni].span, a+pos, sl);
        link_child(last, ni);
        last = ni; pos += sl;
    }
    return last;
}
// insert-or-lookup exact atom a[0..len); returns its key
static uint64_t ins(const char* a, size_t len){
    uint32_t cur = 0; size_t pos = 0;
    for (;;){
        g_desc++;
        if (pos == len){
            if (N[cur].key) return N[cur].key;
            N[cur].key = next_key++; key_node.push_back(cur); return N[cur].key;
        }
        uint32_t c;
        if (is_fat(cur)) c = fat[fid(cur)*256 + (uint8_t)a[pos]];   // O(1)
        else {
            c = N[cur].child; int steps = 0;
            while (c && N[c].span[0] != a[pos]){ g_sib++; steps++; c = N[c].sibling; }
            if (steps >= PROMOTE_AT) promote(cur);                  // fat next time
        }
        if (!c){
            uint32_t t = append_chain(cur, a, pos, len);
            N[t].key = next_key++; key_node.push_back(t); return N[t].key;
        }
        uint8_t sl = N[c].span_len;
        size_t maxm = std::min<size_t>(sl, len-pos), m = 0;
        while (m < maxm && N[c].span[m] == a[pos+m]) m++;
        if (m == sl){ pos += sl; cur = c; continue; }        // full edge match -> descend
        // partial match -> split c at m: c=[span[0..m]], tail=[span[m..]] inherits c's key/children
        uint32_t tail = new_node(c);
        N[tail].span_len = sl - m; memcpy(N[tail].span, N[c].span + m, sl - m);
        N[tail].key = N[c].key;
        N[tail].child = N[c].child;                          // tail inherits c's children (incl FATBIT)
        if (is_fat(tail)){
            uint32_t base = fid(tail)*256;
            for (int b = 0; b < 256; b++){ uint32_t ch = fat[base+b]; if (ch) N[ch].parent = tail; }
        } else {
            for (uint32_t ch = N[tail].child; ch; ch = N[ch].sibling) N[ch].parent = tail;
        }
        if (N[c].key) key_node[N[c].key] = tail;             // old key now terminates at tail
        N[c].span_len = (uint8_t)m; N[c].key = 0; N[c].child = 0;  // c now thin, no children
        N[tail].sibling = 0;
        link_child(c, tail);                                 // tail is c's first child
        pos += m;
        if (pos == len){ N[c].key = next_key++; key_node.push_back(c); return N[c].key; }
        uint32_t t = append_chain(c, a, pos, len);
        N[t].key = next_key++; key_node.push_back(t); return N[t].key;
    }
}
static void reconstruct(uint64_t key, std::string& out){
    out.clear();
    uint32_t node = key_node[key];
    static thread_local std::vector<std::pair<const char*,uint8_t>> parts;
    parts.clear();
    while (node != 0){ parts.push_back({N[node].span, N[node].span_len}); node = N[node].parent; }
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) out.append(it->first, it->second);
}

int main(int argc, char** argv){
    const char* manifest = argc>1 && strcmp(argv[1],"--manifest")==0 ? argv[2] : nullptr;
    if (!manifest){ fprintf(stderr,"usage: trie-bench2 --manifest F [--verify]\n"); return 2; }
    bool verify = false; for(int i=1;i<argc;i++) if(!strcmp(argv[i],"--verify")) verify=true;
    N.reserve(4000000); fat.reserve(20000*256);
    N.push_back(Node{}); key_node.push_back(0);   // root + key-0 sentinel
    promote(0);                                    // root is fat from the start (was root_child[256])

    FILE* mf = fopen(manifest,"r"); if(!mf){ perror("manifest"); return 2; }
    char line[8192];
    std::vector<std::string> srcs; size_t raw_bytes=0, occ=0; long ntu=0, skipped=0;
    while (fgets(line,sizeof line,mf)){
        size_t L=strlen(line); while(L&&(line[L-1]=='\n'||line[L-1]=='\r')) line[--L]=0; if(!L) continue;
        FILE* tf=fopen(line,"rb"); if(!tf){ skipped++; continue; }
        std::string s; fseek(tf,0,SEEK_END); long sz=ftell(tf); fseek(tf,0,SEEK_SET);
        if(sz>0){ s.resize(sz); if(fread(&s[0],1,sz,tf)!=(size_t)sz) s.clear(); } fclose(tf);
        if(!s.empty()){ raw_bytes+=s.size(); srcs.push_back(std::move(s)); ntu++; }
    }
    fclose(mf);

    std::string chk; long verify_fail=0;
    auto t0 = clk::now();
    for (auto& s : srcs){
        size_t i=0, n=s.size();
        while (i<n){
            size_t j=s.find('\n',i); size_t end = (j==std::string::npos)? n : j+1;
            uint64_t k = ins(s.data()+i, end-i); occ++;
            if (verify){ reconstruct(k, chk); if(chk.size()!=end-i || memcmp(chk.data(), s.data()+i, end-i)!=0) verify_fail++; }
            i = end;
        }
    }
    double dt = s_since(t0);
    double mb = raw_bytes/1048576.0;
    fprintf(stderr,
      "TUs=%ld skipped=%ld  raw=%.1f MB  atom-occurrences=%zu  distinct(keys)=%llu\n"
      "trie nodes=%zu (%.1f MB)  fat nodes=%llu fat-tables=%.1f MB  verify_fail=%ld\n"
      "walk time=%.2f s  =>  THROUGHPUT = %.0f MB/s  (%.2f GB/s)\n"
      "  avg sibling-steps/lookup=%.2f  avg node-descents/lookup=%.2f\n",
      ntu, skipped, mb, occ, (unsigned long long)(next_key-1),
      N.size(), N.size()*64/1048576.0, (unsigned long long)g_fat_nodes, fat.size()*4/1048576.0, verify_fail,
      dt, mb/dt, mb/dt/1024.0,
      g_sib/(double)occ, g_desc/(double)occ);

    // fanout histogram (children per node) — sizes the per-node child structure (variant B)
    {
      const int NB=10; uint64_t hist[NB]={0}; uint32_t maxf=0; uint64_t fatmass=0, nodes_gt8=0;
      auto bucket=[](uint32_t f)->int{ if(f==0)return 0; if(f==1)return 1; if(f==2)return 2;
        if(f<=4)return 3; if(f<=8)return 4; if(f<=16)return 5; if(f<=32)return 6; if(f<=64)return 7;
        if(f<=128)return 8; return 9; };
      for (uint32_t u=0; u<N.size(); u++){
        uint32_t cnt=0;
        if (is_fat(u)){ uint32_t base=fid(u)*256; for(int b=0;b<256;b++) if(fat[base+b]) cnt++; }
        else { for(uint32_t c=N[u].child;c;c=N[c].sibling) cnt++; }
        hist[bucket(cnt)]++; if(cnt>maxf)maxf=cnt; if(cnt>8){nodes_gt8++; fatmass+=cnt;}
      }
      const char* lbl[NB]={"0","1","2","3-4","5-8","9-16","17-32","33-64","65-128","129+"};
      fprintf(stderr,"fanout: max=%u  nodes>8children=%llu (hold %llu children total)\n",
              maxf,(unsigned long long)nodes_gt8,(unsigned long long)fatmass);
      for(int i=0;i<NB;i++) fprintf(stderr,"  fanout %-7s : %llu nodes\n",lbl[i],(unsigned long long)hist[i]);
    }
    return verify_fail?1:0;
}
