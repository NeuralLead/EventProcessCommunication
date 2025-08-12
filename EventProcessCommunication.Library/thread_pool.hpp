#pragma once

#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <functional>
#include <atomic>

//#define WITH_INTHREAD_EXCEPTION true

class ThreadPool {
public:
    explicit ThreadPool(size_t num_threads) : stop_flag(false) {
        for (size_t i = 0; i < num_threads; ++i) {
            workers.emplace_back([this]() {
                while (true) {
                    std::function<void()> task;

                    {   // scoped lock
                        std::unique_lock<std::mutex> lock(queue_mutex);
                        condition.wait(lock, [this]() {
                            return stop_flag || !tasks.empty();
                            });

                        if (stop_flag && tasks.empty())
                            return;

                        task = std::move(tasks.front());
                        tasks.pop();
                    }

#if defined(WITH_INTHREAD_EXCEPTION)
                    try {
                        task();  // Esegui il task
                    }
                    catch (...) {
                        // gestisci eccezioni
                        //std::cerr << "Exception " << e.what() << std::endl;
                    }
#else
                    task();  // Esegui il task
#endif
                }
            });
        }
    }

    // Submit un nuovo task al pool
    void enqueue(std::function<void()> task) {
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            if (stop_flag) return; // ignore task on shutdown
            tasks.emplace(std::move(task));
        }
        condition.notify_one();
    }

    // Distruttore: attende la fine di tutti i thread
    ~ThreadPool() {
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            stop_flag = true;
        }
        condition.notify_all();
        for (auto& worker : workers)
            if (worker.joinable())
                worker.join();
    }

private:
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;

    std::mutex queue_mutex;
    std::condition_variable condition;
    std::atomic<bool> stop_flag;
};