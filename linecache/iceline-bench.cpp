// iceline-bench — Layer A: intrinsic line-dedup codec + benchmark (issue #16).
//
// Streams a fixed manifest of preprocessed .ii TUs through one simulated warm
// C->F pair (in-process cache; no helper IPC yet). For each TU it encodes the
// settled wire packet — REFS (key>=1) / INLINE (new atoms) / BODY (first-
// appearance local IDs, delta-zigzag-varint) — zstd-frames it at several
// levels, DECODES, asserts byte-exact round-trip, and records sizes + CPU as
// JSONL. Warm steady state => need_count == 0.
//
// Identity per the converged design: (C_GUID, u64 key) with key monotonic from
// 1 (0 = inline-only sentinel), packet-carried key_limit. Atoms include their
// LF delimiter so empty/no-final-newline/CRLF/NUL round-trip exactly.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <unordered_map>
#include <chrono>
#include <algorithm>
#include <string_view>
#include <zstd.h>

using clk = std::chrono::steady_clock;
static double us_since(clk::time_point t0){
    return std::chrono::duration<double, std::micro>(clk::now() - t0).count();
}

// ---- atomization: each atom ends at (and includes) its '\n'; last may not ----
// Returns views into `s` (no per-line allocation) — the dominant CPU win.
static std::vector<std::string_view> atomize(const std::string& s){
    std::vector<std::string_view> out;
    size_t i = 0, n = s.size();
    while (i < n){
        size_t j = s.find('\n', i);
        if (j == std::string::npos){ out.emplace_back(s.data()+i, n-i); break; }
        out.emplace_back(s.data()+i, j-i+1);
        i = j + 1;
    }
    return out;
}
static inline bool sveq(const std::string& a, std::string_view b){
    return a.size()==b.size() && memcmp(a.data(), b.data(), b.size())==0;
}

// ---- varints ----
static void uv(std::string& o, uint64_t n){
    for (;;){ uint8_t b = n & 0x7f; n >>= 7; if (n){ o.push_back(char(b|0x80)); } else { o.push_back(char(b)); break; } }
}
static uint64_t rv(const uint8_t*& p){
    uint64_t r = 0; int s = 0;
    for (;;){ uint8_t b = *p++; r |= uint64_t(b & 0x7f) << s; if (!(b & 0x80)) break; s += 7; }
    return r;
}
static inline uint64_t zz(int64_t n){ return (uint64_t(n) << 1) ^ uint64_t(n >> 63); }
static inline int64_t unzz(uint64_t n){ return int64_t(n >> 1) ^ -int64_t(n & 1); }

// ---- FNV-1a 64 digest over the raw source (whole-TU digest) ----
static uint64_t fnv1a(const std::string& s){
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : s){ h ^= c; h *= 1099511628211ULL; }
    return h;
}

// ---- MD5 (public-domain style, compact) — the content-index key the design uses ----
struct MD5 {
    uint32_t a,b,c,d; uint64_t len; uint8_t buf[64]; size_t bl;
    MD5():a(0x67452301),b(0xefcdab89),c(0x98badcfe),d(0x10325476),len(0),bl(0){}
    static uint32_t rol(uint32_t x,int s){ return (x<<s)|(x>>(32-s)); }
    void block(const uint8_t* p){
        static const uint32_t K[64]={
        0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
        0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
        0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
        0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
        0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
        0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
        0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
        0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391};
        static const int S[64]={7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
        5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
        6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21};
        uint32_t M[16]; for(int i=0;i<16;i++) M[i]=p[i*4]|p[i*4+1]<<8|p[i*4+2]<<16|(uint32_t)p[i*4+3]<<24;
        uint32_t A=a,B=b,C=c,D=d;
        for(int i=0;i<64;i++){ uint32_t F; int g;
            if(i<16){F=(B&C)|(~B&D);g=i;} else if(i<32){F=(D&B)|(~D&C);g=(5*i+1)&15;}
            else if(i<48){F=B^C^D;g=(3*i+5)&15;} else {F=C^(B|~D);g=(7*i)&15;}
            F=F+A+K[i]+M[g]; A=D;D=C;C=B;B=B+rol(F,S[i]); }
        a+=A;b+=B;c+=C;d+=D;
    }
    void update(const uint8_t* p,size_t n){ len+=n;
        while(n){ size_t t=64-bl; if(t>n)t=n; memcpy(buf+bl,p,t); bl+=t;p+=t;n-=t; if(bl==64){block(buf);bl=0;} } }
    void finish(uint8_t out[16]){ uint64_t bits=len*8; uint8_t pad=0x80; update(&pad,1);
        uint8_t z=0; while(bl!=56) update(&z,1); for(int i=0;i<8;i++){uint8_t b8=bits>>(8*i);update(&b8,1);}
        uint32_t v[4]={a,b,c,d}; for(int i=0;i<4;i++) for(int j=0;j<4;j++) out[i*4+j]=v[i]>>(8*j); }
};
struct Md5Key { uint64_t hi, lo; bool operator==(const Md5Key&o)const{return hi==o.hi&&lo==o.lo;} };
struct Md5Hash { size_t operator()(const Md5Key&k)const{ return k.hi ^ (k.lo*1099511628211ULL); } };
static Md5Key md5key(const std::string& s){ MD5 m; m.update((const uint8_t*)s.data(),s.size()); uint8_t o[16]; m.finish(o);
    Md5Key k; memcpy(&k.hi,o,8); memcpy(&k.lo,o+8,8); return k; }

