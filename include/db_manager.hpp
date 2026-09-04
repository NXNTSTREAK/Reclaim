#pragma once
#include <string>
#include <vector>
#include <mutex>
#include <iostream>
#include <sqlite3.h>
#include "payment.hpp"
#include "recovery_attempt.hpp"

class DatabaseManager {
private:
    sqlite3* db{nullptr};
    std::mutex db_mtx; // Guards SQLite connection & compiled statements

    // Pre-compiled prepared statement handles
    sqlite3_stmt* stmt_upsert_payment{nullptr};
    sqlite3_stmt* stmt_update_payment_state{nullptr};
    sqlite3_stmt* stmt_insert_attempt{nullptr};
    sqlite3_stmt* stmt_update_attempt_state{nullptr};
    sqlite3_stmt* stmt_check_seen_event{nullptr};
    sqlite3_stmt* stmt_insert_seen_event{nullptr};
    sqlite3_stmt* stmt_atomic_credit_ledger{nullptr};

    void init_tables();
    void prepare_statements();
    void finalize_statements();

public:
    DatabaseManager();
    ~DatabaseManager();

    bool open(const std::string& db_path = "recovery_engine.db");
    void close();

    // Idempotency Operations
    bool is_event_seen(const std::string& event_id);
    bool record_seen_event(const std::string& event_id);

    // Payment Operations
    bool upsert_payment(const PaymentEvent& event);
    bool update_payment_state(const std::string& payment_id, PaymentState state);
    PaymentState fetch_payment_state(const std::string& payment_id);

    // Recovery Attempt Operations
    bool record_recovery_attempt(const RecoveryAttempt& attempt);
    bool update_attempt_state(const std::string& attempt_id, AttemptState state, int64_t completed_at = 0);

    // Atomic Financial Credit Guardrail
    bool credit_financial_ledger_atomically(
        const std::string& payment_id,
        const std::string& attempt_id,
        int64_t amount_paise
    );

    // Startup Hydration Helpers
    std::vector<RecoveryAttempt> fetch_pending_attempts();
    std::vector<RecoveryAttempt> fetch_pending_retries();
    int fetch_attempt_count(const std::string& payment_id);
    void reset_if_requested();
    int64_t fetch_total_revenue_recovered();
    int64_t fetch_total_revenue_at_risk();
};
