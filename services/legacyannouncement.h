/* -*- mode: C++; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 99; -*- */
/* vim: set ts=4 sw=4 et tw=99:  */
/*
    This file is part of Icecream.

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

#ifndef ICECREAM_LEGACYANNOUNCEMENT_H
#define ICECREAM_LEGACYANNOUNCEMENT_H

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <vector>

namespace LegacySchedulerAnnouncement
{

/* The legacy ICE packet stores a one-byte payload length INCLUDING its NUL.
   Therefore at most 254 netname bytes are representable. */
constexpr std::size_t NetnameLengthOffset = 4 + sizeof(uint64_t);
constexpr std::size_t PayloadOffset = NetnameLengthOffset + 1;
constexpr std::size_t MaxNetnameBytes = 254;

/* Encode the pre-protocol-38 scheduler announcement exactly as old receivers
   expect it: native-endian start time, one-byte NUL-inclusive name length,
   and an explicitly terminated (possibly truncated) netname. */
std::vector<char> encode(unsigned char protocol, const char *netname, time_t starttime);

}

#endif
