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

#ifndef ICECREAM_COMPILER_INPUT_H
#define ICECREAM_COMPILER_INPUT_H

#include <cstddef>
#include <memory>
#include <sys/types.h>

class FileChunkMsg;
class MsgChannel;

enum class CompilerInputReadResult
{
    NoMessage,
    Chunk,
    End,
    MessageAfterEnd,
    UnexpectedMessage,
    UnexpectedEof
};

enum class CompilerInputWriteResult
{
    Pending,
    ChunkComplete,
    Interrupted,
    Failed
};

// Input-only attachment point inside work_it.  It intentionally has no
// compiler-process or result lifecycle responsibilities.
class CompilerInputSource
{
public:
    virtual ~CompilerInputSource() = default;

    virtual int poll_fd() const = 0;
    virtual bool complete() const = 0;
    virtual bool has_pending() const = 0;
    virtual std::size_t pending_size() const = 0;
    virtual std::size_t pending_compressed_size() const = 0;
    virtual std::size_t pending_offset() const = 0;

    virtual CompilerInputReadResult read_next(unsigned int job_stat[]) = 0;
    virtual CompilerInputWriteResult write_pending(int compiler_stdin_fd) = 0;
    virtual void discard_pending() = 0;
    virtual void disable() = 0;
    virtual void mark_complete() = 0;
};

class LegacyChunkSource : public CompilerInputSource
{
public:
    LegacyChunkSource(MsgChannel *channel, int client_fd);
    ~LegacyChunkSource() override;

    int poll_fd() const override;
    bool complete() const override;
    bool has_pending() const override;
    std::size_t pending_size() const override;
    std::size_t pending_compressed_size() const override;
    std::size_t pending_offset() const override;

    CompilerInputReadResult read_next(unsigned int job_stat[]) override;
    CompilerInputWriteResult write_pending(int compiler_stdin_fd) override;
    void discard_pending() override;
    void disable() override;
    void mark_complete() override;

protected:
    virtual ssize_t write_bytes(int fd, const void *buffer, std::size_t size);

private:
    MsgChannel *channel_;
    int client_fd_;
    bool complete_;
    std::unique_ptr<FileChunkMsg> pending_;
    std::size_t offset_;
};

#endif
