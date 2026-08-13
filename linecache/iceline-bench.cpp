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
#include <zstd.h>

using clk = std::chrono::steady_clock;
static double us_since(clk::time_point t0){
    return std::chrono::duration<double, std::micro>(clk::now() - t0).count();
}

// ---- atomization: each atom ends at (and includes) its '\n'; last may not ----
static std::vector<std::string> atomize(const std::string& s){
    std::vector<std::string> out;
    size_t i = 0, n = s.size();
    while (i < n){
        size_t j = s.find('\n', i);
        if (j == std::string::npos){ out.emplace_back(s.substr(i)); break; }
        out.emplace_back(s.substr(i, j - i + 1));
        i = j + 1;
    }
    return out;
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

// ---- FNV-1a 64 digest over the raw source ----
static uint64_t fnv1a(const std::string& s){
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : s){ h ^= c; h *= 1099511628211ULL; }
    return h;
}

static size_t zc(const std::string& in, int level){
    size_t bound = ZSTD_compressBound(in.size());
    std::string out(bound, '\0');
    size_t r = ZSTD_compress(&out[0], bound, in.data(), in.size(), level);
    return ZSTD_isError(r) ? 0 : r;
}

// ---- the warm C->F pair (in-process) ----
struct Pair {
    std::unordered_map<std::string, uint64_t> c_key;   // atom -> key (C dict)
    uint64_t next_key = 1;                              // 0 = inline sentinel
    std::unordered_map<uint64_t, std::string> f_mirror; // key -> atom (F cache)
    size_t c_bytes = 0, f_bytes = 0;                    // resident content bytes
};

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
    std::vector<std::string> atoms = atomize(src);
    std::unordered_map<std::string, uint32_t> local;   // atom -> local id
    std::vector<uint32_t> body;                        body.reserve(atoms.size());
    std::vector<uint64_t> mapper;                      // local id -> key (first-appearance order)
    std::vector<uint32_t> new_ids;                     // local ids new to C this TU (INLINE)
    for (auto& a : atoms){
        auto it = local.find(a);
        uint32_t lid;
        if (it == local.end()){
            lid = uint32_t(local.size());
            local.emplace(a, lid);
            auto ck = P.c_key.find(a);
            uint64_t key;
            if (ck == P.c_key.end()){                  // new to C -> assign key, INLINE
                key = P.next_key++;
                P.c_key.emplace(a, key);
                P.c_bytes += a.size();
                new_ids.push_back(lid);
            } else {
                key = ck->second;                      // known -> REF
            }
            mapper.push_back(key);
        } else {
            lid = it->second;
        }
        body.push_back(lid);
    }
    // resolve new_atom pointers to the distinct atom bytes (by local id)
    // build local id -> atom bytes
    std::vector<const std::string*> id_atom(local.size(), nullptr);
    for (auto& kv : local) id_atom[kv.second] = &kv.first;
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
    for (uint32_t lid : new_ids){ const std::string* a = id_atom[lid]; uv(in, lid); uv(in, a->size()); in += *a; }
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
    for (uint32_t lid : new_ids){ const std::string* a = id_atom[lid]; uint64_t k = P.c_key[*a]; P.f_mirror[k] = *a; P.f_bytes += a->size(); }
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
        r.cur_z1 = zc(src, 1);
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
