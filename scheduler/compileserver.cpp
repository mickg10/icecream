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

#include "compileserver.h"

#include <algorithm>
#include <fcntl.h>
#include <netdb.h>
#include <time.h>
#include <unistd.h>

#include "../services/logging.h"
#include "../services/job.h"

#include "job.h"
#include "scheduler.h"


unsigned int CompileServer::s_hostIdCounter = 0;

CompileServer::CompileServer(const int fd, struct sockaddr *_addr, const socklen_t _len, const bool text_based)
    : MsgChannel(fd, _addr, _len, text_based)
    , m_remotePort(0)
    , m_hostId(0)
    , m_nodeName()
    , m_busyInstalling(0)
    , m_hostPlatform()
    , m_load(1000)
    , m_maxJobs(0)
    , m_noRemote(false)
    , m_jobList()
    , m_state(CONNECTED)
    , m_type(UNKNOWN)
    , m_chrootPossible(false)
    , m_featuresSupported(0)
    , m_clientCount(0)
    , m_submittedJobsCount(0)
    , m_connectionGeneration([] {
          /* Process-wide monotonic connection stamp.  admittedJobsTotal()
             restarts with each connection object; the generation lets an
             observer detect the restart instead of silently undercounting
             across a reconnect.  */
          static unsigned int next_generation = 0;
          return ++next_generation;
      }())
    , m_lastPickId(0)
    , m_compilerVersions()
    , m_lastCompiledJobs()
    , m_lastRequestedJobs()
    , m_cumCompiled()
    , m_cumRequested()
    , m_clientLocalMap()
    , m_blacklist()
    , m_inProbe()
{
}

void CompileServer::pick_new_id()
{
    assert(!m_hostId);
    m_hostId = ++s_hostIdCounter;
}

bool CompileServer::check_remote(const Job *job) const
{
    bool local = (job->submitter() == this);
    return local || !m_noRemote;
}

bool CompileServer::platforms_compatible(const string &target) const
{
    if (target == hostPlatform()) {
        return true;
    }

    // the below doesn't work as the unmapped platform is transferred back to the
    // client and that asks the daemon for a platform he can't install (see TODO)

    static multimap<string, string> platform_map;

    if (platform_map.empty()) {
        platform_map.insert(make_pair(string("i386"), string("i486")));
        platform_map.insert(make_pair(string("i386"), string("i586")));
        platform_map.insert(make_pair(string("i386"), string("i686")));
        platform_map.insert(make_pair(string("i386"), string("x86_64")));

        platform_map.insert(make_pair(string("i486"), string("i586")));
        platform_map.insert(make_pair(string("i486"), string("i686")));
        platform_map.insert(make_pair(string("i486"), string("x86_64")));

        platform_map.insert(make_pair(string("i586"), string("i686")));
        platform_map.insert(make_pair(string("i586"), string("x86_64")));

        platform_map.insert(make_pair(string("i686"), string("x86_64")));

        platform_map.insert(make_pair(string("ppc"), string("ppc64")));
        platform_map.insert(make_pair(string("s390"), string("s390x")));
    }

    multimap<string, string>::const_iterator end = platform_map.upper_bound(target);

    for (multimap<string, string>::const_iterator it = platform_map.lower_bound(target);
            it != end;
            ++it) {
        if (it->second == hostPlatform()) {
            return true;
        }
    }

    return false;
}

/* Given a candidate CS and a JOB, check if any of the requested
   environments could be installed on the CS.  This is the case if that
   env can be run there, i.e. if the host platforms of the CS and of the
   environment are compatible.  Return an empty string if none can be
   installed, otherwise return the platform of the first found
   environments which can be installed.  */
string CompileServer::can_install(const Job *job, bool ignore_installing) const
{
    // trace() << "can_install host: '" << cs->host_platform << "' target: '"
    //         << job->target_platform << "'" << endl;
    if (!ignore_installing && busyInstalling()) {
#if DEBUG_SCHEDULER > 0
        trace() << nodeName() << " is busy installing since " << time(0) - busyInstalling()
                << " seconds." << endl;
#endif
        return string();
    }

    Environments environments = job->environments();
    for (Environments::const_iterator it = environments.begin();
            it != environments.end(); ++it) {
        if (platforms_compatible(it->first) && !blacklisted(job, *it)) {
            return it->first;
        }
    }

    return string();
}

int CompileServer::maxPreloadCount() const
{
    // Always allow one job to be preloaded (sent to the compile server
    // even though there is no compile slot free for it). Since servers
    // with multiple cores are capable of handling many jobs at once,
    // allow one extra preload job for each 4 cores, to minimize stalls
    // when the compile server is waiting for more jobs to be received.
    return 1 + (m_maxJobs / 4);
}

