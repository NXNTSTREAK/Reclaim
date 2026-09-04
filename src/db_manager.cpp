#include "db_manager.hpp"
#include <chrono>
#include <sstream>
#include <memory>

DatabaseManager::DatabaseManager() {}

DatabaseManager::~DatabaseManager() {
    close();
}

bool DatabaseManager::open(const std::string& db_path) {
    std::lock_guard<std::mutex> lock(db_mtx);

    // Open database using SQLITE_OPEN_FULLMUTEX for thread safety across worker pool
    int rc = sqlite3_open_v2(
        db_path.c_str(), 
        &db, 
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, 
        nullptr
    );

    if (rc != SQLITE_OK) {
        std::cerr << "[DB ERROR] Cannot open database " << db_path << ": " 
                  << (db ? sqlite3_errmsg(db) : "Unknown error") << "\n";
        return false;
    }

    // Performance PRAGMAs
    char* err_msg = nullptr;
    const char* pragma_sql = 
        "PRAGMA journal_mode = WAL;"
        "PRAGMA synchronous = NORMAL;"
        "PRAGMA cache_size = -64000;"
        "PRAGMA temp_store = MEMORY;"
        "PRAGMA foreign_keys = ON;";

    if (sqlite3_exec(db, pragma_sql, nullptr, nullptr, &err_msg) != SQLITE_OK) {
        std::cerr << "[DB ERROR] PRAGMA initialization failed: " << (err_msg ? err_msg : "") << "\n";
        if (err_msg) sqlite3_free(err_msg);
        return false;
    }

    init_tables();
    reset_if_requested();
    prepare_statements();
    std::cout << "[DB] Direct SQLite3 database initialized in WAL mode: " << db_path << "\n";
    return true;
}

void DatabaseManager::init_tables() {
    const char* create_tables_sql = R"(
        CREATE TABLE IF NOT EXISTS payments (
            payment_id TEXT PRIMARY KEY,
            amount_paise INTEGER NOT NULL,
            current_state TEXT NOT NULL,
            created_at INTEGER NOT NULL,
            updated_at INTEGER NOT NULL
        );

        CREATE TABLE IF NOT EXISTS recovery_attempts (
            attempt_id TEXT PRIMARY KEY,
            payment_id TEXT NOT NULL,
            strategy TEXT NOT NULL,
            state TEXT NOT NULL,
            razorpay_link_id TEXT,
            created_at INTEGER NOT NULL,
            expires_at INTEGER NOT NULL,
            completed_at INTEGER DEFAULT 0,
            FOREIGN KEY(payment_id) REFERENCES payments(payment_id)
        );

        CREATE TABLE IF NOT EXISTS seen_events (
            event_id TEXT PRIMARY KEY,
            processed_at INTEGER NOT NULL
        );

        CREATE TABLE IF NOT EXISTS financial_ledger (
            ledger_id TEXT PRIMARY KEY,
            payment_id TEXT NOT NULL,
            attempt_id TEXT,
            amount_paise INTEGER NOT NULL,
            entry_type TEXT NOT NULL,
            created_at INTEGER NOT NULL,
            FOREIGN KEY(payment_id) REFERENCES payments(payment_id)
        );
    )";

    char* err_msg = nullptr;
    if (sqlite3_exec(db, create_tables_sql, nullptr, nullptr, &err_msg) != SQLITE_OK) {
        std::cerr << "[DB ERROR] Table creation failed: " << (err_msg ? err_msg : "") << "\n";
        if (err_msg) sqlite3_free(err_msg);
    }
}

