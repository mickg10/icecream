/* -*- mode: C++; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 99; -*- */
/* vim: set ts=4 sw=4 et tw=99:  */
/*
    This file is part of Icecream.  See statewriter.h for the design.

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

#include "statewriter.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <string>
#include <vector>

/* Frame: 4-byte little-endian payload length, 1 sink byte, payload.  */
static const size_t kFrameHeader = 5;

static std::string frame_record(StateWriter::Sink sink, const std::string &line)
{
    std::string frame;
    const uint32_t len = (uint32_t)line.size();
    frame.reserve(kFrameHeader + line.size());
    frame.push_back(char(len & 0xff));
    frame.push_back(char((len >> 8) & 0xff));
    frame.push_back(char((len >> 16) & 0xff));
    frame.push_back(char((len >> 24) & 0xff));
    frame.push_back(char(sink));
    frame += line;
    return frame;
}

StateWriter::~StateWriter()
{
    if (started()) {
        shutdown(2000);
    }
}

bool StateWriter::start(const std::string &jsonl_path, const std::string &statelog_path)
{
    if (started()) {
        return true;
    }
    int fds[2];
    if (pipe(fds) < 0) {
        return false;
    }
    const pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return false;
    }
    if (pid == 0) {
        /* Writer process.  Keep only the pipe read end and stderr; the
           daemon's sockets and files are none of our business (and must
           not be pinned here).  */
        close(fds[1]);
        signal(SIGHUP, SIG_IGN);
        signal(SIGINT, SIG_IGN);
        signal(SIGTERM, SIG_DFL);
        statewriter_child_loop(fds[0], jsonl_path, statelog_path);
        _exit(0);
    }
    close(fds[0]);
    fcntl(fds[1], F_SETFL, O_NONBLOCK);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    m_pipe_fd = fds[1];
    m_pid = pid;
    return true;
}

bool StateWriter::alive()
{
    if (m_pid <= 0) {
        return false;
    }
    int status = 0;
    const pid_t r = waitpid(m_pid, &status, WNOHANG);
    if (r == m_pid || (r < 0 && errno == ECHILD)) {
        /* Exited -- possibly already reaped by the daemon's generic
           waitpid(-1) zombie sweep, which collects this pid like any other
           child's.  */
        m_pid = -1;
        return false;
    }
    return true;
}

void StateWriter::drop_from_front_until_bounded()
{
    while (m_queued_bytes > max_queued_bytes && m_queue.size() > 1) {
        /* Never drop the frame currently in flight (front while
           m_front_ofs > 0): a torn frame would desynchronise the stream.
           Drop the oldest COMPLETE frame instead.  */
        const size_t victim = m_front_ofs ? 1 : 0;
        m_queued_bytes -= m_queue[victim].size();
        m_queue.erase(m_queue.begin() + victim);
        ++m_dropped;
    }
}

void StateWriter::enqueue(Sink sink, const std::string &line)
{
    if (!started()) {
        return;
    }
    if (line.size() > max_record_bytes) {
        /* One pathological record must not displace the whole queue's
           worth of history.  */
        ++m_oversized;
        return;
    }
    m_queue.push_back(frame_record(sink, line));
    m_queued_bytes += m_queue.back().size();
    drop_from_front_until_bounded();
    pump();
}

bool StateWriter::pump()
{
    if (m_pipe_fd < 0) {
        return false;
    }
    while (!m_queue.empty()) {
        const std::string &front = m_queue.front();
        const size_t remaining = front.size() - m_front_ofs;
        const ssize_t n = write(m_pipe_fd, front.data() + m_front_ofs, remaining);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return true;    // pipe full; the writer is behind
            }
            /* Writer gone (EPIPE) or the pipe is broken: stop writing and
               count everything still queued as dropped.  */
            close(m_pipe_fd);
            m_pipe_fd = -1;
            m_dropped += m_queue.size();
            m_queue.clear();
            m_queued_bytes = 0;
            m_front_ofs = 0;
            return false;
        }
        m_front_ofs += (size_t)n;
        m_queued_bytes -= (size_t)n;
        if (m_front_ofs >= front.size()) {
            m_queue.pop_front();
            m_front_ofs = 0;
        }
    }
    return false;
}

