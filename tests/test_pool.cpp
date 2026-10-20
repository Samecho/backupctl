#include "backup/thread_pool.h"

#include <gtest/gtest.h>
#include <atomic>

using namespace backup;

TEST(ThreadPool, BoundedQueueAndExceptionPropagation) {
    ThreadPool pool(3, 2);
    std::atomic<int> completed{0};
    std::vector<std::future<int>> tasks;
    for (int i = 0; i < 200; ++i) {
        tasks.push_back(pool.enqueue([&completed, i] {
            ++completed;
            return i * i;
        }));
    }
    for (size_t i = 0; i < tasks.size(); ++i) {
        EXPECT_EQ(tasks[i].get(), static_cast<int>(i * i));
    }
    EXPECT_EQ(completed.load(), 200);
    EXPECT_LE(pool.peak_queue(), 2);
    auto failure = pool.enqueue([]() -> int { throw Error("io", "injected worker failure"); });
    EXPECT_THROW(failure.get(), Error);
    pool.shutdown();
    EXPECT_THROW(pool.enqueue([] {}), Error);
}
