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
#include <stdint.h>
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

    enum AssignmentPolicy {
        ASSIGNMENT_LEGACY,
        ASSIGNMENT_ADVISORY,
        ASSIGNMENT_ENFORCING_COMPAT,
        ASSIGNMENT_STRICT_NONCE
    };

    enum AssignmentPhase {
        ASSIGNMENT_NONE,
        ASSIGNMENT_PREPARED,
        ASSIGNMENT_READY,
        ASSIGNMENT_REVOKE_PENDING,
        ASSIGNMENT_CLAIMED_OR_LATER,
        ASSIGNMENT_TERMINAL
    };

    Job(const unsigned int _id, CompileServer *subm);
    ~Job();

    unsigned int id() const;

    unsigned int localClientId() const;
    void setLocalClientId(const unsigned int id);

    State state() const;
    void setState(const State state);

    AssignmentPolicy assignmentPolicy() const { return m_assignmentPolicy; }
    void setAssignmentPolicy(AssignmentPolicy policy) { m_assignmentPolicy = policy; }
    AssignmentPhase assignmentPhase() const { return m_assignmentPhase; }
    void setAssignmentPhase(AssignmentPhase phase) { m_assignmentPhase = phase; }
    uint64_t assignmentEpoch() const { return m_assignmentEpoch; }
    uint64_t assignmentNonce() const { return m_assignmentNonce; }
    void setCompileIdentity(uint64_t c_guid, uint64_t tu_seq)
    { m_cGuid = c_guid; m_tuSeq = tu_seq; }
    uint64_t cGuid() const { return m_cGuid; }
    uint64_t tuSeq() const { return m_tuSeq; }
    void setAssignmentIdentity(uint64_t epoch, uint64_t nonce)
    {
        m_assignmentEpoch = epoch;
        m_assignmentNonce = nonce;
    }
    bool assignmentFenced() const
    {
        return m_assignmentPolicy != ASSIGNMENT_LEGACY;
    }
    bool assignmentReadyGated() const
    {
        return m_assignmentPolicy == ASSIGNMENT_ENFORCING_COMPAT
            || m_assignmentPolicy == ASSIGNMENT_STRICT_NONCE;
    }
    bool assignmentReplySent() const { return m_assignmentReplySent; }
    void setAssignmentReplySent(bool sent) { m_assignmentReplySent = sent; }

    /* Prepared modes freeze the complete legacy UseCS projection at the
       dispatch decision.  READY may arrive much later, after selection
       inputs have changed, but it exposes exactly this immutable choice. */
    void setDispatchProjection(const std::string &platform, bool got_env,
                               uint32_t matched_job_id)
    {
        m_dispatchPlatform = platform;
        m_dispatchGotEnv = got_env;
        m_dispatchMatchedJobId = matched_job_id;
    }
    const std::string &dispatchPlatform() const { return m_dispatchPlatform; }
    bool dispatchGotEnv() const { return m_dispatchGotEnv; }
    uint32_t dispatchMatchedJobId() const { return m_dispatchMatchedJobId; }

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
    // Queue membership bookkeeping owned by JobRequestsGroup: the stored
    // list position makes removal O(1), and the flag plus the
    // (submitter, niceness) pair -- unique per group -- prove membership
    // without scanning.
    bool queued() const { return m_queued; }
    void setQueued(bool value) { m_queued = value; }
    std::list<Job *>::iterator queueIt() const { return m_queueIt; }
    void setQueueIt(std::list<Job *>::iterator it) { m_queueIt = it; }

    CompileServer *server() const;
    void setServer(CompileServer *server);

    CompileServer *submitter() const;
    void setSubmitter(CompileServer *submitter);
    /* Sever the submitter WITHOUT breaking the lifetime contract: the
       live-job and outstanding-dispatch accounting this job holds on its
       submitter are discharged exactly once, HERE, and the destructor knows
       it no longer owns them.
       Used when a submitting daemon disconnects while this job is still
       COMPILING on a live worker -- the job outlives its submitter.  The
       node name is snapshotted for logs/diagnostics.  */
    void detachSubmitter();
    bool submitterDetached() const { return m_submitterDetached; }
    const std::string &submitterName() const { return m_submitterName; }
    uint64_t submitterGeneration() const { return m_submitterGeneration; }

    /* Centralized exact terminal authority (phase- and origin-sensitive).
       Pointer AND generation must both match: node-name equality never
       grants authority, and a detached job accepts no submitter-origin
       terminal at all.  */
    enum TerminalDecision {
        ACCEPT_WORKER,
        ACCEPT_SUBMITTER,
        REJECT_WRONG_WORKER,
        REJECT_WRONG_SUBMITTER,
        REJECT_DETACHED_SUBMITTER
    };
    TerminalDecision authorizeTerminal(const CompileServer *sender,
                                       uint64_t sender_generation,
                                       bool from_server) const;

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

    void setCacheRequest(uint32_t protocol, uint32_t profile_mask,
                         uint32_t affinity_profile_mask,
                         uint32_t affinity_port,
                         const std::string &affinity_host,
                         uint32_t retry_avoid_port,
                         const std::string &retry_avoid_host)
    {
        m_cacheProtocol = protocol;
        m_cacheProfileMask = profile_mask;
        m_cacheAffinityProfileMask = affinity_profile_mask;
        m_cacheAffinityPort = affinity_port;
        m_cacheAffinityHost = affinity_host;
        m_cacheRetryAvoidPort = retry_avoid_port;
        m_cacheRetryAvoidHost = retry_avoid_host;
    }
    uint32_t cacheProtocol() const { return m_cacheProtocol; }
    uint32_t cacheProfileMask() const { return m_cacheProfileMask; }
    uint32_t cacheAffinityProfileMask() const
    { return m_cacheAffinityProfileMask; }
    uint32_t cacheAffinityPort() const { return m_cacheAffinityPort; }
    const std::string &cacheAffinityHost() const
    { return m_cacheAffinityHost; }
    uint32_t cacheRetryAvoidPort() const { return m_cacheRetryAvoidPort; }
    const std::string &cacheRetryAvoidHost() const
    { return m_cacheRetryAvoidHost; }