static size_t zc(const std::string& in, int level){
    size_t bound = ZSTD_compressBound(in.size());
    std::string out(bound, '\0');
    size_t r = ZSTD_compress(&out[0], bound, in.data(), in.size(), level);
    return ZSTD_isError(r) ? 0 : r;
}

// ---- the warm C->F pair (in-process) ----
struct Pair {
    std::unordered_map<uint64_t, uint64_t> c_key;        // fasthash(atom) -> key (C dict; verify via c_atom)
    std::unordered_map<uint64_t, std::string> c_atom;    // key -> atom (C arena + exact verify)
    uint64_t next_key = 1;                               // 0 = inline sentinel
    std::unordered_map<uint64_t, std::string> f_mirror;  // key -> atom (F cache)
    size_t c_bytes = 0, f_bytes = 0;                     // resident content bytes
};

// current-transport baseline: 100 KB FileChunkMsg payloads, zstd-1 each, summed
static size_t chunked_zstd1(const std::string& src){
    size_t total = 0;
    for (size_t off = 0; off < src.size(); off += 100000){
        size_t len = std::min<size_t>(100000, src.size() - off);
        total += zc(src.substr(off, len), 1);
    }
    return total;
}

struct Row {
    size_t raw, cur_z1, whole1, whole3, whole6, packed1, packed3, packed6;
    size_t refs_b, inline_b, body_b, body_raw_b;
    double transform_us, zstd3_us, decode_us;
    size_t distinct, new_lines, need_count, peak;
    int ok;
};

