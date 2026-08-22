/* -*- mode: C++; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 99; -*- */
/* vim: set ts=4 sw=4 et tw=99:  */
/*
    This file is part of Icecream.

    Copyright (c) 2004 Michael Matz <matz@suse.de>
                  2004 Stephan Kulow <coolo@suse.de>

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


#ifndef ICECREAM_COMM_H
#define ICECREAM_COMM_H

#ifdef __linux__
#  include <stdint.h>
#endif
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include "job.h"
#include <deque>
#include <stdint.h>

// if you increase the PROTOCOL_VERSION, add a macro below and use that
#define PROTOCOL_VERSION 50
// if you increase the MIN_PROTOCOL_VERSION, comment out macros below and clean up the code
#define MIN_PROTOCOL_VERSION 21
#define PROTOCOL_VERSION_JOB_TIMING 47
#define PROTOCOL_VERSION_JOB_LOCAL_FLAGS 48
#define PROTOCOL_VERSION_ASSIGNMENT_FENCE 49
#define PROTOCOL_VERSION_ASSIGNMENT_IDENTITY 50

#define MAX_SCHEDULER_PONG 3
// MAX_SCHEDULER_PING must be multiple of MAX_SCHEDULER_PONG
#define MAX_SCHEDULER_PING 12 * MAX_SCHEDULER_PONG
// maximum amount of time in seconds a daemon can be busy installing
#define MAX_BUSY_INSTALLING 120

// comparison for protocol version checks
#define IS_PROTOCOL_VERSION(x, c) ((c)->protocol >= (x))

class MsgChannel;

// Terms used:
// S  = scheduler
// C  = client
// CS = daemon

class Msg {
public:
    enum Value: uint32_t {
        // so far unknown
        UNKNOWN = 'A',

        /* When the scheduler didn't get STATS from a CS
           for a specified time (e.g. 10m), then he sends a
           ping */
        PING,

        /* Either the end of file chunks or connection (A<->A) */
        END,

        TIMEOUT, // unused

        // C --> CS
        GET_NATIVE_ENV,
        // CS -> C
        NATIVE_ENV,

        // C --> S
        GET_CS,
        // S --> C
        USE_CS,  // = 'H'
        // C --> CS
        COMPILE_FILE, // = 'I'
        // generic file transfer
        FILE_CHUNK,
        // CS --> C
        COMPILE_RESULT,

        // CS --> S (after the C got the CS from the S, the CS tells the S when the C asks him)
        JOB_BEGIN,
        JOB_DONE,     // = 'M'

        // C --> CS, CS --> S (forwarded from C), _and_ CS -> C as start ping
        JOB_LOCAL_BEGIN, // = 'N'
        JOB_LOCAL_DONE,

        // CS --> S, first message sent
        LOGIN,
        // CS --> S (periodic)
        STATS,

        // messages between monitor and scheduler
        MON_LOGIN,
        MON_GET_CS,
        MON_JOB_BEGIN, // = 'T'
        MON_JOB_DONE,
        MON_LOCAL_JOB_BEGIN,
        MON_STATS,

        TRANFER_ENV, // = 'X'

        TEXT,
        STATUS_TEXT, // = 'Z'
        GET_INTERNALS,

        // S --> CS, answered by LOGIN
        CS_CONF,

        // C --> CS, after installing an environment
        VERIFY_ENV,
        // CS --> C
        VERIFY_ENV_RESULT,
        // C --> CS, CS --> S (forwarded from C), to not use given host for given environment
        BLACKLIST_HOST_ENV,
        // S --> CS
        NO_CS,
        // C --> CS
        JOB_TIMING,

        // Protocol 49 fork-private block, on the persistent scheduler <->
        // worker link only.  Values are explicit so a future upstream append
        // cannot silently alias this vocabulary.
        // S --> CS: install one assignment before it can be exposed to C.
        ASSIGN_PREPARE = 0x49f00000,
        // CS --> S: the matching assignment is installed.
        ASSIGN_READY = 0x49f00001,
        // S --> CS: withdraw an assignment that has not been claimed.
        REVOKE_BEFORE_START = 0x49f00002,
        // CS --> S: the ordered claim/revoke outcome.
        REVOKE_RESULT = 0x49f00003
    };

    Msg() = default;
    constexpr Msg(Value value)
        : value_{value}
    {}

    constexpr operator Value() const { return value_; }
    explicit operator bool() = delete;

    /* Payload invariants which cannot be expressed by the frame length alone.
       The default keeps all historical message classes unchanged. */
    virtual bool valid_payload() const { return true; }

    std::basic_string<char> to_string() const {
        switch (value_) {
            case UNKNOWN:
                return "UNKNOWN";
            case PING:
                return "PING";
            case END:
                return "END";
            case TIMEOUT:
                return "TIMEOUT";
            case GET_NATIVE_ENV:
                return "GET_NATIVE_ENV";
            case NATIVE_ENV:
                return "NATIVE_ENV";
            case GET_CS:
                return "GET_CS";
            case USE_CS:
                return "USE_CS";
            case COMPILE_FILE:
                return "COMPILE_FILE";
            case FILE_CHUNK:
                return "FILE_CHUNK";
            case COMPILE_RESULT:
                return "COMPILE_RESULT";
            case JOB_BEGIN:
                return "JOB_BEGIN";
            case JOB_DONE:
                return "JOB_DONE";
            case JOB_LOCAL_BEGIN:
                return "JOB_LOCAL_BEGIN";
            case JOB_LOCAL_DONE:
                return "JOB_LOCAL_DONE";
            case LOGIN:
                return "LOGIN";
            case STATS:
                return "STATS";
            case MON_LOGIN:
                return "MON_LOGIN";
            case MON_GET_CS:
                return "MON_GET_CS";
            case MON_JOB_BEGIN:
                return "MON_JOB_BEGIN";
            case MON_JOB_DONE:
                return "MON_JOB_DONE";
            case MON_LOCAL_JOB_BEGIN:
                return "MON_LOCAL_JOB_BEGIN";
            case MON_STATS:
                return "MON_STATS";
            case TRANFER_ENV:
                return "TRANFER_ENV";
            case TEXT:
                return "TEXT";
            case STATUS_TEXT:
                return "STATUS_TEXT";
            case GET_INTERNALS:
                return "GET_INTERNALS";
            case CS_CONF:
                return "CS_CONF";
            case VERIFY_ENV:
                return "VERIFY_ENV";
            case VERIFY_ENV_RESULT:
                return "VERIFY_ENV_RESULT";
            case BLACKLIST_HOST_ENV:
                return "BLACKLIST_HOST_ENV";
            case NO_CS:
                return "NO_CS";
            case JOB_TIMING:
                return "JOB_TIMING";
            case ASSIGN_PREPARE:
                return "ASSIGN_PREPARE";
            case ASSIGN_READY:
                return "ASSIGN_READY";
            case REVOKE_BEFORE_START:
                return "REVOKE_BEFORE_START";
            case REVOKE_RESULT:
                return "REVOKE_RESULT";
        }
        return nullptr;
    }

    virtual ~Msg() {}
    virtual void fill_from_channel(MsgChannel *c);
    virtual void send_to_channel(MsgChannel *c) const;