void StateWriter::shutdown(unsigned int drain_msec)
{
    if (m_pipe_fd >= 0) {
        pump();
        if (!m_queue.empty()) {
            m_dropped += m_queue.size();
            m_queue.clear();
            m_queued_bytes = 0;
            m_front_ofs = 0;
        }
        close(m_pipe_fd);   // EOF tells the writer to drain and exit
        m_pipe_fd = -1;
    }
    if (m_pid > 0) {
        const unsigned int step_ms = 50;
        unsigned int waited = 0;
        for (;;) {
            int status = 0;
            const pid_t r = waitpid(m_pid, &status, WNOHANG);
            if (r == m_pid || (r < 0 && errno == ECHILD)) {
                m_pid = -1;   // exited (or already reaped by the daemon)
                return;
            }
            if (waited >= drain_msec) {
                break;
            }
            struct timespec ts = { 0, (long)step_ms * 1000000L };
            nanosleep(&ts, nullptr);
            waited += step_ms;
        }
        /* Wait only for the configured drain interval, then stop caring:
           a blocked regular-file write must not block daemon shutdown.  */
        kill(m_pid, SIGKILL);
        waitpid(m_pid, nullptr, 0);
        m_pid = -1;
    }
}

/* ---------------- writer-process side ---------------- */

/* Append with a per-record byte offset preserved across partial writes and
   error backoff: a record is never restarted from byte zero, so a failure
   after a partial write cannot leave a prefix followed by a duplicate of
   the whole record (which would corrupt the JSONL stream).  Returns false
   only if the record had to be abandoned (fd trouble persisted).  */
static bool write_record_offset(int &fd, const char *path, int open_flags,
                                const std::string &payload, size_t &ofs)
{
    for (int attempt = 0; attempt < 60; ++attempt) {
        if (fd < 0) {
            fd = open(path, open_flags, 0644);
            if (fd < 0) {
                struct timespec ts = { 5, 0 };
                nanosleep(&ts, nullptr);
                continue;
            }
        }
        while (ofs < payload.size()) {
            const ssize_t n = write(fd, payload.data() + ofs, payload.size() - ofs);
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n <= 0) {
                close(fd);
                fd = -1;
                break;
            }
            ofs += (size_t)n;
        }
        if (ofs >= payload.size()) {
            return true;
        }
        struct timespec ts = { 5, 0 };
        nanosleep(&ts, nullptr);
    }
    return false;
}

static bool read_full(int fd, char *buf, size_t want)
{
    size_t got = 0;
    while (got < want) {
        const ssize_t n = read(fd, buf + got, want - got);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return false;   // EOF or error
        }
        got += (size_t)n;
    }
    return true;
}

void statewriter_child_loop(int pipe_fd, const std::string &jsonl_path,
                            const std::string &statelog_path)
{
    int jsonl_fd = -1;
    for (;;) {
        char header[kFrameHeader];
        if (!read_full(pipe_fd, header, sizeof(header))) {
            break;   // daemon closed the pipe: drain done
        }
        const uint32_t len = (uint32_t)(unsigned char)header[0]
                           | ((uint32_t)(unsigned char)header[1] << 8)
                           | ((uint32_t)(unsigned char)header[2] << 16)
                           | ((uint32_t)(unsigned char)header[3] << 24);
        const int sink = (unsigned char)header[4];
        if (len > 1024 * 1024) {
            break;   // corrupt frame; nothing downstream can be trusted
        }
        std::string payload(len, '\0');
        if (len && !read_full(pipe_fd, &payload[0], len)) {
            break;
        }
        payload += '\n';

        if (sink == StateWriter::SINK_JSONL && !jsonl_path.empty()) {
            size_t ofs = 0;
            write_record_offset(jsonl_fd, jsonl_path.c_str(),
                                O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC,
                                payload, ofs);
        } else if (sink == StateWriter::SINK_STATELOG && !statelog_path.empty()) {
            /* Open-append-close per record: rotation and reopen need no
               signalling, and the record rate here is one per interval.  */
            int fd = -1;
            size_t ofs = 0;
            write_record_offset(fd, statelog_path.c_str(),
                                O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC,
                                payload, ofs);
            if (fd >= 0) {
                close(fd);
            }
        }
    }
    if (jsonl_fd >= 0) {
        close(jsonl_fd);
    }
    close(pipe_fd);
}
