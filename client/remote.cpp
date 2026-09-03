/* -*- mode: C++; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 99; -*- */
/* vim: set ts=4 sw=4 et tw=99:  */
/*
    This file is part of Icecream.

    Copyright (c) 2004 Stephan Kulow <coolo@suse.de>
                  2002, 2003 by Martin Pool <mbp@samba.org>

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

#include "config.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <poll.h>

#ifdef __FreeBSD__
// Grmbl  Why is this needed?  We don't use readv/writev
#include <sys/uio.h>
#endif

#include <fcntl.h>
#include <signal.h>
#include <limits.h>
#include <assert.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <map>
#include <algorithm>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <vector>

#include <comm.h>
#include "client.h"
#include "tempfile.h"
#include "md5.h"
#include "util.h"
#include "input_pump.h"
#include "p50_compile_binding.h"
#include "cache/p50_control_operation.h"
#include "cache/p50_daemon_control.h"
#include "cache/p50_sidecar_supervisor.h"
#include "services/util.h"
#include "pipes.h"

#include <chrono>
#include <memory>
#include <optional>

#ifndef O_LARGEFILE
#define O_LARGEFILE 0
#endif

namespace
{

struct CharBufferDeleter {
    char *buf;
    explicit CharBufferDeleter(char *b) : buf(b) {}
    ~CharBufferDeleter() {
        free(buf);
    }
};

void append_p50_compile_identity_trace(const CompileJob &job,
                                       const CompileResultMsg &result) noexcept
{
    const char *path = ::getenv("ICECC_P50_COMPILE_IDENTITY_TRACE");
    if (path == nullptr || *path == '\0' || !job.hasAssignmentIdentity()
            || !job.hasCompileIdentity() || !result.compileIdentityMatches(job))
        return;

    char line[512];
    const int length = ::snprintf(
        line, sizeof(line),
        "{\"record\":\"compile-result-identity\",\"job_id\":%u,"
        "\"assignment_epoch\":%llu,\"assignment_nonce\":%llu,"
        "\"c_guid\":%llu,\"tu_seq\":%llu}\n",
        job.jobID(),
        static_cast<unsigned long long>(result.assignmentEpoch()),
        static_cast<unsigned long long>(result.assignmentNonce()),
        static_cast<unsigned long long>(result.cGuid()),
        static_cast<unsigned long long>(result.tuSeq()));
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(line))
        return;

    const int fd = ::open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd < 0)
        return;
    size_t offset = 0;
    while (offset < static_cast<size_t>(length)) {
        const ssize_t written = ::write(fd, line + offset,
                                        static_cast<size_t>(length) - offset);
        if (written > 0) {
            offset += static_cast<size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR)
            continue;
        break;
    }
    (void)::close(fd);
}

class TempSourceFile
{
public:
    explicit TempSourceFile(char *path) noexcept
        : path_(path)
    {
    }

    ~TempSourceFile()
    {
        if (path_ != nullptr) {
            if (linked_)
                (void)::unlink(path_);
            free(path_);
        }
    }

    TempSourceFile(const TempSourceFile &) = delete;
    TempSourceFile &operator=(const TempSourceFile &) = delete;

    const char *path() const noexcept { return path_; }

    bool unlink_now() noexcept
    {
        if (!linked_)
            return true;
        if (::unlink(path_) != 0)
            return false;
        linked_ = false;
        return true;
    }

private:
    char *path_ = nullptr;
    bool linked_ = true;
};

/* The S7 runner may opt in to retaining the completed preprocessor output.
   This is deliberately a client-only observation seam: production behavior
   is unchanged when the variable is absent, while a configured destination
   must be copied successfully before the normal unlink is allowed to proceed. */
