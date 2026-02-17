/* -*- mode: C++; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 99; -*- */
/* vim: set ts=4 sw=4 et tw=99:  */
/*
 * icecc -- A simple distributed compiler system
 *
 * Copyright (C) 2003, 2004 by the Icecream Authors
 *
 * based on distcc
 * Copyright (C) 2002, 2003 by Martin Pool <mbp@samba.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */


/* 4: The noise of a multitude in the
 * mountains, like as of a great people; a
 * tumultuous noise of the kingdoms of nations
 * gathered together: the LORD of hosts
 * mustereth the host of the battle.
 *      -- Isaiah 13 */



#include "config.h"

// Required by strsignal() on some systems.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <cassert>
#include <limits.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <comm.h>
#include <vector>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <chrono>

#include "client.h"
#include "platform.h"
#include "util.h"
#include "argv.h"

using namespace std;

extern const char *rs_program_name;
std::string invocation_cmdline;
InvocationTiming invocation_timing;

uint64_t invocation_now_msec()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

static uint32_t clamp_u32(uint64_t value)
{
    return value > 0xffffffffULL ? 0xffffffffU : uint32_t(value);
}

static uint32_t invocation_elapsed_msec()
{
    if (!invocation_timing.submit_ts) {
        return 0;
    }
    const uint64_t now = invocation_now_msec();
    if (now <= invocation_timing.submit_msec) {
        return 0;
    }
    return clamp_u32(now - invocation_timing.submit_msec);
}

static void maybe_nice_preprocess_only(int argv_result)
{
    if (!(argv_result & PreprocessOnly)) {
        return;
    }

    errno = 0;
    int current_nice = getpriority(PRIO_PROCESS, 0);
    if (current_nice == -1 && errno != 0) {
        current_nice = 0;
    }

    // "nice 20" for preprocess-only jobs: lowest scheduling priority.
    // Note: Linux nice range is [-20, 19].
    if (current_nice >= 19) {
        return;
    }

    if (setpriority(PRIO_PROCESS, 0, 19) != 0) {
        log_warning() << "failed to set nice(19) for preprocess-only job: "
                      << strerror(errno) << endl;
    }
}

void invocation_timing_reset()
{
    invocation_timing.submit_ts = uint32_t(time(nullptr));
    invocation_timing.submit_msec = invocation_now_msec();
    invocation_timing.enqueue_msec = 0;
    invocation_timing.start_msec = 0;
    invocation_timing.finish_msec = 0;
    invocation_timing.waitforcs_msec = 0;
    invocation_timing.local_queue_msec = 0;
    invocation_timing.exec_msec = 0;
    invocation_timing.scheduler_job_id = 0;
    invocation_timing.compile_job_id = 0;
    invocation_timing.exitcode = 0;
    invocation_timing.mode.clear();
    invocation_timing.sent = false;
}

void invocation_timing_mark_enqueue(const std::string &mode)
{
    if (!invocation_timing.enqueue_msec) {
        invocation_timing.enqueue_msec = invocation_elapsed_msec();
    }
    if (!mode.empty()) {
        invocation_timing.mode = mode;
    }
}

void invocation_timing_mark_start(const std::string &mode)
{
    if (!invocation_timing.start_msec) {
        invocation_timing.start_msec = invocation_elapsed_msec();
    }
    if (!mode.empty()) {
        invocation_timing.mode = mode;
    }
    if (invocation_timing.start_msec >= invocation_timing.enqueue_msec) {
        invocation_timing.local_queue_msec = invocation_timing.start_msec - invocation_timing.enqueue_msec;
    }
}

void invocation_timing_mark_finish(int exitcode)
{
    invocation_timing.finish_msec = invocation_elapsed_msec();
    invocation_timing.exitcode = exitcode;
    if (invocation_timing.finish_msec >= invocation_timing.start_msec) {
        invocation_timing.exec_msec = invocation_timing.finish_msec - invocation_timing.start_msec;
    }
    if (!invocation_timing.waitforcs_msec && invocation_timing.start_msec >= invocation_timing.enqueue_msec
            && invocation_timing.mode.find("remote") != std::string::npos) {
        invocation_timing.waitforcs_msec = invocation_timing.start_msec - invocation_timing.enqueue_msec;
    }
}

void invocation_timing_set_scheduler_job_id(uint32_t job_id)
{
    if (job_id && !invocation_timing.scheduler_job_id) {
        invocation_timing.scheduler_job_id = job_id;
    }
}

