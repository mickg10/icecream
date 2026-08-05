/* -*- mode: C++; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 99; -*- */
/* vim: set ts=4 sw=4 et tw=99:  */
/*
    This file is part of Icecream.

    UTF-8 scalar-value validation.

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

#ifndef ICECREAM_UTF8_H
#define ICECREAM_UTF8_H

#include <stddef.h>

/* Length of the well-formed UTF-8 sequence starting at p (at most avail
   bytes), or 0 if the bytes do not begin a well-formed sequence.

   "Well-formed" means an encoded Unicode SCALAR VALUE per RFC 3629 /
   Unicode table 3-7, i.e. this rejects everything a lead-byte + trailing
   0b10xxxxxx shape check accepts but strict JSON consumers do not:

     - overlong encodings (C0/C1 leads; E0 followed by 80-9F;
       F0 followed by 80-8F);
     - surrogate code points U+D800-U+DFFF (ED followed by A0-BF);
     - values above U+10FFFF (F4 followed by 90-BF, and F5-FF leads);
     - stray continuation bytes and truncated sequences.  */
static inline size_t utf8_scalar_sequence_length(const unsigned char *p, size_t avail)
{
    if (avail == 0) {
        return 0;
    }
    const unsigned char c = p[0];
    if (c < 0x80) {
        return 1;
    }
    if (c < 0xc2) {
        return 0;       /* continuation byte, or C0/C1 overlong lead */
    }
    if (c < 0xe0) {     /* two bytes: U+0080..U+07FF */
        if (avail < 2 || (p[1] & 0xc0) != 0x80) {
            return 0;
        }
        return 2;
    }
    if (c < 0xf0) {     /* three bytes: U+0800..U+FFFF minus surrogates */
        if (avail < 3 || (p[1] & 0xc0) != 0x80 || (p[2] & 0xc0) != 0x80) {
            return 0;
        }
        if (c == 0xe0 && p[1] < 0xa0) {
            return 0;   /* overlong: would decode below U+0800 */
        }
        if (c == 0xed && p[1] > 0x9f) {
            return 0;   /* surrogate U+D800..U+DFFF */
        }
        return 3;
    }
    if (c < 0xf5) {     /* four bytes: U+10000..U+10FFFF */
        if (avail < 4 || (p[1] & 0xc0) != 0x80 || (p[2] & 0xc0) != 0x80
            || (p[3] & 0xc0) != 0x80) {
            return 0;
        }
        if (c == 0xf0 && p[1] < 0x90) {
            return 0;   /* overlong: would decode below U+10000 */
        }
        if (c == 0xf4 && p[1] > 0x8f) {
            return 0;   /* above U+10FFFF */
        }
        return 4;
    }
    return 0;           /* F5-FF: can only encode above U+10FFFF */
}

#endif