static bool retain_p50_preprocessed_capture(const char *source) noexcept
{
    const char *capture = ::getenv("ICECC_P50_PREPROCESSED_CAPTURE");
    if (capture == nullptr)
        return true;
    if (*capture == '\0' || *capture != '/')
        return false;

    const int input = ::open(source, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (input < 0)
        return false;
    const int output = ::open(capture, O_WRONLY | O_CREAT | O_EXCL |
                              O_CLOEXEC | O_NOFOLLOW, 0600);
    if (output < 0) {
        (void)::close(input);
        return false;
    }
    bool success = true;
    char buffer[64 * 1024];
    for (;;) {
        const ssize_t read_bytes = ::read(input, buffer, sizeof(buffer));
        if (read_bytes == 0)
            break;
        if (read_bytes < 0) {
            if (errno == EINTR)
                continue;
            success = false;
            break;
        }
        ssize_t written_total = 0;
        while (written_total < read_bytes) {
            const ssize_t written = ::write(output, buffer + written_total,
                                            static_cast<size_t>(read_bytes - written_total));
            if (written > 0) {
                written_total += written;
                continue;
            }
            if (written < 0 && errno == EINTR)
                continue;
            success = false;
            break;
        }
        if (!success)
            break;
    }
    if (success && ::fsync(output) != 0)
        success = false;
    if (::close(input) != 0)
        success = false;
    if (::close(output) != 0)
        success = false;
    if (!success)
        (void)::unlink(capture);
    return success;
}

icecc::p50::OwnedSourceFd prepare_complete_p50_source(
    CompileJob &job, const char *preproc_file, int &cpp_status)
{
    cpp_status = 0;
    if (preproc_file != nullptr) {
        if (!retain_p50_preprocessed_capture(preproc_file))
            throw client_error(
                11, "Error 11 - unable to retain configured preprocessed input");
        const int fd = ::open(preproc_file, O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            throw client_error(11, "Error 11 - unable to open preprocessed file");
        return icecc::p50::OwnedSourceFd(fd);
    }

    char *temporary_path = nullptr;
    if (dcc_make_tmpnam("icecc-p50", ".ix", &temporary_path, 0) != 0 ||
        temporary_path == nullptr)
        throw client_error(10, "Error 10 - unable to create preprocessor output");
    TempSourceFile temporary(temporary_path);
    int write_fd = ::open(temporary.path(), O_WRONLY | O_TRUNC | O_CLOEXEC);
    if (write_fd < 0)
        throw client_error(10, "Error 10 - unable to open preprocessor output");

    const pid_t cpp_pid = call_cpp(job, write_fd);
    if (cpp_pid == -1) {
        (void)::close(write_fd); // call_cpp closes it only after a successful fork.
        throw client_error(18, "Error 18 - (fork error?)");
    }

    int wait_status = 255;
    pid_t waited;
    do {
        waited = ::waitpid(cpp_pid, &wait_status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited != cpp_pid)
        throw client_error(18, "Error 18 - unable to wait for local cpp");

    cpp_status = shell_exit_status(wait_status);
    if (cpp_status != 0) {
        log_warning() << "call_cpp process failed with exit status "
                      << cpp_status << std::endl;
        if (!compiler_is_clang(job) && compiler_only_rewrite_includes(job))
            throw remote_error(
                103,
                "Error 103 - local cpp invocation failed, trying to recompile locally");
        return icecc::p50::OwnedSourceFd(-1);
    }

    if (!retain_p50_preprocessed_capture(temporary.path()))
        throw client_error(
            11, "Error 11 - unable to retain configured preprocessed input");

    const int read_fd = ::open(temporary.path(), O_RDONLY | O_CLOEXEC);
    if (read_fd < 0)
        throw client_error(11, "Error 11 - unable to reopen preprocessed file");
    if (!temporary.unlink_now()) {
        (void)::close(read_fd);
        throw client_error(11, "Error 11 - unable to unlink preprocessed file");
    }
    return icecc::p50::OwnedSourceFd(read_fd);
}

std::optional<uint32_t> p50_profile_wire(
    icecc::p50::ProfileId profile) noexcept
{
    switch (profile) {
    case icecc::p50::ProfileId::P29:
        return CACHE_PROFILE_P29;
    case icecc::p50::ProfileId::P29V1:
        return CACHE_PROFILE_P29V1;
    case icecc::p50::ProfileId::ZSTD_TU:
        return CACHE_PROFILE_ZSTD_TU;
    case icecc::p50::ProfileId::Z3_LONG:
        return CACHE_PROFILE_ZSTD_ROUTE;
    case icecc::p50::ProfileId::GRZ:
        return CACHE_PROFILE_GRZ;
    default:
        return std::nullopt;
    }
}

std::optional<uint32_t> p50_source_mode_wire(
    icecc::p50::ProfileId profile) noexcept
{
    switch (profile) {
    case icecc::p50::ProfileId::P29:
        return P50_SOURCE_MODE_P29;
    case icecc::p50::ProfileId::P29V1:
        return P50_SOURCE_MODE_P29V1;
    case icecc::p50::ProfileId::ZSTD_TU:
        return P50_SOURCE_MODE_ZSTD_TU;
    case icecc::p50::ProfileId::Z3_LONG:
        return P50_SOURCE_MODE_ZSTD_ROUTE;
    case icecc::p50::ProfileId::GRZ:
        return P50_SOURCE_MODE_GRZ_RESIDUAL;
    default:
        return std::nullopt;
    }
}

icecc::p50::local::P50SourceTransferResult p50_transfer_error(
    uint16_t error_code) noexcept
{
    icecc::p50::local::P50SourceTransferResult result;
    result.code = icecc::p50::local::SourceTransferResultCode::Error;
    result.error_code = error_code == 0 ? 1 : error_code;
    return result;
}

icecc::p50::local::P50SourceTransferResult transfer_p50_source(
    CompileJob &job, const UseCSMsg &assignment, MsgChannel &local_daemon,
    icecc::p50::OwnedSourceFd source, icecc::p50::ProfileId profile)
{
    using namespace icecc::p50;
    using namespace icecc::p50::local;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(120);
    const std::optional<uint32_t> profile_wire = p50_profile_wire(profile);
    const std::optional<uint32_t> source_mode = p50_source_mode_wire(profile);
    if (!profile_wire.has_value() || !source_mode.has_value() || !source)
        return p50_transfer_error(1);
    P50CacheSessionFdRequestFields fd_request;
    fd_request.wire_job_id = assignment.job_id;
    fd_request.assignment_epoch = assignment.assignmentEpoch();
    fd_request.assignment_nonce = assignment.assignmentNonce();
    fd_request.profile = *profile_wire;
    if (!fd_request.valid())
        return p50_transfer_error(1);

    // The ordinary local daemon channel is the assignment authority.  The
    // daemon returns one already-authenticated sidecar control descriptor;
    // no wrapper-local TCP sender or C-store identity is created here.
    if (!local_daemon.send_msg(P50CacheSessionFdRequestMsg(fd_request)))
        return p50_transfer_error(2);
    P50CacheControlIdentity control_identity;
    const int control_fd = local_daemon.receive_p50_cache_fd_reply(
        fd_request, control_identity, deadline);
    if (control_fd < 0 || !control_identity.valid()) {
        if (control_fd >= 0)
            ::close(control_fd);
        return p50_transfer_error(3);
    }

    const int source_dup = ::fcntl(source.get(), F_DUPFD_CLOEXEC, 0);
    if (source_dup < 0) {
        ::close(control_fd);
        return p50_transfer_error(4);
    }

    P50SourceTransferRequest request;
    request.wire_job_id = assignment.job_id;
    request.assignment_epoch = assignment.assignmentEpoch();
    request.assignment_nonce = assignment.assignmentNonce();
    request.selected_f_host = assignment.hostname;
    request.selected_f_ordinary_port = assignment.port;
    request.selected_f_cache_port = assignment.cache_endpoint_port;
    request.cache_protocol = assignment.cache_protocol;
    request.cache_profile = fd_request.profile;
    request.logical_job = job.jobID();
    request.compiler_attempt = job.assignmentNonce();
    request.source_request_id = assignment.assignmentNonce();
    request.source_mode = *source_mode;
    if (!request.valid()) {
        ::close(source_dup);
        ::close(control_fd);
        return p50_transfer_error(5);
    }

    const auto clock = sidecar::process_monotonic_clock_identity();
    const auto absolute_deadline = sidecar::AbsoluteMonotonicDeadline::from_steady_time_point(
        deadline, clock.clock_domain_id, clock.time_namespace_id);
    const Identity identity{control_identity.generation, control_identity.attempt};
    const ControlOperation operation = make_source_transfer_operation(
        identity, request, absolute_deadline);
    CredentialExpectation credentials;
    credentials.uid = control_identity.peer_uid;
    credentials.gid = control_identity.peer_gid;

    DaemonControlOperation control;
    const DaemonControlStatus started = control.begin_authenticated(
        control_fd, operation, source_dup, credentials, identity, deadline,
        DaemonControlLimits{}, DaemonControlFdOwnership::Owned);
    if (started != DaemonControlStatus::InProgress)
        return p50_transfer_error(6);

    while (!control.done()) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            (void)control.advance(now, 0);
            break;
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - now);
        const int timeout = static_cast<int>(std::max<int64_t>(1,
            std::min<int64_t>(remaining.count(), INT_MAX)));
        pollfd descriptor{control.native_handle(), control.desired_events(), 0};
        const int ready = ::poll(&descriptor, 1, timeout);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready < 0) {
            (void)control.advance(std::chrono::steady_clock::now(), POLLERR);
            break;
        }
        (void)control.advance(std::chrono::steady_clock::now(),
                              ready == 0 ? short{0} : descriptor.revents);
    }
    if (control.status() != DaemonControlStatus::Complete ||
        !control.source_transfer_result().has_value()) {
        log_warning() << "P50 cache control operation ended "
                      << daemon_control_status_name(control.status()) << std::endl;
        return p50_transfer_error(7);
    }
    return *control.source_transfer_result();
}

}

using namespace std;

std::string remote_daemon;