void invocation_timing_set_compile_job_id(uint32_t job_id)
{
    if (job_id && !invocation_timing.compile_job_id) {
        invocation_timing.compile_job_id = job_id;
    }
}

bool invocation_timing_send(MsgChannel *local_daemon)
{
    if (!local_daemon || invocation_timing.sent || !invocation_timing.finish_msec
            || !IS_PROTOCOL_VERSION(PROTOCOL_VERSION_JOB_TIMING, local_daemon)) {
        return false;
    }

    const JobTimingMsg msg(invocation_timing.submit_ts,
                           invocation_timing.enqueue_msec,
                           invocation_timing.start_msec,
                           invocation_timing.finish_msec,
                           invocation_timing.waitforcs_msec,
                           invocation_timing.local_queue_msec,
                           invocation_timing.exec_msec,
                           invocation_timing.scheduler_job_id,
                           invocation_timing.compile_job_id,
                           invocation_timing.exitcode,
                           invocation_timing.mode.empty() ? string("unknown") : invocation_timing.mode);
    const bool ok = local_daemon->send_msg(msg);
    if (ok) {
        invocation_timing.sent = true;
    }
    return ok;
}

static std::string shell_quote_arg(const std::string &arg)
{
    if (arg.empty()) {
        return "''";
    }

    if (arg.find_first_of(" \t\r\n'\"`$\\|&;<>()[\\]{}*?!") == std::string::npos) {
        return arg;
    }

    std::string quoted = "'";
    for (char c : arg) {
        if (c == '\'') {
            quoted += "'\"'\"'";
        } else {
            quoted += c;
        }
    }
    quoted += "'";
    return quoted;
}

static std::string format_command_line(int argc, char **argv)
{
    if (!argv || argc <= 0) {
        return std::string();
    }

    std::string out;
    for (int i = 0; i < argc; ++i) {
        if (i) {
            out += ' ';
        }
        out += shell_quote_arg(argv[i] ? std::string(argv[i]) : std::string());
    }
    return out;
}

static void dcc_show_usage()
{
    printf(
        "Usage:\n"
        "   icecc [compiler] [compile options] -o OBJECT -c SOURCE\n"
        "   icecc --build-native [compiler] [file...]\n"
        "   icecc --help\n"
        "\n"
        "Options:\n"
        "   --help                     explain usage and exit\n"
        "   --version                  show version and exit\n"
        "   --build-native             create icecc environment\n"
        "   --dump-daemon              print local iceccd internals and exit\n"
        "Environment Variables:\n"
        "   ICECC                      If set to \"no\", just exec the real compiler.\n"
        "                              If set to \"disable\", just exec the real compiler, but without\n"
        "                              notifying the daemon and only run one job at a time.\n"
        "   ICECC_VERSION              use a specific icecc environment, see icecc-create-env\n"
        "   ICECC_DEBUG                [info | warning | debug]\n"
        "                              sets verboseness of icecream client.\n"
        "   ICECC_LOGFILE              if set, additional debug information is logged to the specified file\n"
        "   ICECC_REPEAT_RATE          the number of jobs out of 1000 that should be\n"
        "                              compiled on multiple hosts to ensure that they're\n"
        "                              producing the same output.  The default is 0.\n"
        "   ICECC_PREFERRED_HOST       overrides scheduler decisions if set.\n"
        "   ICECC_CC                   set C compiler name (default gcc).\n"
        "   ICECC_CXX                  set C++ compiler name (default g++).\n"
        "   ICECC_REMOTE_CPP           set to 1 or 0 to override remote preprocessing\n"
        "   ICECC_IGNORE_UNVERIFIED    if set, hosts where environment cannot be verified are not used.\n"
        "   ICECC_EXTRAFILES           additional files used in the compilation.\n"
        "   ICECC_COLOR_DIAGNOSTICS    set to 1 or 0 to override color diagnostics support.\n"
        "   ICECC_CARET_WORKAROUND     set to 1 or 0 to override gcc show caret workaround.\n"
        "   ICECC_COMPRESSION          if set, the libzstd compression level (1 to 19, default: 1)\n"
        "   ICECC_ENV_COMPRESSION      compression type for icecc environments [none|gzip|bzip2|zstd|xz]\n"
        "   ICECC_SLOW_NETWORK         set to 1 to send network data in smaller chunks\n"
        );
}

