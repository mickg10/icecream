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

//#define ICECC_DEBUG 1
#ifndef _GNU_SOURCE
// getopt_long
#define _GNU_SOURCE 1
#endif
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <dirent.h>
#include <fcntl.h>
#include <errno.h>
#include <netdb.h>
#include <getopt.h>
#include <limits>

#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/un.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <pwd.h>

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/utsname.h>

#ifdef HAVE_ARPA_NAMESER_H
#  include <arpa/nameser.h>
#endif

#ifdef HAVE_SYS_VFS_H
#include <sys/vfs.h>
#endif

#include <arpa/inet.h>

#ifdef HAVE_RESOLV_H
#  include <resolv.h>
#endif
#include <netdb.h>

#ifndef RUSAGE_SELF
#  define RUSAGE_SELF (0)
#endif
#ifndef RUSAGE_CHILDREN
#  define RUSAGE_CHILDREN (-1)
#endif

#ifdef HAVE_LIBCAP_NG
#  include <cap-ng.h>
#endif

#include <archive.h>

#include <chrono>
#include <deque>
#include <map>
#include <algorithm>
#include <set>
#include <fstream>
#include <sstream>
#include <string>

#include "ncpus.h"
#include "exitcode.h"
#include "serve.h"
#include "workit.h"
#include "logging.h"
#include <comm.h>
#include "load.h"
#include "environment.h"
#include "platform.h"
#include "util.h"
#include "getifaddrs.h"

static std::string pidFilePath;
static volatile sig_atomic_t exit_main_loop = 0;

#ifndef __attribute_warn_unused_result__
#define __attribute_warn_unused_result__
#endif

using namespace std;

static uint64_t monotonic_msec()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

struct FdSnapshot {
    long open_count;
    rlim_t soft_limit;
    rlim_t hard_limit;
    FdSnapshot()
        : open_count(-1)
        , soft_limit(RLIM_INFINITY)
        , hard_limit(RLIM_INFINITY) {}
};

static FdSnapshot collect_fd_snapshot()
{
    FdSnapshot snapshot;

    struct rlimit lim;
    if (getrlimit(RLIMIT_NOFILE, &lim) == 0) {
        snapshot.soft_limit = lim.rlim_cur;
        snapshot.hard_limit = lim.rlim_max;
    }

    DIR *dir = opendir("/proc/self/fd");
    if (!dir) {
        return snapshot;
    }

    long count = 0;
    while (readdir(dir) != nullptr) {
        ++count;
    }
    closedir(dir);

    // ".", "..", and the descriptor used by opendir() itself.
    if (count >= 3) {
        count -= 3;
    } else {
        count = 0;
    }

    snapshot.open_count = count;
    return snapshot;
}

static string json_escape(const string &s)
{
    string out;
    out.reserve(s.size() + 16);

    for (unsigned char c : s) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20) {
                char buf[7];
                snprintf(buf, sizeof(buf), "\\u%04x", int(c));
                out += buf;
            } else {
                out += char(c);
            }
        }
    }

    return out;
}

struct Client {
public:
    /*
     * UNKNOWN: Client was just created - not supposed to be long term
     * GOTNATIVE: Client asked us for the native env - this is the first step
     * PENDING_USE_CS: We have a CS from scheduler and need to tell the client
     *          as soon as there is a spot available on the local machine
     * JOBDONE: This was compiled by a local client and we got a jobdone - awaiting END
     * LINKJOB: This is a local job (aka link job) by a local client we told the scheduler about
     *          and await the finish of it
     * TOINSTALL: We're receiving an environment transfer and wait for it to complete.
     * WAITINSTALL: Client is waiting for the environment transfer unpacking child to finish.
     * TOCOMPILE: We're supposed to compile it ourselves
     * WAITFORCS: Client asked for a CS and we asked the scheduler - waiting for its answer
     * WAITCOMPILE: Client got a CS and will ask him now (it's not me)
     * CLIENTWORK: Client is busy working and we reserve the spot (job_id is set if it's a scheduler job)
     * WAITFORCHILD: Client is waiting for the compile job to finish.
     * WAITCREATEENV: We're waiting for icecc-create-env to finish.
     */
    enum Status { UNKNOWN, GOTNATIVE, PENDING_USE_CS, JOBDONE, LINKJOB, TOINSTALL, WAITINSTALL, TOCOMPILE,
                  WAITFORCS, WAITCOMPILE, CLIENTWORK, WAITFORCHILD, WAITCREATEENV,
                  LASTSTATE = WAITCREATEENV
                } status;
    Client() {
        created_ts = time(nullptr);
        created_msec = monotonic_msec();
        status_since_msec = created_msec;
        last_waitforcs_msec = 0;
        env_bytes_received = 0;
        job_id = 0;
        last_known_job_id = 0;
        channel = nullptr;
        job = nullptr;
        usecsmsg = nullptr;
        client_id = 0;
        niceness = 0;
        status = UNKNOWN;
        pipe_from_child = -1;
        pipe_to_child = -1;
        child_pid = -1;
        fulljob = false;
    }

    void set_status(Status new_status, const char* why = nullptr) {
        status = new_status;
        status_since_msec = monotonic_msec();
        status_why = why ? why : "";
    }

    static string status_str(Status status) {
        switch (status) {
        case UNKNOWN:
            return "unknown";
        case GOTNATIVE:
            return "gotnative";
        case PENDING_USE_CS:
            return "pending_use_cs";
        case JOBDONE:
            return "jobdone";
        case LINKJOB:
            return "linkjob";
        case TOINSTALL:
            return "toinstall";
        case WAITINSTALL:
            return "waitinstall";
        case TOCOMPILE:
            return "tocompile";
        case WAITFORCS:
            return "waitforcs";
        case CLIENTWORK:
            return "clientwork";
        case WAITCOMPILE:
            return "waitcompile";
        case WAITFORCHILD:
            return "waitforchild";
        case WAITCREATEENV:
            return "waitcreateenv";
        }

        assert(false);
        return string(); // shutup gcc
    }

    ~Client() {
        status = (Status) - 1;
        delete channel;
        channel = nullptr;
        delete usecsmsg;
        usecsmsg = nullptr;
        delete job;
        job = nullptr;

        if (pipe_from_child >= 0) {
            if (-1 == close(pipe_from_child) && (errno != EBADF)){
                log_perror("Failed to close pipe from child process");
            }
        }
        if (pipe_to_child >= 0) {
            if (-1 == close(pipe_to_child) && (errno != EBADF)){
                log_perror("Failed to close pipe to child process");
            }
        }

    }
    uint32_t job_id;
    string outfile; // only useful for LINKJOB or TOINSTALL/WAITINSTALL
    MsgChannel *channel;
    UseCSMsg *usecsmsg;
    CompileJob *job;
    int client_id;
    uint32_t niceness; // nice priority (0-20), for PENDING_USE_CS
    // pipe from child process with end status, only valid if WAITFORCHILD or TOINSTALL/WAITINSTALL
    int pipe_from_child;
    // pipe to child process, only valid if TOINSTALL/WAITINSTALL
    int pipe_to_child;
    pid_t child_pid;
    bool fulljob; // during LINKJOB and CLIENTWORK, reserve all slots if set
    string pending_create_env; // only for WAITCREATEENV
    uint64_t created_msec;
    time_t created_ts;
    uint64_t status_since_msec;
    uint64_t last_waitforcs_msec;
    uint64_t env_bytes_received;
    uint32_t last_known_job_id;
    string status_why;

    string dump() const {
        uint64_t age_msec = monotonic_msec() - status_since_msec;
        string ret = status_str(status) + " age_msec=" + toString(age_msec);
        if (last_waitforcs_msec) {
            ret += " last_waitforcs_msec=" + toString(last_waitforcs_msec);
        }
        if (!status_why.empty()) {
            ret += " why=" + status_why;
        }
        ret += " " + channel->dump();

        switch (status) {
        case LINKJOB:
            return ret + " ClientID: " + toString(client_id) + " " + outfile + (fulljob ? " (full)" : "")
                + " PID: " + toString(child_pid);
        case TOINSTALL:
        case WAITINSTALL:
            return ret + " ClientID: " + toString(client_id) + " " + outfile + " PID: " + toString(child_pid)
                + " env_bytes_received=" + toString(env_bytes_received);
        case WAITFORCHILD:
            return ret + " ClientID: " + toString(client_id) + " PID: " + toString(child_pid) + " PFD: " + toString(pipe_from_child);
        case WAITCREATEENV:
            return ret + " " + toString(client_id) + " " + pending_create_env;
        default:
            ret += " ClientID: " + toString(client_id);
            if (job_id) {
                ret += " Job ID: " + toString(job_id);
                if (usecsmsg)
                    ret += " CompileServer: " + usecsmsg->hostname;
            }
            if (niceness != 0)
                ret += " Nice: " + toString(niceness);
            return ret;
        }
    }
};

class Clients : public map<MsgChannel*, Client*>
{
public:
    Clients() {
        active_processes = 0;
    }
    unsigned int active_processes;

    Client *find_by_client_id(int id) const {
        for (auto it : *this)
            if (it.second->client_id == id) {
                return it.second;
            }

        return nullptr;
    }

    Client *find_by_pid(pid_t pid) const {
        for (auto it : *this)
            if (it.second->child_pid == pid) {
                return it.second;
            }

        return nullptr;
    }

    Client *first() {
        iterator it = begin();

        if (it == end()) {
            return nullptr;
        }

        Client *cl = it->second;
        return cl;
    }

    string dump_status(Client::Status s) const {
        int count = 0;

        for (auto it : *this) {
            if (it.second->status == s) {
                count++;
            }
        }

        if (count) {
            return toString(count) + " " + Client::status_str(s) + ", ";
        }

        return string();
    }

    string dump_per_status() const {
        string s;

        for (Client::Status i = Client::UNKNOWN; i <= Client::LASTSTATE;
                i = Client::Status(int(i) + 1)) {
            s += dump_status(i);
        }

        return s;
    }
    Client *get_earliest_client(Client::Status s) const {
        // TODO: possibly speed this up in adding some sorted lists
        Client *client = nullptr;
        int min_client_id = 0;
        uint32_t min_niceness = std::numeric_limits<uint32_t>::max();

        for (auto it : *this) {
            if (it.second->status == s && (!min_client_id || min_client_id > it.second->client_id)
                && it.second->niceness < min_niceness ) {
                client = it.second;
                min_client_id = client->client_id;
                min_niceness = client->niceness;
            }
        }

        return client;
    }
};

static int set_new_pgrp()
{
    /* If we're a session group leader, then we are not able to call
     * setpgid().  However, setsid will implicitly have put us into a new
     * process group, so we don't have to do anything. */

    /* Does everyone have getpgrp()?  It's in POSIX.1.  We used to call
     * getpgid(0), but that is not available on BSD/OS. */
    int pgrp_id = getpgrp();

    if (-1 == pgrp_id){
        log_perror("Failed to get process group ID");
        return EXIT_DISTCC_FAILED;
    }

    if (pgrp_id == getpid()) {
        trace() << "already a process group leader\n";
        return 0;
    }

    if (setpgid(0, 0) == 0) {
        trace() << "entered process group\n";
        return 0;
    }

    trace() << "setpgid(0, 0) failed: " << strerror(errno) << endl;
    return EXIT_DISTCC_FAILED;
}

static void dcc_daemon_terminate(int);

/**
 * Catch all relevant termination signals.  Set up in parent and also
 * applies to children.
 **/
void dcc_daemon_catch_signals()
{
    /* SIGALRM is caught to allow for built-in timeouts when running test
     * cases. */

    signal(SIGTERM, &dcc_daemon_terminate);
    signal(SIGINT, &dcc_daemon_terminate);
    signal(SIGALRM, &dcc_daemon_terminate);
}

pid_t dcc_master_pid;

/**
 * Called when a daemon gets a fatal signal.
 *
 * Some cleanup is done only if we're the master/parent daemon.
 **/
static void dcc_daemon_terminate(int whichsig)
{
    /**
     * This is a signal handler. don't do stupid stuff.
     * Don't call printf. and especially don't call the log_*() functions.
     */

    if (exit_main_loop > 1) {
        // The > 1 is because we get one more signal from the kill(0,...) below.
        // hmm, we got killed already twice. try better
        static const char msg[] = "forced exit.\n";
        ignore_result(write(STDERR_FILENO, msg, strlen( msg )));
        _exit(1);
    }

    // make BSD happy
    signal(whichsig, dcc_daemon_terminate);

    bool am_parent = (getpid() == dcc_master_pid);

    if (am_parent && exit_main_loop == 0) {
        /* kill whole group */
        kill(0, whichsig);

        /* Remove pid file */
        unlink(pidFilePath.c_str());
    }

    ++exit_main_loop;
}

void usage(const char *reason = nullptr)
{
    if (reason) {
        cerr << reason << endl;
    }

    cerr << "usage: iceccd [-n <netname>] [-m <max_processes>] [--no-remote] [-d|--daemonize] [-l logfile] [-s <schedulerhost[:port]>]"
        " [-v[v[v]]] [-u|--user-uid <user_uid>] [-b <env-basedir>] [--cache-limit <MB>] [-N <node_name>] [-i|--interface <net_interface>] [-p|--port <port>]"
        " [--state-jsonl <path>] [--state-interval <sec>] [--state-log]"
        " [--webgui] [--webgui-port <port>] [--webgui-addr <addr>]" << endl;
    exit(1);
}

struct timeval last_stat;

// Initial rlimit for a compile job, measured in megabytes.  Will vary with
// the amount of available memory.
int mem_limit = 100;

// Minimum rlimit for a compile job, measured in megabytes.
const int min_mem_limit = 100;

unsigned int max_kids = 0;

size_t cache_size_limit = 256 * 1024 * 1024;

struct NativeEnvironment {
    string name; // the hash
    // Timestamps for files including compiler binaries, if they have changed since the time
    // the native env was built, it needs to be rebuilt.
    map<string, time_t> filetimes;
    time_t last_use;
    size_t size; // tarball size
    int create_env_pipe; // if in progress of creating the environment
    NativeEnvironment() : last_use( 0 ), size( 0 ), create_env_pipe( 0 ) {}
};

struct ReceivedEnvironment {
    ReceivedEnvironment() : last_use( 0 ), size( 0 ) {}
    time_t last_use;
    size_t size; // directory size
};

struct JobHistoryEntry {
    uint64_t seq;
    time_t start_ts;
    time_t end_ts;
    uint64_t start_msec;
    uint64_t end_msec;
    uint64_t duration_msec;
    int client_id;
    int exitcode;
    uint32_t scheduler_job_id;
    uint32_t compile_job_id;
    string final_status;
    string final_why;
    string target;
    string environment;
    string usecs_host;
    uint16_t usecs_port;
    bool usecs_got_env;
    string outfile;
    string channel;
};

struct WebConnection {
    int fd;
    string inbuf;
    string outbuf;
    bool close_after_write;
    uint64_t created_msec;
    WebConnection() : fd(-1), close_after_write(true), created_msec(0) {}
};

struct Daemon {
    Clients clients;
    // Installed environments received from other nodes. The key is
    // (job->targetPlatform() + "/" job->environmentVersion()).
    map<string, ReceivedEnvironment> received_environments;
    // Map of native environments, the basic one(s) containing just the compiler
    // and possibly more containing additional files (such as compiler plugins).
    // The key is the compiler name and a concatenated list of the additional files
    // (or just the compiler name for the basic ones).
    map<string, NativeEnvironment> native_environments;
    string envbasedir;
    uid_t user_uid;
    gid_t user_gid;
    int warn_icecc_user_errno;
    int tcp_listen_fd;
    int tcp_listen_local_fd; // if tcp_listen is bound to a specific network interface, this one is bound to lo interface
    int unix_listen_fd;
    string machine_name;
    string nodename;
    bool noremote;
    bool custom_nodename;
    size_t cache_size;
    map<int, Client*> fd2client;
    int new_client_id;
    string remote_name;
    time_t next_scheduler_connect;
    unsigned long icecream_load;
    struct timeval icecream_usage;
    int current_load;
    int num_cpus;
    MsgChannel *scheduler;
    DiscoverSched *discover;
    string netname;
    string schedname;
    int scheduler_port;
    string daemon_interface;
    int daemon_port;
    unsigned int supported_features;

    int max_scheduler_pong;
    int max_scheduler_ping;
    unsigned int current_kids;
    uint64_t accept_errors_total;
    uint64_t accept_emfile_errors;
    int last_accept_errno;
    time_t last_accept_errno_ts;

    // Optional periodic dumps for production debugging.
    std::string state_jsonl_path;
    unsigned int state_dump_interval_s;
    bool state_dump_log;
    uint64_t next_state_dump_msec;
    size_t state_dump_worst_clients;

    bool webgui_enabled;
    int webgui_port;
    string webgui_addr;
    int web_listen_fd;
    map<int, WebConnection> web_connections;
    size_t job_history_capacity;
    uint64_t next_job_history_seq;
    deque<JobHistoryEntry> job_history;

