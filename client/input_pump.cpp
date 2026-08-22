/* -*- mode: C++; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 99; -*- */
/* vim: set ts=4 sw=4 et tw=99:  */
/*
    This file is part of Icecream.

    Copyright (c) 2026 Icecream contributors

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

#include "config.h"

#include "input_pump.h"

#include "client.h"
#include "comm.h"
#include "logging.h"

#include <cerrno>
#include <cstddef>
#include <unistd.h>

namespace
{

constexpr std::size_t LegacyChunkSize = 100000;

class OwnedInputFd
{
public:
    explicit OwnedInputFd(int fd)
        : fd_(fd)
    {
    }

    ~OwnedInputFd()
    {
        close_now();
    }

    OwnedInputFd(const OwnedInputFd &) = delete;
    OwnedInputFd &operator=(const OwnedInputFd &) = delete;

    int get() const
    {
        return fd_;
    }

    void close_now()
    {
        if (fd_ < 0) {
            return;
        }
        const int fd = fd_;
        fd_ = -1;
        if (close(fd) == -1 && errno != EBADF) {
            log_perror("close failed");
        }
    }

private:
    int fd_;
};

void throw_remote_status(const std::string &status, MsgChannel *channel)
{
    log_error() << "Remote status (compiled on " << channel->name << "): "
                << status << std::endl;
    throw client_error(23, "Error 23 - Remote status (compiled on " + channel->name
                               + ")\n" + status);
}

}

LegacyRemoteSink::LegacyRemoteSink(MsgChannel *channel)
    : channel_(channel)
{
}

void LegacyRemoteSink::send_fd(int fd)
{
    OwnedInputFd input(fd);
    unsigned char buffer[LegacyChunkSize];
    off_t offset = 0;
    std::size_t uncompressed = 0;
    std::size_t compressed = 0;

    do {
        ssize_t bytes;

        do {
            bytes = read(input.get(), buffer + offset, sizeof(buffer) - offset);

            if (bytes < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
                continue;
            }

            if (bytes < 0) {
                log_perror("write_fd_to_server() reading from fd");
                throw client_error(16, "Error 16 - error reading local file");
            }

            break;
        } while (1);

        offset += bytes;

        if (!bytes || offset == sizeof(buffer)) {
            if (offset) {
                FileChunkMsg fcmsg(buffer, offset);

                if (!channel_->send_msg(fcmsg)) {
                    const int send_errno = errno;
                    if (std::optional<std::string> status = channel_->take_error_status()) {
                        throw_remote_status(*status, channel_);
                    }

                    errno = send_errno;
                    log_error() << "write of source chunk to host "
                                << channel_->name.c_str() << std::endl;
                    log_perror("failed ");
                    throw client_error(15, "Error 15 - write to host failed");
                }

                uncompressed += fcmsg.len;
                compressed += fcmsg.compressed;
                offset = 0;
            }

            if (!bytes) {
                break;
            }
        }
    } while (1);

    if (compressed) {
        trace() << "sent " << compressed << " bytes (" << (compressed * 100 / uncompressed)
                << "%)" << std::endl;
    }
}

bool LegacyRemoteSink::send_end()
{
    return channel_->send_msg(EndMsg());
}