protected:
    Value value_;
};

enum Compression {
    C_LZO = 0,
    C_ZSTD = 1
};

// The remote node is capable of unpacking environment compressed as .tar.xz .
const int NODE_FEATURE_ENV_XZ = ( 1 << 0 );
// The remote node is capable of unpacking environment compressed as .tar.zst .
const int NODE_FEATURE_ENV_ZSTD = ( 1 << 1 );

// a list of pairs of host platform, filename
typedef std::list<std::pair<std::string, std::string> > Environments;

// CLOCK_MONOTONIC seconds/milliseconds; immune to wall-clock steps.  Used
// for deferred-output deadline accounting (see MsgChannel::deferred_*).
time_t icecream_monotonic_seconds();
uint64_t icecream_monotonic_msec();

// How long undelivered deferred output may wait before its peer is treated
// as dead.  The same budget the historical blocking send granted.
#define ICECC_DEFERRED_SEND_TIMEOUT_MSEC 30000

// MsgChannel supports backpressure-tolerant sends (SendDeferrable,
// has_pending_write(), flush_pending()).
#define ICECC_MSGCHANNEL_HAS_DEFERRED_SEND 1

class MsgChannel
{
public:
    enum SendFlags {
        SendBlocking = 1 << 0,
        SendNonBlocking = 1 << 1,
        SendBulkOnly = 1 << 2,
        // Tolerate backpressure instead of failing the channel: if the peer's
        // receive buffer is full (EAGAIN, or the poll timeout expires when
        // combined with SendBlocking), keep the unsent bytes queued in the
        // write buffer and report success.  The caller must later call
        // flush_pending() when the socket becomes writable again (e.g. from a
        // POLLOUT event; see has_pending_write()).  Queued bytes are flushed
        // in order, so the byte stream stays intact even if a message was
        // partially transmitted when the buffer filled up.
        SendDeferrable = 1 << 3
    };

    virtual ~MsgChannel();

    void setBulkTransfer();

    std::string dump() const;
    // NULL  <--> channel closed or timeout
    // Will warn in log if EOF and !eofAllowed.
    Msg *get_msg(int timeout = 10, bool eofAllowed = false);

    // false <--> error (msg not send)
    bool send_msg(const Msg &, int SendFlags = SendBlocking);

    // True if a previous send left bytes queued in the write buffer (a
    // SendDeferrable send that ran into backpressure, or a message so far only
    // collected by SendBulkOnly).
    bool has_pending_write(void) const
    {
        return msgtogo > 0;
    }

    // Bytes currently queued for the peer (deferred + bulk-collected).
    size_t pending_bytes(void) const
    {
        return msgtogo;
    }

    // Message-boundary delivery tracking.  framesQueued() advances once per
    // fully composed frame; framesFlushed() advances once the LAST byte of
    // that frame has left the send buffer.  "send_msg returned true" means
    // queued, not delivered -- callers that need delivery (e.g. a request
    // whose reply must not be matched before the request was even on the
    // wire) compare their recorded framesQueued() against framesFlushed().
    uint64_t framesQueued(void) const
    {
        return frames_queued_seq;
    }
    uint64_t framesFlushed(void) const
    {
        return frames_queued_seq - pending_frame_ends.size();
    }

    // Test-only (armed by the daemon under ICECC_TEST_USECS_CUT_AT): force the
    // next flush to send exactly `n` more bytes and then fail as if the peer
    // vanished with the frame incomplete.  This makes the four exact UseCS
    // injection positions -- before byte 1 (n == 0), partial header, partial
    // body, final-byte-short -- deterministically reproducible without a
    // sleep, for the prerequisite usecs-cut-* witnesses.  One-shot and a pure
    // no-op unless explicitly armed; it never runs on the production path.
    void testCutNextFlushAfter(size_t n)
    {
        test_cut_bytes = n;
        test_cut_armed = true;
    }