    Daemon() {
        warn_icecc_user_errno = 0;
        if (getuid() == 0) {
            struct passwd *pw = getpwnam("icecc");

            if (pw) {
                user_uid = pw->pw_uid;
                user_gid = pw->pw_gid;
            } else {
                warn_icecc_user_errno = errno ? errno : ENOENT; // apparently errno can be 0 on error here
                user_uid = 65534;
                user_gid = 65533;
            }
        } else {
            user_uid = getuid();
            user_gid = getgid();
        }

        envbasedir = "/var/tmp/icecc-envs";
        tcp_listen_fd = -1;
        tcp_listen_local_fd = -1;
        unix_listen_fd = -1;
        new_client_id = 0;
        next_scheduler_connect = 0;
        cache_size = 0;
        noremote = false;
        custom_nodename = false;
        icecream_load = 0;
        icecream_usage.tv_sec = icecream_usage.tv_usec = 0;
        current_load = - 1000;
        num_cpus = 0;
        scheduler = nullptr;
        discover = nullptr;
        scheduler_port = 8765;
        daemon_interface = "";
        daemon_port = 10245;
        max_scheduler_pong = MAX_SCHEDULER_PONG;
        max_scheduler_ping = MAX_SCHEDULER_PING;
        current_kids = 0;
        accept_errors_total = 0;
        accept_emfile_errors = 0;
        last_accept_errno = 0;
        last_accept_errno_ts = 0;
        state_dump_interval_s = 0;
        state_dump_log = false;
        next_state_dump_msec = 0;
        state_dump_worst_clients = 10;
        webgui_enabled = false;
        webgui_port = 8768;
        webgui_addr = "127.0.0.1";
        web_listen_fd = -1;
        job_history_capacity = 20000;
        next_job_history_seq = 1;
    }

    ~Daemon() {
        delete discover;
    }

    bool reannounce_environments() __attribute_warn_unused_result__;
    void answer_client_requests();
    bool handle_transfer_env(Client *client, EnvTransferMsg *msg) __attribute_warn_unused_result__;
    bool handle_env_install_child_done(Client *client);
    bool finish_transfer_env(Client *client, bool cancel = false);
    bool handle_get_native_env(Client *client, GetNativeEnvMsg *msg) __attribute_warn_unused_result__;
    bool finish_get_native_env(Client *client, string env_key);
    void handle_old_request();
    bool handle_compile_file(Client *client, Msg *msg) __attribute_warn_unused_result__;
    bool handle_activity(Client *client) __attribute_warn_unused_result__;
    bool handle_file_chunk_env(Client *client, Msg *msg) __attribute_warn_unused_result__;
    void handle_end(Client *client, int exitcode);
    int scheduler_get_internals() __attribute_warn_unused_result__;
    void clear_children();
    int scheduler_use_cs(UseCSMsg *msg) __attribute_warn_unused_result__;
    int scheduler_no_cs(NoCSMsg *msg) __attribute_warn_unused_result__;
    bool handle_get_cs(Client *client, Msg *msg) __attribute_warn_unused_result__;
    bool handle_local_job(Client *client, Msg *msg) __attribute_warn_unused_result__;
    bool handle_job_done(Client *cl, JobDoneMsg *m) __attribute_warn_unused_result__;
    bool handle_compile_done(Client *client) __attribute_warn_unused_result__;
    bool handle_verify_env(Client *client, VerifyEnvMsg *msg) __attribute_warn_unused_result__;
    bool handle_blacklist_host_env(Client *client, Msg *msg) __attribute_warn_unused_result__;
    int handle_cs_conf(ConfCSMsg *msg);
    string dump_internals() const;
    string determine_nodename();
    void determine_system();
    void determine_supported_features();
    bool maybe_stats(bool force_check = false);
    bool send_scheduler(const Msg &msg) __attribute_warn_unused_result__;
    void close_scheduler();
    bool reconnect();
    int working_loop();
    bool setup_listen_fds();
    bool setup_listen_tcp_fd( int& fd, const string& interface );
    bool setup_listen_unix_fd();
    bool setup_web_listen_fd();
    void close_web();
    void handle_web_accept();
    void handle_web_connection(int fd, short revents);
    void drop_web_connection(int fd);
    string webgui_html() const;
    string dump_clients_json() const;
    string dump_job_history_json(size_t limit) const;
    void remember_finished_job(const Client *client, int exitcode);
    static bool should_track_client_job(const Client *client);
    static bool parse_http_request(const string &request, string &method, string &path);
    static size_t parse_jobs_limit(const string &path);
    void queue_web_response(int fd, int status_code, const char *status_text,
                            const char *content_type, const string &body);
    void note_accept_error(const char *where, int err);
    void check_cache_size(const string &new_env);
    void remove_native_environment(const string& env_key);
    void remove_environment(const string& env_key);
    bool create_env_finished(string env_key);

    void maybe_dump_state();
    std::string dump_state_json() const;
    bool append_state_jsonl_line(const std::string &line) const;
};

bool Daemon::setup_listen_fds()
{
    tcp_listen_fd = -1;
    tcp_listen_local_fd = -1;
    unix_listen_fd = -1;

    if (!noremote) { // if we only listen to local clients, there is no point in going TCP
        if( !setup_listen_tcp_fd( tcp_listen_fd, daemon_interface ))
            return false;
        // We should always listen on the loopback interface, so if we're binding only
        // to a specific interface, bind also to the loopback.
        if( !daemon_interface.empty()) {
            if( !setup_listen_tcp_fd( tcp_listen_local_fd, "lo" ))
                return false;
        }
    }
    if( !setup_listen_unix_fd())
        return false;
    if (!setup_web_listen_fd())
        return false;
    return true;
}

bool Daemon::setup_listen_tcp_fd( int& fd, const string& interface )
{
    if( !interface.empty())
        trace() << "starting to listen on interface " << interface << endl;
    else
        trace() << "starting to listen on all interfaces" << endl;

    if ((fd = socket(PF_INET, SOCK_STREAM, 0)) < 0) {
        log_perror("Failed to create TCP listen socket.");
        return false;
    }

    int optval = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0) {
        log_perror("Failed to set 'Reuse Address(SO_REUSEADDR)' option on TCP Listen Socket");
        return false;
    }

    struct sockaddr_in myaddr;
    if (!build_address_for_interface(myaddr, interface, daemon_port)) {
        return false;
    }

    int count = 5;
    while (count) {
        if (::bind(fd, (struct sockaddr *)&myaddr, sizeof(myaddr)) < 0) {
            log_perror("Failed to bind address to TCP listen socket");
            sleep(2);
            if (!--count) {
                return false;
            }
            continue;
        } else {
            break;
        }
    }

    if (listen(fd, 1024) < 0) {
        log_perror("Failed to set TCP socket for listening to incoming connections");
        return false;
    }

    fcntl(fd, F_SETFD, FD_CLOEXEC);
    return true;
}

