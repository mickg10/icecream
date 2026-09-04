/* -*- mode: C++; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 99; -*- */
/* vim: set ts=4 sw=4 et tw=99:  */
/*
  This file is part of Icecream.

  Copyright (c) 2004-2014 Stephan Kulow <coolo@suse.de>

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License along
  with this program; if not, write to the Free Software Foundation, Inc.,
  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#ifndef ICECREAM_COMPILE_JOB_H
#define ICECREAM_COMPILE_JOB_H

#include <array>
#include <list>
#include <cstdint>
#include <string>
#include <iostream>
#include <sstream>

typedef enum {
    Arg_Local,  // Local-only args.
    Arg_Remote, // Remote-only args.
    Arg_Rest    // Args to use both locally and remotely.
} Argument_Type;

class ArgumentsList : public std::list<std::pair<std::string, Argument_Type> >
{
public:
    void append(std::string s, Argument_Type t) {
        push_back(make_pair(s, t));
    }
};

/* Protocol-50 compiler-input selector carried by CompileFileMsg.  This
   deliberately mirrors only the immutable identity needed to attach an
   already-committed InputRecord; the cache endpoint types remain outside the
   historical services library. Registry values are scoped to CacheWire
   revision 1: P29V1=1, ZSTD_TU=2, and ZSTD_ROUTE=3. A legacy compile has the
   one canonical all-zero representation. */
struct CompileInputIdentity
{
    static constexpr uint32_t P29V1Profile = 1;
    static constexpr uint32_t ZstdTuProfile = 2;
    static constexpr uint32_t ZstdRouteProfile = 3;

    uint32_t profile = 0;
    std::array<uint8_t, 16> c_store_guid{};
    uint64_t tu_seq = 0;
    uint64_t raw_bytes = 0;
    std::array<uint8_t, 16> raw_digest{};
    uint64_t attempt_id = 0;
    uint64_t request_id = 0;

    bool whollyAbsent() const
    {
        const std::array<uint8_t, 16> zero{};
        return profile == 0 && c_store_guid == zero && tu_seq == 0
            && raw_bytes == 0 && raw_digest == zero && attempt_id == 0
            && request_id == 0;
    }

    bool validPresent() const
    {
        const std::array<uint8_t, 16> zero{};
        /* TU_SEQ zero and an all-zero content digest are valid values.  The
           namespace GUID and the two replay/ownership identities reserve
           zero, so presence cannot be confused with the legacy encoding. */
        return (profile == P29V1Profile || profile == ZstdTuProfile ||
                profile == ZstdRouteProfile) && c_store_guid != zero
            && attempt_id != 0 && request_id != 0;
    }
};

/* Protocol-50 CompileFile wire-audit delta (owner three-bucket ruling).

   BOUND: the pre-existing assignment epoch/nonce/wire-id remains the ordinary
   assignment authority.  For a nonzero selector, profile is restricted to a
   revision-scoped compiler-input registry value (P29V1, ZSTD_TU, or
   ZSTD_ROUTE);
   C_STORE_GUID, TU_SEQ, raw length/digest, ATTEMPT_ID, and REQUEST_ID are
   serialized and recovered byte-exact, and the selector is admitted only
   alongside a complete nonzero assignment identity.

   WIRE-PLACEHOLDER / DERIVED-GUARD: the mandatory seventeen-word all-zero
   selector is the sole legacy-input encoding on a P50 CompileFile frame.
   Exact-tail length, wholly-absent-or-valid-present shape, and pre-P50 erasure
   refusal are decoder/sender guards; they do not claim an attached record.

   CURRENTLY MODEL-UNREPRESENTED: this foundational wire slice does not yet
   claim that the named InputRecord was committed, attached, or consumed by a
   compiler.  Those product transitions become representable only in the M3
   attachment convergence and must bind these fields to Level-2 trace actions
   in that same candidate.  p50assignment retains the P43/P48/P49 byte
   fixtures and covers the exact P50 present/absent fixture delta plus malformed
   short, long, partial, and zero-required-identity mutations. */

class CompileJob
{
public:
    typedef enum {
        Lang_C,
        Lang_CXX,
        Lang_OBJC,
        Lang_OBJCXX,
        Lang_Custom
    } Language;

    typedef enum {
        Flag_None = 0,
        Flag_g = 0x1,
        Flag_g3 = 0x2,
        Flag_O = 0x4,
        Flag_O2 = 0x8,
        Flag_Ol2 = 0x10
    } Flag;

    CompileJob()
        : m_id(0)
        , m_assignment_epoch(0)
        , m_assignment_nonce(0)
        , m_c_guid(0)
        , m_tu_seq(0)
        , m_dwarf_fission(false)
        , m_block_rewrite_includes(false)
    {
        setTargetPlatform();
    }

    void setCompilerName(const std::string &name)
    {
        m_compiler_name = name;
    }

    std::string compilerName() const
    {
        return m_compiler_name;
    }

    void setLanguage(Language lg)
    {
        m_language = lg;
    }

    Language language() const
    {
        return m_language;
    }

    // Not used remotely.
    void setCompilerPathname(const std::string& pathname)
    {
        m_compiler_pathname = pathname;
    }

    // Not used remotely.
    // Use find_compiler(), as this may be empty.
    std::string compilerPathname() const
    {
        return m_compiler_pathname;
    }

    void setEnvironmentVersion(const std::string &ver)
    {
        m_environment_version = ver;
    }

    std::string environmentVersion() const
    {
        return m_environment_version;
    }