private:
    const unsigned int m_id;
    unsigned int m_localClientId;
    State m_state;
    AssignmentPolicy m_assignmentPolicy = ASSIGNMENT_LEGACY;
    AssignmentPhase m_assignmentPhase = ASSIGNMENT_NONE;
    uint64_t m_assignmentEpoch = 0;
    uint64_t m_assignmentNonce = 0;
    uint64_t m_cGuid = 0;
    uint64_t m_tuSeq = 0;
    std::string m_dispatchPlatform;
    bool m_dispatchGotEnv = false;
    uint32_t m_dispatchMatchedJobId = 0;
    bool m_assignmentReplySent = false;
    CompileServer *m_server;  // on which server we build
    CompileServer *m_submitter;
    bool m_submitterDetached = false;
    std::string m_submitterName;
    uint64_t m_submitterGeneration = 0;  // who submitted us
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
    bool m_queued = false;
    std::list<Job *>::iterator m_queueIt;
    uint64_t m_estimateSnapshotMsec = 0;
    std::string m_selectedEnvironment;
    std::list<Job *> m_masterJobFor;
    unsigned int m_argFlags;
    std::string m_language; // for debugging
    std::string m_preferredHost; // for debugging daemons
    int m_minimalHostVersion; // minimal version required for the the remote server
    unsigned int m_requiredFeatures; // flags the job requires on the remote server
    int m_niceness; // nice priority (0-20)
    uint32_t m_cacheProtocol = 0;
    uint32_t m_cacheProfileMask = 0;
    uint32_t m_cacheAffinityProfileMask = 0;
    uint32_t m_cacheAffinityPort = 0;
    std::string m_cacheAffinityHost;
    uint32_t m_cacheRetryAvoidPort = 0;
    std::string m_cacheRetryAvoidHost;
};

#endif