bool Daemon::setup_listen_unix_fd()
{
    if ((unix_listen_fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
        log_perror("Failed to create a Unix scoket for listening");
        return false;
    }

    struct sockaddr_un myaddr;

    memset(&myaddr, 0, sizeof(myaddr));

    myaddr.sun_family = AF_UNIX;

    bool reset_umask = false;
    mode_t old_umask = 0;

    if (getenv("ICECC_TEST_SOCKET") == nullptr) {
#ifdef HAVE_LIBCAP_NG
        // We run as system daemon (UID has been already changed).
        if (capng_have_capability( CAPNG_EFFECTIVE, CAP_SYS_CHROOT )) {
#else
        if (getuid() == 0) {
#endif
            string default_socket = "/var/run/icecc/iceccd.socket";
            strncpy(myaddr.sun_path, default_socket.c_str() , sizeof(myaddr.sun_path) - 1);
            myaddr.sun_path[sizeof(myaddr.sun_path) - 1] = '\0';
            if(default_socket.length() > sizeof(myaddr.sun_path) - 1) {
                log_error() << "default socket path too long for sun_path" << endl;
            }
            if (-1 == unlink(myaddr.sun_path) && errno != ENOENT){
                log_perror("unlink failed") << "\t" << myaddr.sun_path << endl;
            }
            old_umask = umask(0);
            reset_umask = true;
        } else { // Started by user.
            if( getenv( "HOME" )) {
                string socket_path = getenv("HOME");
                socket_path.append("/.iceccd.socket");
                strncpy(myaddr.sun_path, socket_path.c_str(), sizeof(myaddr.sun_path) - 1);
                myaddr.sun_path[sizeof(myaddr.sun_path) - 1] = '\0';
                if(socket_path.length() > sizeof(myaddr.sun_path) - 1) {
                    log_error() << "$HOME/.iceccd.socket path too long for sun_path" << endl;
                }
                if (-1 == unlink(myaddr.sun_path) && errno != ENOENT){
                    log_perror("unlink failed") << "\t" << myaddr.sun_path << endl;
                }
            } else {
                log_error() << "launched by user, but $HOME not set" << endl;
                return false;
            }
        }
    } else {
        string test_socket = getenv("ICECC_TEST_SOCKET");
        strncpy(myaddr.sun_path, test_socket.c_str(), sizeof(myaddr.sun_path) - 1);
        myaddr.sun_path[sizeof(myaddr.sun_path) - 1] = '\0';
        if(test_socket.length() > sizeof(myaddr.sun_path) - 1) {
            log_error() << "$ICECC_TEST_SOCKET path too long for sun_path" << endl;
        }
        if (-1 == unlink(myaddr.sun_path) && errno != ENOENT){
            log_perror("unlink failed") << "\t" << myaddr.sun_path << endl;
        }
        old_umask = umask(0);
        reset_umask = true;
    }

    if (::bind(unix_listen_fd, (struct sockaddr*)&myaddr, sizeof(myaddr)) < 0) {
        log_perror("Failed to bind address to unix listen socket");

        if (reset_umask) {
            umask(old_umask);
        }

        return false;
    }

    if (reset_umask) {
        umask(old_umask);
    }

    if (listen(unix_listen_fd, 1024) < 0) {
        log_perror("Failed to set unix socket for listening");
        return false;
    }

    fcntl(unix_listen_fd, F_SETFD, FD_CLOEXEC);

    return true;
}

bool Daemon::setup_web_listen_fd()
{
    if (!webgui_enabled) {
        return true;
    }

    if (web_listen_fd != -1) {
        return true;
    }

    auto parse_webgui_bind_addr = [](const string &input, in_addr &out_addr, string &normalized) {
        string candidate = input;
        if (candidate.empty() || candidate == "localhost") {
            candidate = "127.0.0.1";
        }
        if (inet_pton(AF_INET, candidate.c_str(), &out_addr) != 1) {
            return false;
        }
        normalized = candidate;
        return true;
    };

    web_listen_fd = socket(PF_INET, SOCK_STREAM, 0);
    if (web_listen_fd < 0) {
        log_perror("Failed to create web gui socket");
        return false;
    }

    int optval = 1;
    if (setsockopt(web_listen_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0) {
        log_perror("Failed to set SO_REUSEADDR on web gui socket");
        if (-1 == close(web_listen_fd) && (errno != EBADF)) {
            log_perror("Failed to close web gui socket");
        }
        web_listen_fd = -1;
        return false;
    }

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(webgui_port);
    string normalized_addr;
    if (!parse_webgui_bind_addr(webgui_addr, addr.sin_addr, normalized_addr)) {
        log_error() << "Invalid web gui bind address '" << webgui_addr
                    << "'. Use an IPv4 address or 'localhost'." << endl;
        if (-1 == close(web_listen_fd) && (errno != EBADF)) {
            log_perror("Failed to close web gui socket");
        }
        web_listen_fd = -1;
        return false;
    }
    webgui_addr = normalized_addr;

    if (::bind(web_listen_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
        log_error() << "Failed to bind web gui on " << webgui_addr << ":" << webgui_port
                    << ": " << strerror(errno) << endl;
        if (-1 == close(web_listen_fd) && (errno != EBADF)) {
            log_perror("Failed to close web gui socket");
        }
        web_listen_fd = -1;
        return false;
    }

    if (listen(web_listen_fd, 128) < 0) {
        log_perror("Failed to listen on web gui socket");
        if (-1 == close(web_listen_fd) && (errno != EBADF)) {
            log_perror("Failed to close web gui socket");
        }
        web_listen_fd = -1;
        return false;
    }

    fcntl(web_listen_fd, F_SETFD, FD_CLOEXEC);
    int flags = fcntl(web_listen_fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(web_listen_fd, F_SETFL, flags | O_NONBLOCK);
    }

    log_info() << "web gui listening on http://" << webgui_addr << ":" << webgui_port << endl;
    return true;
}

void Daemon::close_web()
{
    for (auto it = web_connections.begin(); it != web_connections.end(); ++it) {
        if (-1 == close(it->first) && (errno != EBADF)) {
            log_perror("Failed to close web gui client socket");
        }
    }
    web_connections.clear();

    if (web_listen_fd != -1) {
        if (-1 == close(web_listen_fd) && (errno != EBADF)) {
            log_perror("Failed to close web gui listen socket");
        }
        web_listen_fd = -1;
    }
}

void Daemon::drop_web_connection(int fd)
{
    if (-1 == close(fd) && (errno != EBADF)) {
        log_perror("Failed to close web gui connection");
    }
    web_connections.erase(fd);
}

void Daemon::note_accept_error(const char *where, int err)
{
    ++accept_errors_total;
    if (err == EMFILE || err == ENFILE) {
        ++accept_emfile_errors;
    }
    last_accept_errno = err;
    last_accept_errno_ts = time(nullptr);

    static uint64_t last_log_msec = 0;
    const uint64_t now_msec = monotonic_msec();
    const bool force_log = (err == EMFILE || err == ENFILE);
    if (!force_log && now_msec - last_log_msec < 5000) {
        return;
    }
    last_log_msec = now_msec;

    const FdSnapshot fd = collect_fd_snapshot();
    ostringstream msg;
    msg << "accept failed (" << where << "): " << strerror(err) << " (errno " << err << ")";
    if (fd.open_count >= 0) {
        msg << ", open_fds=" << fd.open_count;
        if (fd.soft_limit != RLIM_INFINITY) {
            msg << ", nofile_soft_limit=" << (unsigned long long)fd.soft_limit;
        }
    }
    log_error() << msg.str() << endl;
}

void Daemon::queue_web_response(int fd, int status_code, const char *status_text,
                                const char *content_type, const string &body)
{
    auto it = web_connections.find(fd);
    if (it == web_connections.end()) {
        return;
    }

    ostringstream out;
    out << "HTTP/1.1 " << status_code << " " << status_text << "\r\n";
    out << "Content-Type: " << content_type << "\r\n";
    out << "Cache-Control: no-store\r\n";
    out << "Connection: close\r\n";
    out << "Content-Length: " << body.size() << "\r\n";
    out << "\r\n";
    out << body;

    it->second.outbuf = out.str();
    it->second.close_after_write = true;
}

bool Daemon::parse_http_request(const string &request, string &method, string &path)
{
    size_t line_end = request.find("\r\n");
    if (line_end == string::npos) {
        line_end = request.find('\n');
    }
    if (line_end == string::npos) {
        return false;
    }

    string line = request.substr(0, line_end);
    istringstream in(line);
    string version;
    if (!(in >> method >> path >> version)) {
        return false;
    }
    return true;
}

size_t Daemon::parse_jobs_limit(const string &path)
{
    static const size_t kDefaultLimit = 200;
    static const size_t kMaxLimit = 20000;

    size_t query_pos = path.find('?');
    if (query_pos == string::npos) {
        return kDefaultLimit;
    }
    string query = path.substr(query_pos + 1);

    size_t limit_pos = query.find("limit=");
    if (limit_pos == string::npos) {
        return kDefaultLimit;
    }

    const char *limit_text = query.c_str() + limit_pos + strlen("limit=");
    char *end = nullptr;
    unsigned long limit = strtoul(limit_text, &end, 10);
    if (end == limit_text) {
        return kDefaultLimit;
    }

    if (limit == 0) {
        return kDefaultLimit;
    }

    if (limit > kMaxLimit) {
        limit = kMaxLimit;
    }
    return size_t(limit);
}

string Daemon::webgui_html() const
{
    return R"HTML(<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>iceccd web gui</title>
  <style>
    :root {
      --bg-0: #06090f;
      --bg-1: #11192a;
      --bg-card: rgba(12, 21, 37, 0.86);
      --fg: #eff6ff;
      --fg-dim: #96acc7;
      --border: #2a3a53;
      --good: #22c55e;
      --warn: #f59e0b;
      --bad: #ef4444;
      --info: #3b82f6;
      --violet: #8b5cf6;
      --teal: #14b8a6;
      --shadow: 0 14px 36px rgba(2, 8, 23, 0.55);
    }
    * { box-sizing: border-box; }
    body {
      margin: 0;
      color: var(--fg);
      font-family: "JetBrains Mono", "IBM Plex Mono", "SFMono-Regular", Menlo, Consolas, monospace;
      background:
        radial-gradient(1400px 680px at 8% -20%, rgba(20, 184, 166, 0.22), transparent 65%),
        radial-gradient(1200px 640px at 88% -25%, rgba(139, 92, 246, 0.24), transparent 68%),
        linear-gradient(160deg, var(--bg-1), var(--bg-0));
      min-height: 100vh;
    }
    .shell {
      max-width: 1700px;
      margin: 0 auto;
      padding: 18px 16px 22px;
    }
    .header {
      display: flex;
      justify-content: space-between;
      gap: 12px;
      align-items: baseline;
      flex-wrap: wrap;
      margin-bottom: 10px;
    }
    .title {
      margin: 0;
      font-size: 24px;
      letter-spacing: 0.4px;
      color: #f8fbff;
      text-shadow: 0 3px 12px rgba(0, 0, 0, 0.33);
    }
    .meta-line {
      margin-top: 3px;
      color: var(--fg-dim);
      font-size: 12px;
    }
    .chip {
      display: inline-flex;
      align-items: center;
      gap: 8px;
      border: 1px solid var(--border);
      border-radius: 999px;
      padding: 6px 12px;
      font-size: 12px;
      color: var(--fg-dim);
      background: rgba(9, 15, 28, 0.65);
    }
    .dot {
      width: 8px;
      height: 8px;
      border-radius: 50%;
      background: var(--warn);
      box-shadow: 0 0 8px rgba(245, 158, 11, 0.6);
    }
    .dot.good {
      background: var(--good);
      box-shadow: 0 0 9px rgba(34, 197, 94, 0.7);
    }
    .metrics {
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(205px, 1fr));
      gap: 10px;
    }
    .card {
      border: 1px solid var(--border);
      border-radius: 13px;
      padding: 12px 12px 10px;
      background: var(--bg-card);
      box-shadow: var(--shadow);
      backdrop-filter: blur(3px);
    }
    .label {
      color: var(--fg-dim);
      text-transform: uppercase;
      font-size: 11px;
      letter-spacing: 0.4px;
    }
    .value {
      margin-top: 4px;
      font-size: 22px;
      font-weight: 700;
      line-height: 1.1;
      color: #f8fbff;
    }
    .value.small {
      font-size: 17px;
    }
    .good { color: var(--good); }
    .warn { color: var(--warn); }
    .bad { color: var(--bad); }
    .info { color: #8db8ff; }
    .layout {
      display: grid;
      grid-template-columns: 0.95fr 1.05fr;
      gap: 12px;
      margin-top: 12px;
    }
    .panel {
      border: 1px solid var(--border);
      border-radius: 13px;
      background: rgba(8, 14, 24, 0.82);
      box-shadow: var(--shadow);
      overflow: hidden;
    }
    .panel-head {
      padding: 10px 12px;
      display: flex;
      justify-content: space-between;
      align-items: center;
      gap: 10px;
      border-bottom: 1px solid rgba(44, 61, 84, 0.68);
      background: linear-gradient(180deg, rgba(18, 31, 50, 0.75), rgba(9, 17, 29, 0.75));
    }
    .panel-title {
      margin: 0;
      font-size: 13px;
      letter-spacing: 0.3px;
      text-transform: uppercase;
      color: #b8c8dc;
    }
    .panel-sub {
      font-size: 11px;
      color: var(--fg-dim);
    }
    .content {
      padding: 10px 12px 12px;
    }
    .bars {
      display: grid;
      gap: 6px;
    }
    .bar-row {
      display: grid;
      grid-template-columns: 140px 1fr 44px;
      gap: 8px;
      align-items: center;
      font-size: 11px;
    }
    .bar-name {
      color: #bdd0ea;
      overflow: hidden;
      text-overflow: ellipsis;
      white-space: nowrap;
    }
    .bar-wrap {
      height: 9px;
      border-radius: 999px;
      background: rgba(42, 58, 83, 0.65);
      border: 1px solid rgba(53, 74, 103, 0.75);
      overflow: hidden;
    }
    .bar-fill {
      height: 100%;
      background: linear-gradient(90deg, var(--teal), var(--violet));
      box-shadow: 0 0 8px rgba(20, 184, 166, 0.32);
    }
    .bar-val {
      text-align: right;
      color: #a8bfd8;
      font-size: 10px;
    }
    table {
      width: 100%;
      border-collapse: collapse;
      font-size: 11px;
    }
    th, td {
      text-align: left;
      padding: 7px 8px;
      border-bottom: 1px solid rgba(41, 58, 81, 0.72);
      vertical-align: top;
    }
    th {
      position: sticky;
      top: 0;
      background: #111e31;
      color: #cbdaec;
      z-index: 1;
      font-weight: 700;
    }
    tbody tr:nth-child(even) {
      background: rgba(10, 16, 30, 0.42);
    }
    td.dim {
      color: var(--fg-dim);
    }
    .status {
      font-weight: 700;
      text-transform: lowercase;
    }
    .scroll {
      max-height: 355px;
      overflow: auto;
    }
    .control {
      display: inline-flex;
      align-items: center;
      gap: 7px;
      color: var(--fg-dim);
      font-size: 11px;
    }
    select {
      background: rgba(9, 15, 26, 0.96);
      color: #dbeafe;
      border: 1px solid #3b5273;
      border-radius: 7px;
      padding: 2px 5px;
      font-family: inherit;
      font-size: 11px;
    }
    @media (max-width: 1180px) {
      .layout {
        grid-template-columns: 1fr;
      }
    }
  </style>
</head>
<body>
  <div class="shell">
    <div class="header">
      <div>
        <h1 class="title">iceccd live dashboard</h1>
        <div class="meta-line" id="meta">connecting...</div>
      </div>
      <div class="chip">
        <span class="dot" id="scheduler-dot"></span>
        <span id="scheduler-chip">scheduler: unknown</span>
      </div>
    </div>

    <div class="metrics">
      <div class="card"><div class="label">Scheduler</div><div class="value small" id="scheduler">-</div></div>
      <div class="card"><div class="label">Slot usage</div><div class="value" id="slots">-</div></div>
      <div class="card"><div class="label">Connected clients</div><div class="value info" id="clients-total">-</div></div>
      <div class="card"><div class="label">FD usage</div><div class="value" style="color:#fbbf24" id="fds">-</div></div>
      <div class="card"><div class="label">Waiting for scheduler</div><div class="value warn" id="waitforcs">-</div></div>
      <div class="card"><div class="label">Waiting remote compile</div><div class="value" style="color:#a78bfa" id="waitcompile">-</div></div>
      <div class="card"><div class="label">Local queue</div><div class="value" style="color:#60a5fa" id="tocompile">-</div></div>
      <div class="card"><div class="label">Local child running</div><div class="value good" id="waitforchild">-</div></div>
      <div class="card"><div class="label">Current load</div><div class="value" id="load">-</div></div>
    </div>

    <div class="layout">
      <div class="panel">
        <div class="panel-head">
          <h2 class="panel-title">Status Distribution</h2>
          <div class="panel-sub">share of live clients</div>
        </div>
        <div class="content">
          <div class="bars" id="status-bars"></div>
        </div>
      </div>
      <div class="panel">
        <div class="panel-head">
          <h2 class="panel-title">Running / Pending Clients</h2>
          <div class="panel-sub" id="clients-sub">0 rows</div>
        </div>
        <div class="scroll">
          <table>
            <thead>
              <tr><th>client</th><th>status</th><th>age(ms)</th><th>job</th><th>target/env</th><th>host</th><th>why</th></tr>
            </thead>
            <tbody id="clients-body"></tbody>
          </table>
        </div>
      </div>
    </div>

    <div class="panel" style="margin-top:12px;">
      <div class="panel-head">
        <h2 class="panel-title">Recent Jobs (in-memory ring buffer)</h2>
        <div class="control">
          <label for="job-limit">rows</label>
          <select id="job-limit">
            <option value="200">200</option>
            <option value="500" selected>500</option>
            <option value="1000">1000</option>
            <option value="5000">5000</option>
            <option value="20000">20000</option>
          </select>
          <span id="jobs-sub">0 rows</span>
        </div>
      </div>
      <div class="scroll" style="max-height:460px;">
        <table>
          <thead>
            <tr><th>seq</th><th>client</th><th>duration(ms)</th><th>exit</th><th>final</th><th>scheduler/compile job</th><th>target/env</th><th>remote host</th><th>why</th></tr>
          </thead>
          <tbody id="jobs-body"></tbody>
        </table>
      </div>
    </div>
  </div>
  <script>
    function fmt(value) { return value === null || value === undefined || value === "" ? "-" : String(value); }
    function setText(id, value) { document.getElementById(id).textContent = fmt(value); }
    function statusClass(status) {
      if (status === "waitforcs") return "warn";
      if (status === "waitcompile" || status === "clientwork" || status === "waitforchild") return "info";
      if (status === "jobdone") return "good";
      if (status === "unknown") return "bad";
      return "";
    }
    function makeCell(tr, text, className) {
      const td = document.createElement("td");
      td.textContent = fmt(text);
      if (className) td.className = className;
      tr.appendChild(td);
    }
    function renderStatusBars(byStatus, total) {
      const bars = document.getElementById("status-bars");
      bars.innerHTML = "";
      const rows = [];
      for (const [name, meta] of Object.entries(byStatus)) {
        const count = Number(meta && meta.count || 0);
        if (!count) continue;
        rows.push({ name, count });
      }
      rows.sort((a, b) => b.count - a.count);
      if (!rows.length) {
        const empty = document.createElement("div");
        empty.className = "panel-sub";
        empty.textContent = "No active clients";
        bars.appendChild(empty);
        return;
      }
      for (const row of rows) {
        const pct = total > 0 ? (100 * row.count / total) : 0;
        const root = document.createElement("div");
        root.className = "bar-row";
        const name = document.createElement("div");
        name.className = "bar-name";
        name.textContent = row.name;
        const wrap = document.createElement("div");
        wrap.className = "bar-wrap";
        const fill = document.createElement("div");
        fill.className = "bar-fill";
        fill.style.width = `${Math.max(2, pct)}%`;
        wrap.appendChild(fill);
        const val = document.createElement("div");
        val.className = "bar-val";
        val.textContent = `${row.count} (${pct.toFixed(1)}%)`;
        root.appendChild(name);
        root.appendChild(wrap);
        root.appendChild(val);
        bars.appendChild(root);
      }
    }
    function renderClients(rows) {
      const body = document.getElementById("clients-body");
      body.innerHTML = "";
      const limit = 600;
      const clipped = rows.slice(0, limit);
      for (const row of clipped) {
        const tr = document.createElement("tr");
        const job = row.job || {};
        const usecs = row.usecs || {};
        makeCell(tr, row.client_id);
        makeCell(tr, row.status, `status ${statusClass(row.status)}`);
        makeCell(tr, row.age_msec);
        makeCell(tr, row.scheduler_job_id);
        makeCell(tr, `${fmt(job.target)} / ${fmt(job.env)}`);
        makeCell(tr, `${fmt(usecs.hostname)}:${fmt(usecs.port)}`);
        makeCell(tr, row.why, "dim");
        body.appendChild(tr);
      }
      setText("clients-sub", `${rows.length} rows`);
    }
    function renderJobs(rows) {
      const body = document.getElementById("jobs-body");
      body.innerHTML = "";
      for (const row of rows) {
        const tr = document.createElement("tr");
        makeCell(tr, row.seq);
        makeCell(tr, row.client_id);
        makeCell(tr, row.duration_msec);
        makeCell(tr, row.exitcode);
        makeCell(tr, row.final_status, `status ${statusClass(row.final_status)}`);
        makeCell(tr, `${fmt(row.scheduler_job_id)} / ${fmt(row.compile_job_id)}`);
        makeCell(tr, `${fmt(row.target)} / ${fmt(row.environment)}`);
        makeCell(tr, `${fmt(row.usecs_host)}:${fmt(row.usecs_port)}`);
        makeCell(tr, row.final_why, "dim");
        body.appendChild(tr);
      }
      setText("jobs-sub", `${rows.length} rows`);
    }
    async function refresh() {
      try {
        const limit = Number(document.getElementById("job-limit").value || "500");
        const [stateRes, clientsRes, jobsRes] = await Promise.all([
          fetch("/api/state", { cache: "no-store" }),
          fetch("/api/clients", { cache: "no-store" }),
          fetch(`/api/jobs?limit=${limit}`, { cache: "no-store" })
        ]);
        if (!stateRes.ok || !clientsRes.ok || !jobsRes.ok) {
          throw new Error(`HTTP ${stateRes.status}/${clientsRes.status}/${jobsRes.status}`);
        }
        const state = await stateRes.json();
        const clients = await clientsRes.json();
        const jobs = await jobsRes.json();

        const by = state.clients.by_status;
        const schedulerConnected = !!state.scheduler.connected;
        setText("meta", `${state.node} | ts=${state.ts} | refreshed=${new Date().toLocaleTimeString()}`);
        setText("scheduler", schedulerConnected ? state.scheduler.name : "disconnected");
        setText("slots", `${state.slots.used} / ${state.slots.max_kids}`);
        setText("clients-total", state.clients.total);
        if (state.fds && state.fds.soft_limit > 0) {
          setText("fds", `${state.fds.open} / ${state.fds.soft_limit} (${state.fds.util_pct}%)`);
        } else if (state.fds) {
          setText("fds", state.fds.open);
        } else {
          setText("fds", "-");
        }
        setText("waitforcs", by.waitforcs.count);
        setText("waitcompile", by.waitcompile.count);
        setText("tocompile", by.tocompile.count);
        setText("waitforchild", by.waitforchild.count);
        setText("load", state.stats.current_load);
        setText("scheduler-chip", schedulerConnected ? `scheduler: ${state.scheduler.name}` : "scheduler: disconnected");
        document.getElementById("scheduler-dot").className = schedulerConnected ? "dot good" : "dot";

        renderStatusBars(by, Number(state.clients.total || 0));

        renderClients(clients.clients || []);
        renderJobs(jobs.jobs || []);
      } catch (error) {
        setText("meta", "error: " + error);
      }
    }
    document.getElementById("job-limit").addEventListener("change", refresh);
    setInterval(refresh, 2000);
    refresh();
  </script>
</body>
</html>)HTML";
}

string Daemon::dump_clients_json() const
{
    const uint64_t now_msec = monotonic_msec();
    vector<pair<uint64_t, const Client *>> ordered;
    ordered.reserve(clients.size());

    for (const auto &it : clients) {
        const Client *client = it.second;
        ordered.push_back(make_pair(now_msec - client->status_since_msec, client));
    }

    sort(ordered.begin(), ordered.end(),
         [](const pair<uint64_t, const Client *> &a, const pair<uint64_t, const Client *> &b) {
             return a.first > b.first;
         });

    ostringstream o;
    o << "{";
    o << "\"type\":\"iceccd_clients\",";
    o << "\"ts\":" << (long long)time(nullptr) << ",";
    o << "\"mono_msec\":" << (unsigned long long)now_msec << ",";
    o << "\"total\":" << ordered.size() << ",";
    o << "\"clients\":[";

    for (size_t i = 0; i < ordered.size(); ++i) {
        const uint64_t age_msec = ordered[i].first;
        const Client *client = ordered[i].second;
        if (i) {
            o << ",";
        }

        o << "{";
        o << "\"client_id\":" << client->client_id << ",";
        o << "\"status\":\"" << Client::status_str(client->status) << "\",";
        o << "\"age_msec\":" << (unsigned long long)age_msec << ",";
        o << "\"why\":\"" << json_escape(client->status_why) << "\",";
        o << "\"scheduler_job_id\":" << client->last_known_job_id << ",";
        o << "\"last_waitforcs_msec\":" << (unsigned long long)client->last_waitforcs_msec << ",";
        o << "\"env_bytes_received\":" << (unsigned long long)client->env_bytes_received << ",";
        o << "\"job\":";
        if (client->job) {
            o << "{";
            o << "\"job_id\":" << client->job->jobID() << ",";
            o << "\"target\":\"" << json_escape(client->job->targetPlatform()) << "\",";
            o << "\"env\":\"" << json_escape(client->job->environmentVersion()) << "\"";
            o << "}";
        } else {
            o << "null";
        }
        o << ",";
        o << "\"usecs\":";
        if (client->usecsmsg) {
            o << "{";
            o << "\"hostname\":\"" << json_escape(client->usecsmsg->hostname) << "\",";
            o << "\"port\":" << client->usecsmsg->port << ",";
            o << "\"got_env\":" << (client->usecsmsg->got_env ? "true" : "false") << ",";
            o << "\"host_platform\":\"" << json_escape(client->usecsmsg->host_platform) << "\",";
            o << "\"matched_job_id\":" << client->usecsmsg->matched_job_id;
            o << "}";
        } else {
            o << "null";
        }
        o << ",";
        o << "\"outfile\":\"" << json_escape(client->outfile) << "\",";
        o << "\"channel\":\"" << json_escape(client->channel ? client->channel->dump() : string()) << "\"";
        o << "}";
    }

    o << "]";
    o << "}";
    return o.str();
}

bool Daemon::should_track_client_job(const Client *client)
{
    if (!client) {
        return false;
    }

    if (client->job || client->job_id || client->last_known_job_id || client->usecsmsg || !client->outfile.empty()) {
        return true;
    }

    switch (client->status) {
    case Client::PENDING_USE_CS:
    case Client::JOBDONE:
    case Client::LINKJOB:
    case Client::TOCOMPILE:
    case Client::WAITFORCS:
    case Client::WAITCOMPILE:
    case Client::CLIENTWORK:
    case Client::WAITFORCHILD:
        return true;
    default:
        return false;
    }
}

void Daemon::remember_finished_job(const Client *client, int exitcode)
{
    if (!should_track_client_job(client)) {
        return;
    }

    JobHistoryEntry entry;
    entry.seq = next_job_history_seq++;
    entry.start_ts = client->created_ts;
    entry.end_ts = time(nullptr);
    entry.start_msec = client->created_msec;
    entry.end_msec = monotonic_msec();
    entry.duration_msec = entry.end_msec >= entry.start_msec ? (entry.end_msec - entry.start_msec) : 0;
    entry.client_id = client->client_id;
    entry.exitcode = exitcode;
    entry.scheduler_job_id = client->last_known_job_id;
    entry.compile_job_id = client->job ? client->job->jobID() : 0;
    entry.final_status = Client::status_str(client->status);
    entry.final_why = client->status_why;
    entry.target = client->job ? client->job->targetPlatform() : string();
    entry.environment = client->job ? client->job->environmentVersion() : string();
    entry.usecs_host = client->usecsmsg ? client->usecsmsg->hostname : string();
    entry.usecs_port = client->usecsmsg ? client->usecsmsg->port : 0;
    entry.usecs_got_env = client->usecsmsg ? client->usecsmsg->got_env : false;
    entry.outfile = client->outfile;
    entry.channel = client->channel ? client->channel->dump() : string();

    if (job_history.size() >= job_history_capacity) {
        job_history.pop_front();
    }
    job_history.push_back(entry);
}

string Daemon::dump_job_history_json(size_t limit) const
{
    if (limit == 0 || limit > job_history.size()) {
        limit = job_history.size();
    }

    ostringstream o;
    o << "{";
    o << "\"type\":\"iceccd_job_history\",";
    o << "\"ts\":" << (long long)time(nullptr) << ",";
    o << "\"capacity\":" << job_history_capacity << ",";
    o << "\"size\":" << job_history.size() << ",";
    o << "\"returned\":" << limit << ",";
    o << "\"jobs\":[";

    for (size_t i = 0; i < limit; ++i) {
        const JobHistoryEntry &entry = job_history[job_history.size() - 1 - i];
        if (i) {
            o << ",";
        }

        o << "{";
        o << "\"seq\":" << (unsigned long long)entry.seq << ",";
        o << "\"client_id\":" << entry.client_id << ",";
        o << "\"start_ts\":" << (long long)entry.start_ts << ",";
        o << "\"end_ts\":" << (long long)entry.end_ts << ",";
        o << "\"start_msec\":" << (unsigned long long)entry.start_msec << ",";
        o << "\"end_msec\":" << (unsigned long long)entry.end_msec << ",";
        o << "\"duration_msec\":" << (unsigned long long)entry.duration_msec << ",";
        o << "\"exitcode\":" << entry.exitcode << ",";
        o << "\"scheduler_job_id\":" << entry.scheduler_job_id << ",";
        o << "\"compile_job_id\":" << entry.compile_job_id << ",";
        o << "\"final_status\":\"" << json_escape(entry.final_status) << "\",";
        o << "\"final_why\":\"" << json_escape(entry.final_why) << "\",";
        o << "\"target\":\"" << json_escape(entry.target) << "\",";
        o << "\"environment\":\"" << json_escape(entry.environment) << "\",";
        o << "\"usecs_host\":\"" << json_escape(entry.usecs_host) << "\",";
        o << "\"usecs_port\":" << entry.usecs_port << ",";
        o << "\"usecs_got_env\":" << (entry.usecs_got_env ? "true" : "false") << ",";
        o << "\"outfile\":\"" << json_escape(entry.outfile) << "\",";
        o << "\"channel\":\"" << json_escape(entry.channel) << "\"";
        o << "}";
    }

    o << "]";
    o << "}";
    return o.str();
}

void Daemon::handle_web_accept()
{
    if (web_listen_fd < 0) {
        return;
    }

    for (;;) {
        sockaddr_storage addr;
        socklen_t addr_len = sizeof(addr);
        int fd = accept(web_listen_fd, reinterpret_cast<sockaddr *>(&addr), &addr_len);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                return;
            }
            note_accept_error("webgui", errno);
            return;
        }

        int flags = fcntl(fd, F_GETFL, 0);
        if (flags >= 0) {
            fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        }
        fcntl(fd, F_SETFD, FD_CLOEXEC);

        WebConnection conn;
        conn.fd = fd;
        conn.created_msec = monotonic_msec();
        web_connections[fd] = conn;
    }
}

void Daemon::handle_web_connection(int fd, short revents)
{
    auto it = web_connections.find(fd);
    if (it == web_connections.end()) {
        return;
    }

    if (revents & (POLLERR | POLLHUP | POLLNVAL)) {
        drop_web_connection(fd);
        return;
    }

    WebConnection &conn = it->second;

    if (revents & POLLIN) {
        for (;;) {
            char buf[4096];
            ssize_t n = read(fd, buf, sizeof(buf));
            if (n > 0) {
                conn.inbuf.append(buf, n);
                if (conn.inbuf.size() > 64 * 1024) {
                    queue_web_response(fd, 413, "Payload Too Large", "text/plain; charset=utf-8",
                                       "request too large\n");
                    break;
                }
                continue;
            }
            if (n == 0) {
                drop_web_connection(fd);
                return;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                break;
            }
            drop_web_connection(fd);
            return;
        }

        if (conn.outbuf.empty() && conn.inbuf.find("\r\n\r\n") != string::npos) {
            string method;
            string path;
            if (!parse_http_request(conn.inbuf, method, path)) {
                queue_web_response(fd, 400, "Bad Request", "text/plain; charset=utf-8", "bad request\n");
            } else if (method != "GET") {
                queue_web_response(fd, 405, "Method Not Allowed", "text/plain; charset=utf-8", "only GET supported\n");
            } else {
                string route = path;
                const size_t query_pos = route.find('?');
                if (query_pos != string::npos) {
                    route.erase(query_pos);
                }

                if (route == "/" || route == "/index.html") {
                    queue_web_response(fd, 200, "OK", "text/html; charset=utf-8", webgui_html());
                } else if (route == "/api/state") {
                    queue_web_response(fd, 200, "OK", "application/json; charset=utf-8", dump_state_json());
                } else if (route == "/api/clients") {
                    queue_web_response(fd, 200, "OK", "application/json; charset=utf-8", dump_clients_json());
                } else if (route == "/api/jobs") {
                    const size_t limit = parse_jobs_limit(path);
                    queue_web_response(fd, 200, "OK", "application/json; charset=utf-8", dump_job_history_json(limit));
                } else if (route == "/api/internals") {
                    queue_web_response(fd, 200, "OK", "text/plain; charset=utf-8", dump_internals());
                } else {
                    queue_web_response(fd, 404, "Not Found", "text/plain; charset=utf-8", "not found\n");
                }
            }
        }
    }

    if ((revents & POLLOUT) && !conn.outbuf.empty()) {
        while (!conn.outbuf.empty()) {
            ssize_t n = write(fd, conn.outbuf.data(), conn.outbuf.size());
            if (n > 0) {
                conn.outbuf.erase(0, n);
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
                break;
            }
            drop_web_connection(fd);
            return;
        }

        if (conn.outbuf.empty() && conn.close_after_write) {
            drop_web_connection(fd);
            return;
        }
    }
}

void Daemon::determine_system()
{
    struct utsname uname_buf;

    if (uname(&uname_buf)) {
        log_perror("uname call failed. Unable to determine system node name and platform");
        return;
    }

    if (nodename.length() && (nodename != uname_buf.nodename)) {
        custom_nodename  = true;
    }

    if (!custom_nodename) {
        nodename = uname_buf.nodename;
    }

    machine_name = determine_platform();
}

string Daemon::determine_nodename()
{
    if (custom_nodename && !nodename.empty()) {
        return nodename;
    }

    // perhaps our host name changed due to network change?
    struct utsname uname_buf;

    if (!uname(&uname_buf)) {
        nodename = uname_buf.nodename;
    }

    return nodename;
}

void Daemon::determine_supported_features()
{
    supported_features = 0;
    struct archive* a = archive_read_new();
    static bool test_disable = false;
    // Make one of the two remotes in tests say it doesn't support xz/zstd tarballs.
    if( getenv( "ICECC_TESTS" ) != nullptr && nodename == "remoteice2" )
        test_disable = true;
    (void)test_disable;
#ifdef HAVE_LIBARCHIVE_XZ
    if( !test_disable && archive_read_support_filter_xz(a) >= ARCHIVE_WARN ) // includes ARCHIVE_OK
        supported_features = supported_features | NODE_FEATURE_ENV_XZ;
#endif
#ifdef HAVE_LIBARCHIVE_ZSTD
    if( !test_disable && archive_read_support_filter_zstd(a) >= ARCHIVE_WARN ) // includes ARCHIVE_OK
        supported_features = supported_features | NODE_FEATURE_ENV_ZSTD;
#endif
    // sanity checks
    if( archive_read_support_filter_gzip(a) < ARCHIVE_WARN ) // error
        log_error() << "No support for uncompressing gzip available." << endl;
    if( archive_read_support_format_tar(a) < ARCHIVE_WARN ) // error
        log_error() << "No support for unpacking tar available." << endl;
    archive_read_free(a);
}

bool Daemon::send_scheduler(const Msg& msg)
{
    if (!scheduler) {
        log_warning() << "no scheduler" << endl;
        return false;
    }

    if (!scheduler->send_msg(msg)) {
        log_error() << "sending message to scheduler failed.." << endl;
        close_scheduler();
        return false;
    }

    return true;
}

bool Daemon::reannounce_environments()
{
    log_info() << "reannounce_environments " << endl;
    LoginMsg lmsg(0, nodename, "", supported_features);
    lmsg.envs = available_environments(envbasedir);
    return send_scheduler(lmsg);
}

void Daemon::close_scheduler()
{
    if (!scheduler) {
        return;
    }

    delete scheduler;
    scheduler = nullptr;
    delete discover;
    discover = nullptr;
    next_scheduler_connect = time(nullptr) + 20 + (rand() & 31);
    static bool fast_reconnect = getenv( "ICECC_TESTS" ) != nullptr;
    if( fast_reconnect )
        next_scheduler_connect = time(nullptr) + 3;
}

bool Daemon::maybe_stats(bool force_check)
{
    struct timeval now;
    gettimeofday(&now, nullptr);

    time_t diff_sent = (now.tv_sec - last_stat.tv_sec) * 1000 + (now.tv_usec - last_stat.tv_usec) / 1000;

    if (diff_sent >= max_scheduler_pong * 1000 || force_check) {
        StatsMsg msg;
        unsigned int memory_fillgrade;
        unsigned long idleLoad = 0;
        unsigned long niceLoad = 0;

        fill_stats(idleLoad, niceLoad, memory_fillgrade, &msg, clients.active_processes);

        time_t diff_stat = (now.tv_sec - last_stat.tv_sec) * 1000 + (now.tv_usec - last_stat.tv_usec) / 1000;
        last_stat = now;

        /* icecream_load contains time in milliseconds we have used for icecream */
        /* idle time could have been used for icecream, so claim it */
        icecream_load += idleLoad * diff_stat / 1000;

        /* add the time of our childrens, but only the time since the last run */
        struct rusage ru;

        if (!getrusage(RUSAGE_CHILDREN, &ru)) {
            uint32_t ice_msec = ((ru.ru_utime.tv_sec - icecream_usage.tv_sec) * 1000
                                 + (ru.ru_utime.tv_usec - icecream_usage.tv_usec) / 1000) / num_cpus;

            /* heuristics when no child terminated yet: account 25% of total nice as our clients */
            if (!ice_msec && current_kids) {
                ice_msec = (niceLoad * diff_stat) / (4 * 1000);
            }

            icecream_load += ice_msec * diff_stat / 1000;

            icecream_usage.tv_sec = ru.ru_utime.tv_sec;
            icecream_usage.tv_usec = ru.ru_utime.tv_usec;
        }

        unsigned int idle_average = icecream_load;

        if (diff_sent) {
            idle_average = icecream_load * 1000 / diff_sent;
        }

        if (idle_average > 1000)
           idle_average = 1000;

        msg.load = std::max((1000 - idle_average), memory_fillgrade);

#ifdef HAVE_SYS_VFS_H
        struct statfs buf;
        int ret = statfs(envbasedir.c_str(), &buf);

        // Require at least 25MiB of free disk space per build.
        if (!ret && long(buf.f_bavail) < ((long(max_kids + 1 - current_kids) * 25 * 1024 * 1024) / buf.f_bsize)) {
            msg.load = 1000;
        }

#endif

        mem_limit = std::max(int(msg.freeMem / std::min(std::max(max_kids, 1U), 4U)), min_mem_limit);

        if (abs(int(msg.load) - current_load) >= 100
            || (msg.load == 1000 && current_load != 1000)
            || (msg.load != 1000 && current_load == 1000)) {
            if (!send_scheduler(msg)) {
                return false;
            }
        }

        icecream_load = 0;
        current_load = msg.load;
    }

    return true;
}

string Daemon::dump_internals() const
{
    string result;
    const uint64_t now_msec = monotonic_msec();

    result += "Node Name: " + nodename + "\n";
    result += "  Remote name: " + remote_name + "\n";

    struct StatusAgg {
        uint32_t count = 0;
        uint64_t total_age_msec = 0;
        uint64_t max_age_msec = 0;
    };
    vector<StatusAgg> status_aggs(Client::LASTSTATE + 1);
    for (const auto &it : clients) {
        const Client *client = it.second;
        StatusAgg &agg = status_aggs[int(client->status)];
        ++agg.count;
        const uint64_t age_msec = now_msec - client->status_since_msec;
        agg.total_age_msec += age_msec;
        agg.max_age_msec = std::max(agg.max_age_msec, age_msec);
    }

    result += "  Clients: " + toString(clients.size()) + " (" + clients.dump_per_status() + ")\n";
    result += "  Slots: active_processes=" + toString(clients.active_processes)
        + ", current_kids=" + toString(current_kids)
        + ", used=" + toString(current_kids + clients.active_processes)
        + ", max_kids=" + toString(max_kids) + "\n";

    const FdSnapshot fd_snapshot = collect_fd_snapshot();
    result += "  FDs: open=" + toString(fd_snapshot.open_count);
    if (fd_snapshot.soft_limit != RLIM_INFINITY) {
        result += ", soft_limit=" + toString((unsigned long long)fd_snapshot.soft_limit);
    } else {
        result += ", soft_limit=unlimited";
    }
    if (fd_snapshot.hard_limit != RLIM_INFINITY) {
        result += ", hard_limit=" + toString((unsigned long long)fd_snapshot.hard_limit);
    } else {
        result += ", hard_limit=unlimited";
    }
    result += ", accept_errors_total=" + toString((unsigned long long)accept_errors_total)
        + ", accept_emfile_errors=" + toString((unsigned long long)accept_emfile_errors)
        + "\n";

    if (scheduler) {
        time_t scheduler_last_talk_age_s = time(nullptr) - scheduler->last_talk;
        if (scheduler_last_talk_age_s < 0) {
            scheduler_last_talk_age_s = 0;
        }
        result += "  Scheduler: connected name=" + scheduler->name
            + " proto=" + toString(scheduler->protocol)
            + " last_talk_age_s=" + toString(scheduler_last_talk_age_s) + "\n";
    } else {
        time_t retry_in_s = next_scheduler_connect - time(nullptr);
        if (retry_in_s < 0) {
            retry_in_s = 0;
        }
        result += "  Scheduler: disconnected next_retry_in_s=" + toString(retry_in_s) + "\n";
    }

    auto append_status_wait = [&](Client::Status status, const string &label) {
        const StatusAgg &agg = status_aggs[int(status)];
        if (!agg.count) {
            return;
        }
        result += "  Wait " + label
            + ": count=" + toString(agg.count)
            + " avg_age_msec=" + toString(agg.total_age_msec / agg.count)
            + " max_age_msec=" + toString(agg.max_age_msec) + "\n";
    };
    append_status_wait(Client::WAITFORCS, "scheduler(waitforcs)");
    append_status_wait(Client::WAITCOMPILE, "remote(waitcompile)");
    append_status_wait(Client::PENDING_USE_CS, "local_slot(pending_use_cs)");
    append_status_wait(Client::TOCOMPILE, "local_queue(tocompile)");
    append_status_wait(Client::WAITFORCHILD, "local_child(waitforchild)");
    append_status_wait(Client::WAITCREATEENV, "create_env(waitcreateenv)");

    {
        const StatusAgg &toinstall = status_aggs[int(Client::TOINSTALL)];
        const StatusAgg &waitinstall = status_aggs[int(Client::WAITINSTALL)];
        const uint32_t count = toinstall.count + waitinstall.count;
        if (count) {
            const uint64_t total_age_msec = toinstall.total_age_msec + waitinstall.total_age_msec;
            const uint64_t max_age_msec = std::max(toinstall.max_age_msec, waitinstall.max_age_msec);
            result += "  Wait env_install(toinstall+waitinstall): count=" + toString(count)
                + " avg_age_msec=" + toString(total_age_msec / count)
                + " max_age_msec=" + toString(max_age_msec) + "\n";
        }
    }

    for (const auto &it : fd2client)  {
        result += "  fd2client[" + toString(it.first) + "] = " + it.second->dump() + "\n";
    }

    for (const auto& client : clients)  {
        result += "  client " + toString(client.second->client_id) + ": " + client.second->dump() + "\n";
    }

    if (cache_size) {
        result += "  Cache Size: " + toString(cache_size) + "\n";
    }

    result += "  Architecture: " + machine_name + "\n";

    for (const auto & native_environment : native_environments) {
        result += "  NativeEnv (" + native_environment.first + "): " + native_environment.second.name
            + ", size " + toString(native_environment.second.size) + (native_environment.second.create_env_pipe ? " (creating)" : "" ) + "\n";
    }

    if (!received_environments.empty()) {
        result += "  Now: " + toString(time(nullptr)) + "\n";
        for (const auto& it : received_environments )
            result += "  ReceivedEnv[" + it.first  + "] last_use " + toString(it.second.last_use)
                + ", size " + toString(it.second.size) + "\n";
    }

    result += "  Current kids: " + toString(current_kids) + " (max: " + toString(max_kids) + ")\n";

    result += "  Supported features: " + supported_features_to_string(supported_features) + "\n";

    if (scheduler) {
        result += "  Scheduler protocol: " + toString(scheduler->protocol) + "\n";
    }

    StatsMsg msg;
    unsigned int memory_fillgrade = 0;
    unsigned long idleLoad = 0;
    unsigned long niceLoad = 0;

    fill_stats(idleLoad, niceLoad, memory_fillgrade, &msg, clients.active_processes);
    result += "  cpu: " + toString(idleLoad) + " idle, "
              + toString(niceLoad) + " nice\n";
    result += "  load: " + toString(msg.loadAvg1 / 1000.) + ", icecream_load: "
              + toString(icecream_load) + "\n";
    result += "  memory: " + toString(memory_fillgrade)
              + " (free: " + toString(msg.freeMem) + ")\n";

    return result;
}

bool Daemon::append_state_jsonl_line(const std::string &line) const
{
    if (state_jsonl_path.empty()) {
        return true;
    }

    const string data = line + "\n";
    int fd = ::open(state_jsonl_path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) {
        log_error() << "failed to open state jsonl file " << state_jsonl_path
                    << ": " << strerror(errno) << endl;
        return false;
    }

    const char *buf = data.data();
    size_t to_write = data.size();
    while (to_write) {
        ssize_t n = ::write(fd, buf, to_write);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            log_error() << "failed to write state jsonl file " << state_jsonl_path
                        << ": " << strerror(errno) << endl;
            if (-1 == close(fd) && (errno != EBADF)){
                log_perror("Failed to close state jsonl file");
            }
            return false;
        }
        buf += n;
        to_write -= n;
    }

    if (-1 == close(fd) && (errno != EBADF)){
        log_perror("Failed to close state jsonl file");
    }
    return true;
}

