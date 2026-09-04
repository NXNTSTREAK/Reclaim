#pragma once
#include <queue>
#include <mutex>
#include <condition_variable>
#include "payment.hpp" 

class EventQueue {
private:
    std::queue<PaymentEvent> q;
    std::mutex mtx;
    std::condition_variable cv;

public:

    // Imma use Producer-Consumer Model to maintain the concurrency of the event queue. The Producer will push events into the queue, and the Consumer will pop events from the queue. The mutex and condition variable will ensure that the queue is accessed safely across threads.


    // The Producer calls this when a webhook arrives
    void push(const PaymentEvent& event) {
        std::lock_guard<std::mutex> lock(mtx);
        q.push(event);
        cv.notify_all();
        
    }

    // The Consumer calls this to get the next event
    PaymentEvent pop() {
        std::unique_lock<std::mutex> lock(mtx);
        

        //This piece of shit tryna check spurious wakeup, where processes wake up even when the condition is not met, so we use a while loop to check if the queue is empty, and if it is, we wait again. 
        //Syntax is cv.wait(lock, condition) 
        cv.wait(lock, [this]() { return !q.empty(); });     
        //This a lambda fn that checks if queue is empty, basically instead of writing a whole fn somewhere else, this checks allows us to write that in a single line; this an anon, inline fn.
        
        
        PaymentEvent event = q.front();
        q.pop();
        
        return event;
    }
};