Environments
parse_icecc_version(const string &target_platform, const string &prefix)
{
    Environments envs;

    string icecc_version = getenv("ICECC_VERSION");
    assert(!icecc_version.empty());

    // free after the C++-Programming-HOWTO
    string::size_type lastPos = icecc_version.find_first_not_of(',', 0);
    string::size_type pos     = icecc_version.find(',', lastPos);
    bool def_targets = icecc_version.find('=') != string::npos;

    list<string> platforms;

    while (pos != string::npos || lastPos != string::npos) {
        string couple = icecc_version.substr(lastPos, pos - lastPos);
        string platform = target_platform;
        string version = couple;
        string::size_type colon = couple.find(':');

        if (colon != string::npos) {
            platform = couple.substr(0, colon);
            version = couple.substr(colon + 1, couple.length());
        }

        // Skip delimiters.  Note the "not_of"
        lastPos = icecc_version.find_first_not_of(',', pos);
        // Find next "non-delimiter"
        pos = icecc_version.find(',', lastPos);

        if (def_targets) {
            colon = version.find('=');

            if (colon != string::npos) {
                if (prefix != version.substr(colon + 1, version.length())) {
                    continue;
                }

                version = version.substr(0, colon);
            } else if (!prefix.empty()) {
                continue;
            }
        }

        if (find(platforms.begin(), platforms.end(), platform) != platforms.end()) {
            log_error() << "there are two environments for platform " << platform << " - ignoring " << version << endl;
            continue;
        }

        if (::access(version.c_str(), R_OK) < 0) {
            log_error() << "$ICECC_VERSION has to point to an existing file to be installed " << version << endl;
            continue;
        }

        struct stat st;

        if (lstat(version.c_str(), &st) || !S_ISREG(st.st_mode) || st.st_size < 500) {
            log_error() << "$ICECC_VERSION has to point to an existing file to be installed " << version << endl;
            continue;
        }

        envs.push_back(make_pair(platform, version));
        platforms.push_back(platform);
    }

    return envs;
}

static bool
endswith(const string &orig, const char *suff, string &ret)
{
    size_t len = strlen(suff);

    if (orig.size() > len && orig.substr(orig.size() - len) == suff) {
        ret = orig.substr(0, orig.size() - len);
        return true;
    }

    return false;
}

static Environments
rip_out_paths(const Environments &envs, map<string, string> &version_map, map<string, string> &versionfile_map)
{
    version_map.clear();

    Environments env2;

    static const char *suffs[] = { ".tar.xz", ".tar.zst", ".tar.bz2", ".tar.gz", ".tar", ".tgz", nullptr };

    string versfile;

    // host platform + filename
    for (const std::pair<std::string, std::string> &env : envs) {
        for (int i = 0; suffs[i] != nullptr; i++)
            if (endswith(env.second, suffs[i], versfile)) {
                versionfile_map[env.first] = env.second;
                versfile = find_basename(versfile);
                version_map[env.first] = versfile;
                env2.push_back(make_pair(env.first, versfile));
            }
    }

    return env2;
}


string
get_absfilename(const string &_file)
{
    string file;

    if (_file.empty()) {
        return _file;
    }

    if (_file.at(0) != '/') {
        file = get_cwd() + '/' + _file;
    } else {
        file = _file;
    }

    string dots = "/../";
    string::size_type idx = file.find(dots);

    while (idx != string::npos) {
        if (idx == 0) {
            file.replace(0, dots.length(), "/");
        } else {
          string::size_type slash = file.rfind('/', idx - 1);
          file.replace(slash, idx-slash+dots.length(), "/");
        }
        idx = file.find(dots);
    }

    idx = file.find("/./");

    while (idx != string::npos) {
        file.replace(idx, 3, "/");
        idx = file.find("/./");
    }

    idx = file.find("//");

    while (idx != string::npos) {
        file.replace(idx, 2, "/");
        idx = file.find("//");
    }

    return file;
}

static int get_niceness()
{
    errno = 0;
    int niceness = getpriority( PRIO_PROCESS, getpid());
    if( niceness == -1 && errno != 0 )
        niceness = 0;
    return niceness;
}

static UseCSMsg *get_server(MsgChannel *local_daemon)
{
    int timeout = 4 * 60;
    if( get_niceness() > 0 ) // low priority jobs may take longer to get a slot assigned
        timeout = 60 * 60;
    // P50 depth batches can legitimately wait longer for a scheduler slot
    // while other relationships are completing.  The runner already exports
    // this authenticated run limit; use it for the assignment wait too so a
    // client cannot terminate before its input-ready witness is published.
    const char *configured_timeout = ::getenv("ICECC_P50_C1F1_TIMEOUT");
    if (configured_timeout != nullptr && *configured_timeout != '\0') {
        char *end = nullptr;
        errno = 0;
        const long parsed = ::strtol(configured_timeout, &end, 10);
        if (errno == 0 && end != configured_timeout && *end == '\0' && parsed > 0 &&
            parsed <= INT_MAX)
            timeout = std::max(timeout, static_cast<int>(parsed));
    }
    Msg *umsg = local_daemon->get_msg( timeout );

    if (!umsg || *umsg != Msg::USE_CS) {
        log_warning() << "reply was not expected use_cs " << (umsg ? umsg->to_string() : Msg(Msg::UNKNOWN).to_string())  << endl;
        ostringstream unexpected_msg;
        unexpected_msg << "Error 1 - expected use_cs reply, but got " << (umsg ? umsg->to_string() : Msg(Msg::UNKNOWN).to_string()) << " instead";
        delete umsg;
        throw client_error(1, unexpected_msg.str());
    }

    UseCSMsg *usecs = dynamic_cast<UseCSMsg *>(umsg);
    return usecs;
}

static void check_for_failure(Msg *msg, MsgChannel *cserver)
{
    if (msg && *msg == Msg::STATUS_TEXT) {
        log_error() << "Remote status (compiled on " << cserver->name << "): "
                    << static_cast<StatusTextMsg*>(msg)->text << endl;
        throw client_error(23, "Error 23 - Remote status (compiled on " + cserver->name + ")\n" +
                                 static_cast<StatusTextMsg*>(msg)->text );
    }
}