    // True while a deferrable send has left output undelivered.  An explicit
    // flag rather than a timestamp test: an age of zero is ambiguous during
    // the first second of a backlog, and owners must distinguish "not armed"
    // from "armed, just now".
    bool deferred_output_armed(void) const
    {
        return pending_write_armed;
    }

    // Absolute CLOCK_MONOTONIC millisecond deadline for the current backlog
    // (meaningful only while deferred_output_armed()).  Owners enforce it:
    // the kernel TCP_USER_TIMEOUT bound is #ifdef'd and SO_KEEPALIVE does
    // not cover a peer whose TCP keeps ACKing while the process never reads.
    uint64_t deferred_output_deadline_msec(void) const
    {
        return pending_write_deadline_msec;
    }

    // Try to write queued output without blocking.  A still-full peer buffer
    // just leaves the remaining bytes queued and returns true; false is
    // returned only if the connection hit a real error (the channel is in the
    // error state / at_eof() afterwards).
    bool flush_pending(void);

    bool has_msg(void) const
    {
        return eof || instate == HAS_MSG;
    }

    // Returns ture if there were no errors filling inbuf.
    bool read_a_bit(void);

    bool at_eof(void) const
    {
        return instate != HAS_MSG && eof;
    }

    bool is_text_based(void) const
    {
        return text_based;
    }

    void readcompressed(unsigned char **buf, size_t &_uclen, size_t &_clen);
    void writecompressed(const unsigned char *in_buf,
                         size_t _in_len, size_t &_out_len);
    void write_environments(const Environments &envs);
    void read_environments(Environments &envs);
    void read_line(std::string &line);
    void write_line(const std::string &line);

    bool eq_ip(const MsgChannel &s) const;

    MsgChannel &operator>>(uint32_t &);
    MsgChannel &operator>>(std::string &);
    MsgChannel &operator>>(std::list<std::string> &);

    MsgChannel &operator<<(uint32_t);
    MsgChannel &operator<<(const std::string &);
    MsgChannel &operator<<(const std::list<std::string> &);

    // our filedesc
    int fd;

    // the minimum protocol version between me and him
    int protocol;
    // the actual maximum protocol the remote supports
    int maximum_remote_protocol;

    std::string name;
    time_t last_talk;

protected:
    MsgChannel(int _fd, struct sockaddr *, socklen_t, bool text = false);

    bool wait_for_protocol();
    // returns false if there was an error sending something; send_flags is a
    // combination of SendFlags bits (SendBlocking / SendDeferrable matter here)
    bool flush_writebuf(int send_flags);
    void writefull(const void *_buf, size_t count);
    // returns false if there was an error in the protocol setup
    bool update_state(void);
    void chop_input(void);
    void chop_output(void);
    bool wait_for_msg(int timeout);
    void set_error(bool silent = false);

    char *msgbuf;
    size_t msgbuflen;
    size_t msgofs;
    size_t msgtogo;
    uint64_t total_appended = 0;
    uint64_t total_drained = 0;
    uint64_t frames_queued_seq = 0;
    std::deque<uint64_t> pending_frame_ends;   // append offsets of frame ends
    // test-only one-shot mid-frame cut; see testCutNextFlushAfter()
    size_t test_cut_bytes = 0;
    bool test_cut_armed = false;
    // deferred-output deadline state; see deferred_output_armed()
    bool pending_write_armed;
    uint64_t pending_write_deadline_msec;
    char *inbuf;
    size_t inbuflen;
    size_t inofs;
    size_t intogo;

    enum {
        NEED_PROTO,
        NEED_LEN,
        FILL_BUF,
        HAS_MSG,
        ERROR
    } instate;

    uint32_t inmsglen;
    bool eof;
    bool text_based;

private:
    friend class Service;

    // deep copied
    struct sockaddr *addr;
    socklen_t addr_len;
    bool set_error_recursion;
};

// just convenient functions to create MsgChannels
class Service
{
public:
    static MsgChannel *createChannel(const std::string &host, unsigned short p, int timeout);
    static MsgChannel *createChannel(const std::string &domain_socket);
    static MsgChannel *createChannel(int remote_fd, struct sockaddr *, socklen_t);
};

class Broadcasts
{
public:
    // Broadcasts a message about this scheduler and its information.
    static void broadcastSchedulerVersion(int scheduler_port, const char* netname, time_t starttime);
    // Checks if the data received is a scheduler version broadcast.
    static bool isSchedulerVersion(const char* buf, int buflen);
    // Reads data from a scheduler version broadcast.
    static void getSchedulerVersionData( const char* buf, int* protocol, time_t* time, std::string* netname );
    /// Broadcasts the given data on the given port.
    static const int BROAD_BUFLEN = 268;
private:
    static void broadcastData(int port, const char* buf, int size);
};

// --------------------------------------------------------------------------
// this class is also used by icecream-monitor
class DiscoverSched
{
public:
    /* Connect to a scheduler waiting max. TIMEOUT seconds.
       schedname can be the hostname of a box running a scheduler, to avoid
       broadcasting, port can be specified explicitly */
    DiscoverSched(const std::string &_netname = std::string(),
                  int _timeout = 2,
                  const std::string &_schedname = std::string(),
                  int port = 0);
    ~DiscoverSched();

    bool timed_out();

    int listen_fd() const
    {
        return schedname.empty() ? ask_fd : -1;
    }

    int connect_fd() const
    {
        return schedname.empty() ? -1 : ask_fd;
    }

    // compat for icecream monitor
    int get_fd() const
    {
        return listen_fd();
    }

    /* Attempt to get a conenction to the scheduler.

       Continue to call this while it returns NULL and timed_out()
       returns false. If this returns NULL you should wait for either
       more data on listen_fd() (use select), or a timeout of your own.
       */
    MsgChannel *try_get_scheduler();

