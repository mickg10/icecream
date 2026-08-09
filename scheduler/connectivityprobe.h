/* -*- mode: C++; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 99; -*- */
/*
    A bounded, monotonic state machine for the scheduler's daemon inbound
    connectivity probe.  Kept separate from CompileServer so the retry and
    socket lifecycle can be exhaustively unit-tested without a scheduler.
*/
#ifndef ICECC_CONNECTIVITYPROBE_H
#define ICECC_CONNECTIVITYPROBE_H

#include <stdint.h>
#include <sys/socket.h>

#include <string>

class ConnectivityProbe
{
public:
    enum StartResult {
        NotDue,
        InProgress,
        ImmediateSuccess,
        ImmediateFailure
    };

    ConnectivityProbe();
    virtual ~ConnectivityProbe();

    StartResult start(const std::string &hostname, unsigned int port);
    void recordResult(bool accepting);
    void cancel();

    bool inProgress() const { return m_fd >= 0; }
    int fd() const { return m_fd; }
    bool connected() const;
    bool deadlineExpired() const;

    /* Whole seconds, rounded up for poll(); 0 means due now. */
    unsigned int connectionTimeoutSeconds() const;
    unsigned int nextTimeoutSeconds() const;

    bool accepting() const { return m_accepting; }
    unsigned int attempt() const { return m_attempt; }
    uint64_t nextDueMsec() const { return m_nextDueMsec; }
    uint64_t deadlineMsec() const { return m_deadlineMsec; }

protected:
    /* Test seam: unit tests replace every external effect deterministically. */
    virtual uint64_t nowMsec() const;
    virtual bool resolve(const std::string &hostname, unsigned int port,
                         struct sockaddr_storage *address, socklen_t *address_len,
                         int *family, int *socktype, int *protocol) const;
    virtual int openSocket(int family, int socktype, int protocol) const;
    virtual bool makeNonBlocking(int fd) const;
    virtual int beginConnect(int fd, const struct sockaddr *address,
                             socklen_t address_len) const;
    virtual int lastError() const;
    virtual bool socketError(int fd, int *error) const;
    virtual void closeSocket(int fd) const;

private:
    static unsigned int backoffSeconds(unsigned int attempt);
    void closeCurrentSocket();

    int m_fd;
    unsigned int m_attempt;
    uint64_t m_nextDueMsec;
    uint64_t m_deadlineMsec;
    bool m_accepting;
};

#endif
