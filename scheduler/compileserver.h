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

    State state() const;
    void setState(const State state);

    Type type() const;
    void setType(const Type type);

    bool chrootPossible() const;
    void setChrootPossible(const bool possible);

    bool featuresSupported(unsigned int features) const;
    unsigned int supportedFeatures() const;
    void setSupportedFeatures(unsigned int features);

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

    int getClientLocalJobId(const int localJobId);
    void insertClientLocalJobId(const int localJobId, const int newJobId, bool fulljob);
    void eraseClientLocalJobId(const int localJobId);

    map<const CompileServer *, Environments> blacklist() const;
    Environments getEnvsForBlacklistedCS(const CompileServer *cs);
    void blacklistCompileServer(CompileServer *cs, const std::pair<std::string, std::string> &env);
    void eraseCSFromBlacklist(CompileServer *cs);

    int getInFd() const;
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
    int m_clientCount; // number of client connections the daemon has
    int m_submittedJobsCount;
    uint64_t m_admittedJobsTotal = 0;
    unsigned int m_connectionGeneration = 0;   // set once in the ctor
    bool m_stallReported = false;
    unsigned int m_lastPickId;

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
    time_t m_nextConnTime;
    time_t m_lastConnStartTime;
    bool m_acceptingInConnection;
};

#endif