static void icerun_show_usage()
{
    printf(
        "Usage:\n"
        "   icerun [command]\n"
        "   icerun --help\n"
        "\n"
        "Options:\n"
        "   --help                     explain usage and exit\n"
        "   --version                  show version and exit\n"
        "Environment Variables:\n"
        "   ICECC                      if set to \"no\", just exec the real command\n"
        "   ICECC_DEBUG                [info | warning | debug]\n"
        "                              sets verboseness of icecream client.\n"
        "   ICECC_LOGFILE              if set, additional debug information is logged to the specified file\n"
        "\n");
}

volatile bool local = false;

static void dcc_client_signalled(int whichsig)
{
    if (!local) {
#ifdef HAVE_STRSIGNAL
        log_info() << rs_program_name << ": " << strsignal(whichsig) << endl;
#else
        log_info() << "terminated by signal " << whichsig << endl;
#endif
        //    dcc_cleanup_tempfiles();
    }

    signal(whichsig, SIG_DFL);
    raise(whichsig);
}

static void dcc_client_catch_signals()
{
    signal(SIGTERM, &dcc_client_signalled);
    signal(SIGINT, &dcc_client_signalled);
    signal(SIGHUP, &dcc_client_signalled);
}

/*
 * @param args Are [compiler] [extra files...]
 * Compiler can be "gcc", "clang" or a binary (possibly including a path).
 */
static int create_native(char **args)
{
    char **extrafiles = args;
    string machine_name = determine_platform();

    string compiler = "gcc";
    if (machine_name.compare(0, 6, "Darwin") == 0) {
        compiler = "clang";
    }
    if (args[0]) {
        if( strcmp(args[0], "clang") == 0 || strcmp(args[0], "gcc") == 0 ) {
            compiler = args[ 0 ];
            ++extrafiles;
        } else if( access( args[0], R_OK ) == 0 && access( args[ 0 ], X_OK ) != 0 ) {
            // backwards compatibility, the first argument is already an extra file
        } else {
            compiler = compiler_path_lookup( get_c_compiler( args[ 0 ] ));
            if (compiler.empty()) {
                log_error() << "compiler not found" << endl;
                return 1;
            }
            ++extrafiles;
        }
    }

    vector<char*> argv;

    argv.push_back(strdup(BINDIR "/icecc-create-env"));
    argv.push_back(strdup(compiler.c_str()));

    for (int extracount = 0; extrafiles[extracount]; extracount++) {
        argv.push_back(strdup("--addfile"));
        argv.push_back(strdup(extrafiles[extracount]));
    }

    if( const char* env_compression = getenv( "ICECC_ENV_COMPRESSION" )) {
        argv.push_back(strdup("--compression"));
        argv.push_back(strdup(env_compression));
    }

    argv.push_back(nullptr);
    execv(argv[0], argv.data());
    ostringstream errmsg;
    errmsg << "execv " << argv[0] << " failed";
    log_perror(errmsg.str());
    return 1;
}

static MsgChannel* get_local_daemon()
{
    MsgChannel* local_daemon;
    if (getenv("ICECC_TEST_SOCKET") == nullptr) {
        /* try several options to reach the local daemon - 3 sockets, one TCP */
        local_daemon = Service::createChannel("/var/run/icecc/iceccd.socket");

        if (!local_daemon) {
            local_daemon = Service::createChannel("/var/run/iceccd.socket");
        }

        if (!local_daemon && getenv("HOME")) {
            string path = getenv("HOME");
            path += "/.iceccd.socket";
            local_daemon = Service::createChannel(path);
        }

        if (!local_daemon) {
            local_daemon = Service::createChannel("127.0.0.1", 10245, 0/*timeout*/);
        }
    } else {
        local_daemon = Service::createChannel(getenv("ICECC_TEST_SOCKET"));
        if (!local_daemon) {
            log_error() << "test socket error" << endl;
            exit( EXIT_TEST_SOCKET_ERROR );
        }
    }
    return local_daemon;
}

static void debug_arguments(int argc, char** argv, bool original)
{
    const string argstxt = format_command_line(argc, argv);
    if( original ) {
        trace() << "invoked as: " << argstxt << endl;
    } else {
        trace() << "expanded as: " << argstxt << endl;
    }
}

class ArgumentExpander
{
public:
    ArgumentExpander(int *argcp, char ***argvp)
    {
        oldargv = *argvp;
        oldargc = *argcp;
        expandargv(argcp, argvp);

        newargv = *argvp;
        if (newargv == oldargv)
            newargv = nullptr;
    }

