/* -*- mode: C++; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 99; -*- */
/* vim: set ts=4 sw=4 et tw=99:  */
/*
    This file is part of Icecream.

    Copyright (c) 2004 Michael Matz <matz@suse.de>
                  2004 Stephan Kulow <coolo@suse.de>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#ifndef JOB_H
#define JOB_H

#include <list>
#include <string>
#include <time.h>

#include "../services/comm.h"

class CompileServer;

class Job
{
public:
    enum State {
        PENDING,
        WAITINGFORCS,
        COMPILING
    };

    Job(const unsigned int _id, CompileServer *subm);
    ~Job();

    unsigned int id() const;

    unsigned int localClientId() const;
    void setLocalClientId(const unsigned int id);

    State state() const;
    void setState(const State state);

    // True while this job holds one of its submitter's dispatch credits
    // (dispatched, not yet confirmed by JobBegin or released on teardown).
    bool dispatchOutstanding() const { return m_dispatchOutstanding; }
    void setDispatchOutstanding(bool value) { m_dispatchOutstanding = value; }
    // Monotonic time this job's dispatch credit was debited; the submitter's
    // stall bound is enforced against its OLDEST unconfirmed dispatch, so a
    // single lost assignment cannot hide behind later ones that confirm.
    uint64_t dispatchDebitMsec() const { return m_dispatchDebitMsec; }
    void setDispatchDebitMsec(uint64_t msec) { m_dispatchDebitMsec = msec; }
    // Monotonic enqueue time: ordering, aging and promotion deadlines are
    // computed from this so a wall-clock step can neither postpone nor
    // fast-forward them.  enqueueTime() (wall) remains display-only.
    uint64_t enqueueMonoMsec() const { return m_enqueueMonoMsec; }
    // Scheduling estimate frozen at enqueue: a queued job's rank cannot
    // drift as EWMA updates, the global average, or the 24h expiry change
    // underneath it, which makes each dispatch decision reproducible and
    // is what lets an index treat the score key as static.
    uint64_t estimateSnapshotMsec() const { return m_estimateSnapshotMsec; }
    void setEstimateSnapshotMsec(uint64_t msec) { m_estimateSnapshotMsec = msec; }
    // Toolchain identity actually chosen at dispatch (the offer matching
    // the host platform); runtime estimates are recorded under this, not
    // under whatever the client happened to list first.
    const std::string &selectedEnvironment() const { return m_selectedEnvironment; }
    void setSelectedEnvironment(const std::string &env) { m_selectedEnvironment = env; }

    CompileServer *server() const;
    void setServer(CompileServer *server);

    CompileServer *submitter() const;
    void setSubmitter(CompileServer *submitter);

    Environments environments() const;
    void setEnvironments(const Environments &environments);
    void appendEnvironment(const std::pair<std::string, std::string> &env);
    void clearEnvironments();

    time_t startTime() const;
    void setStartTime(const time_t time);

    time_t startOnScheduler() const;
    void setStartOnScheduler(const time_t time);

    time_t doneTime() const;
    void setDoneTime(const time_t time);

    time_t enqueueTime() const;
    time_t stateChangeTime() const;

    std::string targetPlatform() const;
    void setTargetPlatform(const std::string &platform);

    const std::string &fileName() const;
    void setFileName(const std::string &fileName);

    std::list<Job *> masterJobFor() const;
    void appendJob(Job *job);

    unsigned int argFlags() const;
    void setArgFlags(const unsigned int argFlags);

    std::string language() const;
    void setLanguage(const std::string &language);

    std::string preferredHost() const;
    void setPreferredHost(const std::string &host);

    int minimalHostVersion() const;
    void setMinimalHostVersion( int version );

    unsigned int requiredFeatures() const;
    void setRequiredFeatures(unsigned int features);

    int niceness() const;
    void setNiceness( int niceness );

private:
    const unsigned int m_id;
    unsigned int m_localClientId;
    State m_state;
    CompileServer *m_server;  // on which server we build
    CompileServer *m_submitter;  // who submitted us
    Environments m_environments;
    time_t m_startTime;  // _local_ to the compiler server
    time_t m_startOnScheduler;  // starttime local to scheduler
    /**
     * the end signal from client and daemon is a bit of a race and
     * in 99.9% of all cases it's catched correctly. But for the remaining
     * 0.1% we need a solution too - otherwise these jobs are eating up slots.
     * So the solution is to track done jobs (client exited, daemon didn't signal)
     * and after 10s no signal, kill the daemon (and let it rehup) **/
    time_t m_doneTime;
    time_t m_enqueueTime;  // when the job was enqueued (scheduler-local)
    time_t m_stateChangeTime;  // last state transition time (scheduler-local)

    std::string m_targetPlatform;
    std::string m_fileName;
    bool m_dispatchOutstanding = false;
    uint64_t m_dispatchDebitMsec = 0;
    uint64_t m_enqueueMonoMsec = 0;
    uint64_t m_estimateSnapshotMsec = 0;
    std::string m_selectedEnvironment;
    std::list<Job *> m_masterJobFor;
    unsigned int m_argFlags;
    std::string m_language; // for debugging
    std::string m_preferredHost; // for debugging daemons
    int m_minimalHostVersion; // minimal version required for the the remote server
    unsigned int m_requiredFeatures; // flags the job requires on the remote server
    int m_niceness; // nice priority (0-20)
};

#endif