std::string Daemon::dump_state_json() const
{
    const time_t now_s = time(nullptr);
    const uint64_t now_msec = monotonic_msec();
    const FdSnapshot fd_snapshot = collect_fd_snapshot();

    StatsMsg msg;
    unsigned int memory_fillgrade = 0;
    unsigned long idleLoad = 0;
    unsigned long niceLoad = 0;
    fill_stats(idleLoad, niceLoad, memory_fillgrade, &msg, clients.active_processes);

    struct StatusAgg {
        uint32_t count = 0;
        uint64_t total_age_msec = 0;
        uint64_t max_age_msec = 0;
    };
    vector<StatusAgg> status_aggs(Client::LASTSTATE + 1);

    auto is_worst_candidate = [](Client::Status s) -> bool {
        switch (s) {
        case Client::WAITFORCS:
        case Client::PENDING_USE_CS:
        case Client::WAITCOMPILE:
        case Client::CLIENTWORK:
        case Client::TOCOMPILE:
        case Client::WAITFORCHILD:
        case Client::TOINSTALL:
        case Client::WAITINSTALL:
        case Client::WAITCREATEENV:
            return true;
        default:
            return false;
        }
    };

    vector<pair<uint64_t, const Client *>> worst_clients;
    worst_clients.reserve(clients.size());

    for (const auto &it : clients) {
        const Client *client = it.second;
        StatusAgg &agg = status_aggs[int(client->status)];
        ++agg.count;
        const uint64_t age_msec = now_msec - client->status_since_msec;
        agg.total_age_msec += age_msec;
        agg.max_age_msec = std::max(agg.max_age_msec, age_msec);
        if (is_worst_candidate(client->status)) {
            worst_clients.push_back(make_pair(age_msec, client));
        }
    }

    std::sort(worst_clients.begin(), worst_clients.end(),
        [](const pair<uint64_t, const Client *> &a, const pair<uint64_t, const Client *> &b) {
            return a.first > b.first;
        });

    ostringstream o;
    o << "{";
    o << "\"type\":\"iceccd_state\",";
    o << "\"ts\":" << (long long)now_s << ",";
    o << "\"mono_msec\":" << (unsigned long long)now_msec << ",";
    o << "\"pid\":" << (long)getpid() << ",";
    o << "\"node\":\"" << json_escape(nodename) << "\",";
    o << "\"remote_name\":\"" << json_escape(remote_name) << "\",";
    o << "\"daemon_port\":" << daemon_port << ",";
    o << "\"netname\":\"" << json_escape(netname) << "\",";
    o << "\"noremote\":" << (noremote ? "true" : "false") << ",";

    o << "\"scheduler\":{";
    if (scheduler) {
        time_t scheduler_last_talk_age_s = time(nullptr) - scheduler->last_talk;
        if (scheduler_last_talk_age_s < 0) {
            scheduler_last_talk_age_s = 0;
        }
        o << "\"connected\":true,";
        o << "\"name\":\"" << json_escape(scheduler->name) << "\",";
        o << "\"protocol\":" << scheduler->protocol << ",";
        o << "\"last_talk_age_s\":" << (long long)scheduler_last_talk_age_s;
    } else {
        time_t retry_in_s = next_scheduler_connect - time(nullptr);
        if (retry_in_s < 0) {
            retry_in_s = 0;
        }
        o << "\"connected\":false,";
        o << "\"next_retry_in_s\":" << (long long)retry_in_s;
    }
    o << "},";

    o << "\"slots\":{";
    o << "\"max_kids\":" << max_kids << ",";
    o << "\"current_kids\":" << current_kids << ",";
    o << "\"active_processes\":" << clients.active_processes << ",";
    o << "\"used\":" << (current_kids + clients.active_processes);
    o << "},";

    o << "\"fds\":{";
    o << "\"open\":" << fd_snapshot.open_count << ",";
    if (fd_snapshot.soft_limit == RLIM_INFINITY) {
        o << "\"soft_limit\":-1,";
    } else {
        o << "\"soft_limit\":" << (unsigned long long)fd_snapshot.soft_limit << ",";
    }
    if (fd_snapshot.hard_limit == RLIM_INFINITY) {
        o << "\"hard_limit\":-1,";
    } else {
        o << "\"hard_limit\":" << (unsigned long long)fd_snapshot.hard_limit << ",";
    }
    long long headroom = -1;
    long long util_pct = -1;
    if (fd_snapshot.open_count >= 0 && fd_snapshot.soft_limit != RLIM_INFINITY) {
        headroom = (long long)fd_snapshot.soft_limit - (long long)fd_snapshot.open_count;
        if (headroom < 0) {
            headroom = 0;
        }
        if (fd_snapshot.soft_limit > 0) {
            util_pct = ((long long)fd_snapshot.open_count * 100) / (long long)fd_snapshot.soft_limit;
        }
    }
    o << "\"headroom\":" << headroom << ",";
    o << "\"util_pct\":" << util_pct << ",";
    o << "\"accept_errors_total\":" << (unsigned long long)accept_errors_total << ",";
    o << "\"accept_emfile_errors\":" << (unsigned long long)accept_emfile_errors << ",";
    o << "\"last_accept_errno\":" << last_accept_errno << ",";
    o << "\"last_accept_errno_ts\":" << (long long)last_accept_errno_ts;
    o << "},";

    o << "\"stats\":{";
    o << "\"current_load\":" << current_load << ",";
    o << "\"loadAvg1\":" << msg.loadAvg1 << ",";
    o << "\"loadAvg5\":" << msg.loadAvg5 << ",";
    o << "\"loadAvg10\":" << msg.loadAvg10 << ",";
    o << "\"cpu_idle\":" << idleLoad << ",";
    o << "\"cpu_nice\":" << niceLoad << ",";
    o << "\"memory_fillgrade\":" << memory_fillgrade << ",";
    o << "\"freeMemMB\":" << msg.freeMem;
    o << "},";

    o << "\"cache\":{";
    o << "\"cache_size\":" << (unsigned long long)cache_size << ",";
    o << "\"cache_size_limit\":" << (unsigned long long)cache_size_limit << ",";
    o << "\"native_envs\":" << native_environments.size() << ",";
    o << "\"received_envs\":" << received_environments.size();
    o << "},";

    o << "\"clients\":{";
    o << "\"total\":" << clients.size() << ",";
    o << "\"by_status\":{";
    bool first = true;
    for (Client::Status s = Client::UNKNOWN; s <= Client::LASTSTATE;
            s = Client::Status(int(s) + 1)) {
        const StatusAgg &agg = status_aggs[int(s)];
        const uint64_t avg_age_msec = agg.count ? (agg.total_age_msec / agg.count) : 0;
        if (!first) {
            o << ",";
        }
        first = false;
        o << "\"" << Client::status_str(s) << "\":{";
        o << "\"count\":" << agg.count << ",";
        o << "\"avg_age_msec\":" << (unsigned long long)avg_age_msec << ",";
        o << "\"max_age_msec\":" << (unsigned long long)agg.max_age_msec;
        o << "}";
    }
    o << "},";

    const size_t worst_limit = std::min(state_dump_worst_clients, worst_clients.size());
    o << "\"worst_limit\":" << worst_limit << ",";
    o << "\"worst\":[";
    for (size_t i = 0; i < worst_limit; ++i) {
        const uint64_t age_msec = worst_clients[i].first;
        const Client *client = worst_clients[i].second;
        if (i) {
            o << ",";
        }
        o << "{";
        o << "\"client_id\":" << client->client_id << ",";
        o << "\"status\":\"" << Client::status_str(client->status) << "\",";
        o << "\"age_msec\":" << (unsigned long long)age_msec << ",";
        o << "\"why\":\"" << json_escape(client->status_why) << "\",";
        o << "\"last_waitforcs_msec\":" << (unsigned long long)client->last_waitforcs_msec << ",";
        o << "\"scheduler_job_id\":" << client->job_id << ",";
        o << "\"job\":";
        if (client->job) {
            o << "{";
            o << "\"job_id\":" << client->job->jobID() << ",";
            o << "\"target\":\"" << json_escape(client->job->targetPlatform()) << "\",";
            o << "\"env\":\"" << json_escape(client->job->environmentVersion()) << "\"";
            o << "}";
        } else {
            o << "null";
        }
        o << ",";
        o << "\"usecs\":";
        if (client->usecsmsg) {
            o << "{";
            o << "\"hostname\":\"" << json_escape(client->usecsmsg->hostname) << "\",";
            o << "\"port\":" << client->usecsmsg->port << ",";
            o << "\"got_env\":" << (client->usecsmsg->got_env ? "true" : "false") << ",";
            o << "\"host_platform\":\"" << json_escape(client->usecsmsg->host_platform) << "\",";
            o << "\"matched_job_id\":" << client->usecsmsg->matched_job_id;
            o << "}";
        } else {
            o << "null";
        }
        o << ",";
        o << "\"outfile\":\"" << json_escape(client->outfile) << "\",";
        o << "\"pending_create_env\":\"" << json_escape(client->pending_create_env) << "\",";
        o << "\"env_bytes_received\":" << (unsigned long long)client->env_bytes_received << ",";
        o << "\"channel\":\"" << json_escape(client->channel ? client->channel->dump() : string()) << "\"";
        o << "}";
    }
    o << "]";

    o << "}";
    o << "}";

    return o.str();
}

