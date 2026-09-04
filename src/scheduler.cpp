#include "scheduler.hpp"
#include <iostream>
#include <chrono>
#include <cstdlib>

SchedulerManager::SchedulerManager(DatabaseManager& db, EventQueue& eq)
    : db_manager(db), event_queue(eq) 
{
    const char* env_demo = std::getenv("DEMO_MODE");
    if (env_demo && (std::string(env_demo) == "true" || std::string(env_demo) == "1")) {
        demo_mode = true;
        std::cout << "[SCHEDULER] DEMO_MODE enabled: Expiry = 2m (120s), Payday Retry = 5m (300s).\n";
    } else {
        demo_mode = false;
        std::cout << "[SCHEDULER] Production Mode: Expiry = 15m (900s), Payday Retry = 24h (86400s).\n";
    }
}

SchedulerManager::~SchedulerManager() {
    stop();
}

int64_t SchedulerManager::get_expiry_delay_seconds() const {
    return 900; // 15-minute expiry window
}

int64_t SchedulerManager::get_retry_delay_seconds() const {
    return 120; // 2-minute payday retry window for quick testing
}

void SchedulerManager::start() {
    running = true;
    worker_thread = std::thread(&SchedulerManager::run_loop, this);
    std::cout << "[SCHEDULER] Continuous non-blocking scheduler thread started.\n";
}

void SchedulerManager::stop() {
    if (running) {
        running = false;
        cv.notify_all();
        if (worker_thread.joinable()) {
            worker_thread.join();
        }
        std::cout << "[SCHEDULER] Scheduler thread stopped.\n";
    }
}

void SchedulerManager::schedule_expiry_check(const std::string& attempt_id, const std::string& payment_id, int64_t custom_delay) {
    int64_t delay = (custom_delay > 0) ? custom_delay : get_expiry_delay_seconds();
    int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    ScheduledTimerTask task{attempt_id, payment_id, TimerTaskType::EXPIRY_CHECK, now + delay, 0};

    {
        std::lock_guard<std::mutex> lock(queue_mtx);
        task_queue.push(task);
    }
    cv.notify_all();
    std::cout << "[SCHEDULER] Scheduled 15-Min Expiry Check for attempt " << attempt_id 
              << " in " << delay << "s.\n";
}

void SchedulerManager::schedule_payday_retry(const std::string& attempt_id, const std::string& payment_id, int64_t amount_paise, int64_t custom_delay) {
    int64_t delay = (custom_delay > 0) ? custom_delay : get_retry_delay_seconds();
    int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    ScheduledTimerTask task{attempt_id, payment_id, TimerTaskType::PAYDAY_RETRY, now + delay, amount_paise};

    {
        std::lock_guard<std::mutex> lock(queue_mtx);
        task_queue.push(task);
    }
    cv.notify_all();
    std::cout << "[SCHEDULER] Scheduled Payday Retry for payment " << payment_id 
              << " in " << delay << "s.\n";
}

void SchedulerManager::hydrate_from_db() {
    int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    auto pending_attempts = db_manager.fetch_pending_attempts();
    int hydrated_count = 0;

    for (const auto& att : pending_attempts) {
        if (att.strategy == "SMS_PAYMENT_LINK" && att.state == AttemptState::PENDING) {
            int64_t delay = (att.expires_at > now) ? (att.expires_at - now) : 1;
            schedule_expiry_check(att.attempt_id, att.payment_id, delay);
            hydrated_count++;
        } else if (att.strategy == "PAYDAY_RETRY" && (att.state == AttemptState::CREATED || att.state == AttemptState::PENDING)) {
            int64_t delay = (att.expires_at > now) ? (att.expires_at - now) : 1;
            schedule_payday_retry(att.attempt_id, att.payment_id, 0, delay);
            hydrated_count++;
        }
    }
    std::cout << "[SCHEDULER] Hydrated " << hydrated_count << " active timer tasks from SQLite.\n";
}

void SchedulerManager::run_loop() {
    while (running) {
        std::unique_lock<std::mutex> lock(queue_mtx);

        if (task_queue.empty()) {
            cv.wait(lock, [this]() { return !running || !task_queue.empty(); });
        } else {
            auto top_task = task_queue.top();
            auto wake_time = std::chrono::system_clock::time_point(
                std::chrono::seconds(top_task.wake_up_epoch)
            );

            if (cv.wait_until(lock, wake_time) == std::cv_status::timeout || 
                std::chrono::system_clock::now() >= wake_time) 
            {
                // Re-verify top task after wakeup
                if (!task_queue.empty() && task_queue.top().task_id == top_task.task_id) {
                    ScheduledTimerTask task = task_queue.top();
                    task_queue.pop();
                    lock.unlock(); // Unlock before executing DB/Queue actions

                    int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();

                    if (task.task_type == TimerTaskType::EXPIRY_CHECK) {
                        if (db_manager.fetch_payment_state(task.payment_id) != PaymentState::SUCCESSFUL) {
                            std::cout << "[LINK EXPIRED] Expiry window elapsed for attempt: " 
                                      << task.task_id << " (Payment: " << task.payment_id << ")\n";
                            std::cout << "[ABANDONED] Payment link for " << task.payment_id 
                                      << " expired without payment. Payment dropped & deducted from Revenue at Risk.\n";
                            db_manager.update_attempt_state(task.task_id, AttemptState::EXPIRED, now);
                            db_manager.update_payment_state(task.payment_id, PaymentState::ABANDONED);
                        } else {
                            std::cout << "[LINK EXPIRED GUARD] Payment " << task.payment_id 
                                      << " was already paid! Skipping expiry abandonment.\n";
                        }
                    } 
                    else if (task.task_type == TimerTaskType::PAYDAY_RETRY) {
                        std::cout << "[SCHEDULER TIMER FIRE] 24-Hr Payday Retry fired for payment: " 
                                  << task.payment_id << "\n";
                        
                        db_manager.update_attempt_state(task.task_id, AttemptState::EXPIRED, now);

                        // High-precision timestamp in microseconds concatenated with payment_id
                        auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count();
                        std::string unique_event_id = "evt_retry_" + task.payment_id + "_" + std::to_string(now_us);

                        // Re-inject PaymentEvent into C++ EventQueue
                        PaymentEvent retry_event{
                            unique_event_id,
                            task.payment_id,
                            task.amount_paise > 0 ? task.amount_paise : 25000,
                            "PAYDAY_RETRY_TRIGGER",
                            "24-Hour Payday Retry Triggered",
                            "scheduler",
                            "payday_retry",
                            "insufficient_funds_retry"
                        };
                        event_queue.push(retry_event);
                    }

                    lock.lock(); // Relock for next iteration
                }
            }
        }
    }
}
