/* -*- mode: C++; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 99; -*- */
#include "connectivityprobe.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <unistd.h>

namespace {
const uint64_t kProbeTimeoutMsec = 5000;
const uint64_t kSuccessfulRecheckMsec = 60 * 1000;
}

ConnectivityProbe::ConnectivityProbe()
    : m_fd(-1)
    , m_attempt(0)
    , m_nextDueMsec(0)
    , m_deadlineMsec(0)
    , m_accepting(true)
{
}

ConnectivityProbe::~ConnectivityProbe()
{
    closeCurrentSocket();
}

uint64_t ConnectivityProbe::nowMsec() const
{
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<milliseconds>(
        steady_clock::now().time_since_epoch()).count());
}

bool ConnectivityProbe::resolve(const std::string &hostname, unsigned int port,
                                struct sockaddr_storage *address,
                                socklen_t *address_len, int *family,
                                int *socktype, int *protocol) const
{
    if (!address || !address_len || !family || !socktype || !protocol
            || hostname.empty() || port == 0 || port > 65535) {
        return false;
    }

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
#ifdef AI_ADDRCONFIG
    hints.ai_flags = AI_ADDRCONFIG;
#endif

    char service[16];
    snprintf(service, sizeof(service), "%u", port);
    struct addrinfo *results = nullptr;
    if (getaddrinfo(hostname.c_str(), service, &hints, &results) != 0 || !results) {
        return false;
    }

    bool found = false;
    for (const struct addrinfo *it = results; it; it = it->ai_next) {
        if (!it->ai_addr || it->ai_addrlen == 0
                || it->ai_addrlen > sizeof(*address)
                || (it->ai_family != AF_INET && it->ai_family != AF_INET6)
                || it->ai_socktype != SOCK_STREAM) {
            continue;
        }
        memset(address, 0, sizeof(*address));
        memcpy(address, it->ai_addr, it->ai_addrlen);
        *address_len = static_cast<socklen_t>(it->ai_addrlen);
        *family = it->ai_family;
        *socktype = it->ai_socktype;
        *protocol = it->ai_protocol;
        found = true;
        break;
    }
    freeaddrinfo(results);
    return found;
}

int ConnectivityProbe::openSocket(int family, int socktype, int protocol) const
{
    return socket(family, socktype, protocol);
}

bool ConnectivityProbe::makeNonBlocking(int fd) const
{
    const int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

int ConnectivityProbe::beginConnect(int fd, const struct sockaddr *address,
                                    socklen_t address_len) const
{
    return connect(fd, address, address_len);
}

int ConnectivityProbe::lastError() const
{
    return errno;
}

bool ConnectivityProbe::socketError(int fd, int *error) const
{
    if (!error) {
        return false;
    }
    socklen_t error_len = sizeof(*error);
    return getsockopt(fd, SOL_SOCKET, SO_ERROR, error, &error_len) == 0
           && error_len == sizeof(*error);
}

void ConnectivityProbe::closeSocket(int fd) const
{
    while (close(fd) < 0 && errno == EINTR) {
    }
}

void ConnectivityProbe::closeCurrentSocket()
{
    if (m_fd >= 0) {
        closeSocket(m_fd);
        m_fd = -1;
    }
    m_deadlineMsec = 0;
}

ConnectivityProbe::StartResult ConnectivityProbe::start(const std::string &hostname,
                                                         unsigned int port)
{
    const uint64_t now = nowMsec();
    if (inProgress() || now < m_nextDueMsec) {
        return NotDue;
    }

    struct sockaddr_storage address;
    socklen_t address_len = 0;
    int family = 0;
    int socktype = 0;
    int protocol = 0;
    if (!resolve(hostname, port, &address, &address_len, &family, &socktype, &protocol)) {
        return ImmediateFailure;
    }

    const int fd = openSocket(family, socktype, protocol);
    if (fd < 0) {
        return ImmediateFailure;
    }
    if (!makeNonBlocking(fd)) {
        closeSocket(fd);
        return ImmediateFailure;
    }

    m_fd = fd;
    m_deadlineMsec = now + kProbeTimeoutMsec;
    const int rc = beginConnect(fd, reinterpret_cast<const struct sockaddr *>(&address),
                                address_len);
    if (rc == 0) {
#ifdef ICECC_TEST_CONNPROBE_MUTANT_IMMEDIATE_FAILURE
        /* Negative control for the historical immediate-connect
           misclassification.  The normal state machine settles this as
           success without consulting elapsed-time state or SO_ERROR. */
        return ImmediateFailure;
#else
        return ImmediateSuccess;
#endif
    }

    const int error = lastError();
    if (error == EINPROGRESS || error == EAGAIN || error == EWOULDBLOCK) {
        return InProgress;
    }

    closeCurrentSocket();
    return ImmediateFailure;
}

bool ConnectivityProbe::connected() const
{
    if (!inProgress()) {
        return false;
    }
    int error = 0;
    return socketError(m_fd, &error) && error == 0;
}

bool ConnectivityProbe::deadlineExpired() const
{
    return inProgress() && nowMsec() >= m_deadlineMsec;
}

unsigned int ConnectivityProbe::connectionTimeoutSeconds() const
{
    if (!inProgress()) {
        return 0;
    }
    const uint64_t now = nowMsec();
    if (now >= m_deadlineMsec) {
        return 0;
    }
    return static_cast<unsigned int>((m_deadlineMsec - now + 999) / 1000);
}

unsigned int ConnectivityProbe::nextTimeoutSeconds() const
{
    if (inProgress()) {
        return connectionTimeoutSeconds();
    }
    const uint64_t now = nowMsec();
    if (now >= m_nextDueMsec) {
        return 0;
    }
    return static_cast<unsigned int>((m_nextDueMsec - now + 999) / 1000);
}

unsigned int ConnectivityProbe::backoffSeconds(unsigned int attempt)
{
    static const unsigned int table[] = {
        2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096
    };
#ifdef ICECC_TEST_CONNPROBE_MUTANT_BYTECOUNT
    /* Exact historical defect, compiled only by the negative-control test:
       the retry bound is the table's byte size, so repeated failures walk
       beyond the final element.  The sanitizer gate must reject this build. */
    const size_t table_size = sizeof(table);
#else
    const size_t table_size = sizeof(table) / sizeof(table[0]);
#endif
    const size_t index = std::min<size_t>(attempt, table_size - 1);
    return table[index];
}

void ConnectivityProbe::recordResult(bool accepting)
{
    closeCurrentSocket();
    const uint64_t now = nowMsec();
    if (accepting) {
        m_accepting = true;
        m_attempt = 0;
        m_nextDueMsec = now + kSuccessfulRecheckMsec;
        return;
    }

    m_accepting = false;
    const unsigned int delay = backoffSeconds(m_attempt);
    m_nextDueMsec = now + uint64_t(delay) * 1000ULL;
#ifdef ICECC_TEST_CONNPROBE_MUTANT_BYTECOUNT
    /* Keep the historical byte-count bound in the mutant so ASan observes
       the real out-of-range transition rather than a synthetic assertion. */
    if (m_attempt < sizeof(unsigned int[12]) - 1) {
        ++m_attempt;
    }
#else
    if (m_attempt < 11) {
        ++m_attempt;
    }
#endif
}

void ConnectivityProbe::cancel()
{
    closeCurrentSocket();
    m_attempt = 0;
    m_nextDueMsec = 0;
}