void Daemon::maybe_dump_state()
{
    if (state_dump_interval_s == 0 || (state_jsonl_path.empty() && !state_dump_log)) {
        return;
    }

    const uint64_t now = monotonic_msec();
    if (!next_state_dump_msec) {
        next_state_dump_msec = now;
    }

    if (now < next_state_dump_msec) {
        return;
    }

    const string line = dump_state_json();

    if (state_dump_log) {
        // Write raw JSON to the same output as error logs, regardless of verbosity.
        // This keeps the line machine-parsable and avoids requiring `-vv`.
        if (logfile_error) {
            (*logfile_error) << line << "\n";
            logfile_error->flush();
        }
    }

    append_state_jsonl_line(line);

    const uint64_t interval_msec = uint64_t(state_dump_interval_s) * 1000;
    do {
        next_state_dump_msec += interval_msec;
    } while (next_state_dump_msec <= now);
}

int Daemon::scheduler_get_internals()
{
    trace() << "handle_get_internals " << dump_internals() << endl;
    return send_scheduler(StatusTextMsg(dump_internals())) ? 0 : 1;
}

int Daemon::scheduler_use_cs(UseCSMsg *msg)
{
    Client *c = clients.find_by_client_id(msg->client_id);
    trace() << "scheduler_use_cs " << msg->job_id << " " << msg->client_id
            << " " << c << " " << msg->hostname << " " << remote_name <<  endl;

    if (!c) {
        if (send_scheduler(JobDoneMsg(msg->job_id, 107, JobDoneMsg::FROM_SUBMITTER, clients.size()))) {
            return 0;
        }

        return 1;
    }

    if (c->status == Client::WAITFORCS) {
        c->last_waitforcs_msec = monotonic_msec() - c->status_since_msec;
    }

    if (msg->hostname == remote_name && int(msg->port) == daemon_port) {
        c->usecsmsg = new UseCSMsg(msg->host_platform, "127.0.0.1", daemon_port, msg->job_id, true, 1,
                                   msg->matched_job_id);
        c->set_status(Client::PENDING_USE_CS, "scheduler_use_cs: local compile");
    } else {
        c->usecsmsg = new UseCSMsg(msg->host_platform, msg->hostname, msg->port,
                                   msg->job_id, true, 1, msg->matched_job_id);

        if (!c->channel->send_msg(*msg)) {
            handle_end(c, 143);
            return 0;
        }

        c->set_status(Client::WAITCOMPILE, "scheduler_use_cs: remote compile");
    }

    c->job_id = msg->job_id;
    c->last_known_job_id = msg->job_id;

    return 0;
}

int Daemon::scheduler_no_cs(NoCSMsg *msg)
{
    Client *c = clients.find_by_client_id(msg->client_id);
    trace() << "scheduler_no_cs " << msg->job_id << " " << msg->client_id
            << " " << c << " " <<  endl;

    if (!c) {
        if (send_scheduler(JobDoneMsg(msg->job_id, 107, JobDoneMsg::FROM_SUBMITTER, clients.size()))) {
            return 0;
        }

        return 1;
    }

    if (c->status == Client::WAITFORCS) {
        c->last_waitforcs_msec = monotonic_msec() - c->status_since_msec;
    }

    c->usecsmsg = new UseCSMsg(string(), "127.0.0.1", daemon_port, msg->job_id, true, 1, 0);
    c->set_status(Client::PENDING_USE_CS, "scheduler_no_cs: local compile");

    c->job_id = msg->job_id;
    c->last_known_job_id = msg->job_id;

    return 0;

}

bool Daemon::handle_transfer_env(Client *client, EnvTransferMsg *emsg)
{
    log_info() << "handle_transfer_env, client status " << Client::status_str(client->status) <<  endl;

    assert(client->status != Client::TOINSTALL &&
           client->status != Client::WAITINSTALL &&
           client->status != Client::TOCOMPILE &&
           client->status != Client::WAITCOMPILE);
    assert(client->pipe_from_child < 0);
    assert(client->pipe_to_child < 0);

    string target = emsg->target;

    if (target.empty()) {
        target =  machine_name;
    }

    int pipe_from_child = -1;
    int pipe_to_child = -1;
    FileChunkMsg *fmsg = nullptr;

    pid_t pid = start_install_environment(envbasedir, target, emsg->name, client->channel,
                    pipe_to_child, pipe_from_child, fmsg, user_uid, user_gid, nice_level);

    if( pid <= 0 ) {
        delete fmsg;
        remove_environment_files(envbasedir, target + "/" + emsg->name);
        handle_end(client, 144);
        return false;
    }

    client->set_status(Client::TOINSTALL, "handle_transfer_env: receiving environment");
    client->env_bytes_received = 0;
    client->outfile = target + "/" + emsg->name;
    current_kids++;

    trace() << "PID of child thread running untaring environment: " << pid << endl;
    client->pipe_to_child = pipe_to_child;
    client->pipe_from_child = pipe_from_child;
    client->child_pid = pid;

    if (!handle_file_chunk_env(client, fmsg)) {
        delete fmsg;
        return false;
    }

    delete fmsg;
    return true;
}

