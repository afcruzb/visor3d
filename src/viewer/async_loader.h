#pragma once

#include <chrono>
#include <functional>
#include <future>
#include <optional>
#include <string>

#include "../core/loader.h"

namespace v3d {

// Loads scenes on detached worker threads, so quitting never waits for a slow
// STEP import. Only the newest request matters: asking for a file while
// another one loads queues it (replacing any earlier queued file) and the
// superseded result is dropped.
class AsyncLoader {
public:
    struct Result {
        std::string path;
        Scene scene;
        std::chrono::steady_clock::time_point finished;
    };

    // `wake` runs on the worker thread when a result is ready, e.g. to post an
    // event that wakes the UI loop.
    AsyncLoader(LoadOptions options, std::function<void()> wake);

    void request(const std::string& path);
    bool busy() const { return pending_.valid(); }
    // The newest requested file while busy (the queued one, if any).
    const std::string& target() const { return queued_.empty() ? loading_ : queued_; }
    // The finished load, if any, waiting up to `wait` for it.
    std::optional<Result> poll(std::chrono::milliseconds wait = {});

private:
    void start(const std::string& path);

    LoadOptions options_;
    std::function<void()> wake_;
    std::future<Result> pending_;
    std::string loading_, queued_;
};

}  // namespace v3d
