#pragma once
#include <string>
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <cstdint>

#include "event_queue.hpp"
#include "db_manager.hpp"
#include "recovery_attempt.hpp"

enum class TimerTaskType {
    EXPIRY_CHECK,
    PAYDAY_RETRY
};

struct ScheduledTimerTask {
    std::string task_id;          // attempt_id or payment_id
    std::string payment_id;
    TimerTaskType task_type;
    int64_t wake_up_epoch;       // Target epoch timestamp in seconds
    int64_t amount_paise;

    // Min-heap comparator for priority_queue (earliest wake_up_epoch on top)
    bool operator>(const ScheduledTimerTask& other) const {
        return wake_up_epoch > other.wake_up_epoch;
    }
};

class SchedulerManager {
private:
    std::priority_queue<
        ScheduledTimerTask, 
        std::vector<ScheduledTimerTask>, 
        std::greater<ScheduledTimerTask>
    > task_queue;

    std::mutex queue_mtx;
    std::condition_variable cv;
    std::thread worker_thread;
    std::atomic<bool> running{false};

    bool demo_mode{false};
    DatabaseManager& db_manager;
    EventQueue& event_queue;

    void run_loop();

public:
    SchedulerManager(DatabaseManager& db, EventQueue& eq);
    ~SchedulerManager();

    void start();
    void stop();

    // Environment-aware time calculation methods
    int64_t get_expiry_delay_seconds() const;
    int64_t get_retry_delay_seconds() const;
    bool is_demo_mode() const { return demo_mode; }

    // Schedule Task Methods
    void schedule_expiry_check(const std::string& attempt_id, const std::string& payment_id, int64_t custom_delay = 0);
    void schedule_payday_retry(const std::string& attempt_id, const std::string& payment_id, int64_t amount_paise, int64_t custom_delay = 0);

    // Hydration on startup
    void hydrate_from_db();
};
