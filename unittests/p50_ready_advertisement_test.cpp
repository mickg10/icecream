#include "cache/p50_ready_advertisement.h"
#include "services/comm.h"

#include <cstdio>
#include <cstdlib>
#include <limits>

using icecc::p50::advertisement::Controller;
using icecc::p50::advertisement::Error;
using icecc::p50::advertisement::Observation;
using icecc::p50::advertisement::Snapshot;
using icecc::p50::advertisement::Update;
using icecc::p50::sidecar::State;

static int failures = 0;

#define CHECK(condition, text) do {                                      \
    if (condition) { std::fprintf(stderr, "ok       - %s\n", text); }  \
    else { std::fprintf(stderr, "FAILED   - %s\n", text); ++failures; } \
} while (0)

static Observation observation(State state, bool authenticated,
                               uint64_t exits = 0,
                               bool listener_bound = true,
                               uint32_t port = 10245)
{
    return Observation{listener_bound, port, state, authenticated, exits};
}

static bool is_absent(const Snapshot& snapshot)
{
    return snapshot.endpoint_port == 0 && snapshot.protocol == 0
        && snapshot.profile_mask == 0 && snapshot.absent()
        && !snapshot.present();
}

static bool is_exact_present(const Snapshot& snapshot, uint32_t port = 10245)
{
    return snapshot.endpoint_port == port
        && snapshot.protocol == CACHE_WIRE_PROTOCOL_V1
        && snapshot.profile_mask == CACHE_PROFILE_ZSTD_TU
        && snapshot.present() && !snapshot.absent();
}

static void test_pre_ready_and_authentication_gate()
{
    Controller controller;
    CHECK(is_absent(controller.snapshot()),
          "controller starts with canonical absent advertisement");

    Update update = controller.observe(observation(State::Starting, true));
    CHECK(update.count == 0 && update.error == Error::None
              && is_absent(controller.snapshot()),
          "authenticated Starting sidecar cannot advertise before READY");

    update = controller.observe(observation(State::Ready, false));
    CHECK(update.count == 0 && update.error == Error::None
              && is_absent(controller.snapshot()),
          "READY sidecar cannot advertise before private authentication");

    update = controller.observe(observation(State::Ready, true));
    CHECK(update.count == 1 && is_exact_present(update.transitions[0])
              && is_exact_present(controller.snapshot()),
          "READY authenticated sidecar publishes the exact runnable profile");

    update = controller.observe(observation(State::Ready, true));
    CHECK(update.count == 0 && update.error == Error::None
              && is_exact_present(controller.snapshot()),
          "duplicate ready observation produces no Login churn");
}

static void test_withdrawal_levels()
{
    Controller controller;
    (void)controller.observe(observation(State::Ready, true));

    Update update = controller.observe(observation(State::DegradedLegacy, true));
    CHECK(update.count == 1 && is_absent(update.transitions[0])
              && is_absent(controller.snapshot()),
          "degraded supervisor withdraws an advertised capability");

    (void)controller.observe(observation(State::Ready, true));
    update = controller.observe(observation(State::Ready, false));
    CHECK(update.count == 1 && is_absent(update.transitions[0]),
          "private authentication loss withdraws an advertised capability");

    (void)controller.observe(observation(State::Ready, true));
    update = controller.observe(observation(State::Ready, true, 0, false));
    CHECK(update.count == 1 && is_absent(update.transitions[0]),
          "public listener loss withdraws an advertised capability");
}

static void test_crash_edges_preserve_withdraw_before_republish()
{
    Controller controller;
    (void)controller.observe(observation(State::Ready, true));

    Update update = controller.observe(observation(State::Ready, true, 1));
    CHECK(update.count == 2 && is_absent(update.transitions[0])
              && is_exact_present(update.transitions[1])
              && is_exact_present(controller.snapshot()),
          "compressed crash and recovery withdraws before republishing");

    update = controller.observe(observation(State::Ready, false, 2));
    CHECK(update.count == 1 && is_absent(update.transitions[0])
              && is_absent(controller.snapshot()),
          "crash without a new authenticated relationship remains absent");

    update = controller.observe(observation(State::Ready, true, 2));
    CHECK(update.count == 1 && is_exact_present(update.transitions[0]),
          "later authenticated recovery publishes one new presence");
}