void DatabaseManager::prepare_statements() {
    finalize_statements(); // Clean up if re-preparing

    const char* sql_upsert_payment = 
        "INSERT INTO payments (payment_id, amount_paise, current_state, created_at, updated_at) "
        "VALUES (?, ?, ?, ?, ?) "
        "ON CONFLICT(payment_id) DO UPDATE SET "
        "amount_paise=excluded.amount_paise, updated_at=excluded.updated_at;";
    sqlite3_prepare_v2(db, sql_upsert_payment, -1, &stmt_upsert_payment, nullptr);

    const char* sql_update_payment_state = 
        "UPDATE payments SET current_state = ?, updated_at = ? WHERE payment_id = ?;";
    sqlite3_prepare_v2(db, sql_update_payment_state, -1, &stmt_update_payment_state, nullptr);

    const char* sql_insert_attempt = 
        "INSERT INTO recovery_attempts (attempt_id, payment_id, strategy, state, razorpay_link_id, created_at, expires_at, completed_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?);";
    sqlite3_prepare_v2(db, sql_insert_attempt, -1, &stmt_insert_attempt, nullptr);

    const char* sql_update_attempt_state = 
        "UPDATE recovery_attempts SET state = ?, completed_at = ? WHERE attempt_id = ?;";
    sqlite3_prepare_v2(db, sql_update_attempt_state, -1, &stmt_update_attempt_state, nullptr);

    const char* sql_check_seen = "SELECT 1 FROM seen_events WHERE event_id = ?;";
    sqlite3_prepare_v2(db, sql_check_seen, -1, &stmt_check_seen_event, nullptr);

    const char* sql_insert_seen = "INSERT INTO seen_events (event_id, processed_at) VALUES (?, ?);";
    sqlite3_prepare_v2(db, sql_insert_seen, -1, &stmt_insert_seen_event, nullptr);

    const char* sql_atomic_credit = 
        "INSERT INTO financial_ledger (ledger_id, payment_id, attempt_id, amount_paise, entry_type, created_at) "
        "SELECT ?, ?, ?, ?, 'RECOVERY_CREDIT', ? "
        "WHERE NOT EXISTS ("
        "  SELECT 1 FROM financial_ledger WHERE payment_id = ? AND entry_type = 'RECOVERY_CREDIT'"
        ");";
    sqlite3_prepare_v2(db, sql_atomic_credit, -1, &stmt_atomic_credit_ledger, nullptr);
}

void DatabaseManager::finalize_statements() {
    if (stmt_upsert_payment) { sqlite3_finalize(stmt_upsert_payment); stmt_upsert_payment = nullptr; }
    if (stmt_update_payment_state) { sqlite3_finalize(stmt_update_payment_state); stmt_update_payment_state = nullptr; }
    if (stmt_insert_attempt) { sqlite3_finalize(stmt_insert_attempt); stmt_insert_attempt = nullptr; }
    if (stmt_update_attempt_state) { sqlite3_finalize(stmt_update_attempt_state); stmt_update_attempt_state = nullptr; }
    if (stmt_check_seen_event) { sqlite3_finalize(stmt_check_seen_event); stmt_check_seen_event = nullptr; }
    if (stmt_insert_seen_event) { sqlite3_finalize(stmt_insert_seen_event); stmt_insert_seen_event = nullptr; }
    if (stmt_atomic_credit_ledger) { sqlite3_finalize(stmt_atomic_credit_ledger); stmt_atomic_credit_ledger = nullptr; }
}

void DatabaseManager::close() {
    std::lock_guard<std::mutex> lock(db_mtx);
    if (db) {
        finalize_statements();
        sqlite3_close(db);
        db = nullptr;
    }
}

bool DatabaseManager::is_event_seen(const std::string& event_id) {
    std::lock_guard<std::mutex> lock(db_mtx);
    if (!stmt_check_seen_event) return false;

    sqlite3_reset(stmt_check_seen_event);
    sqlite3_bind_text(stmt_check_seen_event, 1, event_id.c_str(), -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt_check_seen_event);
    return (rc == SQLITE_ROW);
}

bool DatabaseManager::record_seen_event(const std::string& event_id) {
    std::lock_guard<std::mutex> lock(db_mtx);
    if (!stmt_insert_seen_event) return false;

    int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    sqlite3_reset(stmt_insert_seen_event);
    sqlite3_bind_text(stmt_insert_seen_event, 1, event_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt_insert_seen_event, 2, now);

    return (sqlite3_step(stmt_insert_seen_event) == SQLITE_DONE);
}

bool DatabaseManager::upsert_payment(const PaymentEvent& event) {
    std::lock_guard<std::mutex> lock(db_mtx);
    if (!stmt_upsert_payment) return false;

    int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    sqlite3_reset(stmt_upsert_payment);
    sqlite3_bind_text(stmt_upsert_payment, 1, event.payment_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt_upsert_payment, 2, event.amount_paise);
    sqlite3_bind_text(stmt_upsert_payment, 3, "CREATED", -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt_upsert_payment, 4, now);
    sqlite3_bind_int64(stmt_upsert_payment, 5, now);

    return (sqlite3_step(stmt_upsert_payment) == SQLITE_DONE);
}