    // Returns the hostname of the scheduler - set by constructor or by try_get_scheduler
    std::string schedulerName() const
    {
        return schedname;
    }

    // Returns the network name of the scheduler - set by constructor or by try_get_scheduler
    std::string networkName() const
    {
        return netname;
    }

    /* Return a list of all reachable netnames.  We wait max. WAITTIME
       milliseconds for answers.  */
    static std::list<std::string> getNetnames(int waittime = 2000, int port = 8765);

    // Checks if the data is from a scheduler discovery broadcast, returns version of the sending
    // daemon is yes.
    static bool isSchedulerDiscovery(const char* buf, int buflen, int* daemon_version);
    // Prepares data for sending a reply to a scheduler discovery broadcast.
    static int prepareBroadcastReply(char* buf, const char* netname, time_t starttime);

private:
    struct sockaddr_in remote_addr;
    std::string netname;
    std::string schedname;
    int timeout;
    int ask_fd;
    int ask_second_fd; // for debugging
    time_t time0;
    unsigned int sport;
    int best_version;
    time_t best_start_time;
    std::string best_schedname;
    int best_port;
    bool multiple;

    void attempt_scheduler_connect();
    void sendSchedulerDiscovery( int version );
    static bool get_broad_answer(int ask_fd, int timeout, char *buf2, struct sockaddr_in *remote_addr,
                 socklen_t *remote_len);
    static void get_broad_data(const char* buf, const char** name, int* version, time_t* start_time);
};
// --------------------------------------------------------------------------

/* Return a list of all reachable netnames.  We wait max. WAITTIME
   milliseconds for answers.  */
std::list<std::string> get_netnames(int waittime = 2000, int port = 8765);

class PingMsg : public Msg
{
public:
    PingMsg()
        : Msg(Msg::PING) {}
};

class EndMsg : public Msg
{
public:
    EndMsg()
        : Msg(Msg::END) {}
};

class GetCSMsg : public Msg
{
public:
    GetCSMsg()
        : Msg(Msg::GET_CS)
        , count(1)
        , arg_flags(0)
        , client_id(0)
        , client_count(0)
        , niceness(0)
        {}

    GetCSMsg(const Environments &envs, const std::string &f,
             CompileJob::Language _lang, unsigned int _count,
             std::string _target, unsigned int _arg_flags,
             const std::string &host, int _minimal_host_version,
             unsigned int _required_features,
             int _niceness,
             unsigned int _client_count = 0,
             const std::string &_command_summary = "");

    virtual void fill_from_channel(MsgChannel *c);
    virtual void send_to_channel(MsgChannel *c) const;

    Environments versions;
    std::string filename;
    CompileJob::Language lang;
    uint32_t count; // the number of UseCS messages to answer with - usually 1
    std::string target;
    uint32_t arg_flags;
    uint32_t client_id;
    std::string preferred_host;
    int minimal_host_version;
    uint32_t required_features;
    uint32_t client_count; // number of CS -> C connections at the moment
    uint32_t niceness; // nice priority (0-20)
    std::string command_summary;
};

class UseCSMsg : public Msg
{
public:
    UseCSMsg()
        : Msg(Msg::USE_CS)
        , job_id(0)
        , port(0)
        , got_env(0)
        , client_id(0)
        , matched_job_id(0)
        , assignment_epoch_hi(0)
        , assignment_epoch_lo(0)
        , assignment_nonce_hi(0)
        , assignment_nonce_lo(0) {}
    UseCSMsg(std::string platform, std::string host, unsigned int p, unsigned int id, bool gotit,
             unsigned int _client_id, unsigned int matched_host_jobs,
             uint64_t assignment_epoch = 0, uint64_t assignment_nonce = 0)
        : Msg(Msg::USE_CS),
          job_id(id),
          hostname(host),
          port(p),
          host_platform(platform),
          got_env(gotit),
          client_id(_client_id),
          matched_job_id(matched_host_jobs),
          assignment_epoch_hi(uint32_t(assignment_epoch >> 32)),
          assignment_epoch_lo(uint32_t(assignment_epoch)),
          assignment_nonce_hi(uint32_t(assignment_nonce >> 32)),
          assignment_nonce_lo(uint32_t(assignment_nonce)) {}

    virtual void fill_from_channel(MsgChannel *c);
    virtual void send_to_channel(MsgChannel *c) const;
    virtual bool valid_payload() const;

    uint64_t assignmentEpoch() const
    {
        return (uint64_t(assignment_epoch_hi) << 32) | assignment_epoch_lo;
    }
    uint64_t assignmentNonce() const
    {
        return (uint64_t(assignment_nonce_hi) << 32) | assignment_nonce_lo;
    }
    bool hasAssignmentIdentity() const
    {
        return assignmentEpoch() != 0 && assignmentNonce() != 0;
    }
    bool applyAssignmentTo(CompileJob *job) const;

    uint32_t job_id;
    std::string hostname;
    uint32_t port;
    std::string host_platform;
    uint32_t got_env;
    uint32_t client_id;
    uint32_t matched_job_id;
    /* Protocol 50 appends exactly these four words.  job_id above remains the
       sole serialized wire id. */
    uint32_t assignment_epoch_hi;
    uint32_t assignment_epoch_lo;
    uint32_t assignment_nonce_hi;
    uint32_t assignment_nonce_lo;
};

class NoCSMsg : public Msg
{
public:
    NoCSMsg()
        : Msg(Msg::NO_CS) {}
    NoCSMsg(unsigned int id, unsigned int _client_id)
        : Msg(Msg::NO_CS),
          job_id(id),
          client_id(_client_id) {}

