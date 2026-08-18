// zstd-3-per-TU: the naive no-cross-TU-memory baseline.
// Each TU's raw bytes compressed independently at level 3; cumulative wire per TU.
// Parallel across TUs, emitted in manifest order.  Self-checks that the per-TU sum
// equals the independently accumulated total.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdlib>

extern "C" {
size_t ZSTD_compress(void *, size_t, const void *, size_t, int);
size_t ZSTD_compressBound(size_t);
unsigned ZSTD_isError(size_t);
}

int main(int argc, char **argv) {
    const char *manifest = nullptr, *out = nullptr, *wire = nullptr;
    int level = 3, threads = int(std::thread::hardware_concurrency());
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--manifest") && i + 1 < argc) manifest = argv[++i];
        else if (!strcmp(argv[i], "--curve") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--level") && i + 1 < argc) level = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-j") && i + 1 < argc) threads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--wire") && i + 1 < argc) wire = argv[++i];
    }
    if (!manifest || !out) {
        fprintf(stderr, "usage: zstd3tu --manifest F --curve OUT [--level L] [-j N]\n");
        return 2;
    }
    std::vector<std::string> paths;
    { std::ifstream in(manifest); std::string line;
      while (std::getline(in, line)) if (!line.empty()) paths.push_back(line); }
    if (paths.empty()) { fprintf(stderr, "empty manifest\n"); return 2; }

    std::vector<uint64_t> raw(paths.size()), wsz(paths.size());
    std::vector<std::vector<char>> frame(wire ? paths.size() : 0);
    std::vector<std::thread> pool;
    if (threads < 1) threads = 1;
    for (int t = 0; t < threads; ++t)
        pool.emplace_back([&, t] {
            std::vector<char> buf, comp;
            for (size_t i = t; i < paths.size(); i += threads) {
                struct stat st {};
                if (stat(paths[i].c_str(), &st) != 0) { fprintf(stderr, "stat: %s\n", paths[i].c_str()); _exit(2); }
                buf.resize(size_t(st.st_size));
                FILE *f = fopen(paths[i].c_str(), "rb");
                if (!f || (st.st_size && fread(buf.data(), 1, buf.size(), f) != buf.size())) {
                    fprintf(stderr, "read: %s\n", paths[i].c_str()); _exit(2); }
                fclose(f);
                size_t cap = ZSTD_compressBound(buf.size());
                if (comp.size() < cap) comp.resize(cap);
                size_t n = ZSTD_compress(comp.data(), cap, buf.data(), buf.size(), level);
                if (ZSTD_isError(n)) { fprintf(stderr, "zstd error\n"); _exit(2); }
                raw[i] = buf.size();
                wsz[i] = n;
                if (wire) frame[i].assign(comp.begin(), comp.begin() + n);
            }
        });
    for (auto &th : pool) th.join();

    FILE *o = fopen(out, "w");
    if (!o) { fprintf(stderr, "open %s\n", out); return 2; }
    fprintf(o, "tu\traw\twire\tcumulative_raw\tcumulative_wire\n");
    uint64_t cr = 0, cw = 0;
    for (size_t i = 0; i < paths.size(); ++i) {
        cr += raw[i]; cw += wsz[i];
        fprintf(o, "%zu\t%llu\t%llu\t%llu\t%llu\n", i,
                (unsigned long long)raw[i], (unsigned long long)wsz[i],
                (unsigned long long)cr, (unsigned long long)cw);
    }
    if (fclose(o) != 0) { fprintf(stderr, "write failed\n"); return 2; }
    // independent totals, so the cumulative column is checked rather than assumed
    if (wire) {   // concatenated per-TU frames, length-prefixed: a real byte wire
        FILE *wf = fopen(wire, "wb");
        if (!wf) { fprintf(stderr, "open %s\n", wire); return 2; }
        for (size_t i = 0; i < paths.size(); ++i) {
            uint32_t L = (uint32_t)frame[i].size();
            if (fwrite(&L, 4, 1, wf) != 1 ||
                (L && fwrite(frame[i].data(), 1, L, wf) != L)) { fprintf(stderr, "wire write\n"); return 2; }
        }
        if (fclose(wf) != 0) { fprintf(stderr, "wire close\n"); return 2; }
    }
    uint64_t tr = 0, tw = 0;
    for (size_t i = 0; i < paths.size(); ++i) { tr += raw[i]; tw += wsz[i]; }
    fprintf(stderr, "ZSTD3TU tus=%zu raw=%llu wire=%llu ratio=%.2f sum_ok=%d level=%d\n",
            paths.size(), (unsigned long long)tr, (unsigned long long)tw,
            tw ? double(tr) / double(tw) : 0.0, (tr == cr && tw == cw) ? 1 : 0, level);
    return (tr == cr && tw == cw) ? 0 : 1;
}
