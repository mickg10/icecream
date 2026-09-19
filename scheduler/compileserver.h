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

#ifndef COMPILESERVER_H
#define COMPILESERVER_H

#include <string>
#include <stdint.h>
#include <list>
#include <set>
#include <map>

#include "../services/comm.h"
#include "jobstat.h"

class Job;

using namespace std;

/* One compile server (receiver, compile daemon)  */
class CompileServer : public MsgChannel
{
public:
    // Assignments dispatched to this submitter that have not yet been
    // confirmed by observable client progress (JobBeginMsg).  Bounds how
    // many farm slots one unresponsive submitter can hold.  Each entry is
    // the monotonic debit time of one unconfirmed dispatch: the liveness
    // bound is a MAXIMUM ASSIGNMENT AGE, enforced against the oldest entry,
    // so one lost assignment cannot hold its slot indefinitely just because
    // later assignments keep confirming.
    unsigned int outstandingDispatches() const
    {
        return (unsigned int)m_outstandingDebits.size();
    }
    void addOutstandingDispatch(uint64_t debit_msec)
    {
        m_outstandingDebits.insert(debit_msec);
    }
    // Returns false when the exact debit is unknown -- an accounting
    // invariant failure the CALLER must log loudly.  Silently charging the
    // oldest entry instead would hide the defect and repeatedly postpone
    // the true oldest job's age bound.
    bool removeOutstandingDispatch(uint64_t debit_msec)
    {
        auto it = m_outstandingDebits.find(debit_msec);
        if (it == m_outstandingDebits.end()) {
            return false;
        }
        m_outstandingDebits.erase(it);
        return true;
    }
    // Age of the oldest unconfirmed dispatch, or 0 if none is outstanding.
    uint64_t oldestOutstandingDispatchMsec(uint64_t now_msec) const
    {
        if (m_outstandingDebits.empty()) {
            return 0;
        }
        const uint64_t oldest = *m_outstandingDebits.begin();
        return now_msec > oldest ? now_msec - oldest : 0;
    }

    enum State {
        CONNECTED,
        LOGGEDIN
    };

    enum Type {
        UNKNOWN,
        DAEMON,
        MONITOR,
        LINE
    };

    CompileServer(const int fd, struct sockaddr *_addr, const socklen_t _len, const bool text_based);
    ~CompileServer() override;

    void pick_new_id();

    bool check_remote(const Job *job) const;
    bool platforms_compatible(const string &target) const;
    string can_install(const Job *job, bool ignore_installing = false) const;
    bool is_eligible_ever(const Job *job) const;
    bool is_eligible_now(const Job *job) const;

    unsigned int remotePort() const;
    void setRemotePort(const unsigned int port);

    unsigned int hostId() const;
    void setHostId(const unsigned int id);

    string nodeName() const;
    void setNodeName(const string &name);

    bool matches(const string& nm) const;

    time_t busyInstalling() const;
    void setBusyInstalling(const time_t time);

    string hostPlatform() const;
    void setHostPlatform(const string &platform);

    unsigned int load() const;
    void setLoad(const unsigned int load);

    int maxJobs() const;
    void setMaxJobs(const int jobs);
    int maxPreloadCount() const;
    int currentJobCount() const;
    int currentJobCountRemote() const;
    int currentJobCountLocal() const;

    bool noRemote() const;
    void setNoRemote(const bool value);

    const list<Job *>& jobList() const;
    void appendJob(Job *job);
    void removeJob(Job *job);
    unsigned int lastPickedId();
    /* Monotonic per-process pick clock: incremented once per recorded
       assignment; wire-id wrap cannot disturb it.  0 = never picked.  */
    uint64_t lastPickSeq() const { return m_lastPickSeq; }
    static uint64_t pickSequence() { return s_pickSequence; }

    State state() const;
    void setState(const State state);

    Type type() const;
    void setType(const Type type);

    bool chrootPossible() const;
    void setChrootPossible(const bool possible);

    bool featuresSupported(unsigned int features) const;
    unsigned int supportedFeatures() const;
    void setSupportedFeatures(unsigned int features);

    uint32_t cacheEndpointPort() const { return m_cacheEndpointPort; }
    uint32_t cacheProtocol() const { return m_cacheProtocol; }
    uint32_t cacheProfileMask() const { return m_cacheProfileMask; }
    /* A full cache filesystem is normally a hard admission stop.  The
       authenticated P50 route is different: one exact cache-compatible
       assignment must be allowed to reach the worker so the worker can
       return the bounded transport/fallback outcome and the client can
       exclude that endpoint on retry. */
    bool cacheCompatible(const Job *job) const;
    bool cacheLoadProbeUsed() const { return m_cacheLoadProbeUsed; }
    void markCacheLoadProbe() { m_cacheLoadProbeUsed = true; }
    void setCacheAdvertisement(uint32_t endpoint_port, uint32_t protocol,
                               uint32_t profiles)
    {
        m_cacheEndpointPort = endpoint_port;
        m_cacheProtocol = protocol;
        m_cacheProfileMask = profiles;
    }