    virtual void fill_from_channel(MsgChannel *c);
    virtual void send_to_channel(MsgChannel *c) const;

    uint32_t job_id;
    uint32_t client_id;
};

class GetNativeEnvMsg : public Msg
{
public:
    GetNativeEnvMsg()
        : Msg(Msg::GET_NATIVE_ENV) {}

    GetNativeEnvMsg(const std::string &c, const std::list<std::string> &e,
        const std::string &comp)
        : Msg(Msg::GET_NATIVE_ENV)
        , compiler(c)
        , extrafiles(e)
        , compression(comp)
        {}

    virtual void fill_from_channel(MsgChannel *c);
    virtual void send_to_channel(MsgChannel *c) const;

    std::string compiler; // "gcc", "clang" or the actual binary
    std::list<std::string> extrafiles;
    std::string compression; // "" (=default), "none", "gzip", "xz", etc.
};

class UseNativeEnvMsg : public Msg
{
public:
    UseNativeEnvMsg()
        : Msg(Msg::NATIVE_ENV) {}

    UseNativeEnvMsg(std::string _native)
        : Msg(Msg::NATIVE_ENV)
        , nativeVersion(_native) {}

    virtual void fill_from_channel(MsgChannel *c);
    virtual void send_to_channel(MsgChannel *c) const;

    std::string nativeVersion;
};

class CompileFileMsg : public Msg
{
public:
    CompileFileMsg(CompileJob *j, bool delete_job = false)
        : Msg(Msg::COMPILE_FILE)
        , deleteit(delete_job)
        , job(j) {}

    ~CompileFileMsg()
    {
        if (deleteit) {
            delete job;
        }
    }

    virtual void fill_from_channel(MsgChannel *c);
    virtual void send_to_channel(MsgChannel *c) const;
    virtual bool valid_payload() const
    {
        return job != nullptr && job->assignmentIdentityValid();
    }
    CompileJob *takeJob();

private:
    std::string remote_compiler_name() const;

    bool deleteit;
    CompileJob *job;
};

class FileChunkMsg : public Msg
{
public:
    FileChunkMsg(unsigned char *_buffer, size_t _len)
        : Msg(Msg::FILE_CHUNK)
        , buffer(_buffer)
        , len(_len)
        , del_buf(false) {}

    FileChunkMsg()
        : Msg(Msg::FILE_CHUNK)
        , buffer(0)
        , len(0)
        , del_buf(true) {}

    ~FileChunkMsg();

    virtual void fill_from_channel(MsgChannel *c);
    virtual void send_to_channel(MsgChannel *c) const;

    unsigned char *buffer;
    size_t len;
    mutable size_t compressed;
    bool del_buf;

private:
    FileChunkMsg(const FileChunkMsg &);
    FileChunkMsg &operator=(const FileChunkMsg &);
};

class CompileResultMsg : public Msg
{
public:
    CompileResultMsg()
        : Msg(Msg::COMPILE_RESULT)
        , status(0)
        , was_out_of_memory(false)
        , have_dwo_file(false) {}

    virtual void fill_from_channel(MsgChannel *c);
    virtual void send_to_channel(MsgChannel *c) const;

    int status;
    std::string out;
    std::string err;
    bool was_out_of_memory;
    bool have_dwo_file;
};

class JobBeginMsg : public Msg
{
public:
    JobBeginMsg()
        : Msg(Msg::JOB_BEGIN)
        , client_count(0) {}

    JobBeginMsg(unsigned int j, unsigned int _client_count)
        : Msg(Msg::JOB_BEGIN)
        , job_id(j)
        , stime(time(0))
        , client_count(_client_count) {}

    virtual void fill_from_channel(MsgChannel *c);
    virtual void send_to_channel(MsgChannel *c) const;

    uint32_t job_id;
    uint32_t stime;
    uint32_t client_count; // number of CS -> C connections at the moment
};

class JobDoneMsg : public Msg
{
public:
    /* FROM_SERVER: this message was generated by the daemon responsible
          for remotely compiling the job (i.e. job->server).
       FROM_SUBMITTER: this message was generated by the daemon connected
          to the submitting client.  */
    enum from_type {
        FROM_SERVER = 0,
        FROM_SUBMITTER = 1
    };

    // other flags
    enum {
        UnknownJobId = (1 << 1)
    };

    JobDoneMsg(int job_id = 0, int exitcode = -1, unsigned int flags = FROM_SERVER,
               unsigned int _client_count = 0);

    void set_from(from_type from)
    {
        flags |= (uint32_t)from;
    }

    bool is_from_server()
    {
        return (flags & FROM_SUBMITTER) == 0;
    }

    void set_unknown_job_client_id( uint32_t clientId );
    uint32_t unknown_job_client_id() const;
    void set_job_id( uint32_t jobId );

    virtual void fill_from_channel(MsgChannel *c);
    virtual void send_to_channel(MsgChannel *c) const;

    uint32_t real_msec; /* real time it used */
    uint32_t user_msec; /* user time used */
    uint32_t sys_msec; /* system time used */
    uint32_t pfaults; /* page faults */

    int exitcode; /* exit code */

    uint32_t flags;

    uint32_t in_compressed;
    uint32_t in_uncompressed;
    uint32_t out_compressed;
    uint32_t out_uncompressed;

    uint32_t job_id;
    uint32_t client_count; // number of CS -> C connections at the moment
};

class JobLocalBeginMsg : public Msg
{
public:
    enum LocalFlags {
        LocalFlagNone = 0,
        LocalFlagPreprocessOnly = 1 << 0
    };

