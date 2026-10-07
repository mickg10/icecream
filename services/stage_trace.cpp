/* -*- mode: C++; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 99; -*- */
/* vim: set ts=4 sw=4 et tw=99:  */
/*
    This file is part of Icecream.

    Copyright (c) 2026 the Icecream maintainers

    See stage_trace.h for the contract. Implementation notes:

    - enabled() caches its result in a function-local static, initialized
      exactly once (C++11 guarantees thread-safe static init), so the
      getenv() happens at most once per process no matter how many
      threads call it concurrently.
    - The writer buffers into a std::string behind a mutex, one buffer+fd
      pair per distinct "component" string seen in this process (almost
      always exactly one, but a process that plays more than one role
      gets one file per role), and flushes with a plain write(2) when a
      buffer crosses kFlushBytes or on process exit (registered with
      atexit).
    - fsync()/fdatasync() are never called: traces are best-effort and the
      whole point is to not perturb the timing being measured.
*/

#include "stage_trace.h"

#include <fcntl.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <cstdlib>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>

namespace icecc::stagetrace {

namespace {

constexpr size_t kFlushBytes = 32 * 1024;

std::string json_escape(const char *s)
{
    std::string out;
    out.reserve(16);
    for (const char *p = s; *p; ++p) {
        unsigned char c = static_cast<unsigned char>(*p);
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    return out;
}

long cached_tid()
{
#if defined(SYS_gettid)
    return static_cast<long>(syscall(SYS_gettid));
#else
    return static_cast<long>(getpid());
#endif
}

std::string local_hostname()
{
    char host[256];
    if (gethostname(host, sizeof(host)) != 0) {
        host[0] = '\0';
    }
    host[sizeof(host) - 1] = '\0';
    std::string h(host);
    for (char &c : h) {
        if (c == '/' || c == ' ') {
            c = '_';
        }
    }
    return h;
}

struct Entry {
    std::string buffer;
    int fd = -1;
};

// Resolves ICECC_STAGE_TRACE_DIR once and owns one buffered fd per
// component string seen by this process.
class Writer {
public:
    static Writer &instance()
    {
        static Writer w;
        return w;
    }

    void append(const char *component, const std::string &line)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (dir_.empty()) {
            return;
        }
        Entry &e = entries_[component];
        if (e.fd < 0) {
            std::string path = dir_ + "/" + component + "-" + host_ + "-" + std::to_string(pid_) + ".jsonl";
            e.fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (e.fd < 0) {
                return; // best-effort: directory not writable, drop silently
            }
        }
        e.buffer += line;
        if (e.buffer.size() >= kFlushBytes) {
            flush_entry_locked(e);
        }
    }

    void flush()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto &kv : entries_) {
            flush_entry_locked(kv.second);
        }
    }

    void reset_after_fork()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Drop (not flush) whatever the parent had buffered: writing it
        // from the child too would duplicate it. The child keeps the
        // parent's open fds and path, which is intentional -- see header.
        for (auto &kv : entries_) {
            kv.second.buffer.clear();
        }
    }

private:
    Writer()
    {
        const char *dir = std::getenv("ICECC_STAGE_TRACE_DIR");
        if (dir && *dir) {
            dir_ = dir;
            host_ = local_hostname();
            pid_ = getpid();
            std::atexit(&Writer::atexit_flush);
        }
    }

    ~Writer() { flush(); }

    static void flush_entry_locked(Entry &e)
    {
        if (e.fd < 0 || e.buffer.empty()) {
            return;
        }
        const char *data = e.buffer.data();
        size_t remaining = e.buffer.size();
        while (remaining > 0) {
            ssize_t n = ::write(e.fd, data, remaining);
            if (n < 0) {
                break; // best-effort; drop rather than block/retry forever
            }
            data += static_cast<size_t>(n);
            remaining -= static_cast<size_t>(n);
        }
        e.buffer.clear();
    }

    static void atexit_flush() { Writer::instance().flush(); }

    std::mutex mutex_;
    std::unordered_map<std::string, Entry> entries_;
    std::string dir_;
    std::string host_;
    pid_t pid_ = 0;
};

} // namespace

bool enabled()
{
    static const bool kEnabled = []() {
        const char *dir = std::getenv("ICECC_STAGE_TRACE_DIR");
        return dir && *dir;
    }();
    return kEnabled;
}

void record(const char *component, const char *stage,
            int64_t epoch, int64_t wire_id,
            int64_t logical_job, int64_t attempt,
            std::initializer_list<KV> extras)
{
    struct timespec rt, mono;
    clock_gettime(CLOCK_REALTIME, &rt);
    clock_gettime(CLOCK_MONOTONIC, &mono);
    const int64_t realtime_ns = int64_t(rt.tv_sec) * 1000000000LL + rt.tv_nsec;
    const int64_t mono_ns = int64_t(mono.tv_sec) * 1000000000LL + mono.tv_nsec;

    std::string line;
    line.reserve(256);
    line += "{\"realtime_ns\":";
    line += std::to_string(realtime_ns);
    line += ",\"mono_ns\":";
    line += std::to_string(mono_ns);
    line += ",\"component\":\"";
    line += json_escape(component);
    line += "\",\"stage\":\"";
    line += json_escape(stage);
    line += "\",\"epoch\":";
    line += std::to_string(epoch);
    line += ",\"wire_id\":";
    line += std::to_string(wire_id);
    line += ",\"logical_job\":";
    line += std::to_string(logical_job);
    line += ",\"attempt\":";
    line += std::to_string(attempt);
    static const std::string host_cached = local_hostname();
    line += ",\"host\":\"";
    line += json_escape(host_cached.c_str());
    line += "\",\"pid\":";
    line += std::to_string(getpid());
    line += ",\"tid\":";
    line += std::to_string(cached_tid());
    for (const KV &kv : extras) {
        line += ",\"";
        line += json_escape(kv.key);
        line += "\":";
        line += std::to_string(kv.value);
    }
    line += "}\n";

    Writer::instance().append(component, line);
}

void reset_after_fork()
{
    Writer::instance().reset_after_fork();
}

void flush()
{
    Writer::instance().flush();
}

} // namespace icecc::stagetrace
