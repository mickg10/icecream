/*
    stage_trace (services/stage_trace.h): proves the off-knob path opens no
    file descriptor and writes nothing, and that the on-knob path produces
    a well-formed JSONL record with the documented fields.

    Each scenario runs in its own forked child so that ICECC_STAGE_TRACE_DIR
    can be set (or deliberately left unset) before that child's first call
    to stage_trace(); the enabled() cache is a per-process function-local
    static, so this is the only reliable way to exercise both branches from
    one test binary.
*/

#include "../services/stage_trace.h"

#include <dirent.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

using icecc::stagetrace::stage_trace;
using icecc::stagetrace::KV;

static int failures = 0;

static void check(bool ok, const char *what)
{
    std::printf(ok ? "ok       - %s\n" : "FAILED   - %s\n", what);
    if (!ok) {
        ++failures;
    }
    std::fflush(stdout);
}

static int count_open_fds()
{
    DIR *d = opendir("/proc/self/fd");
    if (!d) {
        return -1;
    }
    int n = 0;
    while (dirent *e = readdir(d)) {
        if (e->d_name[0] != '.') {
            ++n;
        }
    }
    closedir(d);
    return n;
}

// Runs entirely inside the "off" child: ICECC_STAGE_TRACE_DIR must already
// be unset in this process's environment (the parent never set it, and
// this process never has).
static int run_off_child(const char *offdir)
{
    const int before = count_open_fds();
    for (int i = 0; i < 1000; ++i) {
        stage_trace("offtest", "noop_stage", i, i, i, i, {{"bytes", i}});
    }
    icecc::stagetrace::flush();
    const int after = count_open_fds();

    bool ok = true;
    ok &= (getenv("ICECC_STAGE_TRACE_DIR") == nullptr);
    ok &= !icecc::stagetrace::enabled();
    ok &= (before == after && before >= 0);

    DIR *d = opendir(offdir);
    if (d) {
        int entries = 0;
        while (dirent *e = readdir(d)) {
            if (e->d_name[0] != '.') {
                ++entries;
            }
        }
        closedir(d);
        ok &= (entries == 0);
    }
    return ok ? 0 : 1;
}

static int run_on_child(const char *ondir)
{
    setenv("ICECC_STAGE_TRACE_DIR", ondir, 1);
    stage_trace("testcomp", "stage_one", 42, 7, 3, 1, {{"bytes", 123}});
    icecc::stagetrace::flush();
    return 0;
}

static pid_t run_child(int (*fn)(const char *), const char *arg)
{
    pid_t pid = fork();
    if (pid == 0) {
        _exit(fn(arg));
    }
    return pid;
}

static bool wait_ok(pid_t pid)
{
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static std::string slurp_only_jsonl(const std::string &dir)
{
    DIR *d = opendir(dir.c_str());
    if (!d) {
        return "";
    }
    std::string name;
    while (dirent *e = readdir(d)) {
        if (std::strstr(e->d_name, ".jsonl")) {
            name = e->d_name;
            break;
        }
    }
    closedir(d);
    if (name.empty()) {
        return "";
    }
    std::ifstream in(dir + "/" + name);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static bool contains(const std::string &hay, const std::string &needle)
{
    return hay.find(needle) != std::string::npos;
}

int main()
{
    char offdir_tmpl[] = "/tmp/stagetrace_off_XXXXXX";
    char ondir_tmpl[] = "/tmp/stagetrace_on_XXXXXX";
    const char *offdir = mkdtemp(offdir_tmpl);
    const char *ondir = mkdtemp(ondir_tmpl);
    check(offdir != nullptr && ondir != nullptr, "temp dirs created");

    unsetenv("ICECC_STAGE_TRACE_DIR");
    pid_t off_pid = run_child(run_off_child, offdir);
    check(wait_ok(off_pid), "off-knob child: no fd opened, no file written, enabled()==false");

    pid_t on_pid = run_child(run_on_child, ondir);
    check(wait_ok(on_pid), "on-knob child exited cleanly");

    std::string content = slurp_only_jsonl(ondir);
    check(!content.empty(), "on-knob child wrote a .jsonl file");
    check(contains(content, "\"component\":\"testcomp\""), "record has component");
    check(contains(content, "\"stage\":\"stage_one\""), "record has stage");
    check(contains(content, "\"epoch\":42"), "record has epoch");
    check(contains(content, "\"wire_id\":7"), "record has wire_id");
    check(contains(content, "\"logical_job\":3"), "record has logical_job");
    check(contains(content, "\"attempt\":1"), "record has attempt");
    check(contains(content, "\"bytes\":123"), "record has kv extra");
    check(contains(content, "\"realtime_ns\":"), "record has realtime_ns");
    check(contains(content, "\"mono_ns\":"), "record has mono_ns");
    check(contains(content, "\"host\":\""), "record has host");
    check(contains(content, "\"pid\":"), "record has pid");
    check(contains(content, "\"tid\":"), "record has tid");

    // Best-effort cleanup; leave nothing behind in /tmp.
    std::string cleanup = std::string("rm -rf ") + offdir + " " + ondir;
    system(cleanup.c_str());

    return failures == 0 ? 0 : 1;
}