    JobLocalBeginMsg(int job_id = 0, const std::string &file = "", bool full = false,
                     const std::string &reason = "", const std::string &_cmdline = "",
                     uint32_t _local_flags = LocalFlagNone)
        : Msg(Msg::JOB_LOCAL_BEGIN)
        , outfile(file)
        , stime(time(0))
        , id(job_id)
        , fulljob(full)
        , local_reason(reason)
        , cmdline(_cmdline)
        , local_flags(_local_flags) {}

    virtual void fill_from_channel(MsgChannel *c);
    virtual void send_to_channel(MsgChannel *c) const;

    std::string outfile;
    uint32_t stime;
    uint32_t id;
    bool fulljob;
    std::string local_reason;
    std::string cmdline;
    uint32_t local_flags;
};

class JobLocalDoneMsg : public Msg
{
public:
    JobLocalDoneMsg(unsigned int id = 0)
        : Msg(Msg::JOB_LOCAL_DONE)
        , job_id(id) {}

    virtual void fill_from_channel(MsgChannel *c);
    virtual void send_to_channel(MsgChannel *c) const;

    uint32_t job_id;
};

class JobTimingMsg : public Msg
{
public:
    JobTimingMsg()
        : Msg(Msg::JOB_TIMING)
        , submit_ts(0)
        , enqueue_msec(0)
        , start_msec(0)
        , finish_msec(0)
        , waitforcs_msec(0)
        , local_queue_msec(0)
        , exec_msec(0)
        , scheduler_job_id(0)
        , compile_job_id(0)
        , exitcode(0) {}

    JobTimingMsg(uint32_t _submit_ts, uint32_t _enqueue_msec, uint32_t _start_msec, uint32_t _finish_msec,
                 uint32_t _waitforcs_msec, uint32_t _local_queue_msec, uint32_t _exec_msec,
                 uint32_t _scheduler_job_id, uint32_t _compile_job_id, int _exitcode,
                 const std::string &_mode)
        : Msg(Msg::JOB_TIMING)
        , submit_ts(_submit_ts)
        , enqueue_msec(_enqueue_msec)
        , start_msec(_start_msec)
        , finish_msec(_finish_msec)
        , waitforcs_msec(_waitforcs_msec)
        , local_queue_msec(_local_queue_msec)
        , exec_msec(_exec_msec)
        , scheduler_job_id(_scheduler_job_id)
        , compile_job_id(_compile_job_id)
        , exitcode(_exitcode)
        , mode(_mode) {}

    virtual void fill_from_channel(MsgChannel *c);
    virtual void send_to_channel(MsgChannel *c) const;

    uint32_t submit_ts;
    uint32_t enqueue_msec;
    uint32_t start_msec;
    uint32_t finish_msec;
    uint32_t waitforcs_msec;
    uint32_t local_queue_msec;
    uint32_t exec_msec;
    uint32_t scheduler_job_id;
    uint32_t compile_job_id;
    int exitcode;
    std::string mode;
};

class LoginMsg : public Msg
{
public:
    LoginMsg(unsigned int myport, const std::string &_nodename, const std::string &_host_platform,
             unsigned int my_features);
    LoginMsg()
        : Msg(Msg::LOGIN)
        , port(0) {}

    virtual void fill_from_channel(MsgChannel *c);
    virtual void send_to_channel(MsgChannel *c) const;

    uint32_t port;
    Environments envs;
    uint32_t max_kids;
    bool noremote;
    bool chroot_possible;
    std::string nodename;
    std::string host_platform;
    uint32_t supported_features; // bitmask of various features the node supports
};

class ConfCSMsg : public Msg
{
public:
    enum FenceMode : uint32_t {
        Legacy = 0,
        Advisory = 1,
        EnforcingCompat = 2,
        StrictNonce = 3
    };

    ConfCSMsg()
        : Msg(Msg::CS_CONF)
        , max_scheduler_pong(MAX_SCHEDULER_PONG)
        , max_scheduler_ping(MAX_SCHEDULER_PING)
        , epoch_hi(0)
        , epoch_lo(0)
        , fence_mode(Legacy) {}

    ConfCSMsg(uint64_t epoch, FenceMode mode)
        : Msg(Msg::CS_CONF)
        , max_scheduler_pong(MAX_SCHEDULER_PONG)
        , max_scheduler_ping(MAX_SCHEDULER_PING)
        , epoch_hi(uint32_t(epoch >> 32))
        , epoch_lo(uint32_t(epoch))
        , fence_mode(mode) {}

    virtual void fill_from_channel(MsgChannel *c);
    virtual void send_to_channel(MsgChannel *c) const;

    uint32_t max_scheduler_pong;
    uint32_t max_scheduler_ping;
    uint32_t epoch_hi;
    uint32_t epoch_lo;
    uint32_t fence_mode;

    uint64_t epoch() const
    {
        return (uint64_t(epoch_hi) << 32) | epoch_lo;
    }
};

class AssignPrepareMsg : public Msg
{
public:
    AssignPrepareMsg()
        : Msg(Msg::ASSIGN_PREPARE)
        , epoch_hi(0), epoch_lo(0), wire_id(0)
        , nonce_hi(0), nonce_lo(0), submitter_hostid(0), flags(0) {}
    AssignPrepareMsg(uint64_t epoch, uint32_t id, uint64_t nonce,
                     uint32_t submitter, uint32_t assignment_flags = 0)
        : Msg(Msg::ASSIGN_PREPARE)
        , epoch_hi(uint32_t(epoch >> 32)), epoch_lo(uint32_t(epoch))
        , wire_id(id)
        , nonce_hi(uint32_t(nonce >> 32)), nonce_lo(uint32_t(nonce))
        , submitter_hostid(submitter), flags(assignment_flags) {}