static void test_invalid_inputs_fail_closed()
{
    Controller controller;
    (void)controller.observe(observation(State::Ready, true, 8));

    Update update = controller.observe(
        observation(State::Ready, true, 8, true, 70000));
    CHECK(update.error == Error::InvalidPublicPort && update.count == 1
              && is_absent(update.transitions[0])
              && is_absent(controller.snapshot()),
          "out-of-range bound public port fails closed");

    update = controller.observe(observation(State::Ready, true, 8, true, 0));
    CHECK(update.error == Error::InvalidPublicPort && update.count == 0
              && is_absent(controller.snapshot()),
          "zero bound public port stays failed closed without duplicate churn");

    update = controller.observe(observation(State::Ready, true, 7));
    CHECK(update.error == Error::CounterRegression && update.count == 0
              && is_absent(controller.snapshot()),
          "supervisor recreation cannot reset the cumulative exit counter");

    update = controller.observe(observation(State::Ready, true, 8));
    CHECK(update.error == Error::None && update.count == 1
              && is_exact_present(update.transitions[0]),
          "counter catch-up permits a fresh exact advertisement");
}

static void test_saturated_exit_counter_fails_closed_permanently()
{
    Controller controller;
    constexpr uint64_t almost_max = std::numeric_limits<uint64_t>::max() - 1;
    constexpr uint64_t saturated = std::numeric_limits<uint64_t>::max();

    Update update = controller.observe(
        observation(State::Ready, true, almost_max));
    CHECK(update.count == 1 && is_exact_present(update.transitions[0]),
          "controller can advertise below the cumulative counter limit");

    update = controller.observe(observation(State::Ready, true, saturated));
    CHECK(update.error == Error::CounterSaturated && update.count == 1
              && is_absent(update.transitions[0])
              && is_absent(controller.snapshot()),
          "saturated crash observation withdraws instead of republishing");

    update = controller.observe(observation(State::Ready, true, saturated));
    CHECK(update.error == Error::CounterSaturated && update.count == 0
              && is_absent(controller.snapshot()),
          "same-counter READY recovery at saturation remains failed closed");

    update = controller.observe(observation(State::Ready, true, almost_max));
    CHECK(update.error == Error::CounterRegression && update.count == 0
              && is_absent(controller.snapshot()),
          "counter wrap or replacement reset cannot escape saturated absence");
}

static void test_initial_nonzero_counter_and_port_change()
{
    Controller controller;
    Update update = controller.observe(observation(State::Ready, true, 19));
    CHECK(update.count == 1 && is_exact_present(update.transitions[0]),
          "first observation establishes a nonzero counter baseline");

    update = controller.observe(observation(State::Ready, true, 19, true, 23456));
    CHECK(update.count == 1 && is_exact_present(update.transitions[0], 23456)
              && is_exact_present(controller.snapshot(), 23456),
          "public listener port replacement publishes one canonical snapshot");
}

int main()
{
    test_pre_ready_and_authentication_gate();
    test_withdrawal_levels();
    test_crash_edges_preserve_withdraw_before_republish();
    test_invalid_inputs_fail_closed();
    test_saturated_exit_counter_fails_closed_permanently();
    test_initial_nonzero_counter_and_port_change();
    if (failures != 0) {
        std::fprintf(stderr, "p50readyadvertisement: %d failure(s)\n", failures);
        return EXIT_FAILURE;
    }
    std::fprintf(stderr, "p50readyadvertisement: ok\n");
    return EXIT_SUCCESS;
}
