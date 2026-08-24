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
#include "digest128.h"
#include "workit.h"

#include <array>
#include <cerrno>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <sys/stat.h>
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
    if (bytes == 0) {
        // A nonempty pending chunk must make progress or fail.  Returning
        // Pending here would leave work_it in a writable-poll busy loop with
        // an unchanged offset.
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

P50AttachedFileSource::P50AttachedFileSource(
    int fd, uint64_t expected_bytes,
    std::array<uint8_t, 16> expected_digest)
    : fd_(fd)
    , expected_bytes_(expected_bytes)
{
    try {
        validate_and_rewind(expected_bytes, expected_digest);
    } catch (...) {
        close_input();
        throw;
    }
}

P50AttachedFileSource::~P50AttachedFileSource()
{
    close_input();
}

void P50AttachedFileSource::close_input() noexcept
{
    if (fd_ >= 0) {
        const int descriptor = fd_;
        fd_ = -1;
        /* On Linux an EINTR close has already released the descriptor; never
           retry against a number another thread could have reused. */
        (void)::close(descriptor);
    }
}

void P50AttachedFileSource::validate_and_rewind(
    uint64_t expected_bytes,
    const std::array<uint8_t, 16> &expected_digest)
{
    if (fd_ < 0)
        throw std::invalid_argument("P50 compiler input descriptor is absent");
    const int status_flags = ::fcntl(fd_, F_GETFL);
    if (status_flags < 0 || (status_flags & O_ACCMODE) != O_RDONLY)
        throw std::invalid_argument("P50 compiler input descriptor is not read-only");
    const int descriptor_flags = ::fcntl(fd_, F_GETFD);
    if (descriptor_flags < 0 ||
        ::fcntl(fd_, F_SETFD, descriptor_flags | FD_CLOEXEC) < 0)
        throw std::runtime_error("P50 compiler input descriptor cannot be made CLOEXEC");

    struct stat metadata{};
    if (::fstat(fd_, &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
        metadata.st_size < 0 ||
        static_cast<uint64_t>(metadata.st_size) != expected_bytes)
        throw std::invalid_argument("P50 compiler input length does not match its claim");
#if defined(F_GET_SEALS) && defined(F_SEAL_WRITE) && defined(F_SEAL_GROW) && defined(F_SEAL_SHRINK)
    const int seals = ::fcntl(fd_, F_GET_SEALS);
    constexpr int required_seals = F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK;
    if (seals < 0 || (seals & required_seals) != required_seals)
        throw std::invalid_argument("P50 compiler input descriptor is not immutable");
#else
    throw std::runtime_error("P50 compiler input sealing is unavailable");
#endif
    if (expected_bytes > std::numeric_limits<size_t>::max())
        throw std::length_error("P50 compiler input exceeds this process address space");
    if (::lseek(fd_, 0, SEEK_SET) != 0)
        throw std::invalid_argument("P50 compiler input is not a byte-zero cursor");

    icecc::Digest128Builder digest;
    std::array<uint8_t, 100000> validation_buffer{};
    uint64_t total = 0;
    for (;;) {
        ssize_t bytes = ::read(fd_, validation_buffer.data(), validation_buffer.size());
        if (bytes < 0 && errno == EINTR)
            continue;
        if (bytes < 0)
            throw std::runtime_error("P50 compiler input validation read failed");
        if (bytes == 0)
            break;
        const uint64_t count = static_cast<uint64_t>(bytes);
        if (total > expected_bytes || count > expected_bytes - total)
            throw std::invalid_argument("P50 compiler input grew during validation");
        digest.append(std::span<const uint8_t>(validation_buffer.data(),
                                               static_cast<size_t>(bytes)));
        total += count;
    }
    if (total != expected_bytes || digest.finish().bytes != expected_digest)
        throw std::invalid_argument("P50 compiler input digest does not match its claim");
    if (::lseek(fd_, 0, SEEK_SET) != 0)
        throw std::runtime_error("P50 compiler input cannot rewind to byte zero");
}

int P50AttachedFileSource::poll_fd() const
{
    return complete_ ? -1 : fd_;
}

bool P50AttachedFileSource::complete() const
{
    return complete_;
}

bool P50AttachedFileSource::has_pending() const
{
    return pending_size_ != 0;
}

std::size_t P50AttachedFileSource::pending_size() const
{
    return pending_size_;
}

std::size_t P50AttachedFileSource::pending_compressed_size() const
{
    // Cache-channel encoded bytes are accounted by the CacheWire ledger, not
    // fabricated from the raw compiler cursor.
    return 0;
}

std::size_t P50AttachedFileSource::pending_offset() const
{
    return offset_;
}

CompilerInputReadResult P50AttachedFileSource::read_next(unsigned int job_stat[])
{
    if (fd_ < 0 || complete_ || has_pending())
        return CompilerInputReadResult::NoMessage;
    for (;;) {
        const ssize_t bytes = ::read(fd_, buffer_.data(), buffer_.size());
        if (bytes < 0 && errno == EINTR)
            continue;
        if (bytes < 0) {
            complete_ = true;
            close_input();
            return CompilerInputReadResult::UnexpectedEof;
        }
        if (bytes == 0) {
            complete_ = true;
            close_input();
            return observed_bytes_ == expected_bytes_
                ? CompilerInputReadResult::End
                : CompilerInputReadResult::UnexpectedEof;
        }
        const uint64_t count = static_cast<uint64_t>(bytes);
        if (observed_bytes_ > expected_bytes_ || count > expected_bytes_ - observed_bytes_) {
            complete_ = true;
            close_input();
            return CompilerInputReadResult::UnexpectedMessage;
        }
        observed_bytes_ += count;
        pending_size_ = static_cast<size_t>(bytes);
        offset_ = 0;
        job_stat[JobStatistics::in_uncompressed] +=
            static_cast<unsigned int>(pending_size_);
        return CompilerInputReadResult::Chunk;
    }
}

CompilerInputWriteResult P50AttachedFileSource::write_pending(int compiler_stdin_fd)
{
    if (!has_pending())
        return CompilerInputWriteResult::ChunkComplete;
    const ssize_t bytes = write_bytes(compiler_stdin_fd, buffer_.data() + offset_,
                                      pending_size_ - offset_);
    if (bytes < 0) {
        if (errno == EINTR)
            return CompilerInputWriteResult::Interrupted;
        discard_pending();
        return CompilerInputWriteResult::Failed;
    }
    if (bytes == 0) {
        discard_pending();
        return CompilerInputWriteResult::Failed;
    }
    offset_ += static_cast<size_t>(bytes);
    if (offset_ == pending_size_) {
        discard_pending();
        return CompilerInputWriteResult::ChunkComplete;
    }
    return CompilerInputWriteResult::Pending;
}

void P50AttachedFileSource::discard_pending()
{
    pending_size_ = 0;
    offset_ = 0;
}

void P50AttachedFileSource::disable()
{
    close_input();
}

void P50AttachedFileSource::mark_complete()
{
    complete_ = true;
    close_input();
}

ssize_t P50AttachedFileSource::write_bytes(int fd, const void *buffer,
                                           std::size_t size)
{
    return ::write(fd, buffer, size);
}
