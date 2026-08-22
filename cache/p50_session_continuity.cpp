#include "p50_session_continuity.h"

#include <stdexcept>

namespace icecc::p50 {

void validate_initial_history_reset_ack(
    const SessionHello& hello,
    const SessionState& staged_cold_state,
    const HistoryReset& requested_reset,
    const SessionState& acknowledged_state) {
    validate_session_state(hello, staged_cold_state);
    validate_session_state(hello, acknowledged_state);

    if (hello.c_store_guid == CStoreGuid{})
        throw std::invalid_argument(
            "initial HISTORY_RESET used zero C_STORE_GUID");
    if (staged_cold_state.f_store_guid == FStoreGuid{})
        throw std::invalid_argument(
            "initial HISTORY_RESET snapshot used zero F_STORE_GUID");
    if (staged_cold_state.namespace_present ||
        staged_cold_state.route_present ||
        staged_cold_state.last_commit)
        throw std::logic_error(
            "initial HISTORY_RESET did not start from a cold snapshot");

    if (requested_reset.history_nonce.value == 0)
        throw std::invalid_argument(
            "initial HISTORY_RESET used zero HISTORY_NONCE");
    const Digest128 expected_initial = initial_route_digest(
        hello.c_store_guid, requested_reset.history_nonce);
    if (requested_reset.initial_state_digest != expected_initial)
        throw std::invalid_argument(
            "initial HISTORY_RESET digest is not canonical");

    if (acknowledged_state.selected_protocol !=
            staged_cold_state.selected_protocol ||
        acknowledged_state.selected_profile !=
            staged_cold_state.selected_profile ||
        acknowledged_state.limits != staged_cold_state.limits ||
        acknowledged_state.f_store_guid !=
            staged_cold_state.f_store_guid)
        throw std::logic_error(
            "HISTORY_RESET acknowledgement changed session identity");

    if (!acknowledged_state.namespace_present ||
        !acknowledged_state.route_present ||
        acknowledged_state.history_nonce !=
            requested_reset.history_nonce ||
        acknowledged_state.next_rel_seq.value != 0 ||
        acknowledged_state.state_digest !=
            requested_reset.initial_state_digest ||
        acknowledged_state.last_commit)
        throw std::logic_error(
            "HISTORY_RESET acknowledgement differs from requested route");
}

}  // namespace icecc::p50