    virtual void fill_from_channel(MsgChannel *c);
    virtual void send_to_channel(MsgChannel *c) const;

    uint64_t epoch() const { return (uint64_t(epoch_hi) << 32) | epoch_lo; }
    uint64_t nonce() const { return (uint64_t(nonce_hi) << 32) | nonce_lo; }

    uint32_t epoch_hi, epoch_lo;
    uint32_t wire_id;
    uint32_t nonce_hi, nonce_lo;
    uint32_t submitter_hostid;
    uint32_t flags;
};

class AssignReadyMsg : public Msg
{
public:
    AssignReadyMsg()
        : Msg(Msg::ASSIGN_READY), epoch_hi(0), epoch_lo(0), wire_id(0)
        , nonce_hi(0), nonce_lo(0) {}
    AssignReadyMsg(uint64_t epoch, uint32_t id, uint64_t nonce)
        : Msg(Msg::ASSIGN_READY), epoch_hi(uint32_t(epoch >> 32))
        , epoch_lo(uint32_t(epoch)), wire_id(id)
        , nonce_hi(uint32_t(nonce >> 32)), nonce_lo(uint32_t(nonce)) {}

    virtual void fill_from_channel(MsgChannel *c);
    virtual void send_to_channel(MsgChannel *c) const;
    uint64_t epoch() const { return (uint64_t(epoch_hi) << 32) | epoch_lo; }
    uint64_t nonce() const { return (uint64_t(nonce_hi) << 32) | nonce_lo; }

    uint32_t epoch_hi, epoch_lo;
    uint32_t wire_id;
    uint32_t nonce_hi, nonce_lo;
};

class RevokeBeforeStartMsg : public Msg
{
public:
    RevokeBeforeStartMsg()
        : Msg(Msg::REVOKE_BEFORE_START), epoch_hi(0), epoch_lo(0), wire_id(0)
        , nonce_hi(0), nonce_lo(0) {}
    RevokeBeforeStartMsg(uint64_t epoch, uint32_t id, uint64_t nonce)
        : Msg(Msg::REVOKE_BEFORE_START), epoch_hi(uint32_t(epoch >> 32))
        , epoch_lo(uint32_t(epoch)), wire_id(id)
        , nonce_hi(uint32_t(nonce >> 32)), nonce_lo(uint32_t(nonce)) {}

    virtual void fill_from_channel(MsgChannel *c);
    virtual void send_to_channel(MsgChannel *c) const;
    uint64_t epoch() const { return (uint64_t(epoch_hi) << 32) | epoch_lo; }
    uint64_t nonce() const { return (uint64_t(nonce_hi) << 32) | nonce_lo; }

    uint32_t epoch_hi, epoch_lo;
    uint32_t wire_id;
    uint32_t nonce_hi, nonce_lo;
};

class RevokeResultMsg : public Msg
{
public:
    enum Result : uint32_t {
        Revoked = 0,
        ClaimedOrLater = 1
    };

    RevokeResultMsg()
        : Msg(Msg::REVOKE_RESULT), epoch_hi(0), epoch_lo(0), wire_id(0)
        , nonce_hi(0), nonce_lo(0), result(Revoked) {}
    RevokeResultMsg(uint64_t epoch, uint32_t id, uint64_t nonce,
                    Result assignment_result)
        : Msg(Msg::REVOKE_RESULT), epoch_hi(uint32_t(epoch >> 32))
        , epoch_lo(uint32_t(epoch)), wire_id(id)
        , nonce_hi(uint32_t(nonce >> 32)), nonce_lo(uint32_t(nonce))
        , result(assignment_result) {}

    virtual void fill_from_channel(MsgChannel *c);
    virtual void send_to_channel(MsgChannel *c) const;
    uint64_t epoch() const { return (uint64_t(epoch_hi) << 32) | epoch_lo; }
    uint64_t nonce() const { return (uint64_t(nonce_hi) << 32) | nonce_lo; }
    bool validResult() const
    {
        return result == Revoked || result == ClaimedOrLater;
    }

    uint32_t epoch_hi, epoch_lo;
    uint32_t wire_id;
    uint32_t nonce_hi, nonce_lo;
    uint32_t result;
};

class StatsMsg : public Msg
{
public:
    StatsMsg()
        : Msg(Msg::STATS)
        , load(0)
        , client_count(0)
    {
    }

    virtual void fill_from_channel(MsgChannel *c);
    virtual void send_to_channel(MsgChannel *c) const;

    /**
     * For now the only load measure we have is the
     * load from 0-1000.
     * This is defined to be a daemon defined value
     * on how busy the machine is. The higher the load
     * is, the slower a given job will compile (preferably
     * linear scale). Load of 1000 means to not schedule
     * another job under no circumstances.
     */
    uint32_t load;

    uint32_t loadAvg1;
    uint32_t loadAvg5;
    uint32_t loadAvg10;
    uint32_t freeMem;

    uint32_t client_count; // number of CS -> C connections at the moment
};

class EnvTransferMsg : public Msg
{
public:
    EnvTransferMsg()
        : Msg(Msg::TRANFER_ENV) {}

    EnvTransferMsg(const std::string &_target, const std::string &_name)
        : Msg(Msg::TRANFER_ENV)
        , name(_name)
        , target(_target) {}

    virtual void fill_from_channel(MsgChannel *c);
    virtual void send_to_channel(MsgChannel *c) const;

    std::string name;
    std::string target;
};

class GetInternalStatus : public Msg
{
public:
    GetInternalStatus()
        : Msg(Msg::GET_INTERNALS) {}