    unsigned int argumentFlags() const;

    void setFlags(const ArgumentsList &flags)
    {
        m_flags = flags;
    }
    std::list<std::string> localFlags() const;
    std::list<std::string> remoteFlags() const;
    std::list<std::string> restFlags() const;
    std::list<std::string> nonLocalFlags() const;
    std::list<std::string> allFlags() const;

    void setInputFile(const std::string &file)
    {
        m_input_file = file;
    }

    std::string inputFile() const
    {
        return m_input_file;
    }

    void setOutputFile(const std::string &file)
    {
        m_output_file = file;
    }

    std::string outputFile() const
    {
        return m_output_file;
    }

    // Since protocol 41 this is just a shortcut saying that allFlags() contains "-gsplit-dwarf".
    void setDwarfFissionEnabled(bool flag)
    {
        m_dwarf_fission = flag;
    }

    bool dwarfFissionEnabled() const
    {
        return m_dwarf_fission;
    }

    void setWorkingDirectory(const std::string& dir)
    {
        m_working_directory = dir;
    }

    std::string workingDirectory() const
    {
        return m_working_directory;
    }

    void setJobID(unsigned int id)
    {
        m_id = id;
    }

    unsigned int jobID() const
    {
        return m_id;
    }

    void setAssignmentIdentity(uint64_t epoch, uint64_t nonce)
    {
        m_assignment_epoch = epoch;
        m_assignment_nonce = nonce;
    }

    uint64_t assignmentEpoch() const
    {
        return m_assignment_epoch;
    }

    uint64_t assignmentNonce() const
    {
        return m_assignment_nonce;
    }

    bool hasAssignmentIdentity() const
    {
        return m_assignment_epoch != 0 && m_assignment_nonce != 0;
    }

    bool assignmentIdentityValid() const
    {
        const bool absent = m_assignment_epoch == 0 && m_assignment_nonce == 0;
        const bool complete = m_assignment_epoch != 0 && m_assignment_nonce != 0
            && m_id != 0;
        return absent || complete;
    }

    void setCompileInputIdentity(const CompileInputIdentity &identity)
    {
        m_compile_input = identity;
    }

    void clearCompileInputIdentity()
    {
        m_compile_input = CompileInputIdentity{};
    }

    const CompileInputIdentity &compileInputIdentity() const
    {
        return m_compile_input;
    }

    bool usesP50Input() const
    {
        return m_compile_input.validPresent();
    }

    bool compileInputIdentityValid() const
    {
        return m_compile_input.whollyAbsent()
            || (m_compile_input.validPresent() && hasAssignmentIdentity()
                && m_id != 0);
    }

    /* C_GUID names the scheduler incarnation.  TU_SEQ is zero-based and
       monotonic within that incarnation, so zero is the first valid value. */
    void setCompileIdentity(uint64_t c_guid, uint64_t tu_seq)
    {
        m_c_guid = c_guid;
        m_tu_seq = tu_seq;
    }

    uint64_t cGuid() const { return m_c_guid; }
    uint64_t tuSeq() const { return m_tu_seq; }
    bool hasCompileIdentity() const { return m_c_guid != 0; }
    bool compileIdentityValid() const
    {
        return m_c_guid != 0 || m_tu_seq == 0;
    }

    void appendFlag(std::string arg, Argument_Type argumentType)
    {
        m_flags.append(arg, argumentType);
    }

    std::string targetPlatform() const
    {
        return m_target_platform;
    }

    void setTargetPlatform(const std::string &_target)
    {
        m_target_platform = _target;
    }

    // Not used remotely.
    void setBlockRewriteIncludes(bool flag)
    {
        m_block_rewrite_includes = flag;
    }

    // Not used remotely.
    bool blockRewriteIncludes() const
    {
        return m_block_rewrite_includes;
    }

private:
    std::list<std::string> flags(Argument_Type argumentType) const;
    void setTargetPlatform();

    unsigned int m_id;
    uint64_t m_assignment_epoch;
    uint64_t m_assignment_nonce;
    uint64_t m_c_guid;
    uint64_t m_tu_seq;
    CompileInputIdentity m_compile_input;
    Language m_language;
    std::string m_compiler_pathname;
    std::string m_compiler_name;
    std::string m_environment_version;
    ArgumentsList m_flags;
    std::string m_input_file, m_output_file;
    std::string m_working_directory;
    std::string m_target_platform;
    bool m_dwarf_fission;
    bool m_block_rewrite_includes;
};

inline void appendList(std::list<std::string> &list, const std::list<std::string> &toadd)
{
    // Cannot splice since toadd is a reference-to-const
    list.insert(list.end(), toadd.begin(), toadd.end());
}

inline std::ostream &operator<<( std::ostream &output,
                                 const CompileJob::Language &l )
{
    switch (l) {
    case CompileJob::Lang_CXX:
        output << "C++";
        break;
    case CompileJob::Lang_C:
        output << "C";
        break;
    case CompileJob::Lang_Custom:
        output << "<custom>";
        break;
    case CompileJob::Lang_OBJC:
        output << "ObjC";
        break;
    case CompileJob::Lang_OBJCXX:
        output << "ObjC++";
        break;
    }
    return output;
}

inline std::string concat_args(const std::list<std::string> &args)
{
    std::stringstream str;
    str << "'";

    for (std::list<std::string>::const_iterator it = args.begin(); it != args.end();) {
        str << *it++;
        if (it != args.end())
            str << ", ";
    }
    return str.str() + "'";
}

#endif
