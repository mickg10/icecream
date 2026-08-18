// Receiver for the zstd-3-per-TU wire.  Consumes ONLY the wire artifact: parses the
// length-delimited frames, decompresses each, and reconstructs every TU, checking each
// against the manifest.  Reports physical bytes as payload + the 4-byte length prefix,
// which is what actually crosses the wire -- the encoder's curve counted payload only.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>
#include <sys/stat.h>

extern "C" {
size_t ZSTD_decompress(void *, size_t, const void *, size_t);
unsigned long long ZSTD_getFrameContentSize(const void *, size_t);
unsigned ZSTD_isError(size_t);
}

int main(int argc, char **argv) {
    const char *wire = nullptr, *manifest = nullptr;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--wire") && i + 1 < argc) wire = argv[++i];
        else if (!strcmp(argv[i], "--manifest") && i + 1 < argc) manifest = argv[++i];
    }
    if (!wire || !manifest) { fprintf(stderr, "usage: zstd3recv --wire W --manifest M\n"); return 2; }

    std::vector<std::string> paths;
    { std::ifstream in(manifest); std::string l; while (std::getline(in, l)) if (!l.empty()) paths.push_back(l); }

    FILE *f = fopen(wire, "rb");
    if (!f) { fprintf(stderr, "open wire\n"); return 2; }
    struct stat st{}; stat(wire, &st);
    std::vector<char> w((size_t)st.st_size);
    if (st.st_size && fread(w.data(), 1, w.size(), f) != w.size()) { fprintf(stderr, "read wire\n"); return 2; }
    fclose(f);

    size_t pos = 0, tu = 0;
    uint64_t physical = 0, payload = 0, raw_out = 0;
    std::vector<char> out, orig;
    while (pos + 4 <= w.size()) {
        uint32_t L; memcpy(&L, w.data() + pos, 4); pos += 4;
        if (pos + L > w.size()) { fprintf(stderr, "truncated frame at TU %zu\n", tu); return 1; }
        unsigned long long n = ZSTD_getFrameContentSize(w.data() + pos, L);
        if (n == (unsigned long long)-1 || n == (unsigned long long)-2) { fprintf(stderr, "bad frame TU %zu\n", tu); return 1; }
        out.resize((size_t)n);
        size_t d = ZSTD_decompress(out.data(), out.size(), w.data() + pos, L);
        if (ZSTD_isError(d) || d != n) { fprintf(stderr, "decompress failed TU %zu\n", tu); return 1; }
        if (tu < paths.size()) {                       // reconstruct and verify against source
            FILE *g = fopen(paths[tu].c_str(), "rb");
            if (!g) { fprintf(stderr, "open %s\n", paths[tu].c_str()); return 1; }
            struct stat s2{}; stat(paths[tu].c_str(), &s2);
            orig.resize((size_t)s2.st_size);
            if (s2.st_size && fread(orig.data(), 1, orig.size(), g) != orig.size()) { fprintf(stderr, "read src\n"); return 1; }
            fclose(g);
            if (orig.size() != d || (d && memcmp(orig.data(), out.data(), d) != 0)) {
                fprintf(stderr, "RECONSTRUCT MISMATCH at TU %zu (%s)\n", tu, paths[tu].c_str()); return 1;
            }
        }
        physical += 4 + L; payload += L; raw_out += d;
        pos += L; tu++;
    }
    if (pos != w.size()) { fprintf(stderr, "trailing bytes after last frame\n"); return 1; }
    printf("ZSTD3RECV tus=%zu manifest_tus=%zu physical=%llu payload=%llu framing=%llu raw=%llu "
           "all_reconstructed=%s\n", tu, paths.size(),
           (unsigned long long)physical, (unsigned long long)payload,
           (unsigned long long)(physical - payload), (unsigned long long)raw_out,
           (tu == paths.size()) ? "YES" : "NO");
    return (tu == paths.size()) ? 0 : 1;
}