bool Daemon::handle_file_chunk_env(Client *client, Msg *msg)
{
    /* this sucks, we can block when we're writing
       the file chunk to the child, but we can't let the child
       handle MsgChannel itself due to MsgChannel's stupid
       caching layer inbetween, which causes us to lose partial
       data after the END msg of the env transfer.  */

    assert(client);
    assert(client->status == Client::TOINSTALL || client->status == Client::WAITINSTALL);
    assert(client->pipe_to_child >= 0);

    if (*msg == Msg::FILE_CHUNK) {
        FileChunkMsg *fcmsg = static_cast<FileChunkMsg *>(msg);
        client->env_bytes_received += fcmsg->len;
        ssize_t len = fcmsg->len;
        off_t off = 0;

        while (len) {
            ssize_t bytes = write(client->pipe_to_child, fcmsg->buffer + off, len);

            if (bytes < 0 && errno == EINTR) {
                continue;
            }
            if (bytes < 0 && errno == EPIPE) {
                // Broken pipe may mean the unpacking has failed, but it also may
                // mean the child has already finished successfully (it seems to happen,
                // maybe some tar implementations add needless trailing bytes?).
                // Wait for the child to finish to find out whether it was ok.
                return true;
            }

            if (bytes == -1) {
                log_perror("write to transfer env pipe failed.");
                handle_end(client, 137);
                return false;
            }

            len -= bytes;
            off += bytes;
        }

        return true;
    }

    if (*msg == Msg::END) {
        trace() << "received end of environment, waiting for child" << endl;
        close(client->pipe_to_child);
        client->pipe_to_child = -1;
        if( client->child_pid >= 0 ) {
            // Transfer done, wait for handle_transfer_env_child_done() to finish the handling.
            client->set_status(Client::WAITINSTALL, "handle_file_chunk_env: waiting for env install child"); // Ignore further messages until child finishes.
            return true;
        }
        // Transfer done, child done, finish.
        return finish_transfer_env( client );
    }

    // unexpected message type
    log_error() << "protocol error while receiving environment (" << msg->to_string() << ")" << endl;
    handle_end(client, 138);
    return false;
}

bool Daemon::handle_env_install_child_done(Client *client)
{
    assert(client->status == Client::TOINSTALL || client->status == Client::WAITINSTALL);
    assert(client->child_pid >= 0);
    assert(client->pipe_from_child >= 0);
    bool success = false;
    for (;;) {
        char resultByte;
        ssize_t n = ::read(client->pipe_from_child, &resultByte, 1);
        if (n == -1 && errno == EINTR)
            continue;
        // The child at the end of start_install_environment() writes status on success.
        if (n == 1 && resultByte == 0 )
            success = true;
        break;
    }
    log_info() << "handle_env_install_child_done PID " << client->child_pid << " for " << client->outfile
        << " status: " << ( success ? "success" : "failed" ) << endl;
    client->child_pid = -1;
    assert(current_kids > 0);
    current_kids--;
    if (client->pipe_from_child >= 0) {
        close(client->pipe_from_child);
        client->pipe_from_child = -1;
    }
    if( !success )
        return finish_transfer_env( client, true ); // cancel
    if( client->pipe_to_child >= 0 ) {
        // we still haven't received END message, wait for that
        assert( client->status == Client::TOINSTALL );
        return true;
    }
    // Child done, transfer done, finish.
    return finish_transfer_env( client );
}

bool Daemon::finish_transfer_env(Client *client, bool cancel)
{
    log_info() << "finish_transfer_env for " << client->outfile
        << ( cancel ? " (cancel)" : "" ) << endl;

    assert(client->outfile.size());
    assert(client->status == Client::TOINSTALL || client->status == Client::WAITINSTALL);

    if (client->pipe_from_child >= 0) {
        assert( cancel ); // If not cancelled, this is closed by handle_env_install_child_done().
        close(client->pipe_from_child);
        client->pipe_from_child = -1;
    }
    if (client->pipe_to_child >= 0) {
        assert( cancel ); // If not cancelled, this is closed by handle_file_chunk_env().
        close(client->pipe_to_child);
        client->pipe_to_child = -1;
    }
    if (client->child_pid >= 0 ) {
        assert( cancel ); // If not cancelled, this is handled by handle_env_install_child_done().
        kill( client->child_pid, SIGTERM );
        int status;
        trace() << "finish_transfer_env kill and waiting for child PID " << client->child_pid <<endl;
        while (waitpid(client->child_pid, &status, 0) < 0 && errno == EINTR)
            ;
        client->child_pid = -1;
        assert(current_kids > 0);
        current_kids--;
    }

    size_t installed_size = 0;
    if( !cancel ) {
        installed_size = finalize_install_environment(envbasedir, client->outfile,
                            user_uid, user_gid);
        log_info() << "installed_size: " << installed_size << endl;
    }
    if( installed_size == 0 )
        remove_environment_files(envbasedir, client->outfile);

    client->set_status(Client::UNKNOWN, cancel ? "finish_transfer_env: canceled" : "finish_transfer_env: done");
    string current = client->outfile;
    client->outfile.clear();

    if (installed_size) {
        cache_size += installed_size;
        received_environments[current].last_use = time(nullptr);
        received_environments[current].size = installed_size;
        log_info() << "installed " << current << " size: " << installed_size
                    << " all: " << cache_size << endl;
    }

    check_cache_size(current);

    bool r = reannounce_environments(); // do that before the file compiles

    if (!maybe_stats(true)) { // update stats in case our disk is too full to accept more jobs
        r = false;
    }

    return r;
}

void Daemon::check_cache_size(const string &new_env)
{
    time_t now = time(nullptr);

    while (cache_size > cache_size_limit) {
        string oldest_received;
        string oldest_native;
        // I don't dare to use (time_t)-1
        time_t oldest_time = time(nullptr) + 90000;

        for (const auto& it : received_environments ) {
            trace() << "considering cached environment: " << it.first << " " << it.second.last_use << " " << oldest_time << endl;

            if (access(string(envbasedir + "/target=" + it.first + "/usr/bin/as").c_str(), X_OK) != 0) {
                trace() << string(envbasedir + "/target=" + it.first + "/usr/bin/as") << " is missing, removing environment" << endl;
                // force removing this one
                oldest_time = 0;
                oldest_received = it.first;
                break;
            }

            // ignore recently used envs (they might be in use _right_ now)
            int keep_timeout = 200;

            if (it.second.last_use < oldest_time && now - it.second.last_use > keep_timeout) {
                bool env_currently_in_use = false;

                for (Clients::const_iterator it2 = clients.begin(); it2 != clients.end(); ++it2)  {
                    if (it2->second->status == Client::TOCOMPILE
                            || it2->second->status == Client::TOINSTALL
                            || it2->second->status == Client::WAITINSTALL
                            || it2->second->status == Client::WAITFORCHILD) {

                        assert(it2->second->job);
                        string envforjob = it2->second->job->targetPlatform() + "/"
                                           + it2->second->job->environmentVersion();

                        if (envforjob == it.first) {
                            env_currently_in_use = true;
                        }
                    }
                }

                if (!env_currently_in_use) {
                    oldest_time = it.second.last_use;
                    oldest_received = it.first;
                }
            }
        }
        for (const auto& it : native_environments ) {
            trace() << "considering native environment: " << it.first << " " << it.second.last_use << " " << oldest_time << endl;

            if (!it.second.name.empty() && access(it.second.name.c_str(), R_OK) != 0) {
                trace() << it.second.name << " is missing, removing environment" << endl;
                // force removing this one
                oldest_time = 0;
                oldest_native = it.first;
                break;
            }

            // ignore recently used envs (they might be in use _right_ now)
            int keep_timeout = 200;

            // Allow removing native environments only after a longer period,
            // unless there are many native environments.
            if (native_environments.size() < 5) {
                keep_timeout = 24 * 60 * 60;    // 1 day
            }

            if (it.second.create_env_pipe) {
                keep_timeout = 365 * 24 * 60 * 60; // do not remove if it's still being created
            }

            if (it.second.last_use < oldest_time && now - it.second.last_use > keep_timeout) {
                oldest_time = it.second.last_use;
                oldest_native = it.first;
            }
        }

        if ((oldest_received.empty() || oldest_received == new_env)
            && (oldest_native.empty() || oldest_native == new_env)) {
            break;
        }

        if (!oldest_native.empty())
            remove_native_environment(oldest_native);
        else
            remove_environment(oldest_received);
    }
}

void Daemon::remove_native_environment(const string& env_key)
{
    assert(!env_key.empty());
    remove_native_environment_files(env_key);
    const NativeEnvironment &env = native_environments[env_key];
    trace() << "removing " << env.name << " " << env.size << endl;
    if (env.create_env_pipe) {
        if ((-1 == close(env.create_env_pipe)) && (errno != EBADF)){
            log_perror("close failed");
        }
        // TODO kill the still running icecc-create-env process?
    }
    assert( cache_size >= env.size );
    cache_size -= env.size;
    native_environments.erase(env_key);
}

void Daemon::remove_environment(const string& env_key)
{
    assert(!env_key.empty());
    remove_environment_files(envbasedir, env_key);
    const ReceivedEnvironment& env = received_environments[env_key];
    trace() << "removing " << envbasedir << "/target=" << env_key << " " << env.size << endl;
    assert( cache_size >= env.size );
    cache_size -= env.size;
    received_environments.erase(env_key);
}

bool Daemon::handle_get_native_env(Client *client, GetNativeEnvMsg *msg)
{
    string env_key;
    map<string, time_t> filetimes;
    struct stat st;

    string compiler = msg->compiler;
    // Older clients passed simply "gcc" or "clang" and not a binary.
    if( !IS_PROTOCOL_VERSION(41, client->channel) && compiler.find('/') == string::npos)
        compiler = "/usr/bin/" + compiler;

    string ccompiler = get_c_compiler(compiler);
    string cppcompiler = get_cpp_compiler(compiler);

    trace() << "get_native_env for " << msg->compiler
        << " (" << ccompiler << "," << cppcompiler << ")" << endl;

    if (stat(ccompiler.c_str(), &st) != 0) {
        log_error() << "Compiler binary " << ccompiler << " for environment not found." << endl;
        client->channel->send_msg(EndMsg());
        handle_end(client, 122);
        return false;
    }
    filetimes[ccompiler] = st.st_mtime;
    if (stat(cppcompiler.c_str(), &st) == 0) {
        // C++ compiler is optional.
        filetimes[cppcompiler] = st.st_mtime;
    }

    env_key = msg->compression + ":" + ccompiler;
    for (list<string>::const_iterator it = msg->extrafiles.begin();
            it != msg->extrafiles.end(); ++it) {
        env_key += ':';
        env_key += *it;

        if (stat(it->c_str(), &st) != 0) {
            log_error() << "Extra file " << *it << " for environment not found." << endl;
            client->channel->send_msg(EndMsg());
            handle_end(client, 122);
            return false;
        }

        filetimes[*it] = st.st_mtime;
    }

    if (native_environments[env_key].name.length()) {
        const NativeEnvironment &env = native_environments[env_key];

        if (env.filetimes != filetimes || access(env.name.c_str(), R_OK) != 0) {
            trace() << "native_env needs rebuild" << endl;
            remove_native_environment(env.name);
        }
    }

    trace() << "get_native_env " << native_environments[env_key].name
            << " (" << env_key << ")" << endl;

    client->set_status(Client::WAITCREATEENV, "handle_get_native_env: waiting for icecc-create-env");
    client->pending_create_env = env_key;

    if (native_environments[env_key].name.length()) { // already available
        return finish_get_native_env(client, env_key);
    } else {
        NativeEnvironment &env = native_environments[env_key]; // also inserts it
        if (!env.create_env_pipe) { // start creating it only if not already in progress
            env.filetimes = filetimes;
            trace() << "start_create_env " << env_key << endl;
            env.create_env_pipe = start_create_env(envbasedir, user_uid, user_gid, ccompiler,
                msg->extrafiles, msg->compression);
        } else {
            trace() << "waiting for already running create_env " << env_key << endl;
        }
    }
    return true;
}

bool Daemon::finish_get_native_env(Client *client, string env_key)
{
    assert(client->status == Client::WAITCREATEENV);
    assert(client->pending_create_env == env_key);
    UseNativeEnvMsg m(native_environments[env_key].name);

    if (!client->channel->send_msg(m)) {
        handle_end(client, 138);
        return false;
    }

    native_environments[env_key].last_use = time(nullptr);
    client->set_status(Client::GOTNATIVE, "finish_get_native_env: sent native env");
    client->pending_create_env.clear();
    return true;
}

bool Daemon::create_env_finished(string env_key)
{
    assert(native_environments.count(env_key));
    NativeEnvironment &env = native_environments[env_key];

    trace() << "create_env_finished " << env_key << endl;
    assert(env.create_env_pipe);
    size_t installed_size = finish_create_env(env.create_env_pipe, envbasedir, env.name);
    env.create_env_pipe = 0;

    // we only clean out cache on next target install
    cache_size += installed_size;
    trace() << "cache_size = " << cache_size << endl;

    if (!installed_size) {
        bool repeat = true;
        while(repeat) {
            repeat = false;
            for (Clients::const_iterator it = clients.begin(); it != clients.end(); ++it)  {
                if (it->second->pending_create_env == env_key) {
                    it->second->channel->send_msg(EndMsg());
                    handle_end(it->second, 121);
                    // The handle_end call invalidates our iterator, so break out of the loop,
                    // but try again just in case, until there's no match.
                    repeat = true;
                    break;
                }
            }
        }
        return false;
    }

    env.last_use = time(nullptr);
    env.size = installed_size;
    check_cache_size(env.name);

    for (Clients::const_iterator it = clients.begin(); it != clients.end(); ++it) {
        if (it->second->pending_create_env == env_key)
            finish_get_native_env(it->second, env_key);
    }
    return true;
}

bool Daemon::handle_job_done(Client *cl, JobDoneMsg *m)
{
    if (cl->status == Client::CLIENTWORK) {
        if(cl->fulljob)
            clients.active_processes -= std::max((unsigned int)1, max_kids);
        else
            clients.active_processes--;
    }

    cl->set_status(Client::JOBDONE, "handle_job_done: reported to scheduler");
    JobDoneMsg *msg = static_cast<JobDoneMsg *>(m);
    trace() << "handle_job_done " << msg->job_id << " " << (cl->fulljob ? "(full) " : "")
        << msg->exitcode << endl;
    cl->fulljob = false;

    if (!m->is_from_server()
            && (m->user_msec + m->sys_msec) <= m->real_msec) {
        icecream_load += (m->user_msec + m->sys_msec) / num_cpus;
    }

    assert(msg->job_id == cl->job_id);
    cl->job_id = 0; // the scheduler doesn't have it anymore

    msg->client_count = clients.size();

    return send_scheduler(*msg);
}

void Daemon::handle_old_request()
{
    while ((current_kids + clients.active_processes) < std::max((unsigned int)1, max_kids)) {

        Client *client = clients.get_earliest_client(Client::LINKJOB);

        if (client) {
            trace() << "send JobLocalBeginMsg to client" << endl;

            if (!client->channel->send_msg(JobLocalBeginMsg())) {
                log_warning() << "can't send start message to client" << endl;
                handle_end(client, 112);
            } else {
                client->set_status(Client::CLIENTWORK, "handle_old_request: local job started");
                if (client->fulljob) { // reserve the entire node
                    clients.active_processes += std::max((unsigned int)1, max_kids);
                    trace() << "pushed full local job " << client->client_id << endl;
                } else {
                    clients.active_processes++;
                    trace() << "pushed local job " << client->client_id << endl;
                }
                if (!send_scheduler(JobLocalBeginMsg(client->client_id, client->outfile,
                        client->fulljob))) {
                    return;
                }
            }

            continue;
        }

        client = clients.get_earliest_client(Client::PENDING_USE_CS);

        if (client) {
            trace() << "pending " << client->dump() << endl;

            if (client->channel->send_msg(*client->usecsmsg)) {
                client->set_status(Client::CLIENTWORK, "handle_old_request: usecs delivered");
                /* we make sure we reserve a spot and the rest is done if the
                 * client contacts as back with a Compile request */
                clients.active_processes++;
            } else {
                handle_end(client, 129);
            }

            continue;
        }

        /* we don't want to handle TOCOMPILE jobs as long as our load
           is too high */
        if (current_load >= 1000) {
            break;
        }

        client = clients.get_earliest_client(Client::TOCOMPILE);

        if (client) {
            CompileJob *job = client->job;
            assert(job);
            int sock = -1;
            pid_t pid = -1;

            trace() << "request for job " << job->jobID() << endl;

            string envforjob = job->targetPlatform() + "/" + job->environmentVersion();
            received_environments[envforjob].last_use = time(nullptr);
            pid = handle_connection(envbasedir, job, client->channel, sock, mem_limit, user_uid, user_gid);
            trace() << "handle connection returned " << pid << endl;

            if (pid > 0) {
                current_kids++;
                client->set_status(Client::WAITFORCHILD, "handle_old_request: compiling locally (child running)");
                client->pipe_from_child = sock;
                client->child_pid = pid;

                if (!send_scheduler(JobBeginMsg(job->jobID(), clients.size()))) {
                    log_info() << "failed sending scheduler about " << job->jobID() << endl;
                }
            } else {
                handle_end(client, 117);
            }

            continue;
        }

        break;
    }
}

