/* -*- mode: C++; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 99; -*- */
/* vim: set ts=4 sw=4 et tw=99:  */
/*
    This file is part of Icecream.

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

#include "legacyannouncement.h"

#include <algorithm>
#include <cstring>

namespace LegacySchedulerAnnouncement
{

std::vector<char> encode(unsigned char protocol, const char *netname, time_t starttime)
{
    const char * const safe_name = netname ? netname : "";
    const std::size_t name_bytes = std::min(std::strlen(safe_name), MaxNetnameBytes);

#ifdef ICECC_TEST_LEGACY_ANNOUNCEMENT_MUTANT_OFF_BY_ONE
    /* Exact historical semantic defect, kept bounded for deterministic
       negative control: strlen was serialized without the trailing NUL and
       only length-1 name bytes were copied. */
    const std::size_t wire_length = name_bytes;
    const std::size_t packet_length = PayloadOffset + std::max<std::size_t>(wire_length, 1);
#else
    const std::size_t wire_length = name_bytes + 1;
    const std::size_t packet_length = PayloadOffset + wire_length;
#endif

    std::vector<char> packet(packet_length, 0);
    packet[0] = 'I';
    packet[1] = 'C';
    packet[2] = 'E';
    packet[3] = static_cast<char>(protocol);

    const uint64_t encoded_time = static_cast<uint64_t>(starttime);
    std::memcpy(packet.data() + 4, &encoded_time, sizeof(encoded_time));
    packet[NetnameLengthOffset] = static_cast<char>(wire_length);

#ifdef ICECC_TEST_LEGACY_ANNOUNCEMENT_MUTANT_OFF_BY_ONE
    if (name_bytes > 1) {
        std::memcpy(packet.data() + PayloadOffset, safe_name, name_bytes - 1);
    }
#else
    if (name_bytes) {
        std::memcpy(packet.data() + PayloadOffset, safe_name, name_bytes);
    }
#endif
    packet.back() = '\0';
    return packet;
}

}