    ~ArgumentExpander()
    {
        if (newargv != nullptr)
            freeargv(newargv);
    }

    bool changed() const
    {
        return newargv != nullptr;
    }

    char** originalArgv() const
    {
        return oldargv;
    }

    int originalArgc() const
    {
        return oldargc;
    }

private:
    char ** newargv;
    char ** oldargv;
    int oldargc;
};

int main(int argc, char **argv)
{
    // expand @responsefile contents to arguments in argv array
    ArgumentExpander expand(&argc, &argv);

    const char *env = getenv("ICECC_DEBUG");
    int debug_level = Error;

    if (env) {
        if (!strcasecmp(env, "info"))  {
            debug_level = Info;
        } else if (!strcasecmp(env, "warning") || !strcasecmp(env, "warnings")) {
            // "warnings was referred to in the --help output, handle it
            // backwards compatibility.
            debug_level = Warning;
        } else { // any other value
            debug_level = Debug;
        }
    }

    std::string logfile;

    if (const char *logfileEnv = getenv("ICECC_LOGFILE")) {
        logfile = logfileEnv;
    }

    setup_debug(debug_level, logfile, "ICECC");

    debug_arguments(expand.originalArgc(), expand.originalArgv(), true);
    if( expand.changed()) {
        debug_arguments(argc, argv, false);
    }
    invocation_cmdline = format_command_line(argc, argv);
    invocation_timing_reset();

    CompileJob job;
    bool icerun = false;

    string compiler_name = argv[0];
    dcc_client_catch_signals();

    std::string cwd = get_cwd();
    if(!cwd.empty())
        job.setWorkingDirectory( cwd );

    if (find_basename(compiler_name) == rs_program_name) {
        if (argc > 1) {
            string arg = argv[1];

            if (arg == "--help") {
                dcc_show_usage();
                return 0;
            }

            if (arg == "--version") {
                printf("ICECC " VERSION "\n");
                return 0;
            }

            if (arg == "--build-native") {
                return create_native(argv + 2);
            }

            if (arg == "--dump-daemon" || arg == "--daemon-internals") {
                MsgChannel *daemon = get_local_daemon();
                if (!daemon) {
                    fprintf(stderr, "icecc: unable to connect to local daemon\n");
                    return 1;
                }

                if (!daemon->send_msg(GetInternalStatus())) {
                    fprintf(stderr, "icecc: failed to request daemon internals\n");
                    delete daemon;
                    return 1;
                }

                Msg *msg = daemon->get_msg(10);
                if (!msg) {
                    fprintf(stderr, "icecc: daemon did not respond\n");
                    delete daemon;
                    return 1;
                }

                if (*msg != Msg::STATUS_TEXT) {
                    fprintf(stderr, "icecc: unexpected reply type '%s'\n", msg->to_string().c_str());
                    delete msg;
                    delete daemon;
                    return 1;
                }

                StatusTextMsg *status_msg = dynamic_cast<StatusTextMsg *>(msg);
                if (status_msg) {
                    fwrite(status_msg->text.data(), 1, status_msg->text.size(), stdout);
                }

                delete msg;
                delete daemon;
                return 0;
            }

            if (arg.size() > 0) {
                job.setCompilerName(arg);
                job.setCompilerPathname(arg);
            }
        }
    } else if (find_basename(compiler_name) == "icerun") {
        icerun = true;

        if (argc > 1) {
            string arg = argv[1];

            if (arg == "--help") {
                icerun_show_usage();
                return 0;
            }

            if (arg == "--version") {
                printf("ICERUN " VERSION "\n");
                return 0;
            }

            if (arg.size() > 0) {
                job.setCompilerName(arg);
                job.setCompilerPathname(arg);
            }
        }
    } else {
        std::string resolved;

        // check if it's a symlink to icerun
        if (resolve_link(compiler_name, resolved) == 0 && find_basename(resolved) == "icerun") {
            icerun = true;
        }
    }

    int sg_level = dcc_recursion_safeguard();

    if (sg_level >= SafeguardMaxLevel) {
        log_error() << "icecream seems to have invoked itself recursively!" << endl;
        return EXIT_RECURSION;
    }
    if (sg_level > 0) {
        log_info() << "recursive invocation from icerun" << endl;
    }

    /* Ignore SIGPIPE; we consistently check error codes and will
     * see the EPIPE. */
    dcc_ignore_sigpipe(1);

    // Connect to the daemon as early as possible, so that in parallel builds there
    // the daemon has as many connections as possible when we start asking for a remote
    // node to build, allowing the daemon/scheduler to do load balancing based on the number
    // of expected build jobs.
    MsgChannel *local_daemon = nullptr;
    const char *icecc = getenv("ICECC");
    if (icecc == nullptr || strcasecmp(icecc, "disable") != 0) {
        local_daemon = get_local_daemon();
    }

    list<string> extrafiles;
    string local_reason;
    auto set_local_reason = [&](const string &reason) {
        if (local_reason.empty()) {
            local_reason = reason;
        }
    };
    bool fulljob = false;
    int argv_result = analyse_argv(argv, job, icerun, &extrafiles);
    if( argv_result & AlwaysLocal ) {
        local = true;
        set_local_reason("argv_always_local");
        fulljob = argv_result & FullJob;
    }
    maybe_nice_preprocess_only(argv_result);

    /* If ICECC is set to disable, then run job locally, without contacting
       the daemon at all. File-based locking will still ensure that all
       calls are serialized up to the number of local cpus available.
       If ICECC is set to no, the job is run locally as well, but it is
       serialized using the daemon.
     */
    if (icecc && !strcasecmp(icecc, "disable")) {
        assert( local_daemon == NULL );
        return build_local(job, nullptr);
    }

    if (icecc && !strcasecmp(icecc, "no")) {
        local = true;
        set_local_reason("icecc_env_no");
    }

    if (!local_daemon) {
        log_warning() << "no local daemon found" << endl;
        return build_local(job, nullptr);
    }

    if (const char *extrafilesenv = getenv("ICECC_EXTRAFILES")) {
        for (;;) {
            const char *colon = strchr(extrafilesenv, ':');
            string file;

            if (colon == nullptr) {
                file = extrafilesenv;
            } else {
                file = string(extrafilesenv, colon - extrafilesenv);
            }

            file = get_absfilename(file);

            struct stat st;
            if (stat(file.c_str(), &st) == 0) {
                extrafiles.push_back(file);
            } else {
                log_warning() << "File in ICECC_EXTRAFILES not found: " << file << endl;
                local = true;
                set_local_reason("extrafile_missing");
                break;
            }

            if (colon == nullptr) {
                break;
            }

            extrafilesenv = colon + 1;
        }
    }

    Environments envs;

    if (!local) {
        if (getenv("ICECC_VERSION")) {     // if set, use it, otherwise take default
            try {
                envs = parse_icecc_version(job.targetPlatform(), find_prefix(job.compilerName()));
            } catch (std::exception& e) {
                // we just build locally
                log_error() <<  "An exception was handled parsing the icecc version.   "
                    "Will build locally.  Exception text was:\n" << e.what() << "\n";
                local = true;
                set_local_reason("icecc_version_parse_failed");
            }
        } else if (!extrafiles.empty() && !IS_PROTOCOL_VERSION(32, local_daemon)) {
            log_warning() << "Local daemon is too old to handle extra files." << endl;
            local = true;
            set_local_reason("daemon_too_old_for_extrafiles");
        } else {
            Msg *umsg = nullptr;
            string compiler;
            if( IS_PROTOCOL_VERSION(41, local_daemon))
                compiler = get_absfilename( find_compiler( job ));
            else // Older daemons understood only two hardcoded compilers.
                compiler = compiler_is_clang(job) ? "clang" : "gcc";
            string env_compression; // empty = default
            if( const char* icecc_env_compression = getenv( "ICECC_ENV_COMPRESSION" ))
                env_compression = icecc_env_compression;
            trace() << "asking for native environment for " << compiler << endl;
            if (!local_daemon->send_msg(GetNativeEnvMsg(compiler, extrafiles,
                env_compression))) {
                log_warning() << "failed to write get native environment" << endl;
                local = true;
                set_local_reason("get_native_env_send_failed");
            } else {
                // the timeout is high because it creates the native version
                umsg = local_daemon->get_msg(4 * 60);
            }

            string native;

            if (umsg && *umsg == Msg::NATIVE_ENV) {
                native = static_cast<UseNativeEnvMsg*>(umsg)->nativeVersion;
            }

            if (native.empty() || ::access(native.c_str(), R_OK) < 0) {
                log_warning() << "daemon can't determine native environment. "
                              "Set $ICECC_VERSION to an icecc environment.\n";
            } else {
                envs.push_back(make_pair(job.targetPlatform(), native));
                log_info() << "native " << native << endl;
            }

            delete umsg;
        }

        // we set it to local so we tell the local daemon about it - avoiding file locking
        if (envs.size() == 0) {
            local = true;
            set_local_reason("no_usable_environment");
        }

        for (Environments::const_iterator it = envs.begin(); it != envs.end(); ++it) {
            trace() << "env: " << it->first << " '" << it->second << "'" << endl;

            if (::access(it->second.c_str(), R_OK) < 0) {
                log_error() << "can't read environment " << it->second << endl;
                local = true;
                set_local_reason("environment_unreadable");
            }
        }
    }

    int ret;

    if (!local) {
        try {
            // How many times out of 1000 should we recompile a job on
            // multiple hosts to confirm that the results are the same?
            const char *s = getenv("ICECC_REPEAT_RATE");
            int rate = s ? atoi(s) : 0;

            invocation_timing_mark_enqueue("remote");
            ret = build_remote(job, local_daemon, envs, rate);
            invocation_timing_mark_finish(ret);
            invocation_timing_send(local_daemon);

            /* We have to tell the local daemon that everything is fine and
               that the remote daemon will send the scheduler our done msg.
               If we don't, the local daemon will have to assume the job failed
               and tell the scheduler - and that fail message may arrive earlier
               than the remote daemon's success msg. */
            if (ret == 0) {
                local_daemon->send_msg(EndMsg());
            }
        } catch (remote_error& error) {
            // log the 'local cpp invocation failed' message by default, so that it's more
            // obvious why the cpp output is there (possibly) twice
            if( error.errorCode == 103 )
                log_error() << "local build forced by remote exception: " << error.what() << endl;
            else
                log_warning() << "local build forced by remote exception: " << error.what() << endl;
            local = true;
            set_local_reason("remote_exception_" + std::to_string(error.errorCode));
        }
        catch (client_error& error) {
            if (remote_daemon.size()) {
                log_error() << "got exception " << error.what()
                            << " (" << remote_daemon.c_str() << ") " << endl;
            } else {
                log_error() << "got exception " << error.what() << " (this should be an exception!)" <<
                            endl;
            }

#if 0
            /* currently debugging a client? throw an error then */
            if (debug_level > Error) {
                return error.errorCode;
            }
#endif

            local = true;
            set_local_reason("client_exception_" + std::to_string(error.errorCode));
        }
        if (local) {
            // TODO It'd be better to reuse the connection, but the daemon
            // internal state gets confused for some reason, so work that around
            // for now by using a new connection.
            delete local_daemon;
            local_daemon = get_local_daemon();
            if (!local_daemon) {
                log_warning() << "no local daemon found" << endl;
                return build_local(job, nullptr);
            }
        }
    }

    if (local) {
        log_block b("building_local");
        struct rusage ru;
        Msg *startme = nullptr;
        uint32_t local_job_flags = JobLocalBeginMsg::LocalFlagNone;
        if (argv_result & PreprocessOnly) {
            local_job_flags |= JobLocalBeginMsg::LocalFlagPreprocessOnly;
        }

        /* Inform the daemon that we like to start a job.  */
        if (invocation_timing.mode.find("remote") != std::string::npos) {
            invocation_timing_mark_enqueue("fallback_local");
        } else {
            invocation_timing_mark_enqueue("local");
        }
        if (local_daemon->send_msg(JobLocalBeginMsg(0, get_absfilename(job.outputFile()), fulljob,
                                                    local_reason.empty() ? "unknown" : local_reason,
                                                    invocation_cmdline, local_job_flags))) {
            /* Now wait until the daemon gives us the start signal.  40 minutes
               should be enough for all normal compile or link jobs, but with expensive jobs
               (which fulljobs may likely be, e.g. LTO linking) use an even larger timeout.  */
            startme = local_daemon->get_msg(fulljob ? 120 * 60 : 40 * 60);
        }

        /* If we can't talk to the daemon anymore we need to fall back
           to lock file locking.  */
        if (!startme || *startme != Msg::JOB_LOCAL_BEGIN) {
            delete startme;
            delete local_daemon;
            return build_local(job, nullptr);
        }

        invocation_timing_mark_start("local");
        ret = build_local(job, local_daemon, &ru);
        invocation_timing_set_compile_job_id(job.jobID());
        invocation_timing_mark_finish(ret);
        invocation_timing_send(local_daemon);
        delete startme;
    }

    delete local_daemon;
    return ret;
}
