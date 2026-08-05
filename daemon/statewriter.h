/* -*- mode: C++; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 99; -*- */
/* vim: set ts=4 sw=4 et tw=99:  */
/*
    This file is part of Icecream.

    Storage sink for state records, running in its OWN PROCESS.

    A writer thread is not a safe fit for a daemon that keeps forking
    compile workers: fork() clones only the calling thread, so a child
    created while the writer holds allocator or libc state is
    nondeterministically fragile no matter how careful the child is.  A
    separate process also makes the nonblocking contract real -- O_NONBLOCK
    is honoured on pipes, unlike regular files, so the daemon's control
    loop performs only nonblocking pipe writes and the writer process does
    all file I/O, where blocking is free.

    Daemon side: a bounded drop-oldest queue of framed records (per-record
    cap included, and the in-flight frame's remaining bytes count toward
    the bound), pumped opportunistically with nonblocking writes; a partial
    frame write preserves its byte offset.  Writer side: reads frames,
    appends to the JSONL file (offset-preserving across partial writes and
    5s error backoff) or the state-log file (open-append-close per record,
    so rotation needs no signal).  Shutdown closes the pipe and waits a
    bounded drain interval before killing the writer.

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

#ifndef ICECREAM_STATEWRITER_H
#define ICECREAM_STATEWRITER_H

#include <stdint.h>
#include <sys/types.h>

#include <deque>
#include <string>

class StateWriter {
public:
    enum Sink {
        SINK_JSONL = 0,
        SINK_STATELOG = 1,
    };

    ~StateWriter();

    /* Fork the writer process.  Call once, after daemonize() and before
       job handling begins; no threads may exist in the daemon.  Paths may
       be empty individually; records for an unconfigured sink are
       discarded at enqueue.  Returns false if the writer could not be
       started (the daemon keeps running; records are then dropped).  */
    bool start(const std::string &jsonl_path, const std::string &statelog_path);

    bool started() const { return m_pid > 0; }
    bool alive();   // reaps on demand; false once the writer has exited

    /* Queue one record (a full line, no trailing newline).  Never blocks;
       drops the oldest queued frames when the bound is exceeded.  */
    void enqueue(Sink sink, const std::string &line);

    /* Nonblocking pipe pump; call from the main loop.  Returns true while
       queued bytes remain (caller may re-poll sooner if it wants).  */
    bool pump();

    /* Close the pipe, give the writer up to drain_msec to finish, then
       kill it.  Remaining queued frames are counted as dropped.  */
    void shutdown(unsigned int drain_msec);

    uint64_t dropped() const { return m_dropped; }
    uint64_t oversized() const { return m_oversized; }
    size_t queued_bytes() const { return m_queued_bytes; }

private:
    static const size_t max_queued_bytes = 4 * 1024 * 1024;
    static const size_t max_record_bytes = 256 * 1024;

    void drop_from_front_until_bounded();

    pid_t m_pid = -1;
    int m_pipe_fd = -1;          // daemon's write end, O_NONBLOCK
    std::deque<std::string> m_queue;   // fully framed records
    size_t m_queued_bytes = 0;   // includes the in-flight frame's remainder
    size_t m_front_ofs = 0;      // bytes of m_queue.front() already written
    uint64_t m_dropped = 0;
    uint64_t m_oversized = 0;
};

/* Writer-process main loop; exposed for the unit test, which runs it in a
   forked child exactly as start() does.  Reads frames from pipe_fd until
   EOF, then drains and returns.  */
void statewriter_child_loop(int pipe_fd, const std::string &jsonl_path,
                            const std::string &statelog_path);

#endif