static void receive_file(const string& output_file, MsgChannel* cserver)
{
    string tmp_file = output_file + "_icetmp";
    int obj_fd = open(tmp_file.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_LARGEFILE, 0666);

    if (obj_fd == -1) {
        std::string errmsg("can't create ");
        errmsg += tmp_file + ":";
        log_perror(errmsg.c_str());
        throw client_error(31, "Error 31 - " + errmsg);
    }

    Msg* msg = nullptr;
    size_t uncompressed = 0;
    size_t compressed = 0;

    while (1) {
        delete msg;

        msg = cserver->get_msg(40);

        if (!msg) {   // the network went down?
            unlink(tmp_file.c_str());
            throw client_error(19, "Error 19 - (network failure?)");
        }

        check_for_failure(msg, cserver);

        if (*msg == Msg::END) {
            break;
        }

        if (*msg != Msg::FILE_CHUNK) {
            unlink(tmp_file.c_str());
            delete msg;
            throw client_error(20, "Error 20 - unexpected message");
        }

        FileChunkMsg *fcmsg = dynamic_cast<FileChunkMsg*>(msg);
        compressed += fcmsg->compressed;
        uncompressed += fcmsg->len;

        if (write(obj_fd, fcmsg->buffer, fcmsg->len) != (ssize_t)fcmsg->len) {
            log_perror("Error writing file: ");
            unlink(tmp_file.c_str());
            delete msg;
            throw client_error(21, "Error 21 - error writing file");
        }
    }

    if (uncompressed)
        trace() << "got " << compressed << " bytes ("
                << (compressed * 100 / uncompressed) << "%)" << endl;

    delete msg;

    if (close(obj_fd) != 0) {
        log_perror("Failed to close temporary file: ");
        if(unlink(tmp_file.c_str()) != 0)
        {
            log_perror("delete temporary file - might be related to close failure above");
        }
        throw client_error(30, "Error 30 - error closing temp file");

    }
    if(rename(tmp_file.c_str(), output_file.c_str()) != 0) {
        log_perror("Failed to rename temporary file: ");
        if(unlink(tmp_file.c_str()) != 0)
        {
            log_perror("delete temporary file - might be related to rename failure above");
        }
        throw client_error(30, "Error 30 - error closing temp file");
    }
}

/* A P50 submitter may definitively reject a result from CompileResultMsg
   metadata (OOM/caret/missing-input fallback) before consuming successful
   output bytes.  The worker must nevertheless finish its output-first stream
   before it can receive the terminal disposition.  Drain those exact output
   frames without publishing a file so neither side can deadlock on a full
   duplex socket after the cancellation frame has been sent. */
static void discard_p50_output_file(MsgChannel *cserver)
{
    for (;;) {
        std::unique_ptr<Msg> msg(cserver->get_msg(40));
        if (!msg)
            throw client_error(19, "Error 19 - network failure while discarding P50 output");
        check_for_failure(msg.get(), cserver);
        if (*msg == Msg::END)
            return;
        if (*msg != Msg::FILE_CHUNK)
            throw client_error(20, "Error 20 - unexpected P50 output message");
    }
}

