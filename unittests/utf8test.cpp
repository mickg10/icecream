/* Boundary tests for utf8_scalar_sequence_length(): every class the
   divergence review listed as accepted-but-invalid under the old
   shape-only check, plus the valid boundaries around each of them.  */
#include "utf8.h"

#include <cstdio>
#include <cstring>

static int failures = 0;

static void check(const char *bytes, size_t n, size_t expect, const char *what)
{
    const size_t got = utf8_scalar_sequence_length(
        reinterpret_cast<const unsigned char *>(bytes), n);
    if (got == expect) {
        fprintf(stderr, "ok       - %s\n", what);
    } else {
        fprintf(stderr, "FAILED   - %s (got %zu, want %zu)\n", what, got, expect);
        ++failures;
    }
}

int main()
{
    /* valid boundaries */
    check("A", 1, 1, "ASCII");
    check("\x7f", 1, 1, "U+007F");
    check("\xc2\x80", 2, 2, "U+0080 (smallest 2-byte)");
    check("\xdf\xbf", 2, 2, "U+07FF (largest 2-byte)");
    check("\xe0\xa0\x80", 3, 3, "U+0800 (smallest 3-byte)");
    check("\xed\x9f\xbf", 3, 3, "U+D7FF (last before surrogates)");
    check("\xee\x80\x80", 3, 3, "U+E000 (first after surrogates)");
    check("\xef\xbf\xbd", 3, 3, "U+FFFD");
    check("\xf0\x90\x80\x80", 4, 4, "U+10000 (smallest 4-byte)");
    check("\xf4\x8f\xbf\xbf", 4, 4, "U+10FFFF (largest scalar)");

    /* overlong encodings */
    check("\xc0\xaf", 2, 0, "C0 AF (overlong '/')");
    check("\xc1\xbf", 2, 0, "C1 BF (overlong)");
    check("\xe0\x9f\xbf", 3, 0, "E0 9F BF (overlong 3-byte)");
    check("\xf0\x8f\xbf\xbf", 4, 0, "F0 8F BF BF (overlong 4-byte)");

    /* surrogates */
    check("\xed\xa0\x80", 3, 0, "ED A0 80 (U+D800, first surrogate)");
    check("\xed\xbf\xbf", 3, 0, "ED BF BF (U+DFFF, last surrogate)");

    /* above U+10FFFF */
    check("\xf4\x90\x80\x80", 4, 0, "F4 90 80 80 (U+110000)");
    check("\xf5\x80\x80\x80", 4, 0, "F5 lead");
    check("\xf7\xbf\xbf\xbf", 4, 0, "F7 lead");

    /* invalid leads and shapes */
    check("\x80", 1, 0, "stray continuation byte");
    check("\xbf", 1, 0, "stray continuation byte BF");
    check("\xfe", 1, 0, "FE lead");
    check("\xff", 1, 0, "FF lead");
    check("\xc2\x41", 2, 0, "2-byte with non-continuation tail");
    check("\xe2\x82\x41", 3, 0, "3-byte with non-continuation tail");

    /* truncation */
    check("\xc2", 1, 0, "truncated 2-byte");
    check("\xe2\x82", 2, 0, "truncated 3-byte");
    check("\xf0\x90\x80", 3, 0, "truncated 4-byte");

    if (failures) {
        fprintf(stderr, "RESULT: FAIL (%d)\n", failures);
        return 1;
    }
    fprintf(stderr, "RESULT: PASS\n");
    return 0;
}
