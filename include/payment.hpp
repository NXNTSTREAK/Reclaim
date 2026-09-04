#pragma once
#include <string>
#include <cstdint> 
#include <iostream>

enum class PaymentState {
    CREATED,
    PENDING,
    FAILED,
    RECOVERING,
    SUCCESSFUL,
    ABANDONED
};

inline std::string to_string(PaymentState state) {
    switch (state) {
        case PaymentState::CREATED:    return "CREATED";
        case PaymentState::PENDING:    return "PENDING";
        case PaymentState::FAILED:     return "FAILED";
        case PaymentState::RECOVERING: return "RECOVERING";
        case PaymentState::SUCCESSFUL: return "SUCCESSFUL";
        case PaymentState::ABANDONED:  return "ABANDONED";
        default:                       return "UNKNOWN";
    }
}

inline PaymentState payment_state_from_string(const std::string& str) {
    if (str == "CREATED")    return PaymentState::CREATED;
    if (str == "PENDING")    return PaymentState::PENDING;
    if (str == "FAILED")     return PaymentState::FAILED;
    if (str == "RECOVERING") return PaymentState::RECOVERING;
    if (str == "SUCCESSFUL") return PaymentState::SUCCESSFUL;
    if (str == "ABANDONED")  return PaymentState::ABANDONED;
    return PaymentState::CREATED;
}

// An event is a pure data structure (struct), not a function.
struct PaymentEvent {
    std::string event_id;      // Razorpay's webhook event ID (for idempotency)
    std::string payment_id;    
    int64_t amount_paise;      // Strictly int64_t paise (e.g., 1050 for ₹10.50)

    std::string error_code;
    std::string error_description;
    std::string error_source;
    std::string error_step;
    std::string error_reason;
};

// The living entity that maintains state.
class Payment {
private:
    std::string id;
    PaymentState current_state;
    int retry_counter;
    int64_t total_amount_paise;

public:
    Payment(std::string payment_id, int64_t amount_paise) 
        : id(payment_id), 
          current_state(PaymentState::CREATED), 
          retry_counter(0), 
          total_amount_paise(amount_paise) 
    {}

    bool transition_state(PaymentState new_state) {
        // Idempotent guard: same-state transitions are always safe no-ops
        if (current_state == new_state) return true;

        bool valid = false;

        switch (current_state) {
            case PaymentState::CREATED:
                if (new_state == PaymentState::PENDING) valid = true;
                break;
            case PaymentState::PENDING:
                if (new_state == PaymentState::SUCCESSFUL || new_state == PaymentState::FAILED) valid = true;
                break;
            case PaymentState::FAILED:
                if (new_state == PaymentState::RECOVERING || new_state == PaymentState::ABANDONED || new_state == PaymentState::PENDING) valid = true;
                break;
            case PaymentState::RECOVERING:
                if (new_state == PaymentState::SUCCESSFUL || new_state == PaymentState::FAILED || new_state == PaymentState::ABANDONED) valid = true;
                break;
            case PaymentState::SUCCESSFUL:
            case PaymentState::ABANDONED:
                // Terminal states - no transitions allowed
                break;
        }

        if (valid) {
            current_state = new_state;
            return true;
        } else {
            std::cerr << "[ERROR] Illegal transition attempted on Payment " << id << "!\n";
            return false;
        }
    }

    PaymentState get_current_state() const { return current_state; }
    int get_retry_counter() const { return retry_counter; }
    int increment_retry_counter() { return ++retry_counter; }
    int64_t get_amount_paise() const { return total_amount_paise; }
    std::string get_id() const { return id; }
};