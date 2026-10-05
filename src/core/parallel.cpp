#include "parallel.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace v3d {

namespace {

struct Group {
    size_t pending;  // tasks of one parallelFor call not finished yet
};

struct Task {
    const std::function<void(size_t)>* fn;
    size_t index;
    Group* group;
};

// Work-sharing pool: one queue, idle workers take from the front, threads
// waiting for their own group take from the back (their most recent tasks).
class Pool {
public:
    Pool() {
        // Detached and never stopped: the pool lives as long as the process.
        for (unsigned i = 1; i < hardwareThreads(); ++i) std::thread([this] { work(); }).detach();
    }

    void run(size_t n, const std::function<void(size_t)>& fn) {
        Group group{n};
        std::unique_lock lock(mutex_);
        for (size_t i = 0; i < n; ++i) queue_.push_back({&fn, i, &group});
        wake_.notify_all();
        while (group.pending) {
            if (queue_.empty()) {
                wake_.wait(lock);
                continue;
            }
            Task t = queue_.back();
            queue_.pop_back();
            execute(lock, t);
        }
    }

private:
    void work() {
        std::unique_lock lock(mutex_);
        while (true) {
            wake_.wait(lock, [&] { return !queue_.empty(); });
            Task t = queue_.front();
            queue_.pop_front();
            execute(lock, t);
        }
    }

    void execute(std::unique_lock<std::mutex>& lock, const Task& t) {
        lock.unlock();
        (*t.fn)(t.index);
        lock.lock();
        if (--t.group->pending == 0) wake_.notify_all();
    }

    std::mutex mutex_;
    std::condition_variable wake_;  // queue got tasks, or a group finished
    std::deque<Task> queue_;
};

Pool& pool() {
    static Pool* p = new Pool;  // leaked on purpose: its workers never exit
    return *p;
}

}  // namespace

unsigned hardwareThreads() {
    static const unsigned n = std::max(1u, std::thread::hardware_concurrency());
    return n;
}

void parallelFor(size_t n, const std::function<void(size_t)>& fn) {
    if (n <= 1 || hardwareThreads() == 1) {
        for (size_t i = 0; i < n; ++i) fn(i);
        return;
    }
    pool().run(n, fn);
}

void startParallelPool() {
    if (hardwareThreads() > 1) pool();
}

void parallelInvoke(std::initializer_list<std::function<void()>> jobs) {
    const std::function<void()>* job = jobs.begin();
    parallelFor(jobs.size(), [job](size_t i) { job[i](); });
}

}  // namespace v3d