    GetInternalStatus(const GetInternalStatus &)
        : Msg(Msg::GET_INTERNALS) {}
};

class MonLoginMsg : public Msg
{
public:
    MonLoginMsg()
        : Msg(Msg::MON_LOGIN) {}
};

class MonGetCSMsg : public GetCSMsg
{
public:
    MonGetCSMsg()
        : GetCSMsg()
    { // overwrite
        value_ = MON_GET_CS;
        clientid = job_id = 0;
    }

    MonGetCSMsg(int jobid, int hostid, const GetCSMsg *m)
        : GetCSMsg(Environments(), m->filename, m->lang, 1, m->target, 0, std::string(), false, m->client_count, m->niceness)
        , job_id(jobid)
        , clientid(hostid)
    {
        value_ = MON_GET_CS;
    }

    virtual void fill_from_channel(MsgChannel *c);
    virtual void send_to_channel(MsgChannel *c) const;

    uint32_t job_id;
    uint32_t clientid;
};

class MonJobBeginMsg : public Msg
{
public:
    MonJobBeginMsg()
        : Msg(Msg::MON_JOB_BEGIN)
        , job_id(0)
        , stime(0)
        , hostid(0) {}

    MonJobBeginMsg(unsigned int id, unsigned int time, int _hostid)
        : Msg(Msg::MON_JOB_BEGIN)
        , job_id(id)
        , stime(time)
        , hostid(_hostid) {}

    virtual void fill_from_channel(MsgChannel *c);
    virtual void send_to_channel(MsgChannel *c) const;

    uint32_t job_id;
    uint32_t stime;
    uint32_t hostid;
};

class MonJobDoneMsg : public JobDoneMsg
{
public:
    MonJobDoneMsg()
        : JobDoneMsg()
    {
        value_ = MON_JOB_DONE;
    }

    MonJobDoneMsg(const JobDoneMsg &o)
        : JobDoneMsg(o)
    {
        value_ = MON_JOB_DONE;
    }
};

class MonLocalJobBeginMsg : public Msg
{
public:
    MonLocalJobBeginMsg()
        : Msg(Msg::MON_LOCAL_JOB_BEGIN) {}

    MonLocalJobBeginMsg(unsigned int id, const std::string &_file, unsigned int time, int _hostid)
        : Msg(Msg::MON_LOCAL_JOB_BEGIN)
        , job_id(id)
        , stime(time)
        , hostid(_hostid)
        , file(_file) {}

    virtual void fill_from_channel(MsgChannel *c);
    virtual void send_to_channel(MsgChannel *c) const;

    uint32_t job_id;
    uint32_t stime;
    uint32_t hostid;
    std::string file;
};

class MonStatsMsg : public Msg
{
public:
    MonStatsMsg()
        : Msg(Msg::MON_STATS) {}

    MonStatsMsg(int id, const std::string &_statmsg)
        : Msg(Msg::MON_STATS)
        , hostid(id)
        , statmsg(_statmsg) {}

    virtual void fill_from_channel(MsgChannel *c);
    virtual void send_to_channel(MsgChannel *c) const;

    uint32_t hostid;
    std::string statmsg;
};

class TextMsg : public Msg
{
public:
    TextMsg()
        : Msg(Msg::TEXT) {}

    TextMsg(const std::string &_text)
        : Msg(Msg::TEXT)
        , text(_text) {}

    TextMsg(const TextMsg &m)
        : Msg(Msg::TEXT)
        , text(m.text) {}

    virtual void fill_from_channel(MsgChannel *c);
    virtual void send_to_channel(MsgChannel *c) const;

    std::string text;
};

class StatusTextMsg : public Msg
{
public:
    StatusTextMsg()
        : Msg(Msg::STATUS_TEXT) {}

    StatusTextMsg(const std::string &_text)
        : Msg(Msg::STATUS_TEXT)
        , text(_text) {}

    virtual void fill_from_channel(MsgChannel *c);
    virtual void send_to_channel(MsgChannel *c) const;

    std::string text;
};

class VerifyEnvMsg : public Msg
{
public:
    VerifyEnvMsg()
        : Msg(Msg::VERIFY_ENV) {}

    VerifyEnvMsg(const std::string &_target, const std::string &_environment)
        : Msg(Msg::VERIFY_ENV)
        , environment(_environment)
        , target(_target) {}

    virtual void fill_from_channel(MsgChannel *c);
    virtual void send_to_channel(MsgChannel *c) const;

    std::string environment;
    std::string target;
};

class VerifyEnvResultMsg : public Msg
{
public:
    VerifyEnvResultMsg()
        : Msg(Msg::VERIFY_ENV_RESULT) {}

    VerifyEnvResultMsg(bool _ok)
        : Msg(Msg::VERIFY_ENV_RESULT)
        , ok(_ok) {}

    virtual void fill_from_channel(MsgChannel *c);
    virtual void send_to_channel(MsgChannel *c) const;

    bool ok;
};

class BlacklistHostEnvMsg : public Msg
{
public:
    BlacklistHostEnvMsg()
        : Msg(Msg::BLACKLIST_HOST_ENV) {}

    BlacklistHostEnvMsg(const std::string &_target, const std::string &_environment, const std::string &_hostname)
        : Msg(Msg::BLACKLIST_HOST_ENV)
        , environment(_environment)
        , target(_target)
        , hostname(_hostname) {}

    virtual void fill_from_channel(MsgChannel *c);
    virtual void send_to_channel(MsgChannel *c) const;

    std::string environment;
    std::string target;
    std::string hostname;
};

#endif
