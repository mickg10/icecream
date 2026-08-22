#pragma once

#include "protocol50.h"

namespace icecc::p50 {

// Validate the SESSION_STATE returned after an initial HISTORY_RESET against
// both the original offer and the exact cold snapshot that authorized the
// reset. HISTORY_RESET changes route history only; it cannot silently select a
// different protocol/profile/limit set or a different F_STORE_GUID.
void validate_initial_history_reset_ack(
    const SessionHello& hello,
    const SessionState& staged_cold_state,
    const HistoryReset& requested_reset,
    const SessionState& acknowledged_state);

}  // namespace icecc::p50
