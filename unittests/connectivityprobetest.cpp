/* Deterministic state-machine tests for scheduler/connectivityprobe.cpp. */
#include "../scheduler/connectivityprobe.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>

static int failures = 0;

#define REQUIRE(cond, what)                                                     \
    do {                                                                        \
        if (cond) {                                                             \
            fprintf(stderr, "ok       - %s\n", what);                         \
        } else {                                                                \
            fprintf(stderr, "FAILED   - %s (at %s:%d)\n", what, __FILE__,    \
                    __LINE__);                                                   \
            ++failures;                                                         \
        }                                                                       \
    } while (0)

class FakeProbe : public ConnectivityProbe
{
public:
    mutable uint64_t now = 100000;
    mutable bool resolve_ok = true;
    mutable int resolve_calls = 0;
    mutable int open_result = 41;
    mutable int open_calls = 0;
    mutable bool nonblock_ok = true;
    mutable int nonblock_calls = 0;
    mutable int connect_result = -1;
    mutable int connect_error = EINPROGRESS;
    mutable int connect_calls = 0;
    mutable bool socket_error_ok = true;
    mutable int socket_error_value = 0;
    mutable int socket_error_calls = 0;
    mutable int close_calls = 0;

protected:
    uint64_t nowMsec() const override { return now; }

    bool resolve(const std::string &, unsigned int port,
                 struct sockaddr_storage *address, socklen_t *address_len,
                 int *family, int *socktype, int *protocol) const override
    {
        ++resolve_calls;
        if (!resolve_ok || !address || !address_len || !family || !socktype
                || !protocol || port == 0 || port > 65535) {
            return false;
        }
        struct sockaddr_in in;
        memset(&in, 0, sizeof(in));
        in.sin_family = AF_INET;
        in.sin_port = htons(static_cast<unsigned short>(port));
        memset(address, 0, sizeof(*address));
        memcpy(address, &in, sizeof(in));
        *address_len = sizeof(in);
        *family = AF_INET;
        *socktype = SOCK_STREAM;
        *protocol = IPPROTO_TCP;
        return true;
    }

    int openSocket(int, int, int) const override
    {
        ++open_calls;
        return open_result;
    }

    bool makeNonBlocking(int) const override
    {
        ++nonblock_calls;
        return nonblock_ok;
    }

    int beginConnect(int, const struct sockaddr *, socklen_t) const override
    {
        ++connect_calls;
        return connect_result;
    }

    int lastError() const override { return connect_error; }

    bool socketError(int, int *error) const override
    {
        ++socket_error_calls;
        if (!socket_error_ok || !error) {
            return false;
        }
        *error = socket_error_value;
        return true;
    }

    void closeSocket(int) const override { ++close_calls; }
};

static void test_failure_validation_and_backoff()
{
    FakeProbe p;
    p.resolve_ok = false;
    REQUIRE(p.start("unresolved.invalid", 10245) == ConnectivityProbe::ImmediateFailure,
            "resolver failure is an explicit immediate probe failure");
    REQUIRE(p.open_calls == 0, "resolver failure never opens a socket");
    p.recordResult(false);
    REQUIRE(!p.accepting(), "a failed probe removes inbound eligibility");
    REQUIRE(p.attempt() == 1, "first failure advances one retry-table element");
    REQUIRE(p.nextDueMsec() == p.now + 2000, "first retry delay is two seconds");
    REQUIRE(p.start("unresolved.invalid", 10245) == ConnectivityProbe::NotDue,
            "retry cannot start before its absolute deadline");

    p.now = p.nextDueMsec();
    p.resolve_ok = true;
    p.open_result = -1;
    REQUIRE(p.start("worker", 10245) == ConnectivityProbe::ImmediateFailure,
            "socket failure is an explicit immediate probe failure");
    REQUIRE(p.connect_calls == 0, "socket failure never calls connect");
    p.recordResult(false);
    REQUIRE(p.attempt() == 2 && p.nextDueMsec() == p.now + 4000,
            "second failure selects the four-second element");

    p.now = p.nextDueMsec();
    p.open_result = 42;
    p.nonblock_ok = false;
    REQUIRE(p.start("worker", 10245) == ConnectivityProbe::ImmediateFailure,
            "fcntl failure is an explicit immediate probe failure");
    REQUIRE(p.close_calls == 1, "fcntl failure closes the newly opened socket exactly once");
    p.recordResult(false);
    REQUIRE(p.attempt() == 3 && p.nextDueMsec() == p.now + 8000,
            "third failure selects the eight-second element");
}

