/* -*- mode: C++; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 99; -*- */
#include "relogin.h"

#include "compileserver.h"

ReloginResult apply_relogin(CompileServer *server, const LoginMsg &login)
{
    if (!server) {
        return ReloginResult::CloseConnection;
    }

    server->setCompilerVersions(login.envs);
    server->setBusyInstalling(0);

    /* Protocol 24 introduced ConfCS.  A modern relogin is complete only when
       that required reply was accepted by the channel; otherwise the caller
       must converge through the ordinary connection teardown path. */
    if (IS_PROTOCOL_VERSION(24, server) && !server->send_msg(ConfCSMsg())) {
        return ReloginResult::CloseConnection;
    }

#ifdef ICECC_TEST_RELOGIN_MUTANT_FALSE_SUCCESS
    /* Exact historical defect: every successful relogin returned false,
       which the drain loop interprets as "the connection was deleted". */
    return ReloginResult::CloseConnection;
#else
    return ReloginResult::KeepConnection;
#endif
}
