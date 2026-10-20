#pragma once

#include "backup/engine.h"
#include "backup/error.h"

#include <condition_variable>
#include <algorithm>
#include <chrono>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace backup {

class ThreadPool {
public:
    ThreadPool(size_t workers, size_t capacity) : capacity_(capacity) {
        require(workers > 0 && capacity > 0, "configuration", "invalid worker pool size");
        try {
            for (size_t i = 0; i < workers; ++i) {
                workers_.emplace_back(&ThreadPool::worker_thread, this);
            }
        } catch (...) {
            shutdown();
            throw;
        }
    }
    ~ThreadPool() { shutdown(); }
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    template<class F>
    auto enqueue(F&& function) -> std::future<std::invoke_result_t<F>> {
        using Result = std::invoke_result_t<F>;
        auto task = std::make_shared<std::packaged_task<Result()>>(std::forward<F>(function));
        auto result = task->get_future();
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            while (!shutdown_ && tasks_.size() >= capacity_) {
                condition_.wait_for(lock, std::chrono::milliseconds(100));
                check_cancelled();
            }
            require(!shutdown_, "cancelled", "worker pool is stopped");
            tasks_.emplace([task] { (*task)(); });
            peak_queue_ = std::max(peak_queue_, tasks_.size());
        }
        condition_.notify_all();
        return result;
    }

    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            shutdown_ = true;
            std::queue<std::function<void()>> empty;
            tasks_.swap(empty);
        }
        condition_.notify_all();
        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        workers_.clear();
    }

    size_t peak_queue() const {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        return peak_queue_;
    }

private:
    void worker_thread() {
        while (true) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(queue_mutex_);
                condition_.wait(lock, [this] { return shutdown_ || !tasks_.empty(); });
                if (shutdown_) {
                    return;
                }
                task = std::move(tasks_.front());
                tasks_.pop();
            }
            condition_.notify_all();
            task();
        }
    }

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    size_t capacity_;
    size_t peak_queue_ = 0;
    mutable std::mutex queue_mutex_;
    std::condition_variable condition_;
    bool shutdown_ = false;
};

} // namespace backup
