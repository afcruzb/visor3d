#include "async_loader.h"

#include <thread>
#include <utility>

namespace v3d {

AsyncLoader::AsyncLoader(LoadOptions options, std::function<void()> wake)
    : options_(options), wake_(std::move(wake)) {}

void AsyncLoader::request(const std::string& path) {
    if (busy())
        queued_ = path;
    else
        start(path);
}

void AsyncLoader::start(const std::string& path) {
    loading_ = path;
    std::promise<Result> promise;
    pending_ = promise.get_future();
    // The worker owns copies of everything it touches: the loader may go away first.
    std::thread([path, options = options_, wake = wake_, promise = std::move(promise)]() mutable {
        Scene scene = loadScene(path, options);
        promise.set_value(Result{path, std::move(scene), std::chrono::steady_clock::now()});
        if (wake) wake();
    }).detach();
}

std::optional<AsyncLoader::Result> AsyncLoader::poll(std::chrono::milliseconds wait) {
    if (!busy() || pending_.wait_for(wait) != std::future_status::ready) return std::nullopt;
    Result result = pending_.get();
    if (!queued_.empty()) {
        start(std::exchange(queued_, {}));
        return std::nullopt;
    }
    loading_.clear();
    return result;
}

}  // namespace v3d