static void test_immediate_and_deferred_success()
{
    FakeProbe immediate;
    immediate.connect_result = 0;
    REQUIRE(immediate.start("worker", 10245) == ConnectivityProbe::ImmediateSuccess,
            "connect(2) immediate success is classified as success");
    REQUIRE(immediate.inProgress(), "immediate success keeps the probe fd until result settlement");
    REQUIRE(immediate.socket_error_calls == 0,
            "immediate success does not perform a redundant getsockopt or resolution");
    immediate.recordResult(true);
    REQUIRE(immediate.accepting(), "immediate success preserves inbound eligibility");
    REQUIRE(immediate.close_calls == 1, "successful settlement closes the probe socket once");
    REQUIRE(immediate.attempt() == 0, "success resets the retry index");
    REQUIRE(immediate.nextDueMsec() == immediate.now + 60000,
            "success schedules a monotonic sixty-second recheck");

    FakeProbe deferred;
    deferred.connect_result = -1;
    deferred.connect_error = EINPROGRESS;
    REQUIRE(deferred.start("worker", 10245) == ConnectivityProbe::InProgress,
            "EINPROGRESS enters the bounded in-progress state");
    REQUIRE(deferred.deadlineMsec() == deferred.now + 5000,
            "the in-progress state has one absolute monotonic deadline");
    REQUIRE(deferred.connectionTimeoutSeconds() == 5,
            "poll timeout is derived from the absolute deadline");
    deferred.now += 4999;
    REQUIRE(!deferred.deadlineExpired() && deferred.connectionTimeoutSeconds() == 1,
            "remaining timeout rounds up and does not expire early");
    REQUIRE(deferred.connected(), "SO_ERROR zero completes a deferred connect successfully");
    REQUIRE(deferred.socket_error_calls == 1,
            "deferred completion reads SO_ERROR exactly once per decision");
    deferred.recordResult(true);
    REQUIRE(!deferred.inProgress() && deferred.close_calls == 1,
            "deferred success settles and closes the probe fd");
}

static void test_timeout_and_socket_error()
{
    FakeProbe timed;
    REQUIRE(timed.start("worker", 10245) == ConnectivityProbe::InProgress,
            "timeout case enters the in-progress state");
    timed.now += 5000;
    REQUIRE(timed.deadlineExpired() && timed.connectionTimeoutSeconds() == 0,
            "the absolute deadline expires without a descriptor event");
    timed.recordResult(false);
    REQUIRE(!timed.inProgress() && timed.close_calls == 1,
            "timeout settlement closes the probe socket");

    FakeProbe refused;
    REQUIRE(refused.start("worker", 10245) == ConnectivityProbe::InProgress,
            "refusal case enters the in-progress state");
    refused.socket_error_value = ECONNREFUSED;
    REQUIRE(!refused.connected(), "nonzero SO_ERROR is a failed completion");
    refused.recordResult(false);
    REQUIRE(refused.close_calls == 1 && !refused.accepting(),
            "failed completion closes the socket and removes eligibility");

    FakeProbe broken_getsockopt;
    REQUIRE(broken_getsockopt.start("worker", 10245) == ConnectivityProbe::InProgress,
            "getsockopt failure case enters the in-progress state");
    broken_getsockopt.socket_error_ok = false;
    REQUIRE(!broken_getsockopt.connected(), "getsockopt failure cannot become success");
    broken_getsockopt.recordResult(false);
}

static void test_repeated_failure_saturation()
{
    FakeProbe p;
    static const unsigned int expected[] = {
        2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096
    };
    for (unsigned int i = 0; i < 100; ++i) {
        const uint64_t before = p.now;
        p.recordResult(false);
        const unsigned int delay = static_cast<unsigned int>((p.nextDueMsec() - before) / 1000);
        const unsigned int want = expected[i < 12 ? i : 11];
        REQUIRE(delay == want, "repeated failure uses a bounded retry-table element");
        REQUIRE(p.attempt() <= 11, "retry index saturates at the final element");
        p.now = p.nextDueMsec();
    }
    REQUIRE(p.attempt() == 11, "one hundred failures leave the index clamped at eleven");

    p.connect_result = 0;
    REQUIRE(p.start("worker", 10245) == ConnectivityProbe::ImmediateSuccess,
            "a probe can recover after a saturated failure sequence");
    p.recordResult(true);
    REQUIRE(p.attempt() == 0 && p.accepting(), "recovery resets the complete failure state");
}

static void test_cancel()
{
    FakeProbe p;
    REQUIRE(p.start("worker", 10245) == ConnectivityProbe::InProgress,
            "cancel case enters the in-progress state");
    p.cancel();
    REQUIRE(!p.inProgress() && p.close_calls == 1,
            "cancel closes an in-progress socket exactly once");
    REQUIRE(p.attempt() == 0 && p.nextDueMsec() == 0,
            "cancel clears retry state without inventing a result");
}

int main()
{
    test_failure_validation_and_backoff();
    test_immediate_and_deferred_success();
    test_timeout_and_socket_error();
    test_repeated_failure_saturation();
    test_cancel();
    fprintf(stderr, "# connectivityprobe: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
