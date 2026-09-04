#include <iostream>
#include <thread>
#include <unordered_set>
#include <unordered_map>
#include <random>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <csignal>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.h"
#include "json.hpp"

using json = nlohmann::json;
#include "ai_agent.hpp"
#include "payment.hpp" 
#include "event_queue.hpp"
#include "policy_engine.hpp"
#include "db_manager.hpp"
#include "scheduler.hpp"

std::atomic<bool> daemon_running{true};

void signal_handler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        std::cout << "\n[DAEMON] Shutdown signal received. Stopping worker pool & schedulers...\n";
        daemon_running = false;
    }
}

void background_worker(EventQueue& eventQueue, DatabaseManager& dbManager, SchedulerManager& schedulerManager) {
    // Persistent HTTP client with keep-alive to avoid TCP handshake overhead
    httplib::Client http_client("127.0.0.1", 8000);
    http_client.set_connection_timeout(10, 0); // 10s connection timeout
    http_client.set_read_timeout(15, 0);       // 15s read timeout for LLM inference
    http_client.set_keep_alive(true);

    while (daemon_running) {
        std::cout << "[WORKER] Waiting for next event on queue...\n";
        PaymentEvent event = eventQueue.pop();
        
        if (event.payment_id == "SHUTDOWN" || event.event_id == "SHUTDOWN") {
            std::cout << "[WORKER] Shutdown sentinel received. Exiting worker loop.\n";
            break;
        }

        std::cout << "[WORKER] Dequeued event: " << event.event_id << " for payment: " << event.payment_id << "\n";

        // 1. Idempotency Guard (DB Persistent + In-Memory)
        std::cout << "[IDEMPOTENCY] Checking if event " << event.event_id << " was already seen...\n";
        if (dbManager.is_event_seen(event.event_id)) {
            std::cout << "[IDEMPOTENCY GUARD] Dropped duplicate event: " << event.event_id << "\n";
            continue;
        }
        std::cout << "[IDEMPOTENCY] Event is new. Recording and proceeding.\n";
        dbManager.record_seen_event(event.event_id);

        // 2. Database Upsert
        std::cout << "[DB] Upserting payment record for " << event.payment_id << " into SQLite...\n";
        dbManager.upsert_payment(event);
        std::cout << "[DB] Upsert complete.\n";

        std::cout << "[ENGINE EVENT] Processing " << event.payment_id << " | Amount: ₹" 
                  << std::fixed << std::setprecision(2) << (event.amount_paise / 100.0) 
                  << " | Reason: " << event.error_reason 
                  << " | Source: " << event.error_source
                  << " | Code: " << event.error_code << "\n";

        // Scope state evaluation
        RecoveryAction action;
        bool should_call_api = false;
        std::string json_data;

        Payment payment(event.payment_id, event.amount_paise);
        int past_attempts = dbManager.fetch_attempt_count(event.payment_id);
        std::cout << "[POLICY] Past attempt count for " << event.payment_id << ": " << past_attempts << "\n";
        for (int r = 0; r < past_attempts; ++r) {
            payment.increment_retry_counter();
        }
        payment.transition_state(PaymentState::PENDING);
        payment.transition_state(PaymentState::FAILED);
        std::cout << "[DB] Marking payment " << event.payment_id << " state -> FAILED in SQLite.\n";
        dbManager.update_payment_state(event.payment_id, PaymentState::FAILED);

        std::cout << "[POLICY ENGINE] Evaluating recovery action for error_reason='" << event.error_reason << "'...\n";
        action = PolicyEngine::evaluate(event, payment);

        int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        if (action == RecoveryAction::RETRY) {
            std::cout << "[POLICY ENGINE] Action -> RETRY (Insufficient Funds payday path). Scheduling payday retry.\n";
            payment.increment_retry_counter();
            payment.transition_state(PaymentState::RECOVERING);
            dbManager.update_payment_state(event.payment_id, PaymentState::RECOVERING);

            std::string attempt_id = "att_retry_" + event.payment_id + "_" + std::to_string(now);
            RecoveryAttempt attempt{
                attempt_id, event.payment_id, "PAYDAY_RETRY", 
                AttemptState::CREATED, "", now, now + schedulerManager.get_retry_delay_seconds(), 0
            };
            std::cout << "[DB] Recording PAYDAY_RETRY attempt " << attempt_id << " in SQLite.\n";
            dbManager.record_recovery_attempt(attempt);
            schedulerManager.schedule_payday_retry(attempt_id, event.payment_id, event.amount_paise);
        }
        else if (action == RecoveryAction::ABANDON) {
            std::cout << "[POLICY ENGINE] Action -> ABANDON (risk check failed or max retries hit). Marking ABANDONED.\n";
            payment.transition_state(PaymentState::ABANDONED);
            dbManager.update_payment_state(event.payment_id, PaymentState::ABANDONED);
        }
        else if (action == RecoveryAction::ESCALATE_TO_AI) {
            std::cout << "[POLICY ENGINE] Action -> ESCALATE_TO_AI. Invoking C++ AIAgent::analyze_failure()...\n";
            AIRecommendation recommendation = AIAgent::analyze_failure(event, payment);
            std::cout << "[AI AGENT] Diagnosis: '" << recommendation.diagnosis 
                      << "' | Suggested: " << recommendation.suggested_action 
                      << " | Confidence: " << recommendation.confidence_score << "\n";

            if (recommendation.suggested_action == "SMS_LINK_ALTERNATE_METHOD" && recommendation.confidence_score > 80) {
                std::cout << "[AI AGENT] Confidence threshold met. Will call Python /generate_link API.\n";
                should_call_api = true;
                json_data = "{"
                    "\"payment_id\": \"" + event.payment_id + "\", "
                    "\"amount\": " + std::to_string(event.amount_paise) + ", "
                    "\"error_code\": \"" + event.error_code + "\", "
                    "\"error_source\": \"" + event.error_source + "\", "
                    "\"error_step\": \"" + event.error_step + "\", "
                    "\"error_reason\": \"" + event.error_reason + "\", "
                    "\"error_description\": \"" + event.error_description + "\""
                "}";
                std::cout << "[HTTP] Payload prepared: " << json_data << "\n";
            } else {
                std::cout << "[AI AGENT] Confidence too low or action mismatch. Skipping API call.\n";
            }
        }

        // 3. Smart-Path HTTP API Call (Outside lock, persistent HTTP client)
        if (should_call_api) {
            std::cout << "[HTTP] Dispatching POST /generate_link request to API bridge...\n";
            auto t_start = std::chrono::steady_clock::now();
            auto res = http_client.Post("/generate_link", json_data, "application/json");
            auto t_end = std::chrono::steady_clock::now();
            double elapsed_ms = std::chrono::duration<double, std::milli>(t_end - t_start).count();
            std::cout << "[HTTP] /generate_link responded in " << std::fixed << std::setprecision(1) << elapsed_ms << "ms.\n";

            if (res && res->status == 200) {
                std::cout << "[HTTP] Response 200 OK. Body: " << res->body << "\n";
                std::string attempt_id = "att_link_" + event.payment_id + "_" + std::to_string(now);

                if (res->body.find("\"action\":\"GENERATE_NEW_LINK\"") != std::string::npos ||
                    res->body.find("\"action\": \"GENERATE_NEW_LINK\"") != std::string::npos) 
                {
                    std::cout << "[RECOVERY] Payment link generated. Transitioning state -> RECOVERING.\n";
                    payment.transition_state(PaymentState::RECOVERING);
                    dbManager.update_payment_state(event.payment_id, PaymentState::RECOVERING);

                    RecoveryAttempt attempt{
                        attempt_id, event.payment_id, "SMS_PAYMENT_LINK", 
                        AttemptState::PENDING, "rzp_link_" + event.payment_id, 
                        now, now + schedulerManager.get_expiry_delay_seconds(), 0
                    };
                    std::cout << "[DB] Recording SMS_PAYMENT_LINK attempt " << attempt_id << " in SQLite.\n";
                    dbManager.record_recovery_attempt(attempt);
                    schedulerManager.schedule_expiry_check(attempt_id, event.payment_id);
                }
                else if (res->body.find("SCHEDULE_PAYDAY_RETRY") != std::string::npos) {
                    std::cout << "[RECOVERY] Python bridge returned SCHEDULE_PAYDAY_RETRY. Recording attempt.\n";
                    RecoveryAttempt attempt{
                        attempt_id, event.payment_id, "PAYDAY_RETRY", 
                        AttemptState::CREATED, "", 
                        now, now + schedulerManager.get_retry_delay_seconds(), 0
                    };
                    dbManager.record_recovery_attempt(attempt);
                    schedulerManager.schedule_payday_retry(attempt_id, event.payment_id, event.amount_paise);
                } else {
                    std::cout << "[RECOVERY] Unrecognised action in response. No attempt recorded.\n";
                }
            } else {
                int status_code = res ? res->status : -1;
                std::cerr << "[WARNING] /generate_link API call FAILED (status=" << status_code << "). Falling back to Payday Retry.\n";
                std::string attempt_id = "att_fb_" + event.payment_id + "_" + std::to_string(now);
                RecoveryAttempt attempt{
                    attempt_id, event.payment_id, "PAYDAY_RETRY", 
                    AttemptState::CREATED, "", 
                    now, now + schedulerManager.get_retry_delay_seconds(), 0
                };
                dbManager.record_recovery_attempt(attempt);
                schedulerManager.schedule_payday_retry(attempt_id, event.payment_id, event.amount_paise);
            }
        } else {
            std::cout << "[WORKER] No API call needed for this event. Processing complete.\n";
        }
    }
}