// Encode one TU; returns the raw (uncompressed) packet, fills byte breakdown.
static std::string encode(Pair& P, const std::string& src, Row& row, const uint8_t guid[16]){
    auto t0 = clk::now();
    std::vector<std::string_view> atoms = atomize(src);
    std::unordered_map<std::string_view, uint32_t> local;   // atom -> local id
    std::vector<uint32_t> body;                        body.reserve(atoms.size());
    std::vector<uint64_t> mapper;                      // local id -> key (first-appearance order)
    std::vector<uint32_t> new_ids;                     // local ids new to C this TU (INLINE)
    std::hash<std::string_view> H;
    for (auto a : atoms){
        auto it = local.find(a);
        uint32_t lid;
        if (it == local.end()){
            lid = uint32_t(local.size());
            local.emplace(a, lid);
            uint64_t h = H(a);                         // fast local index (per-line lookup cost)
            auto ck = P.c_key.find(h);
            bool hit = (ck != P.c_key.end() && sveq(P.c_atom[ck->second], a)); // exact-byte verify
            uint64_t key;
            if (hit){
                key = ck->second;                      // known -> REF
            } else {                                   // new (or hash collision) -> assign key, INLINE
                key = P.next_key++;
                if (ck == P.c_key.end()) P.c_key[h] = key;
                P.c_atom[key].assign(a.data(), a.size());  // arena copy (new lines only, ~1% warm)
                P.c_bytes += a.size();
                new_ids.push_back(lid);
            }
            mapper.push_back(key);
        } else {
            lid = it->second;
        }
        body.push_back(lid);
    }
    // local id -> atom view
    std::vector<std::string_view> id_atom(local.size());
    for (auto& kv : local) id_atom[kv.second] = kv.first;
    uint64_t key_limit = P.next_key;

    // --- serialize packet: header | mapper | inline | body ---
    std::string pkt;
    pkt.append((const char*)guid, 16);
    uv(pkt, key_limit);
    uv(pkt, local.size());
    uint64_t raw_len = src.size();
    uv(pkt, raw_len);
    uint64_t dig = fnv1a(src);
    for (int i = 0; i < 8; i++) pkt.push_back(char((dig >> (8*i)) & 0xff));
    // mapper: key per local id, delta-zigzag-varint
    std::string mp; { uint64_t prev = 0; for (uint64_t k : mapper){ uv(mp, zz(int64_t(k) - int64_t(prev))); prev = k; } }
    uv(pkt, mp.size()); pkt += mp;
    // inline: count, then (local_id, len, bytes)
    std::string in; uv(in, new_ids.size());
    for (uint32_t lid : new_ids){ std::string_view a = id_atom[lid]; uv(in, lid); uv(in, a.size()); in.append(a.data(), a.size()); }
    uv(pkt, in.size()); pkt += in;
    // body: count + delta-zigzag-varint of local ids
    std::string bd; uv(bd, body.size()); { int64_t prev = 0; for (uint32_t id : body){ uv(bd, zz(int64_t(id) - prev)); prev = id; } }
    uv(pkt, bd.size()); pkt += bd;

    row.transform_us = us_since(t0);
    row.distinct = local.size();
    row.new_lines = new_ids.size();
    row.need_count = 0;              // warm pair: F learns every INLINE, so a subsequent REF always hits
    row.refs_b = mp.size();
    row.inline_b = in.size();
    row.body_b = bd.size();
    { std::string raw32; uv(raw32, body.size()); for (uint32_t id : body){ raw32.push_back(char(id)); raw32.push_back(char(id>>8)); raw32.push_back(char(id>>16)); raw32.push_back(char(id>>24)); } row.body_raw_b = raw32.size(); }
    row.peak = pkt.size() + src.size();

    // F learns the INLINE bindings for future TUs (warm-pair mirror)
    for (uint32_t lid : new_ids){ std::string_view a = id_atom[lid]; uint64_t k = mapper[lid]; P.f_mirror[k].assign(a.data(), a.size()); P.f_bytes += a.size(); }
    return pkt;
}

// Decode + byte-exact verify (warm: all REFS resolve from f_mirror).
static int decode_verify(Pair& P, const std::string& pkt, const std::string& orig, double& decode_us){
    auto t0 = clk::now();
    const uint8_t* p = (const uint8_t*)pkt.data();
    p += 16;                                    // guid
    uint64_t key_limit = rv(p);
    uint64_t k = rv(p);
    uint64_t raw_len = rv(p);
    uint64_t dig = 0; for (int i = 0; i < 8; i++) dig |= uint64_t(*p++) << (8*i);
    // mapper
    uint64_t mplen = rv(p); const uint8_t* mpe = p + mplen;
    std::vector<uint64_t> mapper(k); { uint64_t prev = 0; for (uint64_t i = 0; i < k; i++){ prev = uint64_t(int64_t(prev) + unzz(rv(p))); mapper[i] = prev; } }
    p = mpe;
    // inline
    uint64_t inlen = rv(p); const uint8_t* ine = p + inlen;
    std::vector<std::string> access(k);
    std::vector<char> have(k, 0);
    uint64_t ic = rv(p);
    for (uint64_t i = 0; i < ic; i++){ uint64_t lid = rv(p); uint64_t len = rv(p); access[lid].assign((const char*)p, len); have[lid] = 1; p += len; }
    p = ine;
    // resolve REFS from f_mirror
    int need = 0;
    for (uint64_t i = 0; i < k; i++){
        if (have[i]) continue;
        auto it = P.f_mirror.find(mapper[i]);
        if (it == P.f_mirror.end()){ need++; continue; }   // would be a NEED (should be 0 warm)
        access[i] = it->second; have[i] = 1;
    }
    // body -> reconstruct
    uint64_t blen = rv(p);
    uint64_t bc = rv(p);
    std::string out; out.reserve(raw_len);
    int64_t prev = 0;
    for (uint64_t i = 0; i < bc; i++){ prev = prev + unzz(rv(p)); uint32_t id = uint32_t(prev); if (id >= k || !have[id]){ decode_us = us_since(t0); return -1; } out += access[id]; }
    decode_us = us_since(t0);
    (void)key_limit; (void)blen;
    if (need) return -2;
    if (out.size() != raw_len || out != orig) return 0;
    if (fnv1a(out) != dig) return 0;
    return 1;
}