bool Daemon::handle_compile_done(Client *client)
{
    assert(client->status == Client::WAITFORCHILD);
    assert(client->child_pid > 0);
    assert(client->pipe_from_child >= 0);

    JobDoneMsg *msg = new JobDoneMsg(client->job->jobID(), -1, JobDoneMsg::FROM_SERVER, clients.size());
    assert(msg);
    assert(current_kids > 0);
    current_kids--;

    unsigned int job_stat[8];
    int end_status = 151;

    if (read(client->pipe_from_child, job_stat, sizeof(job_stat)) == sizeof(job_stat)) {
        msg->in_uncompressed = job_stat[JobStatistics::in_uncompressed];
        msg->in_compressed = job_stat[JobStatistics::in_compressed];
        msg->out_compressed = msg->out_uncompressed = job_stat[JobStatistics::out_uncompressed];
        end_status = msg->exitcode = job_stat[JobStatistics::exit_code];
        msg->real_msec = job_stat[JobStatistics::real_msec];
        msg->user_msec = job_stat[JobStatistics::user_msec];
        msg->sys_msec = job_stat[JobStatistics::sys_msec];
        msg->pfaults = job_stat[JobStatistics::sys_pfaults];
    }

    close(client->pipe_from_child);
    client->pipe_from_child = -1;
    string envforjob = client->job->targetPlatform() + "/" + client->job->environmentVersion();
    received_environments[envforjob].last_use = time(nullptr);
    if(end_status == EXIT_COMPILER_MISSING) { // Environment damaged?
        remove_environment(envforjob);
        if( !reannounce_environments())
            log_warning() << "failed reannounce environments after failed compile " << client->job->jobID() << endl;
    }

    if(!send_scheduler(*msg))
        log_warning() << "failed sending scheduler about compile done " << client->job->jobID() << endl;
    handle_end(client, end_status);
    delete msg;
    return false;
}

bool Daemon::handle_compile_file(Client *client, Msg *msg)
{
    CompileJob *job = dynamic_cast<CompileFileMsg *>(msg)->takeJob();
    assert(client);
    assert(job);
    client->job = job;
    client->last_known_job_id = job->jobID();

    if (client->status == Client::CLIENTWORK) {
        assert(job->environmentVersion() == "__client");

        if (!send_scheduler(JobBeginMsg(job->jobID(), clients.size()))) {
            trace() << "can't reach scheduler to tell him about compile file job "
                    << job->jobID() << endl;
            return false;
        }

        // no scheduler is not an error case!
    } else {
        client->set_status(Client::TOCOMPILE, "handle_compile_file: queued for local compile");
    }

    return true;
}

bool Daemon::handle_verify_env(Client *client, VerifyEnvMsg *msg)
{
    assert(msg);
    bool ok = verify_env(client->channel, envbasedir, msg->target, msg->environment, user_uid, user_gid);
    trace() << "Verify environment done, " << (ok ? "success" : "failure") << ", environment " << msg->environment
            << " (" << msg->target << ")" << endl;
    VerifyEnvResultMsg resultmsg(ok);

    if (!client->channel->send_msg(resultmsg)) {
        log_error() << "sending verify end result failed.." << endl;
        return false;
    }

    return true;
}

bool Daemon::handle_blacklist_host_env(Client *client, Msg *msg)
{
    // just forward
    assert(dynamic_cast<BlacklistHostEnvMsg *>(msg));
    assert(client);
    (void)client;

    if (!scheduler) {
        return false;
    }

    return send_scheduler(*msg);
}

void Daemon::handle_end(Client *client, int exitcode)
{
    trace() << "handle_end " << client->client_id << " " << client->channel->name << endl;
#ifdef ICECC_DEBUG
    trace() << "handle_end " << client->dump() << endl;
    trace() << dump_internals() << endl;
#endif
    remember_finished_job(client, exitcode);
    fd2client.erase(client->channel->fd);

    if (client->status == Client::TOINSTALL || client->status == Client::WAITINSTALL) {
        finish_transfer_env(client, true);
    }

    if (client->status == Client::CLIENTWORK) {
        if(client->fulljob)
            clients.active_processes -= std::max((unsigned int)1, max_kids);
        else
            clients.active_processes--;
    }
    client->fulljob = false;

    if (client->status == Client::WAITCOMPILE && exitcode == 119) {
        /* the client sent us a real good bye, so forget about the scheduler */
        client->job_id = 0;
    }

    /* Delete from the clients map before send_scheduler, which causes a
       double deletion. */
    if (!clients.erase(client->channel)) {
        log_error() << "client can't be erased: " << client->channel << endl;
        flush_debug();
        log_error() << dump_internals() << endl;
        flush_debug();
        assert(false);
    }

    if (scheduler && client->status != Client::WAITFORCHILD) {
        int job_id = client->job_id;
        bool use_client_id = false;

        if (client->status == Client::TOCOMPILE) {
            job_id = client->job->jobID();
        }

        if (client->status == Client::WAITFORCS) {
            // We don't know the job id, because we haven't received a reply
            // from the scheduler yet. Use client_id to identify the job,
            // the scheduler will use it for matching.
            use_client_id = true;
            assert( client->client_id > 0 );
        }

        if (job_id > 0 || use_client_id) {
            JobDoneMsg::from_type flag = JobDoneMsg::FROM_SUBMITTER;

            switch (client->status) {
            case Client::TOCOMPILE:
                flag = JobDoneMsg::FROM_SERVER;
                break;
            case Client::UNKNOWN:
            case Client::GOTNATIVE:
            case Client::JOBDONE:
            case Client::WAITFORCHILD:
            case Client::LINKJOB:
            case Client::TOINSTALL:
            case Client::WAITINSTALL:
            case Client::WAITCREATEENV:
                assert(false);   // should not have a job_id
                break;
            case Client::WAITCOMPILE:
            case Client::PENDING_USE_CS:
            case Client::CLIENTWORK:
            case Client::WAITFORCS:
                flag = JobDoneMsg::FROM_SUBMITTER;
                break;
            }

            trace() << "scheduler->send_msg( JobDoneMsg( " << client->dump() << ", " << exitcode << "))\n";

            JobDoneMsg msg(job_id, exitcode, flag, clients.size());
            if( use_client_id ) {
                msg.set_unknown_job_client_id( client->client_id );
            }
            if (!send_scheduler(msg)) {
                trace() << "failed to reach scheduler for remote job done msg!" << endl;
            }
        } else if (client->status == Client::CLIENTWORK) {
            // Clientwork && !job_id == LINK
            trace() << "scheduler->send_msg( JobLocalDoneMsg( " << client->client_id << ") );\n";

            if (!send_scheduler(JobLocalDoneMsg(client->client_id))) {
                trace() << "failed to reach scheduler for local job done msg!" << endl;
            }
        }
    }

    delete client;
}

void Daemon::clear_children()
{
    while (!clients.empty()) {
        Client *cl = clients.first();
        handle_end(cl, 116);
    }

    while (current_kids > 0) {
        int status;
        pid_t child;

        while ((child = waitpid(-1, &status, 0)) < 0 && errno == EINTR) {}

        current_kids--;
    }

    // they should be all in clients too
    assert(fd2client.empty());

    fd2client.clear();
    new_client_id = 0;
    trace() << "cleared children\n";
}

bool Daemon::handle_get_cs(Client *client, Msg *msg)
{
    GetCSMsg *umsg = dynamic_cast<GetCSMsg *>(msg);
    assert(client);
    client->niceness = umsg->niceness;
    client->last_waitforcs_msec = 0;
    client->set_status(Client::WAITFORCS, scheduler ? "handle_get_cs: sent GetCS to scheduler" : "handle_get_cs: scheduler missing");
    umsg->client_id = client->client_id;
    trace() << "handle_get_cs " << umsg->client_id << endl;

    if (!scheduler) {
        /* now the thing is this: if there is no scheduler
           there is no point in trying to ask him. So we just
           redefine this as local job */
        client->usecsmsg = new UseCSMsg(umsg->target, "127.0.0.1", daemon_port,
                                        umsg->client_id, true, 1, 0);
        client->set_status(Client::PENDING_USE_CS, "handle_get_cs: scheduler missing, local compile");
        client->job_id = umsg->client_id;
        client->last_known_job_id = umsg->client_id;
        return true;
    }

    umsg->client_count = clients.size();

    return send_scheduler(*umsg);
}

int Daemon::handle_cs_conf(ConfCSMsg *msg)
{
    max_scheduler_pong = msg->max_scheduler_pong;
    max_scheduler_ping = msg->max_scheduler_ping;
    return 0;
}

bool Daemon::handle_local_job(Client *client, Msg *msg)
{
    JobLocalBeginMsg* m = dynamic_cast<JobLocalBeginMsg *>(msg);
    client->set_status(Client::LINKJOB, "handle_local_job: link job");
    client->outfile = m->outfile;
    client->fulljob = m->fulljob;
    return true;
}

bool Daemon::handle_activity(Client *client)
{
    assert(client->status != Client::TOCOMPILE && client->status != Client::WAITINSTALL);

    Msg *msg = client->channel->get_msg(0, true);

    if (!msg) {
        handle_end(client, 118);
        return false;
    }

    bool ret = false;

    if (client->status == Client::TOINSTALL) {
        ret = handle_file_chunk_env(client, msg);
        delete msg;
        return ret;
    }

    switch (*msg) {
    case Msg::GET_NATIVE_ENV:
        ret = handle_get_native_env(client, dynamic_cast<GetNativeEnvMsg *>(msg));
        break;
    case Msg::COMPILE_FILE:
        ret = handle_compile_file(client, msg);
        break;
    case Msg::TRANFER_ENV:
        ret = handle_transfer_env(client, dynamic_cast<EnvTransferMsg*>(msg));
        break;
    case Msg::GET_CS:
        ret = handle_get_cs(client, msg);
        break;
    case Msg::GET_INTERNALS:
        ret = client->channel->send_msg(StatusTextMsg(dump_internals()));
        break;
    case Msg::END:
        handle_end(client, 119);
        ret = false;
        break;
    case Msg::JOB_LOCAL_BEGIN:
        ret = handle_local_job(client, msg);
        break;
    case Msg::JOB_DONE:
        ret = handle_job_done(client, dynamic_cast<JobDoneMsg *>(msg));
        break;
    case Msg::VERIFY_ENV:
        ret = handle_verify_env(client, dynamic_cast<VerifyEnvMsg *>(msg));
        break;
    case Msg::BLACKLIST_HOST_ENV:
        ret = handle_blacklist_host_env(client, msg);
        break;
    default:
        log_error() << "protocol error " << msg->to_string() << " on client "
                    << client->dump() << endl;
        client->channel->send_msg(EndMsg());
        handle_end(client, 120);
        ret = false;
    }

    delete msg;
    return ret;
}

void Daemon::answer_client_requests()
{
#ifdef ICECC_DEBUG

    if (clients.size() + current_kids) {
        log_info() << dump_internals() << endl;
    }

    log_info() << "clients " << clients.dump_per_status() << " " << current_kids
               << " (" << max_kids << ")" << endl;

#endif

    /* reap zombies */
    int status;

    while (waitpid(-1, &status, WNOHANG) < 0 && errno == EINTR) {}

    handle_old_request();

    /* collect the stats after the children exited icecream_load */
    if (scheduler) {
        maybe_stats();
    }

    vector< pollfd > pollfds;
    pollfds.reserve( fd2client.size() + 6 );
    pollfd pfd; // tmp varible

    if (tcp_listen_fd != -1) {
        pfd.fd = tcp_listen_fd;
        pfd.events = POLLIN;
        pollfds.push_back(pfd);
    }
    if (tcp_listen_local_fd != -1) {
        pfd.fd = tcp_listen_local_fd;
        pfd.events = POLLIN;
        pollfds.push_back(pfd);
    }

    pfd.fd = unix_listen_fd;
    pfd.events = POLLIN;
    pollfds.push_back(pfd);

    if (web_listen_fd != -1) {
        pfd.fd = web_listen_fd;
        pfd.events = POLLIN;
        pollfds.push_back(pfd);
    }

    for (const auto &it : web_connections) {
        pfd.fd = it.first;
        pfd.events = POLLIN;
        if (!it.second.outbuf.empty()) {
            pfd.events |= POLLOUT;
        }
        pollfds.push_back(pfd);
    }

    for (auto it = fd2client.begin(); it != fd2client.end();) {
        int i = it->first;
        Client *client = it->second;
        MsgChannel *c = client->channel;
        ++it;
        /* don't select on a fd that we're currently not interested in.
           Avoids that we wake up on an event we're not handling anyway */
        assert(client);
        int current_status = client->status;
        bool ignore_channel = current_status == Client::WAITFORCHILD ||
                              current_status == Client::WAITINSTALL;

        /* when the remote host is full with work, the wait time for it to free up and
           fork a child to compile could be long. If the input is ready to read, we will read
           them and save it for the child; otherwise the write on the client side would be blocked */
        if ( current_status == Client::TOCOMPILE ||
             (!ignore_channel && (!c->has_msg() || handle_activity(client)))) {
            pfd.fd = i;
            pfd.events = POLLIN;
            pollfds.push_back(pfd);
        }

        if ((current_status == Client::WAITFORCHILD
                || current_status == Client::TOINSTALL
                || current_status == Client::WAITINSTALL)
              && client->pipe_from_child != -1) {
            pfd.fd = client->pipe_from_child;
            pfd.events = POLLIN;
            pollfds.push_back(pfd);
        }
    }

    if (scheduler) {
        pfd.fd = scheduler->fd;
        pfd.events = POLLIN;
        pollfds.push_back(pfd);
    } else if (discover && discover->listen_fd() >= 0) {
        /* We don't explicitely check for discover->get_fd() being in
        the selected set below.  If it's set, we simply will return
        and our call will make sure we try to get the scheduler.  */
        pfd.fd = discover->listen_fd();
        pfd.events = POLLIN;
        pollfds.push_back(pfd);
    }

    for (map<string, NativeEnvironment>::const_iterator it = native_environments.begin();
            it != native_environments.end(); ++it) {
        if (it->second.create_env_pipe) {
            pfd.fd = it->second.create_env_pipe;
            pfd.events = POLLIN;
            pollfds.push_back(pfd);
        }
    }

    int poll_timeout_msec = max_scheduler_pong * 1000;
    if (state_dump_interval_s && (!state_jsonl_path.empty() || state_dump_log)) {
        const uint64_t now = monotonic_msec();
        if (!next_state_dump_msec) {
            // Schedule the first dump immediately.
            next_state_dump_msec = now;
        }
        if (now >= next_state_dump_msec) {
            poll_timeout_msec = 0;
        } else {
            const uint64_t to_dump_msec = next_state_dump_msec - now;
            if (to_dump_msec < uint64_t(poll_timeout_msec)) {
                poll_timeout_msec = int(to_dump_msec);
            }
        }
    }

    int ret = poll(pollfds.data(), pollfds.size(), poll_timeout_msec);

    if (ret < 0 && errno != EINTR) {
        log_perror("poll");
        close_scheduler();
        return;
    }
    // Reset debug if needed, but only if we aren't waiting for any child processes to finish,
    // otherwise their debug output could end up reset in the middle (and flush log marks used
    // by tests could be written out before debug output from children).
    if( current_kids == 0 ) {
        reset_debug_if_needed();
    }

    if (ret > 0) {
        bool had_scheduler = scheduler;

        if (scheduler && pollfd_is_set(pollfds, scheduler->fd, POLLIN)) {
            while (!scheduler->read_a_bit() || scheduler->has_msg()) {
                Msg *msg = scheduler->get_msg(0, true);

                if (!msg) {
                    log_warning() << "scheduler closed connection" << endl;
                    close_scheduler();
                    clear_children();
                    return;
                }

                ret = 0;

                switch (*msg) {
                case Msg::PING:

                    if (!IS_PROTOCOL_VERSION(27, scheduler)) {
                        ret = !send_scheduler(PingMsg());
                    }

                    break;
                case Msg::USE_CS:
                    ret = scheduler_use_cs(static_cast<UseCSMsg *>(msg));
                    break;
                case Msg::NO_CS:
                    ret = scheduler_no_cs(static_cast<NoCSMsg *>(msg));
                    break;
                case Msg::GET_INTERNALS:
                    ret = scheduler_get_internals();
                    break;
                case Msg::CS_CONF:
                    ret = handle_cs_conf(static_cast<ConfCSMsg *>(msg));
                    break;
                default:
                    log_error() << "unknown scheduler type " << msg->to_string() << endl;
                    ret = 1;
                }

                delete msg;

                if (ret) {
                    close_scheduler();
                    return;
                }
            }
        }

        if (web_listen_fd != -1 && pollfd_is_set(pollfds, web_listen_fd, POLLIN)) {
            handle_web_accept();
        }

        for (auto it = web_connections.begin(); it != web_connections.end();) {
            const int fd = it->first;
            ++it;

            short revents = 0;
            for (const auto &pollfd : pollfds) {
                if (pollfd.fd == fd) {
                    revents = pollfd.revents;
                    break;
                }
            }
            if (revents) {
                handle_web_connection(fd, revents);
            }
        }

        int listen_fd = -1;

        if (tcp_listen_fd != -1 && pollfd_is_set(pollfds, tcp_listen_fd, POLLIN)) {
            listen_fd = tcp_listen_fd;
        }
        if (tcp_listen_local_fd != -1 && pollfd_is_set(pollfds, tcp_listen_local_fd, POLLIN)) {
            listen_fd = tcp_listen_local_fd;
        }
        if (pollfd_is_set(pollfds, unix_listen_fd, POLLIN)) {
            listen_fd = unix_listen_fd;
        }

        if (listen_fd != -1) {
            struct sockaddr cli_addr;
            socklen_t cli_len = sizeof cli_addr;
            int acc_fd = accept(listen_fd, &cli_addr, &cli_len);

            if (acc_fd < 0) {
                if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
                    note_accept_error("client", errno);
                }
            } else {
                MsgChannel *c = Service::createChannel(acc_fd, &cli_addr, cli_len);

                if (c) {
                    Client *client = new Client;
                    client->client_id = ++new_client_id;
                    client->channel = c;
                    clients[c] = client;

                    fd2client[c->fd] = client;
                    trace() << "accepted " << c->fd << " " << c->name << " as " << client->client_id << endl;

                    while (!c->read_a_bit() || c->has_msg()) {
                        if (!handle_activity(client)) {
                            break;
                        }

                        if (client->status == Client::TOCOMPILE
                                || client->status == Client::WAITFORCHILD
                                || client->status == Client::WAITINSTALL) {
                            break;
                        }
                    }
                }
            }
        } else {
            for (auto it = fd2client.begin(); it != fd2client.end();)  {
                int i = it->first;
                Client *client = it->second;
                MsgChannel *c = client->channel;
                assert(client);
                ++it;

                if (client->status == Client::WAITFORCHILD
                        && client->pipe_from_child >= 0
                        && pollfd_is_set(pollfds, client->pipe_from_child, POLLIN)) {
                    if (!handle_compile_done(client)) {
                        return;
                    }
                }
                if ((client->status == Client::TOINSTALL || client->status == Client::WAITINSTALL)
                        && client->pipe_from_child >= 0
                        && pollfd_is_set(pollfds, client->pipe_from_child, POLLIN)) {
                    if (!handle_env_install_child_done(client)) {
                        return;
                    }
                }

                if (pollfd_is_set(pollfds, i, POLLIN)) {
                    if( client->status == Client::TOCOMPILE )
                    {
                        /* read as the preprocessed input is ready but don't process it and leave it to the child
                           if we didn't read it now, the client would be blocked and timed out */
                        c->read_a_bit();
                    }
                    else
                    {
                        assert(client->status != Client::TOCOMPILE && client->status != Client::WAITINSTALL);

                        while (!c->read_a_bit() || c->has_msg()) {
                            if (!handle_activity(client)) {
                                break;
                            }

                            if (client->status == Client::TOCOMPILE
                                || client->status == Client::WAITFORCHILD
                                || client->status == Client::WAITINSTALL) {
                                break;
                            }
                        }
                    }
                }
            }

            for (map<string, NativeEnvironment>::iterator it = native_environments.begin();
                 it != native_environments.end(); ) {
                if (it->second.create_env_pipe && pollfd_is_set(pollfds, it->second.create_env_pipe, POLLIN)) {
                    if(!create_env_finished(it->first))
                    {
                        native_environments.erase(it++);
                        continue;
                    }
                }
                ++it;
            }
        }

        if (had_scheduler && !scheduler) {
            clear_children();
            return;
        }

    }
}