static int build_remote_int(CompileJob &job, UseCSMsg *usecs, MsgChannel *local_daemon,
                            const string &environment, const string &version_file,
                            const char *preproc_file, bool output)
{
    string hostname = usecs->hostname;
    unsigned int port = usecs->port;
    int job_id = usecs->job_id;
    bool got_env = usecs->got_env;
    invocation_timing_set_compile_job_id(job_id);
    if (!usecs->applyAssignmentTo(&job)) {
        throw client_error(9, "Error 9 - malformed assignment identity");
    }
    job.setEnvironmentVersion(environment);   // hoping on the scheduler's wisdom
    trace() << "Have to use host " << hostname << ":" << port << " - Job ID: "
            << job.jobID() << " - env: " << usecs->host_platform
            << " - has env: " << (got_env ? "true" : "false")
            << " - match j: " << usecs->matched_job_id
            << "\n";

    int status = 255;

    MsgChannel *cserver = nullptr;
    bool p50_input = false;
    bool p50_result_received = false;
    bool p50_disposition_attempted = false;
    bool p50_disposition_sent = false;

    auto send_p50_disposition = [&](ResultDispositionMsg::Disposition disposition) {
        if (!p50_input || p50_disposition_attempted)
            return p50_disposition_sent;
        p50_disposition_attempted = true;
        if (cserver == nullptr)
            return false;

        /* The real C1F1 terminal-flow gate needs the ordinary worker parser,
           not a codec double, to observe malformed and disconnected first
           witnesses.  Arm these two faults only under the existing explicit
           C1F1-required test environment and only where production would send
           Accepted after receiving every output byte. */
        const char *required = getenv("ICECC_P50_C1F1_REQUIRED");
        const char *test_hook = getenv("ICECC_P50_TEST_DISPOSITION");
        if (disposition == ResultDispositionMsg::Accepted &&
            required != nullptr && string(required) == "1" &&
            test_hook != nullptr) {
            const string selected(test_hook);
            if (selected == "malformed") {
                trace() << "P50 terminal test sending malformed disposition for job "
                        << job.jobID() << "\n";
                p50_disposition_sent = cserver->send_msg(EndMsg());
                return p50_disposition_sent;
            }
            if (selected == "disconnect") {
                trace() << "P50 terminal test disconnecting before disposition for job "
                        << job.jobID() << "\n";
                delete cserver;
                cserver = nullptr;
                p50_disposition_sent = true;
                return true;
            }
        }
        const ResultDispositionMsg result_disposition(job, disposition);
        p50_disposition_sent = cserver->send_msg(result_disposition);
        if (!p50_disposition_sent) {
            log_warning() << "failed sending terminal P50 result disposition for job "
                          << job.jobID() << endl;
        }
        return p50_disposition_sent;
    };

    try {
        cserver = Service::createChannel(hostname, port, 10);

        if (!cserver) {
            log_error() << "no server found behind given hostname " << hostname << ":"
                        << port << endl;
            throw client_error(2, "Error 2 - no server found at " + hostname);
        }
        cserver->set_p50_legacy_wire_role(P50LegacyWireRole::C);

        // Environment transfer always stays on the ordinary legacy stream.
        // Source selection happens only after this phase and then remains one
        // whole-job mode: FileChunk or P50 ZSTD_TU, never both.
        LegacyRemoteSink input_sink(cserver);

        if (!got_env) {
            log_block b("Transfer Environment");
            // transfer env
            struct stat buf;

            if (stat(version_file.c_str(), &buf)) {
                log_perror("error stat'ing file") << "\t" << version_file << endl;
                throw client_error(4, "Error 4 - unable to stat version file");
            }

            EnvTransferMsg msg(job.targetPlatform(), job.environmentVersion());

            if (!dcc_lock_host()) {
                log_error() << "can't lock for local cpp" << endl;
                return EXIT_DISTCC_FAILED;
            }
            HostUnlock environment_unlock;

            if (!cserver->send_msg(msg)) {
                throw client_error(6, "Error 6 - send environment to remote failed");
            }

            int env_fd = open(version_file.c_str(), O_RDONLY);

            if (env_fd < 0) {
                throw client_error(5, "Error 5 - unable to open version file:\n\t" + version_file);
            }

            input_sink.send_fd(env_fd);

            if (!input_sink.send_end()) {
                log_error() << "write of environment failed" << endl;
                throw client_error(8, "Error 8 - write environment to remote failed");
            }

            dcc_unlock();

            if (IS_PROTOCOL_VERSION(31, cserver)) {
                VerifyEnvMsg verifymsg(job.targetPlatform(), job.environmentVersion());

                if (!cserver->send_msg(verifymsg)) {
                    throw client_error(22, "Error 22 - error sending environment");
                }

                Msg *verify_msg = cserver->get_msg(60);

                if (verify_msg && *verify_msg == Msg::VERIFY_ENV_RESULT) {
                    if (!static_cast<VerifyEnvResultMsg*>(verify_msg)->ok) {
                        // The remote can't handle the environment at all (e.g. kernel too old),
                        // mark it as never to be used again for this environment.
                        log_warning() << "Host " << hostname
                                      << " did not successfully verify environment."
                                      << endl;
                        BlacklistHostEnvMsg blacklist(job.targetPlatform(),
                                                      job.environmentVersion(), hostname);
                        local_daemon->send_msg(blacklist);
                        delete verify_msg;
                        throw client_error(24, "Error 24 - remote " + hostname + " unable to handle environment");
                    } else
                        trace() << "Verified host " << hostname << " for environment "
                                << job.environmentVersion() << " (" << job.targetPlatform() << ")"
                                << endl;
                    delete verify_msg;
                } else {
                    delete verify_msg;
                    throw client_error(25, "Error 25 - other error verifying environment on remote");
                }
            }
        }

        if (!IS_PROTOCOL_VERSION(31, cserver) && ignore_unverified()) {
            log_warning() << "Host " << hostname << " cannot be verified." << endl;
            throw client_error(26, "Error 26 - environment on " + hostname + " cannot be verified");
        }

        // Older remotes don't set properly -x argument.
        if(( job.language() == CompileJob::Lang_OBJC || job.language() == CompileJob::Lang_OBJCXX )
            && !IS_PROTOCOL_VERSION(38, cserver)) {
            job.appendFlag( "-x", Arg_Remote );
            job.appendFlag( job.language() == CompileJob::Lang_OBJC ? "objective-c" : "objective-c++", Arg_Remote );
        }

        {
            const std::optional<icecc::p50::ProfileId> selected_p50_profile =
                icecc::p50::p50_zstd_selected_profile(*usecs,
                                                       cserver->protocol);
            p50_input = selected_p50_profile.has_value();
            icecc::p50::ProfileId p50_profile =
                icecc::p50::ProfileId::ZSTD_TU;
            const char *p50_profile_name = "ZSTD_TU";
            if (selected_p50_profile) {
                p50_profile = *selected_p50_profile;
                switch (p50_profile) {
                case icecc::p50::ProfileId::P29:
                    p50_profile_name = "P29";
                    break;
                case icecc::p50::ProfileId::P29V1:
                    p50_profile_name = "P29V1";
                    break;
                case icecc::p50::ProfileId::Z3_LONG:
                    p50_profile_name = "ZSTD_ROUTE";
                    break;
                case icecc::p50::ProfileId::GRZ:
                    p50_profile_name = "GRZ_RESIDUAL";
                    break;
                case icecc::p50::ProfileId::ZSTD_TU:
                    break;
                case icecc::p50::ProfileId::Z3_SHARED_LONG:
                    p50_profile_name = "Z3_SHARED_LONG";
                    break;
                }
            }
            if (getenv("ICECC_P50_C1F1_REQUIRED") != nullptr && !p50_input)
                throw remote_error(
                    105,
                    "Error 105 - strict all-P50 assignment has no ZSTD_TU cache handoff");

            if (!dcc_lock_host()) {
                log_error() << "can't lock for local cpp" << endl;
                return EXIT_DISTCC_FAILED;
            }
            HostUnlock input_unlock;

            if (p50_input) {
                int cpp_status = 0;
                icecc::p50::OwnedSourceFd source =
                    prepare_complete_p50_source(job, preproc_file, cpp_status);
                if (!source) {
                    delete cserver;
                    cserver = nullptr;
                    return cpp_status;
                }

                const icecc::p50::local::P50SourceTransferResult transfer =
                    transfer_p50_source(job, *usecs, *local_daemon,
                                        std::move(source), p50_profile);
                const std::optional<CompileInputIdentity> identity =
                    icecc::p50::bind_compile_input(job, p50_profile, transfer);
                if (!identity.has_value()) {
                    log_warning() << p50_profile_name
                                  << " cache source transfer failed closed (status "
                                  << static_cast<unsigned>(transfer.code)
                                  << ", error "
                                  << static_cast<unsigned>(transfer.error_code)
                                  << ", attempts "
                                  << static_cast<unsigned>(transfer.attempts)
                                  << ")" << endl;
                    throw remote_error(
                        106,
                        "Error 106 - P50 cache source transfer did not commit exactly");
                }
                job.setCompileInputIdentity(*identity);
                trace() << p50_profile_name
                        << " source committed for P50 CompileFile: "
                        << identity->raw_bytes << " exact bytes, TU sequence "
                        << identity->tu_seq << endl;
            } else {
                job.clearCompileInputIdentity();
                if (cserver->protocol >= PROTOCOL_VERSION) {
                    const P50LegacyWireIdentity identity{
                        job.jobID(), job.assignmentEpoch(), job.assignmentNonce(),
                        job.cGuid(), job.tuSeq()};
                    if (!cserver->set_p50_legacy_wire_identity(identity)) {
                        throw client_error(
                            106,
                            "Error 106 - legacy wire identity could not be bound");
                    }
                }
            }

            CompileFileMsg compile_file(&job);
            {
                log_block b("send compile_file");

                if (!cserver->send_msg(compile_file)) {
                    log_warning() << "write of job failed" << endl;
                    throw client_error(9, "Error 9 - error sending file to remote");
                }
            }

            if (!p50_input && !preproc_file) {
                int sockets[2];

                if (create_large_pipe(sockets) != 0) {
                    log_perror("build_remote_in pipe");
                    /* for all possible cases, this is something severe */
                    throw client_error(32, "Error 18 - (fork error?)");
                }

                /* This will fork, and return the pid of the child.  It will not
                   return for the child itself.  If it returns normally it will have
                   closed the write fd, i.e. sockets[1].  */
                pid_t cpp_pid = call_cpp(job, sockets[1], sockets[0]);

                if (cpp_pid == -1) {
                    throw client_error(18, "Error 18 - (fork error?)");
                }

                try {
                    log_block bl2("write_fd_to_server from cpp");
                    input_sink.send_fd(sockets[0]);
                } catch (...) {
                    kill(cpp_pid, SIGTERM);
                    throw;
                }

                log_block wait_cpp("wait for cpp");

                while (waitpid(cpp_pid, &status, 0) < 0 && errno == EINTR) {}

                if (shell_exit_status(status) != 0) {   // failure
                    delete cserver;
                    cserver = nullptr;
                    log_warning() << "call_cpp process failed with exit status " << shell_exit_status(status) << endl;
                    // GCC's -fdirectives-only has a number of cases that it doesn't handle properly,
                    // so if in such mode preparing the source fails, try again recompiling locally.
                    // This will cause double error in case it is a real error, but it'll build successfully if
                    // it was just -fdirectives-only being broken. In other cases fail directly, Clang's
                    // -frewrite-includes is much more reliable than -fdirectives-only, so is GCC's plain -E.
                    if( !compiler_is_clang(job) && compiler_only_rewrite_includes(job))
                        throw remote_error(103, "Error 103 - local cpp invocation failed, trying to recompile locally");
                    else
                        return shell_exit_status(status);
                }
            } else if (!p50_input) {
                int cpp_fd = open(preproc_file, O_RDONLY | O_CLOEXEC);

                if (cpp_fd < 0) {
                    throw client_error(11, "Error 11 - unable to open preprocessed file");
                }

                log_block cpp_block("write_fd_to_server preprocessed");
                input_sink.send_fd(cpp_fd);
            }

            if (!p50_input && !input_sink.send_end()) {
                log_warning() << "write of end failed" << endl;
                throw client_error(12, "Error 12 - failed to send file to remote");
            }

            dcc_unlock();
        }

        Msg *msg;
        {
            log_block wait_cs("wait for cs");
            msg = cserver->get_msg(12 * 60);

            if (!msg) {
                throw client_error(14, "Error 14 - error reading message from remote");
            }
        }

        check_for_failure(msg, cserver);

        if (*msg != Msg::COMPILE_RESULT) {
            log_warning() << "waited for compile result, but got " << msg->to_string() << endl;
            delete msg;
            throw client_error(13, "Error 13 - did not get compile response message");
        }

        CompileResultMsg *crmsg = dynamic_cast<CompileResultMsg*>(msg);
        assert(crmsg);
        if (!crmsg->compileIdentityMatches(job)) {
            delete crmsg;
            throw client_error(13, "Error 13 - compile result assignment/C_GUID/TU_SEQ mismatch");
        }
        append_p50_compile_identity_trace(job, *crmsg);
        p50_result_received = p50_input;

        status = crmsg->status;

        if (status && crmsg->was_out_of_memory) {
            (void)send_p50_disposition(ResultDispositionMsg::DefinitiveCancel);
            delete crmsg;
            log_warning() << "the server ran out of memory, recompiling locally" << endl;
            throw remote_error(101, "Error 101 - the server ran out of memory, recompiling locally");
        }

        if (output) {
            if ((!crmsg->out.empty() || !crmsg->err.empty()) && output_needs_workaround(job)) {
                const bool discard_output = p50_input && status == 0;
                const bool discard_dwo = discard_output && crmsg->have_dwo_file;
                (void)send_p50_disposition(ResultDispositionMsg::DefinitiveCancel);
                delete crmsg;
                if (discard_output) {
                    discard_p50_output_file(cserver);
                    if (discard_dwo)
                        discard_p50_output_file(cserver);
                }
                log_warning() << "command needs stdout/stderr workaround, recompiling locally" << endl;
                log_warning() << "(set ICECC_CARET_WORKAROUND=0 to override)" << endl;
                throw remote_error(102, "Error 102 - command needs stdout/stderr workaround, recompiling locally");
            }

            if (crmsg->err.find("file not found") != string::npos) {
                const bool discard_output = p50_input && status == 0;
                const bool discard_dwo = discard_output && crmsg->have_dwo_file;
                (void)send_p50_disposition(ResultDispositionMsg::DefinitiveCancel);
                delete crmsg;
                if (discard_output) {
                    discard_p50_output_file(cserver);
                    if (discard_dwo)
                        discard_p50_output_file(cserver);
                }
                log_warning() << "remote is missing file, recompiling locally" << endl;
                throw remote_error(104, "Error 104 - remote is missing file, recompiling locally");
            }

            ignore_result(write(STDOUT_FILENO, crmsg->out.c_str(), crmsg->out.size()));

            if (colorify_wanted(job)) {
                colorify_output(crmsg->err);
            } else {
                ignore_result(write(STDERR_FILENO, crmsg->err.c_str(), crmsg->err.size()));
            }

            if (status && (crmsg->err.length() || crmsg->out.length())) {
                log_info() << "Compiled on " << hostname << endl;
            }
        }

        bool have_dwo_file = crmsg->have_dwo_file;
        delete crmsg;

        assert(!job.outputFile().empty());

        if (status == 0) {
            receive_file(job.outputFile(), cserver);
            if (have_dwo_file) {
                string dwo_output = job.outputFile().substr(0, job.outputFile().rfind('.')) + ".dwo";
                receive_file(dwo_output, cserver);
            }
        }

        if (!p50_input && cserver->protocol >= PROTOCOL_VERSION &&
            !cserver->p50_legacy_wire_complete()) {
            throw client_error(
                108, "Error 108 - legacy wire witness did not reach completion");
        }

        if (p50_input &&
            !send_p50_disposition(ResultDispositionMsg::Accepted)) {
            throw client_error(
                107,
                "Error 107 - failed to acknowledge complete P50 remote result");
        }

    } catch (...) {
        /* Once CompileResultMsg exists, any local exception before Accepted
           is a definitive rejection attempt.  Never emit a second frame after
           a possibly partial first send: the worker applies first-witness
           semantics and missing/disconnect remains attempt-only. */
        if (p50_input && p50_result_received &&
            !p50_disposition_attempted) {
            (void)send_p50_disposition(ResultDispositionMsg::DefinitiveCancel);
        }
        // Handle pending status messages, if any.
        if(cserver) {
            while(Msg* msg = cserver->get_msg(0, true)) {
                if(*msg == Msg::STATUS_TEXT)
                    log_error() << "Remote status (compiled on " << cserver->name << "): "
                                << static_cast<StatusTextMsg*>(msg)->text << endl;
                delete msg;
            }
            delete cserver;
            cserver = nullptr;
        }

        throw;
    }

    delete cserver;
    return status;
}

