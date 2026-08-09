/* -*- mode: C++; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 99; -*- */
#ifndef RELOGIN_H
#define RELOGIN_H

class CompileServer;
class LoginMsg;

enum class ReloginResult
{
    KeepConnection,
    CloseConnection
};

/* Apply one valid daemon relogin and state explicitly whether the scheduler's
   drain loop still owns a live connection afterwards.  The state update and
   required configuration reply form one transition: a failed reply is not a
   successful relogin. */
ReloginResult apply_relogin(CompileServer *server, const LoginMsg &login);

#endif