bool Daemon::reconnect()
{
    if (scheduler) {
        return true;
    }

    if (!discover && next_scheduler_connect > time(nullptr)) {
        trace() << "Delaying reconnect." << endl;
        return false;
    }

#ifdef ICECC_DEBUG
    trace() << "reconn " << dump_internals() << endl;
#endif

    if (!discover || (nullptr == (scheduler = discover->try_get_scheduler()) && discover->timed_out())) {
        delete discover;
        discover = new DiscoverSched(netname, max_scheduler_pong, schedname, scheduler_port);
    }

    if (!scheduler) {
        log_warning() << "scheduler not yet found/selected." << endl;
        return false;
    }

    delete discover;
    discover = nullptr;
    sockaddr_in name;
    socklen_t len = sizeof(name);
    int error = getsockname(scheduler->fd, (struct sockaddr*)&name, &len);

    if (!error) {
        remote_name = inet_ntoa(name.sin_addr);
    } else {
        remote_name = string();
    }

    log_info() << "Connected to scheduler (I am known as " << remote_name << ")" << endl;
    current_load = -1000;
    gettimeofday(&last_stat, nullptr);
    icecream_load = 0;

    LoginMsg lmsg(daemon_port, determine_nodename(), machine_name, supported_features);
    lmsg.envs = available_environments(envbasedir);
    lmsg.max_kids = max_kids;
    lmsg.noremote = noremote;
    return send_scheduler(lmsg);
}

int Daemon::working_loop()
{
    for (;;) {
        reconnect();
        answer_client_requests();
        maybe_dump_state();

        if (exit_main_loop) {
            close_scheduler();
            clear_children();
            close_web();
            break;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    int max_processes = -1;
    srand(time(nullptr) + getpid());

    Daemon d;

    int debug_level = Error;
    string logfile;
    bool detach = false;
    nice_level = 5; // defined in serve.h

    while (true) {
        int option_index = 0;
        static const struct option long_options[] = {
            { "netname", 1, nullptr, 'n' },
            { "max-processes", 1, nullptr, 'm' },
            { "help", 0, nullptr, 'h' },
            { "daemonize", 0, nullptr, 'd'},
            { "log-file", 1, nullptr, 'l'},
            { "nice", 1, nullptr, 0},
            { "name", 1, nullptr, 'N'},
            { "scheduler-host", 1, nullptr, 's' },
            { "env-basedir", 1, nullptr, 'b' },
            { "user-uid", 1, nullptr, 'u'},
            { "cache-limit", 1, nullptr, 0},
            { "no-remote", 0, nullptr, 0},
            { "interface", 1, nullptr, 'i'},
            { "port", 1, nullptr, 'p'},
            { "state-jsonl", 1, nullptr, 0},
            { "state-interval", 1, nullptr, 0},
            { "state-log", 0, nullptr, 0},
            { "webgui", 0, nullptr, 0},
            { "webgui-port", 1, nullptr, 0},
            { "webgui-addr", 1, nullptr, 0},
            { nullptr, 0, nullptr, 0 }
        };

        const int c = getopt_long(argc, argv, "N:n:m:l:s:hvdb:u:i:p:", long_options, &option_index);

        if (c == -1) {
            break;    // eoo
        }

        switch (c) {
        case 0: {
            string optname = long_options[option_index].name;

            if (optname == "nice") {
                if (optarg && *optarg) {
                    errno = 0;
                    int tnice = atoi(optarg);

                    if (!errno) {
                        nice_level = tnice;
                    }
                } else {
                    usage("Error: --nice requires argument");
                }
            } else if (optname == "name") {
                if (optarg && *optarg) {
                    d.nodename = optarg;
                } else {
                    usage("Error: --name requires argument");
                }
            } else if (optname == "cache-limit") {
                if (optarg && *optarg) {
                    errno = 0;
                    int mb = atoi(optarg);

                    if (!errno) {
                        cache_size_limit = mb * 1024 * 1024;
                    }
                } else {
                    usage("Error: --cache-limit requires argument");
                }
            } else if (optname == "no-remote") {
                d.noremote = true;
            } else if (optname == "state-jsonl") {
                if (optarg && *optarg) {
                    d.state_jsonl_path = optarg;
                    if (!d.state_dump_interval_s) {
                        d.state_dump_interval_s = 30;
                    }
                } else {
                    usage("Error: --state-jsonl requires argument");
                }
            } else if (optname == "state-interval") {
                if (optarg && *optarg) {
                    errno = 0;
                    int interval = atoi(optarg);
                    if (errno || interval <= 0) {
                        usage("Error: --state-interval requires a positive integer");
                    }
                    d.state_dump_interval_s = interval;
                } else {
                    usage("Error: --state-interval requires argument");
                }
            } else if (optname == "state-log") {
                d.state_dump_log = true;
                if (!d.state_dump_interval_s) {
                    d.state_dump_interval_s = 30;
                }
            } else if (optname == "webgui") {
                d.webgui_enabled = true;
            } else if (optname == "webgui-port") {
                if (optarg && *optarg) {
                    errno = 0;
                    int port = atoi(optarg);
                    if (errno || port <= 0 || port > 65535) {
                        usage("Error: --webgui-port requires a valid TCP port");
                    }
                    d.webgui_port = port;
                    d.webgui_enabled = true;
                } else {
                    usage("Error: --webgui-port requires argument");
                }
            } else if (optname == "webgui-addr") {
                if (optarg && *optarg) {
                    string addr = optarg;
                    if (addr.empty()) {
                        usage("Error: --webgui-addr requires argument");
                    }
                    d.webgui_addr = addr;
                    d.webgui_enabled = true;
                } else {
                    usage("Error: --webgui-addr requires argument");
                }
            }

        }
        break;
        case 'd':
            detach = true;
            break;
        case 'N':

            if (optarg && *optarg) {
                d.nodename = optarg;
            } else {
                usage("Error: -N requires argument");
            }

            break;
        case 'l':

            if (optarg && *optarg) {
                logfile = optarg;
            } else {
                usage("Error: -l requires argument");
            }

            break;
        case 'v':

            if (debug_level < MaxVerboseLevel) {
                debug_level++;
            }

            break;
        case 'n':

            if (optarg && *optarg) {
                d.netname = optarg;
            } else {
                usage("Error: -n requires argument");
            }

            break;
        case 'm':

            if (optarg && *optarg) {
                max_processes = atoi(optarg);
            } else {
                usage("Error: -m requires argument");
            }

            break;
        case 's':

            if (optarg && *optarg) {
                string scheduler = optarg;
                size_t colon = scheduler.rfind( ':' );
                if( colon == string::npos ) {
                    d.schedname = scheduler;
                } else {
                    d.schedname = scheduler.substr(0, colon);
                    d.scheduler_port = atoi( scheduler.substr( colon + 1 ).c_str());
                    if( d.scheduler_port == 0 ) {
                        usage("Error: -s requires valid port if hostname includes colon");
                    }
                }
            } else {
                usage("Error: -s requires hostname argument");
            }

            break;
        case 'b':

            if (optarg && *optarg) {
                d.envbasedir = optarg;
            }

            break;
        case 'u':

            if (optarg && *optarg) {
                struct passwd *pw = getpwnam(optarg);

                if (!pw) {
                    usage("Error: -u requires a valid username");
                } else {
                    d.user_uid = pw->pw_uid;
                    d.user_gid = pw->pw_gid;
                    d.warn_icecc_user_errno = 0;

                    if (!d.user_gid || !d.user_uid) {
                        usage("Error: -u <username> must not be root");
                    }
                }
            } else {
                usage("Error: -u requires a valid username");
            }

            break;
        case 'i':

            if (optarg && *optarg) {
                string daemon_interface = optarg;
                if (daemon_interface.empty()) {
                    usage("Error: Invalid network interface specified");
                }

                d.daemon_interface = daemon_interface;
            } else {
                usage("Error: -i requires argument");
            }

            break;
        case 'p':

            if (optarg && *optarg) {
                d.daemon_port = atoi(optarg);

                if (0 == d.daemon_port) {
                    usage("Error: Invalid port specified");
                }
            } else {
                usage("Error: -p requires argument");
            }

            break;
        default:
            usage();
        }
    }

    if (d.warn_icecc_user_errno != 0) {
        log_errno("No icecc user on system. Falling back to nobody.", d.warn_icecc_user_errno);
    }

    umask(022);

    bool remote_disabled = false;
    if (getuid() == 0) {
        if (!logfile.length() && detach) {
            mkdir("/var/log/icecc", S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);
            chmod("/var/log/icecc", S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);
            ignore_result(chown("/var/log/icecc", d.user_uid, d.user_gid));
            logfile = "/var/log/icecc/iceccd.log";
        }

        mkdir("/var/run/icecc", S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);
        chmod("/var/run/icecc", S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);
        ignore_result(chown("/var/run/icecc", d.user_uid, d.user_gid));

#ifdef HAVE_LIBCAP_NG
        capng_clear(CAPNG_SELECT_BOTH);
        capng_update(CAPNG_ADD, (capng_type_t)(CAPNG_EFFECTIVE | CAPNG_PERMITTED), CAP_SYS_CHROOT);
        int r = capng_change_id(d.user_uid, d.user_gid,
                                (capng_flags_t)(CAPNG_DROP_SUPP_GRP | CAPNG_CLEAR_BOUNDING));
        if (r) {
            log_error() << "Error: capng_change_id failed: " << r << endl;
            exit(EXIT_SETUID_FAILED);
        }
#endif
    } else {
#ifdef HAVE_LIBCAP_NG
        // It's possible to have the capability even without being root.
        if (!capng_have_capability( CAPNG_EFFECTIVE, CAP_SYS_CHROOT )) {
#else
        {
#endif
            d.noremote = true;
            remote_disabled = true;
        }
    }

    setup_debug(debug_level, logfile);

    log_info() << "ICECREAM daemon " VERSION " starting up (nice level "
               << nice_level << ") " << endl;
    if (d.state_dump_interval_s && (!d.state_jsonl_path.empty() || d.state_dump_log)) {
        log_info() << "state dumps enabled: interval " << d.state_dump_interval_s << "s"
                   << ", jsonl=" << (d.state_jsonl_path.empty() ? "<disabled>" : d.state_jsonl_path)
                   << ", log=" << (d.state_dump_log ? "true" : "false") << endl;
    }
    if (d.webgui_enabled) {
        log_info() << "web gui requested on " << d.webgui_addr << ":" << d.webgui_port << endl;
    }
    if (remote_disabled)
        log_warning() << "Cannot use chroot, no remote jobs accepted." << endl;
    if (d.noremote)
        d.daemon_port = 0;

    d.determine_system();

    if (chdir("/") != 0) {
        log_error() << "failed to switch to root directory: "
                    << strerror(errno) << endl;
        exit(EXIT_DISTCC_FAILED);
    }

    if (detach)
        if (daemon(0, 0)) {
            log_perror("Failed to run as a daemon.");
            exit(EXIT_DISTCC_FAILED);
        }

    if (dcc_ncpus(&d.num_cpus) == 0) {
        log_info() << d.num_cpus << " CPU(s) online on this server" << endl;
    }

    if (max_processes < 0) {
        max_kids = d.num_cpus;
    } else {
        max_kids = max_processes;
    }

    log_info() << "allowing up to " << max_kids << " active jobs" << endl;

    d.determine_supported_features();
    log_info() << "supported features: " << supported_features_to_string(d.supported_features) << endl;

    int ret;

    /* Still create a new process group, even if not detached */
    trace() << "not detaching\n";

    if ((ret = set_new_pgrp()) != 0) {
        return ret;
    }

    /* Don't catch signals until we've detached or created a process group. */
    dcc_daemon_catch_signals();

    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        log_warning() << "signal(SIGPIPE, ignore) failed: " << strerror(errno) << endl;
        exit(EXIT_DISTCC_FAILED);
    }

    if (signal(SIGCHLD, SIG_DFL) == SIG_ERR) {
        log_warning() << "signal(SIGCHLD) failed: " << strerror(errno) << endl;
        exit(EXIT_DISTCC_FAILED);
    }

    /* This is called in the master daemon, whether that is detached or
     * not.  */
    dcc_master_pid = getpid();

    ofstream pidFile;
    string progName = argv[0];
    progName = find_basename(progName);
    pidFilePath = string(RUNDIR) + string("/") + progName + string(".pid");
    pidFile.open(pidFilePath.c_str());
    pidFile << dcc_master_pid << endl;
    pidFile.close();

    if (!cleanup_cache(d.envbasedir, d.user_uid, d.user_gid)) {
        return 1;
    }

    list<string> nl = get_netnames(200, d.scheduler_port);
    trace() << "Netnames:" << endl;

    for (list<string>::const_iterator it = nl.begin(); it != nl.end(); ++it) {
        trace() << *it << endl;
    }

    if (!d.setup_listen_fds()) { // error
        return 1;
    }

    return d.working_loop();
}