static string
md5_for_file(const string & file)
{
    md5_state_t state;
    string result;

    md5_init(&state);
    FILE *f = fopen(file.c_str(), "rb");

    if (!f) {
        return result;
    }

    md5_byte_t buffer[40000];

    while (true) {
        size_t size = fread(buffer, 1, 40000, f);

        if (!size) {
            break;
        }

        md5_append(&state, buffer, size);
    }

    fclose(f);

    md5_byte_t digest[16];
    md5_finish(&state, digest);

    char digest_cache[33];

    for (int di = 0; di < 16; ++di) {
        sprintf(digest_cache + di * 2, "%02x", digest[di]);
    }

    digest_cache[32] = 0;
    result = digest_cache;
    return result;
}

static bool
maybe_build_local(MsgChannel *local_daemon, UseCSMsg *usecs, CompileJob &job,
                  int &ret)
{
    remote_daemon = usecs->hostname;

    if (usecs->hostname == "127.0.0.1") {
        // If this is a test build, do local builds on the local daemon
        // that has --no-remote, use remote building for the remaining ones.
        if (getenv("ICECC_TEST_REMOTEBUILD") && usecs->port != 0 )
            return false;
        trace() << "building myself, but telling localhost\n";
        int job_id = usecs->job_id;
        invocation_timing_set_scheduler_job_id(job_id);
        invocation_timing_set_compile_job_id(job_id);
        if (!usecs->applyAssignmentTo(&job)) {
            throw client_error(29, "Error 29 - malformed assignment identity");
        }
        job.setEnvironmentVersion("__client");
        CompileFileMsg compile_file(&job);

        if (!local_daemon->send_msg(compile_file)) {
            log_warning() << "write of job failed" << endl;
            throw client_error(29, "Error 29 - write of job failed");
        }

        struct timeval begintv,  endtv;

        struct rusage ru;

        gettimeofday(&begintv, nullptr);

        ret = build_local(job, local_daemon, &ru);

        gettimeofday(&endtv, nullptr);

        // filling the stats, so the daemon can play proxy for us
        JobDoneMsg msg(job_id, ret, JobDoneMsg::FROM_SUBMITTER, 0,
                       job.assignmentEpoch(), job.assignmentNonce(),
                       job.cGuid(), job.tuSeq());

        msg.real_msec = (endtv.tv_sec - begintv.tv_sec) * 1000 + (endtv.tv_usec - begintv.tv_usec) / 1000;

        struct stat st;

        msg.out_uncompressed = 0;
        if (!stat(job.outputFile().c_str(), &st)) {
            msg.out_uncompressed += st.st_size;
        }
        if (!stat((job.outputFile().substr(0, job.outputFile().rfind('.')) + ".dwo").c_str(), &st)) {
            msg.out_uncompressed += st.st_size;
        }

        msg.user_msec = ru.ru_utime.tv_sec * 1000 + ru.ru_utime.tv_usec / 1000;
        msg.sys_msec = ru.ru_stime.tv_sec * 1000 + ru.ru_stime.tv_usec / 1000;
        msg.pfaults = ru.ru_majflt + ru.ru_minflt + ru.ru_nswap;
        msg.exitcode = ret;

        if (msg.user_msec > 50 && msg.out_uncompressed > 1024) {
            trace() << "speed=" << float(msg.out_uncompressed / msg.user_msec) << endl;
        }

        return local_daemon->send_msg(msg);
    }

    return false;
}