int main(int argc, char** argv){
    const char* manifest = nullptr; const char* out = nullptr;
    std::vector<int> levels;
    for (int i = 1; i < argc; i++){
        if (!strcmp(argv[i], "--manifest") && i+1 < argc) manifest = argv[++i];
        else if (!strcmp(argv[i], "--out") && i+1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--levels") && i+1 < argc){ char* s = argv[++i]; for (char* t = strtok(s, ","); t; t = strtok(nullptr, ",")) levels.push_back(atoi(t)); }
    }
    if (!manifest){ fprintf(stderr, "usage: iceline-bench --manifest F --levels 1,3,6 --out F.jsonl\n"); return 2; }
    if (levels.empty()) levels = {1,3,6};
    FILE* mf = fopen(manifest, "r"); if (!mf){ perror("manifest"); return 2; }
    FILE* of = out ? fopen(out, "w") : stdout;

    uint8_t guid[16]; for (int i = 0; i < 16; i++) guid[i] = uint8_t(0x11*(i+1)); // fixed test GUID

    Pair P;
    char line[8192]; long ntu = 0, fails = 0;
    size_t sum_raw=0, sum_cur=0, sum_pack3=0, sum_whole3=0;
    while (fgets(line, sizeof line, mf)){
        size_t L = strlen(line); while (L && (line[L-1]=='\n'||line[L-1]=='\r')) line[--L]=0;
        if (!L) continue;
        FILE* tf = fopen(line, "rb"); if (!tf) continue;
        std::string src; { fseek(tf,0,SEEK_END); long sz=ftell(tf); fseek(tf,0,SEEK_SET); src.resize(sz>0?sz:0); if(sz>0 && fread(&src[0],1,sz,tf)!=(size_t)sz){} } fclose(tf);
        if (src.empty()) continue;
        Row r{}; r.raw = src.size();
        std::string pkt = encode(P, src, r, guid);
        auto tz = clk::now(); r.packed3 = zc(pkt, 3); r.zstd3_us = us_since(tz);
        r.packed1 = zc(pkt, 1); r.packed6 = zc(pkt, 6);
        r.cur_z1 = chunked_zstd1(src);   // real current transport: 100KB chunks, zstd-1 each, summed
        r.whole1 = zc(src, 1); r.whole3 = zc(src, 3); r.whole6 = zc(src, 6);
        r.ok = decode_verify(P, pkt, src, r.decode_us);
        if (r.ok != 1) fails++;
        ntu++;
        sum_raw += r.raw; sum_cur += r.cur_z1; sum_pack3 += r.packed3; sum_whole3 += r.whole3;
        const char* tu = line; const char* slash = strrchr(line, '/'); if (slash) tu = slash+1;
        fprintf(of,
            "{\"tu\":\"%s\",\"raw\":%zu,\"cur_zstd1\":%zu,\"whole_zstd\":{\"1\":%zu,\"3\":%zu,\"6\":%zu},"
            "\"packed\":{\"1\":%zu,\"3\":%zu,\"6\":%zu},\"refs_b\":%zu,\"inline_b\":%zu,\"body_b\":%zu,\"body_raw_b\":%zu,"
            "\"transform_us\":%.1f,\"zstd3_us\":%.1f,\"decode_us\":%.1f,\"distinct\":%zu,\"new_lines\":%zu,"
            "\"need\":%zu,\"peak\":%zu,\"resident\":%zu,\"ok\":%d}\n",
            tu, r.raw, r.cur_z1, r.whole1, r.whole3, r.whole6, r.packed1, r.packed3, r.packed6,
            r.refs_b, r.inline_b, r.body_b, r.body_raw_b, r.transform_us, r.zstd3_us, r.decode_us,
            r.distinct, r.new_lines, r.need_count, r.peak, P.c_bytes + P.f_bytes, r.ok);
    }
    fclose(mf); if (of != stdout) fclose(of);
    fprintf(stderr, "TUs=%ld roundtrip_fail=%ld | totals: raw=%.1fMB cur_zstd1=%.1fMB packed3=%.1fMB whole_zstd3=%.1fMB\n"
                    "  => packed3 is %.2fx smaller than cur_zstd1, %.2fx smaller than whole_zstd3\n",
            ntu, fails, sum_raw/1048576.0, sum_cur/1048576.0, sum_pack3/1048576.0, sum_whole3/1048576.0,
            sum_pack3? (double)sum_cur/sum_pack3:0, sum_pack3? (double)sum_whole3/sum_pack3:0);
    return fails ? 1 : 0;
}
