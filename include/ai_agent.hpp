#pragma once
#include <string>
#include <iostream>
#include <thread>
#include <chrono>
#include "payment.hpp"

struct AIRecommendation {
    std::string diagnosis;
    std::string suggested_action; 
    int confidence_score;         
};

class AIAgent {
public:
    static AIRecommendation analyze_failure(const PaymentEvent& event, const Payment& payment) {

        std::string prompt = "Payment ID: " + event.payment_id + "\n"
                           + "Amount (Paise): " + std::to_string(event.amount_paise) + "\n"
                           + "Error Code: " + event.error_code + "\n"
                           + "Error Reason: " + event.error_reason + "\n"
                           + "Error Description: " + event.error_description + "\n"
                           + "Source: " + event.error_source + " | Step: " + event.error_step + "\n";
                           
        // Non-blocking analysis fast-path simulation
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        if (event.error_reason == "insufficient_funds") {
            return {"Customer lacks balance in account.", "SMS_LINK_ALTERNATE_METHOD", 90};
        } else {
            return {"Gateway or banking timeout occurred.", "SMS_LINK_ALTERNATE_METHOD", 85};
        }
    }
};