void razorpay_polling_thread(EventQueue& eventQueue, DatabaseManager& dbManager) {
    const char* key_id = std::getenv("RAZORPAY_KEY_ID");
    const char* key_secret = std::getenv("RAZORPAY_KEY_SECRET");

    if (!key_id || !key_secret) {
        std::cerr << "[RAZORPAY POLLER WARNING] Missing RAZORPAY_KEY_ID or RAZORPAY_KEY_SECRET. Poller will use dummy simulation mode.\n";
    }

    httplib::Client rzp_client("https://api.razorpay.com");
    if (key_id && key_secret) {
        rzp_client.set_basic_auth(key_id, key_secret);
    }
    rzp_client.set_connection_timeout(10, 0);
    rzp_client.set_read_timeout(10, 0);

    std::cout << "[RAZORPAY POLLER] Active. Polling Razorpay API every 5s for failed payments...\n";

    while (daemon_running) {
        // Sleep in 1-second increments to allow responsive shutdown
        for (int i = 0; i < 5 && daemon_running; ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

        if (!daemon_running) break;

        if (key_id && key_secret) {
            std::cout << "[RAZORPAY POLLER] Polling Razorpay API (GET /v1/payments?count=10)...\n";
            auto t0 = std::chrono::steady_clock::now();
            auto res = rzp_client.Get("/v1/payments?count=10");
            auto t1 = std::chrono::steady_clock::now();
            double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
            std::cout << "[RAZORPAY POLLER] HTTP response received in " << std::fixed << std::setprecision(1) << ms << "ms.\n";
            
            if (res && res->status == 200) {
                try {
                    auto data = json::parse(res->body);
                    if (data.contains("items") && data["items"].is_array()) {
                        int total = (int)data["items"].size();
                        int failed_count = 0;
                        int new_count = 0;
                        std::cout << "[RAZORPAY POLLER] Received " << total << " payments from API.\n";
                        for (auto& item : data["items"]) {
                            std::string status = item.value("status", "");
                            std::string payment_id = item.value("id", "");
                            std::cout << "[RAZORPAY POLLER] Payment " << payment_id << " status=" << status << "\n";
                            if (status == "failed") {
                                failed_count++;
                                std::string event_id = "evt_poll_" + payment_id;
                                
                                if (!dbManager.is_event_seen(event_id)) {
                                    new_count++;
                                    PaymentEvent event{
                                        event_id,
                                        payment_id,
                                        item.value("amount", 0LL),
                                        item.value("error_code", "UNKNOWN"),
                                        item.value("error_description", "No description provided"),
                                        item.value("error_source", "unknown"),
                                        item.value("error_step", "unknown"),
                                        item.value("error_reason", "unknown")
                                    };
                                    
                                    std::cout << "[RAZORPAY POLLER] NEW failed payment found: " 
                                              << payment_id << " | Amount: ₹" << std::fixed << std::setprecision(2) 
                                              << (event.amount_paise / 100.0)
                                              << " | Reason: " << event.error_reason
                                              << " | Source: " << event.error_source << "\n";
                                    eventQueue.push(event);
                                    std::cout << "[RAZORPAY POLLER] Ingested newly failed payment " 
                                              << payment_id << " (₹" << std::fixed << std::setprecision(2) 
                                              << (event.amount_paise / 100.0) << ") from Razorpay API\n";
                                } else {
                                    std::cout << "[RAZORPAY POLLER] Payment " << payment_id << " already seen. Skipping.\n";
                                }
                            }
                        }
                        std::cout << "[RAZORPAY POLLER] Poll complete: total=" << total 
                                  << " failed=" << failed_count 
                                  << " new_ingested=" << new_count << "\n";
                    } else {
                        std::cout << "[RAZORPAY POLLER] No 'items' array in response.\n";
                    }
                } catch (const std::exception& e) {
                    std::cerr << "[RAZORPAY POLLER ERROR] Failed to parse JSON: " << e.what() << "\n";
                }
            }
        }
    }
}

void udp_webhook_listener(EventQueue& eventQueue, DatabaseManager& dbManager) {
    int server_fd;
    struct sockaddr_in address;
    int opt = 1;
    char buffer[4096] = {0};

    if ((server_fd = socket(AF_INET, SOCK_DGRAM, 0)) == 0) {
        std::cerr << "[UDP LISTENER ERROR] Socket creation failed.\n";
        return;
    }

    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = inet_addr("127.0.0.1");
    address.sin_port = htons(9001);

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        std::cerr << "[UDP LISTENER ERROR] Bind to port 9001 failed.\n";
        return;
    }

    std::cout << "[UDP LISTENER] Active on 127.0.0.1:9001. High-speed datagram ingestion enabled.\n";

    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);

    while (daemon_running) {
        memset(buffer, 0, sizeof(buffer));
        ssize_t bytes_read = recvfrom(server_fd, buffer, sizeof(buffer) - 1, 0, (struct sockaddr*)&client_addr, &addr_len);
        if (bytes_read > 0) {
            buffer[bytes_read] = '\0';
            try {
                auto data = json::parse(buffer);
                std::string payment_id = data.value("payment_id", "");
                std::string event_id = data.value("event_id", "evt_udp_" + payment_id);

                if (!payment_id.empty() && !dbManager.is_event_seen(event_id)) {
                    PaymentEvent event{
                        event_id,
                        payment_id,
                        data.value("amount", 0LL),
                        data.value("error_code", "UNKNOWN"),
                        data.value("error_description", "No description provided"),
                        data.value("error_source", "unknown"),
                        data.value("error_step", "unknown"),
                        data.value("error_reason", "unknown")
                    };
                    eventQueue.push(event);
                }
            } catch (const std::exception& e) {
                std::cerr << "[UDP PARSE ERROR] " << e.what() << "\n";
            }
        }
    }
    close(server_fd);
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::cout << std::unitbuf;

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    std::cout << "========================================================\n";
    std::cout << "        RECLAIM - AI REVENUE RECOVERY DAEMON\n";
    std::cout << "========================================================\n";

    // 1. Initialize SQLite Database Manager (WAL Mode)
    DatabaseManager dbManager;
    if (!dbManager.open("recovery_engine.db")) {
        std::cerr << "[FATAL] Failed to initialize SQLite database. Exiting.\n";
        return 1;
    }

    // 2. Initialize Event Queue & Scheduler Manager
    EventQueue eventQueue;
    SchedulerManager schedulerManager(dbManager, eventQueue);

    // Hydrate pending timers from SQLite
    schedulerManager.hydrate_from_db();
    schedulerManager.start();

    // 3. Spawn Worker Thread Pool
    int num_workers = 2;
    std::vector<std::thread> thread_pool;
    std::cout << "[SYSTEM] Starting " << num_workers << " background worker threads...\n";

    for (int i = 0; i < num_workers; i++) {
        thread_pool.emplace_back(std::thread(background_worker, std::ref(eventQueue), std::ref(dbManager), std::ref(schedulerManager)));
    }

    // 4. Ingestion Thread: Pure Live Webhook Mode (Port 9001)
    std::cout << "[SYSTEM] Operating in Webhook Mode. API Poller disabled.\n";
    std::thread udp_thread(udp_webhook_listener, std::ref(eventQueue), std::ref(dbManager));
    udp_thread.detach();

    std::cout << "\n[DAEMON MODE] Engine running continuously. Press Ctrl+C to terminate.\n";

    // Continuous Daemon Execution Loop
    while (daemon_running) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    // Shutdown Sequence
    std::cout << "\n[SHUTDOWN] Injecting shutdown sentinels for " << num_workers << " worker threads...\n";
    for (int i = 0; i < num_workers; i++) {
        eventQueue.push({"SHUTDOWN", "SHUTDOWN", 0, "", "", "", "", ""});
    }

    for (auto& thread : thread_pool) {
        if (thread.joinable()) thread.join();
    }

    schedulerManager.stop();

    std::cout << "\n========================================================\n";
    std::cout << "        DAEMON FINANCIAL LEDGER AUDIT            \n";
    std::cout << "========================================================\n";
    std::cout << std::fixed << std::setprecision(2);
    int64_t total_at_risk = dbManager.fetch_total_revenue_at_risk();
    int64_t total_recovered = dbManager.fetch_total_revenue_recovered();
    std::cout << "Total Revenue at Risk : ₹" << (total_at_risk / 100.0) << "\n";
    std::cout << "Total Revenue Recovered: ₹" << (total_recovered / 100.0) << "\n";
    std::cout << "========================================================\n";

    dbManager.close();
    return 0;
}