bool CompileServer::is_eligible_ever(const Job *job) const
{
    bool jobs_okay = m_maxJobs > 0;
    // We cannot use just 'protocol', because if the scheduler's protocol
    // is lower than the daemon's then this is set to the minimum.
    // But here we are asked about the daemon's protocol version, so check that.
    bool version_okay = job->minimalHostVersion() <= maximum_remote_protocol;
    bool features_okay = featuresSupported(job->requiredFeatures());
    bool eligible = jobs_okay
                    && (m_chrootPossible || job->submitter() == this)
                    && version_okay
                    && features_okay
                    && m_inProbe.accepting()
                    && can_install(job, true).size()
                    && check_remote(job);
#if DEBUG_SCHEDULER > 2
    trace() << nodeName() << " is_eligible_ever: " << eligible << " (jobs_okay " << jobs_okay
        << ", version_okay " << version_okay << ", features_okay " << features_okay
        << ", chroot_or_local " << (m_chrootPossible || job->submitter() == this)
        << ", accepting " << m_inProbe.accepting() << ", can_install " << (can_install(job).size() != 0)
        << ", check_remote " << check_remote(job) << ")" << endl;
#endif
    return eligible;
}

bool CompileServer::is_eligible_now(const Job *job) const
{
    if(!is_eligible_ever(job))
        return false;
    int local_jobs_now = currentJobCountLocal();
    int jobs_now = local_jobs_now + currentJobCountRemote();
    bool jobs_okay = jobs_now < m_maxJobs;
    // allow a job for preloading, but only if the node isn't fully
    // busy with local jobs (that may possibly take long)
    if( m_maxJobs > 0 && jobs_now < m_maxJobs + maxPreloadCount() && local_jobs_now < m_maxJobs)
        jobs_okay = true;
    bool load_okay = m_load < 1000;
    bool eligible = jobs_okay
                    && load_okay
                    && can_install(job, false).size();
#if DEBUG_SCHEDULER > 2
    trace() << nodeName() << " is_eligible_now: " << eligible << " (remote jobs " << m_jobList.size()
        << ", local jobs " << (currentJobCount() - m_jobList.size()) << ", jobs_okay " << jobs_okay
        << ", load_okay " << load_okay << ")" << endl;
#endif
    return eligible;
}

unsigned int CompileServer::remotePort() const
{
    return m_remotePort;
}

void CompileServer::setRemotePort(unsigned int port)
{
    m_remotePort = port;
}

unsigned int CompileServer::hostId() const
{
    return m_hostId;
}

void CompileServer::setHostId(unsigned int id)
{
    m_hostId = id;
}

string CompileServer::nodeName() const
{
    return m_nodeName;
}

void CompileServer::setNodeName(const string &name)
{
    m_nodeName = name;
}

bool CompileServer::matches(const string& nm) const
{
    return m_nodeName == nm || name == nm;
}

time_t CompileServer::busyInstalling() const
{
    return m_busyInstalling;
}

void CompileServer::setBusyInstalling(time_t time)
{
    m_busyInstalling = time;
}

string CompileServer::hostPlatform() const
{
    return m_hostPlatform;
}

void CompileServer::setHostPlatform(const string &platform)
{
    m_hostPlatform = platform;
}

unsigned int CompileServer::load() const
{
    return m_load;
}

void CompileServer::setLoad(unsigned int load)
{
    m_load = load;
}

void CompileServer::applyStats(const StatsMsg &stats)
{
    m_load = stats.load;
#ifdef ICECC_TEST_STATS_MUTANT_CLIENT_COUNT
    /* Exact historical defect: StatsMsg::fill_from_channel() never reads this
       field, so every ordinary heartbeat overwrote the last authoritative
       lifecycle count with the constructor default zero. */
    m_clientCount = stats.client_count;
#endif
}

int CompileServer::maxJobs() const
{
    return m_maxJobs;
}

void CompileServer::setMaxJobs(int jobs)
{
    m_maxJobs = jobs;
}

int CompileServer::currentJobCountRemote() const
{
    return m_jobList.size();
}

int CompileServer::currentJobCountLocal() const
{
    int count = 0;
    for( const std::pair<const int, CompileServer::LocalJobInfo>& info : m_clientLocalMap )
        count += info.second.fulljob ? m_maxJobs : 1;
    return count;
}

int CompileServer::currentJobCount() const
{
    return currentJobCountRemote() + currentJobCountLocal();
}

bool CompileServer::noRemote() const
{
    return m_noRemote;
}

void CompileServer::setNoRemote(bool value)
{
    m_noRemote = value;
    if (value) {
        m_inProbe.cancel();
    }
}

const list<Job *>& CompileServer::jobList() const
{
    return m_jobList;
}

void CompileServer::appendJob(Job *job)
{
    m_lastPickId = job->id();
    m_jobList.push_back(job);
}

void CompileServer::removeJob(Job *job)
{
    m_jobList.remove(job);
}

unsigned int CompileServer::lastPickedId()
{
    return m_lastPickId;
}

CompileServer::State CompileServer::state() const
{
    return m_state;
}

void CompileServer::setState(const CompileServer::State state)
{
    m_state = state;
}

CompileServer::Type CompileServer::type() const
{
    return m_type;
}

void CompileServer::setType(const CompileServer::Type type)
{
    m_type = type;
}

bool CompileServer::chrootPossible() const
{
    return m_chrootPossible;
}

