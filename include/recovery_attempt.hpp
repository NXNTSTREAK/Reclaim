#pragma once
#include <string>
#include <cstdint>

enum class AttemptState {
    CREATED,
    SENT,
    PENDING,
    SUCCESSFUL,
    EXPIRED,
    LATE_PAYMENT
};

inline std::string to_string(AttemptState state) {
    switch (state) {
        case AttemptState::CREATED:      return "CREATED";
        case AttemptState::SENT:         return "SENT";
        case AttemptState::PENDING:      return "PENDING";
        case AttemptState::SUCCESSFUL:   return "SUCCESSFUL";
        case AttemptState::EXPIRED:      return "EXPIRED";
        case AttemptState::LATE_PAYMENT: return "LATE_PAYMENT";
        default:                         return "UNKNOWN";
    }
}

inline AttemptState attempt_state_from_string(const std::string& str) {
    if (str == "CREATED")      return AttemptState::CREATED;
    if (str == "SENT")         return AttemptState::SENT;
    if (str == "PENDING")      return AttemptState::PENDING;
    if (str == "SUCCESSFUL")   return AttemptState::SUCCESSFUL;
    if (str == "EXPIRED")      return AttemptState::EXPIRED;
    if (str == "LATE_PAYMENT") return AttemptState::LATE_PAYMENT;
    return AttemptState::CREATED;
}

struct RecoveryAttempt {
    std::string attempt_id;
    std::string payment_id;
    std::string strategy;         // "SMS_PAYMENT_LINK", "PAYDAY_RETRY", etc.
    AttemptState state;
    std::string razorpay_link_id;
    int64_t created_at;           // Epoch seconds
    int64_t expires_at;           // Epoch seconds
    int64_t completed_at;         // 0 if active, timestamp if terminal

    // State Machine Transition Rule Validation
    static bool is_valid_transition(AttemptState current, AttemptState next) {
        if (current == next) return true; // Idempotent same-state check
        switch (current) {
            case AttemptState::CREATED:
                return (next == AttemptState::SENT || next == AttemptState::PENDING);
            case AttemptState::SENT:
                return (next == AttemptState::PENDING || next == AttemptState::EXPIRED);
            case AttemptState::PENDING:
                return (next == AttemptState::SUCCESSFUL || next == AttemptState::EXPIRED);
            case AttemptState::EXPIRED:
                return (next == AttemptState::LATE_PAYMENT); // Late webhook recovery
            case AttemptState::SUCCESSFUL:
            case AttemptState::LATE_PAYMENT:
                return false; // Terminal states
        }
        return false;
    }
};
