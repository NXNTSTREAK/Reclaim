#pragma once
#include <string>
#include "payment.hpp"

enum class RecoveryAction {
    RETRY,
    ABANDON,
    ESCALATE_TO_AI
};

class PolicyEngine {
private:
    static constexpr int MAX_RETRIES = 3; // Hard limit

public:
    static RecoveryAction evaluate(const PaymentEvent& event, const Payment& payment) {
        if(payment.get_retry_counter() >= MAX_RETRIES) {
            return RecoveryAction::ABANDON;
        }
        

        if(event.error_reason == "payment_risk_check_failed") {
            return RecoveryAction::ABANDON;
        }
        
        // C++ Fast-Path Pre-Flight Gate: Catch insufficient funds & payday retries to bypass HTTP/LLM
        if(event.error_reason.find("insufficient_funds") != std::string::npos) {
            return RecoveryAction::RETRY;
        }
        
        return RecoveryAction::ESCALATE_TO_AI;
        
    }
};