void CompileServer::setChrootPossible(const bool possible)
{
    m_chrootPossible = possible;
}

bool CompileServer::featuresSupported(unsigned int features) const
{
    return ( m_featuresSupported & features ) == features;
}

unsigned int CompileServer::supportedFeatures() const
{
    return m_featuresSupported;
}

void CompileServer::setSupportedFeatures(unsigned int features)
{
    m_featuresSupported = features;
}

int CompileServer::clientCount() const
{
    return m_clientCount;
}

void CompileServer::setClientCount( int clientCount )
{
    m_clientCount = clientCount;
}

int CompileServer::submittedJobsCount() const
{
    return m_submittedJobsCount;
}

void CompileServer::submittedJobsIncrement()
{
    m_submittedJobsCount++;
}

void CompileServer::submittedJobsDecrement()
{
    m_submittedJobsCount--;
}

Environments CompileServer::compilerVersions() const
{
    return m_compilerVersions;
}

void CompileServer::setCompilerVersions(const Environments &environments)
{
    m_compilerVersions = environments;
}

list<JobStat> CompileServer::lastCompiledJobs() const
{
    return m_lastCompiledJobs;
}

void CompileServer::appendCompiledJob(const JobStat &stats)
{
    m_lastCompiledJobs.push_back(stats);
}

void CompileServer::popCompiledJob()
{
    m_lastCompiledJobs.pop_front();
}

list<JobStat> CompileServer::lastRequestedJobs() const
{
    return m_lastRequestedJobs;
}

void CompileServer::appendRequestedJobs(const JobStat &stats)
{
    m_lastRequestedJobs.push_back(stats);
}

void CompileServer::popRequestedJobs()
{
    m_lastRequestedJobs.pop_front();
}

JobStat CompileServer::cumCompiled() const
{
    return m_cumCompiled;
}

void CompileServer::setCumCompiled(const JobStat &stats)
{
    m_cumCompiled = stats;
}

JobStat CompileServer::cumRequested() const
{
    return m_cumRequested;
}

void CompileServer::setCumRequested(const JobStat &stats)
{
    m_cumRequested = stats;
}

int CompileServer::getClientLocalJobId(const int localJobId)
{
    return m_clientLocalMap[localJobId].id;
}

void CompileServer::insertClientLocalJobId(const int localJobId, const int newJobId, bool fulljob)
{
    m_clientLocalMap[localJobId] = LocalJobInfo{newJobId, fulljob};
}

void CompileServer::eraseClientLocalJobId(const int localJobId)
{
    m_clientLocalMap.erase(localJobId);
}

map<const CompileServer *, Environments> CompileServer::blacklist() const
{
    return m_blacklist;
}

Environments CompileServer::getEnvsForBlacklistedCS(const CompileServer *cs)
{
    return m_blacklist[cs];
}

void CompileServer::blacklistCompileServer(CompileServer *cs, const std::pair<std::string, std::string> &env)
{
    m_blacklist[cs].push_back(env);
}

void CompileServer::eraseCSFromBlacklist(CompileServer *cs)
{
    m_blacklist.erase(cs);
}

bool CompileServer::blacklisted(const Job *job, const pair<string, string> &environment) const
{
    Environments blacklist = job->submitter()->getEnvsForBlacklistedCS(this);
    return find(blacklist.begin(), blacklist.end(), environment) != blacklist.end();
}

int CompileServer::getInFd() const
{
    return m_inProbe.fd();
}

void CompileServer::startInConnectionTest()
{
    if (m_noRemote) {
        return;
    }

    const ConnectivityProbe::StartResult result = m_inProbe.start(name, remotePort());
    if (result == ConnectivityProbe::ImmediateSuccess) {
        updateInConnectivity(true);
    } else if (result == ConnectivityProbe::ImmediateFailure) {
        updateInConnectivity(false);
    }
}

void CompileServer::updateInConnectivity(bool acceptingIn)
{
    const bool was_accepting = m_inProbe.accepting();
    m_inProbe.recordResult(acceptingIn);

    if (acceptingIn) {
        if (!was_accepting) {
            trace() << "Client (" << m_nodeName << " " << name << ":" << m_remotePort
                    << ") is accepting incoming connections." << endl;
        }
        return;
    }

    if (was_accepting) {
        trace() << "Client (" << m_nodeName << " " << name << ":" << m_remotePort
                << ") connected but is not able to accept incoming connections." << endl;
    }
    trace() << nodeName() << " failed to accept an incoming connection on "
            << name << ":" << m_remotePort << " attempting again in "
            << m_inProbe.nextTimeoutSeconds() << " seconds" << endl;
}

bool CompileServer::isConnected()
{
    return m_inProbe.connected();
}

time_t CompileServer::getConnectionTimeout()
{
    return static_cast<time_t>(m_inProbe.connectionTimeoutSeconds());
}

bool CompileServer::getConnectionInProgress()
{
    return m_inProbe.inProgress();
}

time_t CompileServer::getNextTimeout()
{
    if (m_noRemote) {
        return -1;
    }
    return static_cast<time_t>(m_inProbe.nextTimeoutSeconds());
}
