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
#include <arpa/inet.h>
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
    , m_inFd(-1)
    , m_inConnAttempt(0)
    , m_nextConnMono(0)
    , m_connStartMono(0)
    , m_acceptingInConnection(true)
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
                    && m_acceptingInConnection
                    && can_install(job, true).size()
                    && check_remote(job);
#if DEBUG_SCHEDULER > 2
    trace() << nodeName() << " is_eligible_ever: " << eligible << " (jobs_okay " << jobs_okay
        << ", version_okay " << version_okay << ", features_okay " << features_okay
        << ", chroot_or_local " << (m_chrootPossible || job->submitter() == this)
        << ", accepting " << m_acceptingInConnection << ", can_install " << (can_install(job).size() != 0)
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
}

const list<Job *>& CompileServer::jobList() const
{
    return m_jobList;
}

uint64_t CompileServer::s_pickSequence = 0;

void CompileServer::appendJob(Job *job)
{
    m_lastPickId = job->id();
    m_lastPickSeq = ++s_pickSequence;
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
    map<int, LocalJobInfo>::const_iterator it = m_clientLocalMap.find(localJobId);
    return it == m_clientLocalMap.end() ? 0 : it->second.id;
}

int CompileServer::insertClientLocalJobId(const int localJobId, const int newJobId, bool fulljob)
{
    int displaced = 0;
    map<int, LocalJobInfo>::iterator it = m_clientLocalMap.find(localJobId);
    if (it != m_clientLocalMap.end()) {
        displaced = it->second.id;
    }
    m_clientLocalMap[localJobId] = LocalJobInfo{newJobId, fulljob};
    return displaced;
}

std::list<int> CompileServer::liveClientLocalJobIds() const
{
    std::list<int> ids;
    for (const std::pair<const int, LocalJobInfo> &info : m_clientLocalMap) {
        ids.push_back(info.second.id);
    }
    return ids;
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
    return m_inFd;
}

CompileServer::~CompileServer()
{
    /* A daemon deleted while its connectivity probe is pending must not
       leak the probe descriptor.  */
    closeProbeFd();
}

void CompileServer::closeProbeFd()
{
    if (m_inFd >= 0) {
        close(m_inFd);
        m_inFd = -1;
    }
}

bool CompileServer::probeCompletionOk()
{
    int error = 0;
    socklen_t err_len = sizeof(error);
    return getsockopt(m_inFd, SOL_SOCKET, SO_ERROR, &error, &err_len) == 0
           && error == 0;
}

bool CompileServer::probeDeadlineExpired() const
{
    const uint64_t now = icecream_monotonic_msec();
    return now >= m_connStartMono + 5000;
}

void CompileServer::startInConnectionTest()
{
    if (m_noRemote || getConnectionInProgress()
            || (m_nextConnMono > icecream_monotonic_msec()))
    {
        return;
    }

    /* Every resource this attempt needs is VALIDATED before use, and every
       terminal path below owns exactly one close/reset of the probe fd
       (via updateInConnectivity).  */
    m_inFd = socket(PF_INET, SOCK_STREAM, 0);
    if (m_inFd < 0)
    {
        updateInConnectivity(false);
        return;
    }
    const int flags = fcntl(m_inFd, F_GETFL, 0);
    if (flags < 0 || fcntl(m_inFd, F_SETFL, flags | O_NONBLOCK) < 0)
    {
        updateInConnectivity(false);
        return;
    }

    /* The peer name the scheduler records is the connection's numeric
       address: convert it directly.  A resolver lookup here could fail or
       block independently of the socket it is supposed to describe.  */
    struct sockaddr_in remote_addr;
    memset(&remote_addr, 0, sizeof(remote_addr));
    remote_addr.sin_family = AF_INET;
    remote_addr.sin_port = htons(remotePort());
    if (inet_pton(AF_INET, name.c_str(), &remote_addr.sin_addr) != 1)
    {
        updateInConnectivity(false);
        return;
    }

    /* The attempt clock starts BEFORE connect: a synchronous success must
       be judged against THIS attempt, not the previous attempt's start
       time (which made getConnectionTimeout() read 0 and record an
       immediate success as a failure).  */
    m_connStartMono = icecream_monotonic_msec();
    int status = connect(m_inFd, (struct sockaddr *)&remote_addr, sizeof(remote_addr));
    if (status == 0)
    {
        /* Synchronous completion IS the success verdict.  */
        updateInConnectivity(true);
    }
    else if (!(errno == EINPROGRESS || errno == EAGAIN))
    {
        updateInConnectivity(false);
    }
}

void CompileServer::updateInConnectivity(bool acceptingIn)
{
    static const time_t time_offset_table[] = {
        2,    4,    8,    16,    32,
        64,  128,  256,   512,  1024,
        2048, 4096
    };
    /* Element count, NOT byte count: sizeof(table) is 96 on this build, so
       the old value let m_inConnAttempt climb to 95 and index the table 84
       slots out of bounds after enough consecutive failures -- a
       long-unreachable daemon could read garbage delays or crash the
       scheduler.  */
    static const size_t table_size =
        sizeof(time_offset_table) / sizeof(time_offset_table[0]);

    //On a successful connection, we should still check back every 1min
    static const time_t check_back_time = 60;

    if(acceptingIn)
    {
        if(!m_acceptingInConnection)
        {
            m_acceptingInConnection = true;
            m_inConnAttempt = 0;
            trace() << "Client (" << m_nodeName <<
                " " << name <<
                ":" << m_remotePort <<
                ") is accepting incoming connections." << endl;
        }
        m_nextConnMono = icecream_monotonic_msec()
                         + (uint64_t)check_back_time * 1000;
        closeProbeFd();
    }
    else
    {
        if(m_acceptingInConnection)
        {
            m_acceptingInConnection = false;
            trace() << "Client (" << m_nodeName <<
                " " << name <<
                ":" << m_remotePort <<
                ") connected but is not able to accept incoming connections." << endl;
        }
        const unsigned int idx =
            m_inConnAttempt < table_size ? m_inConnAttempt : (unsigned int)(table_size - 1);
        m_nextConnMono = icecream_monotonic_msec()
                         + (uint64_t)time_offset_table[idx] * 1000;
        if(m_inConnAttempt < (table_size - 1))
            m_inConnAttempt++;
        trace()  << nodeName() << " failed to accept an incoming connection on "
            << name << ":" << m_remotePort << " attempting again in "
            << (m_nextConnMono - icecream_monotonic_msec()) / 1000 << " seconds" << endl;
        closeProbeFd();
    }

}

bool CompileServer::isConnected()
{
    if (probeDeadlineExpired())
    {
        return false;
    }
    return probeCompletionOk();
}

time_t CompileServer::getConnectionTimeout()
{
    const uint64_t now = icecream_monotonic_msec();
    const uint64_t elapsed = now >= m_connStartMono ? now - m_connStartMono : 0;
    const uint64_t max_ms = 5000;
    /* Whole seconds, rounded UP: a not-yet-expired attempt must never
       report 0 (0 is the expired verdict).  */
    return (elapsed < max_ms) ? (time_t)((max_ms - elapsed + 999) / 1000) : 0;
}

bool CompileServer::getConnectionInProgress()
{
    return (m_inFd != -1);
}

time_t CompileServer::getNextTimeout()
{
    if (m_noRemote)
    {
        return -1;
    }
    if (m_inFd != -1)
    {
        return getConnectionTimeout();
    }
    const uint64_t now = icecream_monotonic_msec();
    if (m_nextConnMono <= now) {
        return 0;
    }
    return (time_t)((m_nextConnMono - now + 999) / 1000);
}
