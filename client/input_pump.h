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

#ifndef ICECREAM_INPUT_PUMP_H
#define ICECREAM_INPUT_PUMP_H

class MsgChannel;

// Attachment point for the source stream of a remote compile.  R6 keeps the
// legacy MsgChannel implementation as the only selectable implementation.
class RemoteInputSink
{
public:
    virtual ~RemoteInputSink() = default;

    // Takes ownership of fd and closes it on success and every error path.
    virtual void send_fd(int fd) = 0;
    virtual bool send_end() = 0;
};

class LegacyRemoteSink final : public RemoteInputSink
{
public:
    explicit LegacyRemoteSink(MsgChannel *channel);

    void send_fd(int fd) override;
    bool send_end() override;

private:
    MsgChannel *channel_;
};

#endif