bool DatabaseManager::update_payment_state(const std::string& payment_id, PaymentState state) {
    std::lock_guard<std::mutex> lock(db_mtx);
    if (!stmt_update_payment_state) return false;

    int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    std::string state_str = to_string(state);

    sqlite3_reset(stmt_update_payment_state);
    sqlite3_bind_text(stmt_update_payment_state, 1, state_str.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt_update_payment_state, 2, now);
    sqlite3_bind_text(stmt_update_payment_state, 3, payment_id.c_str(), -1, SQLITE_TRANSIENT);

    return (sqlite3_step(stmt_update_payment_state) == SQLITE_DONE);
}

bool DatabaseManager::record_recovery_attempt(const RecoveryAttempt& attempt) {
    std::lock_guard<std::mutex> lock(db_mtx);
    if (!stmt_insert_attempt) return false;

    std::string state_str = to_string(attempt.state);

    sqlite3_reset(stmt_insert_attempt);
    sqlite3_bind_text(stmt_insert_attempt, 1, attempt.attempt_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_insert_attempt, 2, attempt.payment_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_insert_attempt, 3, attempt.strategy.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_insert_attempt, 4, state_str.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_insert_attempt, 5, attempt.razorpay_link_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt_insert_attempt, 6, attempt.created_at);
    sqlite3_bind_int64(stmt_insert_attempt, 7, attempt.expires_at);
    sqlite3_bind_int64(stmt_insert_attempt, 8, attempt.completed_at);

    return (sqlite3_step(stmt_insert_attempt) == SQLITE_DONE);
}

bool DatabaseManager::update_attempt_state(const std::string& attempt_id, AttemptState state, int64_t completed_at) {
    std::lock_guard<std::mutex> lock(db_mtx);
    if (!stmt_update_attempt_state) return false;

    std::string state_str = to_string(state);

    sqlite3_reset(stmt_update_attempt_state);
    sqlite3_bind_text(stmt_update_attempt_state, 1, state_str.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt_update_attempt_state, 2, completed_at);
    sqlite3_bind_text(stmt_update_attempt_state, 3, attempt_id.c_str(), -1, SQLITE_TRANSIENT);

    return (sqlite3_step(stmt_update_attempt_state) == SQLITE_DONE);
}

bool DatabaseManager::credit_financial_ledger_atomically(
    const std::string& payment_id,
    const std::string& attempt_id,
    int64_t amount_paise
) {
    std::lock_guard<std::mutex> lock(db_mtx);
    if (!stmt_atomic_credit_ledger) return false;

    int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    std::string ledger_id = "ledg_" + payment_id + "_" + std::to_string(now);

    sqlite3_reset(stmt_atomic_credit_ledger);
    sqlite3_bind_text(stmt_atomic_credit_ledger, 1, ledger_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_atomic_credit_ledger, 2, payment_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_atomic_credit_ledger, 3, attempt_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt_atomic_credit_ledger, 4, amount_paise);
    sqlite3_bind_int64(stmt_atomic_credit_ledger, 5, now);
    sqlite3_bind_text(stmt_atomic_credit_ledger, 6, payment_id.c_str(), -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt_atomic_credit_ledger);
    if (rc == SQLITE_DONE) {
        int changes = sqlite3_changes(db);
        if (changes > 0) {
            std::cout << "[FINANCIAL LEDGER] Successfully credited " << (amount_paise / 100.0) 
                      << " INR for payment " << payment_id << " (Attempt: " << attempt_id << ")\n";
            return true;
        } else {
            std::cout << "[FINANCIAL LEDGER GUARD] Duplicate credit attempt blocked for payment: " << payment_id << "\n";
            return false;
        }
    }
    return false;
}

std::vector<RecoveryAttempt> DatabaseManager::fetch_pending_attempts() {
    std::lock_guard<std::mutex> lock(db_mtx);
    std::vector<RecoveryAttempt> list;
    const char* sql = "SELECT attempt_id, payment_id, strategy, state, razorpay_link_id, created_at, expires_at, completed_at "
                      "FROM recovery_attempts WHERE state IN ('CREATED', 'SENT', 'PENDING');";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            RecoveryAttempt att;
            att.attempt_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            att.payment_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            att.strategy = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            att.state = attempt_state_from_string(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3)));
            const unsigned char* link = sqlite3_column_text(stmt, 4);
            att.razorpay_link_id = link ? reinterpret_cast<const char*>(link) : "";
            att.created_at = sqlite3_column_int64(stmt, 5);
            att.expires_at = sqlite3_column_int64(stmt, 6);
            att.completed_at = sqlite3_column_int64(stmt, 7);
            list.push_back(att);
        }
        sqlite3_finalize(stmt);
    }
    return list;
}

