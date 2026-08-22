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

#include "compiler_input.h"

#include "comm.h"
#include "workit.h"

#include <cerrno>
#include <memory>
#include <unistd.h>

LegacyChunkSource::LegacyChunkSource(MsgChannel *channel, int client_fd)
    : channel_(channel)
    , client_fd_(client_fd)
    , complete_(false)
    , offset_(0)
{
}

LegacyChunkSource::~LegacyChunkSource() = default;

int LegacyChunkSource::poll_fd() const
{
    return client_fd_;
}

bool LegacyChunkSource::complete() const
{
    return complete_;
}

bool LegacyChunkSource::has_pending() const
{
    return pending_ != nullptr;
}

std::size_t LegacyChunkSource::pending_size() const
{
    return pending_ ? pending_->len : 0;
}

std::size_t LegacyChunkSource::pending_compressed_size() const
{
    return pending_ ? pending_->compressed : 0;
}

std::size_t LegacyChunkSource::pending_offset() const
{
    return offset_;
}

CompilerInputReadResult LegacyChunkSource::read_next(unsigned int job_stat[])
{
    if (client_fd_ < 0 || pending_) {
        return CompilerInputReadResult::NoMessage;
    }

    std::unique_ptr<Msg> message(channel_->get_msg(0, true));
    if (message) {
        if (complete_) {
            return CompilerInputReadResult::MessageAfterEnd;
        }

        if (*message == Msg::END) {
            complete_ = true;
            return CompilerInputReadResult::End;
        }

        if (*message == Msg::FILE_CHUNK) {
            pending_.reset(static_cast<FileChunkMsg *>(message.release()));
            offset_ = 0;
            job_stat[JobStatistics::in_uncompressed] += pending_->len;
            job_stat[JobStatistics::in_compressed] += pending_->compressed;
            return CompilerInputReadResult::Chunk;
        }

        complete_ = true;
        return CompilerInputReadResult::UnexpectedMessage;
    }

    if (channel_->at_eof()) {
        complete_ = true;
        return CompilerInputReadResult::UnexpectedEof;
    }

    return CompilerInputReadResult::NoMessage;
}

CompilerInputWriteResult LegacyChunkSource::write_pending(int compiler_stdin_fd)
{
    if (!pending_) {
        return CompilerInputWriteResult::ChunkComplete;
    }

    const ssize_t bytes = write_bytes(compiler_stdin_fd, pending_->buffer + offset_,
                                      pending_->len - offset_);
    if (bytes < 0) {
        if (errno == EINTR) {
            return CompilerInputWriteResult::Interrupted;
        }
        discard_pending();
        return CompilerInputWriteResult::Failed;
    }

    offset_ += static_cast<std::size_t>(bytes);
    if (offset_ == pending_->len) {
        discard_pending();
        return CompilerInputWriteResult::ChunkComplete;
    }
    return CompilerInputWriteResult::Pending;
}

void LegacyChunkSource::discard_pending()
{
    pending_.reset();
    offset_ = 0;
}

void LegacyChunkSource::disable()
{
    client_fd_ = -1;
}

void LegacyChunkSource::mark_complete()
{
    complete_ = true;
}

ssize_t LegacyChunkSource::write_bytes(int fd, const void *buffer, std::size_t size)
{
    return write(fd, buffer, size);
}