    int clientCount() const;
    void setClientCount( int clientCount );
    int submittedJobsCount() const;
    void submittedJobsIncrement();
    void submittedJobsDecrement();
    /* Count of requests this submitter has had ADMITTED (jobs created), as
       opposed to the live count above.  Monotonic for the lifetime of ONE
       connection object -- the object dies with its connection, so across a
       reconnect the count restarts.  connectionGeneration() disambiguates:
       a baseline/delta pair taken under the same generation is a valid
       window; a generation change means the counter was reset in between
       and the observer must resample or sum per generation.  */
    uint64_t admittedJobsTotal() const { return m_admittedJobsTotal; }
    void admittedJobsIncrement() { ++m_admittedJobsTotal; }
    unsigned int connectionGeneration() const { return m_connectionGeneration; }
    /* One-shot latch so an unconfirmed assignment is reported once per
       stall episode instead of every poll.  Cleared when any of this
       submitter's work confirms progress.  */
    bool stallReported() const { return m_stallReported; }
    void setStallReported(bool value) { m_stallReported = value; }

    Environments compilerVersions() const;
    void setCompilerVersions(const Environments &environments);

    list<JobStat> lastCompiledJobs() const;
    void appendCompiledJob(const JobStat &stats);
    void popCompiledJob();

    list<JobStat> lastRequestedJobs() const;
    void appendRequestedJobs(const JobStat &stats);
    void popRequestedJobs();

    JobStat cumCompiled() const;
    void setCumCompiled(const JobStat &stats);

    JobStat cumRequested() const;
    void setCumRequested(const JobStat &stats);


    unsigned int hostidCounter() const;

    /* Checked lookup: 0 = unknown local id (never a default-inserted
       record -- the old operator[] lookup created one and emitted a
       monitor completion for global id 0).  */
    int getClientLocalJobId(const int localJobId);
    /* Returns the global id a duplicate Begin displaced (0 if none): the
       caller must emit its terminal and release it exactly once.  */
    int insertClientLocalJobId(const int localJobId, const int newJobId, bool fulljob);
    void eraseClientLocalJobId(const int localJobId);
    /* Every live local-record global id, for disconnect reconciliation.  */
    std::list<int> liveClientLocalJobIds() const;

    map<const CompileServer *, Environments> blacklist() const;
    Environments getEnvsForBlacklistedCS(const CompileServer *cs);
    void blacklistCompileServer(CompileServer *cs, const std::pair<std::string, std::string> &env);
    void eraseCSFromBlacklist(CompileServer *cs);

    int getInFd() const;
    /* Split completion predicates: the SO_ERROR verdict is read exactly
       once per wake and does NOT run deadline or resolver work; the
       deadline is the probe's own monotonic fact.  A probe transition
       depends only on these two -- never on unrelated ready fds.  */
    bool probeCompletionOk();
    bool probeDeadlineExpired() const;
    /* Consume the probe fd exactly once; safe to call repeatedly.  */
    void closeProbeFd();

    /* Pre-login lease: one absolute monotonic deadline covering protocol
       negotiation AND the first valid LOGIN/MON_LOGIN; never refreshed by
       partial bytes.  The accounted flag makes the population counter's
       increment/decrement exact-once across every teardown path.  */
    void setPreloginDeadline(uint64_t deadline_mono) { m_preloginDeadlineMono = deadline_mono; }
    uint64_t preloginDeadline() const { return m_preloginDeadlineMono; }
    void setPreloginAccounted(bool a) { m_preloginAccounted = a; }
    bool preloginAccounted() const { return m_preloginAccounted; }

    void startInConnectionTest();
    time_t getConnectionTimeout();
    time_t getNextTimeout();
    bool getConnectionInProgress();
    bool isConnected();
    void updateInConnectivity(bool acceptingIn);

    std::multiset<uint64_t> m_outstandingDebits;

private:
    bool blacklisted(const Job *job, const pair<string, string> &environment) const;

    /* The listener port, on which it takes compile requests.  */
    unsigned int m_remotePort;
    unsigned int m_hostId;
    string m_nodeName;
    time_t m_busyInstalling;
    string m_hostPlatform;

    // LOAD is load * 1000
    unsigned int m_load;
    int m_maxJobs;
    bool m_noRemote;
    list<Job *> m_jobList;
    State m_state;
    Type m_type;
    bool m_chrootPossible;
    unsigned int m_featuresSupported;
    /* Inert Login metadata.  Selection and assignment code must not consume
       these fields until a separately reviewed cache-input slice lands. */
    uint32_t m_cacheEndpointPort;
    uint32_t m_cacheProtocol;
    uint32_t m_cacheProfileMask;
    bool m_cacheLoadProbeUsed = false;
    int m_clientCount; // number of client connections the daemon has
    int m_submittedJobsCount;
    uint64_t m_admittedJobsTotal = 0;
    unsigned int m_connectionGeneration = 0;   // set once in the ctor
    bool m_stallReported = false;
    unsigned int m_lastPickId;
    uint64_t m_lastPickSeq = 0;
    uint64_t m_preloginDeadlineMono = 0;
    bool m_preloginAccounted = false;
    static uint64_t s_pickSequence;

    Environments m_compilerVersions;  // Available compilers

    list<JobStat> m_lastCompiledJobs;
    list<JobStat> m_lastRequestedJobs;
    JobStat m_cumCompiled;  // cumulated
    JobStat m_cumRequested;

    static unsigned int s_hostIdCounter;

    // map client ID for daemon to our IDs
    struct LocalJobInfo
    {
        int id;
        bool fulljob;
    };
    map<int, LocalJobInfo> m_clientLocalMap;

    map<const CompileServer *, Environments> m_blacklist;

    int m_inFd;
    unsigned int m_inConnAttempt;
    /* Monotonic milliseconds (icecream_monotonic_msec): retry and attempt
       deadlines must not move when the wall clock steps.  */
    uint64_t m_nextConnMono;
    uint64_t m_connStartMono;
    bool m_acceptingInConnection;
};

#endif