std::vector<RecoveryAttempt> DatabaseManager::fetch_pending_retries() {
    std::lock_guard<std::mutex> lock(db_mtx);
    std::vector<RecoveryAttempt> list;
    const char* sql = "SELECT attempt_id, payment_id, strategy, state, razorpay_link_id, created_at, expires_at, completed_at "
                      "FROM recovery_attempts WHERE strategy = 'PAYDAY_RETRY' AND state IN ('CREATED', 'PENDING');";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            RecoveryAttempt att;
            att.attempt_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            att.payment_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            att.strategy = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            att.state = attempt_state_from_string(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3)));
            const unsigned char* link = sqlite3_column_text(stmt, 4);
            att.razorpay_link_id = link ? reinterpret_cast<const char*>(link) : "";
            att.created_at = sqlite3_column_int64(stmt, 5);
            att.expires_at = sqlite3_column_int64(stmt, 6);
            att.completed_at = sqlite3_column_int64(stmt, 7);
            list.push_back(att);
        }
        sqlite3_finalize(stmt);
    }
    return list;
}

int64_t DatabaseManager::fetch_total_revenue_recovered() {
    std::lock_guard<std::mutex> lock(db_mtx);
    const char* sql = "SELECT COALESCE(SUM(amount_paise), 0) FROM financial_ledger WHERE entry_type = 'RECOVERY_CREDIT';";
    sqlite3_stmt* stmt = nullptr;
    int64_t total = 0;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            total = sqlite3_column_int64(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    return total;
}

int64_t DatabaseManager::fetch_total_revenue_at_risk() {
    std::lock_guard<std::mutex> lock(db_mtx);
    const char* sql = "SELECT COALESCE(SUM(amount_paise), 0) FROM payments;";
    sqlite3_stmt* stmt = nullptr;
    int64_t total = 0;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            total = sqlite3_column_int64(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    return total;
}

int DatabaseManager::fetch_attempt_count(const std::string& payment_id) {
    std::lock_guard<std::mutex> lock(db_mtx);
    const char* sql = "SELECT COUNT(*) FROM recovery_attempts WHERE payment_id = ?;";
    sqlite3_stmt* raw_stmt = nullptr;
    int count = 0;

    if (sqlite3_prepare_v2(db, sql, -1, &raw_stmt, nullptr) == SQLITE_OK) {
        std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt(raw_stmt, sqlite3_finalize);
        sqlite3_bind_text(stmt.get(), 1, payment_id.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt.get()) == SQLITE_ROW) {
            count = sqlite3_column_int(stmt.get(), 0);
        }
    }
    return count;
}

void DatabaseManager::reset_if_requested() {
    const char* env_reset = std::getenv("RESET_DB");
    if (env_reset && (std::string(env_reset) == "true" || std::string(env_reset) == "1")) {
        sqlite3_exec(db, "DELETE FROM financial_ledger;", nullptr, nullptr, nullptr);
        sqlite3_exec(db, "DELETE FROM recovery_attempts;", nullptr, nullptr, nullptr);
        sqlite3_exec(db, "DELETE FROM payments;", nullptr, nullptr, nullptr);
        sqlite3_exec(db, "DELETE FROM seen_events;", nullptr, nullptr, nullptr);
        std::cout << "[DB RESET] Purged all database tables on startup (RESET_DB=true).\n";
    }
}

PaymentState DatabaseManager::fetch_payment_state(const std::string& payment_id) {
    std::lock_guard<std::mutex> lock(db_mtx);
    const char* sql = "SELECT current_state FROM payments WHERE payment_id = ?;";
    sqlite3_stmt* raw_stmt = nullptr;
    PaymentState state = PaymentState::FAILED;

    if (sqlite3_prepare_v2(db, sql, -1, &raw_stmt, nullptr) == SQLITE_OK) {
        std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt(raw_stmt, sqlite3_finalize);
        sqlite3_bind_text(stmt.get(), 1, payment_id.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt.get()) == SQLITE_ROW) {
            const unsigned char* text = sqlite3_column_text(stmt.get(), 0);
            if (text) {
                state = payment_state_from_string(reinterpret_cast<const char*>(text));
            }
        }
    }
    return state;
}