// Minimal version of remote host that we want to use for the job.
static int minimalRemoteVersion( const CompileJob& job)
{
    int version = MIN_PROTOCOL_VERSION;
    if (ignore_unverified()) {
        version = max(version, 31);
    }

    if (job.dwarfFissionEnabled()) {
        version = max(version, 35);
    }

    return version;
}

static unsigned int requiredRemoteFeatures()
{
    unsigned int features = 0;
    if (const char* icecc_env_compression = getenv( "ICECC_ENV_COMPRESSION" )) {
        if( strcmp( icecc_env_compression, "xz" ) == 0 )
            features = features | NODE_FEATURE_ENV_XZ;
        if( strcmp( icecc_env_compression, "zstd" ) == 0 )
            features = features | NODE_FEATURE_ENV_ZSTD;
    }
    return features;
}

int build_remote(CompileJob &job, MsgChannel *local_daemon, const Environments &_envs, int permill)
{
    srand(time(nullptr) + getpid());

    int torepeat = 1;
    bool has_split_dwarf = job.dwarfFissionEnabled();

    if (!compiler_is_clang(job)) {
        if (rand() % 1000 < permill) {
            torepeat = 3;
        }
    }

    if( torepeat == 1 ) {
        trace() << "preparing " << job.inputFile() << " to be compiled for "
                << job.targetPlatform() << "\n";
    } else {
        trace() << "preparing " << job.inputFile() << " to be compiled " << torepeat << " times for "
                << job.targetPlatform() << "\n";
    }

    map<string, string> versionfile_map, version_map;
    Environments envs = rip_out_paths(_envs, version_map, versionfile_map);

    if (!envs.size()) {
        log_error() << "$ICECC_VERSION needs to point to .tar files" << endl;
        throw client_error(22, "Error 22 - $ICECC_VERSION needs to point to .tar files");
    }

    const char *preferred_host = getenv("ICECC_PREFERRED_HOST");

    if (torepeat == 1) {
        string fake_filename;
        list<string> args = job.remoteFlags();

        for (list<string>::const_iterator it = args.begin(); it != args.end(); ++it) {
            fake_filename += "/" + *it;
        }

        args = job.restFlags();

        for (list<string>::const_iterator it = args.begin(); it != args.end(); ++it) {
            fake_filename += "/" + *it;
        }

        fake_filename += get_absfilename(job.inputFile());

        GetCSMsg getcs(envs, fake_filename, job.language(), torepeat,
                       job.targetPlatform(), job.argumentFlags(),
                       preferred_host ? preferred_host : string(),
                       minimalRemoteVersion(job), requiredRemoteFeatures(),
                       get_niceness(), 0, invocation_cmdline);

        trace() << "asking for host to use" << endl;
        if (!local_daemon->send_msg(getcs)) {
            log_warning() << "asked for CS" << endl;
            throw client_error(24, "Error 24 - asked for CS");
        }

        UseCSMsg *usecs = get_server(local_daemon);
        invocation_timing_set_scheduler_job_id(usecs->job_id);
        invocation_timing_set_compile_job_id(usecs->job_id);
        invocation_timing_mark_start(usecs->hostname == "127.0.0.1"
                                     ? string("local_via_scheduler")
                                     : string("remote"));
        int ret;

        try {
            if (!maybe_build_local(local_daemon, usecs, job, ret))
                ret = build_remote_int(job, usecs, local_daemon,
                                       version_map[usecs->host_platform],
                                       versionfile_map[usecs->host_platform],
                                       nullptr, true);
        } catch(...) {
            delete usecs;
            throw;
        }

        delete usecs;
        return ret;
    } else {
        char *preproc = nullptr;
        dcc_make_tmpnam("icecc", ".ix", &preproc, 0);
        const CharBufferDeleter preproc_holder(preproc);
        int cpp_fd = open(preproc, O_WRONLY);

        if (!dcc_lock_host()) {
            log_error() << "can't lock for local cpp" << endl;
            return EXIT_DISTCC_FAILED;
        }
        HostUnlock hostUnlock; // automatic dcc_unlock()

        /* When call_cpp returns normally (for the parent) it will have closed
           the write fd, i.e. cpp_fd.  */
        pid_t cpp_pid = call_cpp(job, cpp_fd);

        if (cpp_pid == -1) {
            ::unlink(preproc);
            throw client_error(10, "Error 10 - (unable to fork process?)");
        }

        int status = 255;
        waitpid(cpp_pid, &status, 0);

        if (shell_exit_status(status)) {   // failure
            log_warning() << "call_cpp process failed with exit status " << shell_exit_status(status) << endl;
            ::unlink(preproc);
            return shell_exit_status(status);
        }
        dcc_unlock();

        char rand_seed[400]; // "designed to be oversized" (Levi's)
        sprintf(rand_seed, "-frandom-seed=%d", rand());
        job.appendFlag(rand_seed, Arg_Remote);

        GetCSMsg getcs(envs, get_absfilename(job.inputFile()), job.language(), torepeat,
                       job.targetPlatform(), job.argumentFlags(),
                       preferred_host ? preferred_host : string(),
                       minimalRemoteVersion(job), 0, get_niceness(), 0,
                       invocation_cmdline);


        if (!local_daemon->send_msg(getcs)) {
            log_warning() << "asked for CS" << endl;
            throw client_error(0, "Error 0 - asked for CS");
        }

        map<pid_t, int> jobmap;
        CompileJob *jobs = new CompileJob[torepeat];
        UseCSMsg **umsgs = new UseCSMsg*[torepeat];

        bool misc_error = false;
        int *exit_codes = new int[torepeat];

        for (int i = 0; i < torepeat; i++) { // init
            exit_codes[i] = 42;
        }


        for (int i = 0; i < torepeat; i++) {
            jobs[i] = job;
            char *buffer = nullptr;

            if (i) {
                dcc_make_tmpnam("icecc", ".o", &buffer, 0);
                jobs[i].setOutputFile(buffer);
            } else {
                buffer = strdup(job.outputFile().c_str());
            }

            const CharBufferDeleter buffer_holder(buffer);

            umsgs[i] = get_server(local_daemon);
            if (i == 0) {
                invocation_timing_set_scheduler_job_id(umsgs[i]->job_id);
                invocation_timing_set_compile_job_id(umsgs[i]->job_id);
                invocation_timing_mark_start(umsgs[i]->hostname == "127.0.0.1"
                                             ? string("local_via_scheduler")
                                             : string("remote"));
            }

            remote_daemon = umsgs[i]->hostname;

            trace() << "got_server_for_job " << umsgs[i]->hostname << endl;

            flush_debug();

            pid_t pid = fork();

            if (pid == -1) {
                log_perror("failure of fork");
                status = -1;
            }

            if (!pid) {
                int ret = 42;

                try {
                    if (!maybe_build_local(local_daemon, umsgs[i], jobs[i], ret))
                        ret = build_remote_int(
                                  jobs[i], umsgs[i], local_daemon,
                                  version_map[umsgs[i]->host_platform],
                                  versionfile_map[umsgs[i]->host_platform],
                                  preproc, i == 0);
                } catch (std::exception& error) {
                    log_info() << "build_remote_int failed and has thrown " << error.what() << endl;
                    kill(getpid(), SIGTERM);
                    return 0; // shouldn't matter
                }

                _exit(ret);
                return 0; // doesn't matter
            }

            jobmap[pid] = i;
        }

        for (int i = 0; i < torepeat; i++) {
            pid_t pid = wait(&status);

            if (pid < 0) {
                log_perror("wait failed");
                status = -1;
            } else {
                if (WIFSIGNALED(status)) {
                    // there was some misc error in processing
                    misc_error = true;
                    break;
                }

                exit_codes[jobmap[pid]] = shell_exit_status(status);
            }
        }

        if (!misc_error) {
            string first_md5 = md5_for_file(jobs[0].outputFile());

            for (int i = 1; i < torepeat; i++) {
                if (!exit_codes[0]) {   // if the first failed, we fail anyway
                    if (exit_codes[i] == 42) { // they are free to fail for misc reasons
                        continue;
                    }

                    if (exit_codes[i]) {
                        log_error() << umsgs[i]->hostname << " compiled with exit code " << exit_codes[i]
                                    << " and " << umsgs[0]->hostname << " compiled with exit code "
                                    << exit_codes[0] << " - aborting!\n";
                        if (-1 == ::unlink(jobs[0].outputFile().c_str())){
                            log_perror("unlink outputFile failed") << "\t" << jobs[0].outputFile() << endl;
                        }
                        if (has_split_dwarf) {
                            string dwo_file = jobs[0].outputFile().substr(0, jobs[0].outputFile().rfind('.')) + ".dwo";
                            if (-1 == ::unlink(dwo_file.c_str())){
                                log_perror("unlink failed") << "\t" << dwo_file << endl;
                            }
                        }
                        exit_codes[0] = -1; // overwrite
                        break;
                    }

                    string other_md5 = md5_for_file(jobs[i].outputFile());

                    if (other_md5 != first_md5) {
                        log_error() << umsgs[i]->hostname << " compiled "
                                    << jobs[0].outputFile() << " with md5 sum " << other_md5
                                    << "(" << jobs[i].outputFile() << ")" << " and "
                                    << umsgs[0]->hostname << " compiled with md5 sum "
                                    << first_md5 << " - aborting!\n";
                        rename(jobs[0].outputFile().c_str(),
                               (jobs[0].outputFile() + ".caught").c_str());
                        rename(preproc, (string(preproc) + ".caught").c_str());
                        if (has_split_dwarf) {
                            string dwo_file = jobs[0].outputFile().substr(0, jobs[0].outputFile().rfind('.')) + ".dwo";
                            rename(dwo_file.c_str(), (dwo_file + ".caught").c_str());
                        }
                        exit_codes[0] = -1; // overwrite
                        break;
                    }
                }

                if (-1 == ::unlink(jobs[i].outputFile().c_str())){
                    log_perror("unlink failed") << "\t" << jobs[i].outputFile() << endl;
                }
                if (has_split_dwarf) {
                    string dwo_file = jobs[i].outputFile().substr(0, jobs[i].outputFile().rfind('.')) + ".dwo";
                    if (-1 == ::unlink(dwo_file.c_str())){
                        log_perror("unlink failed") << "\t" << dwo_file << endl;
                    }
                }
                delete umsgs[i];
            }
        } else {
            if (-1 == ::unlink(jobs[0].outputFile().c_str())){
                log_perror("unlink failed") << "\t" << jobs[0].outputFile() << endl;
            }
            if (has_split_dwarf) {
                string dwo_file = jobs[0].outputFile().substr(0, jobs[0].outputFile().rfind('.')) + ".dwo";
                if (-1 == ::unlink(dwo_file.c_str())){
                    log_perror("unlink failed") << "\t" << dwo_file << endl;
                }
            }

            for (int i = 1; i < torepeat; i++) {
                if (-1 == ::unlink(jobs[i].outputFile().c_str())){
                    log_perror("unlink failed") << "\t" << jobs[i].outputFile() << endl;
                }
                if (has_split_dwarf) {
                    string dwo_file = jobs[i].outputFile().substr(0, jobs[i].outputFile().rfind('.')) + ".dwo";
                    if (-1 == ::unlink(dwo_file.c_str())){
                        log_perror("unlink failed") << "\t" << dwo_file << endl;
                    }
                }
                delete umsgs[i];
            }
        }

        delete umsgs[0];

        int ret = exit_codes[0];
        if (-1 == ::unlink(preproc)){
            log_perror("unlink failed") << "\t" << preproc << endl;
        }

        delete [] umsgs;
        delete [] jobs;
        delete [] exit_codes;

        if (misc_error) {
            throw client_error(27, "Error 27 - misc error");
        }

        return ret;
    }


    return 0;
}
