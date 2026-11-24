#include "audio/thread.hpp"

#include <memory>
#include <mutex>
#include <utility>

namespace data2g::audio {
namespace {

std::mutex mu;
std::shared_ptr<const ThreadInit> hook;

}  // namespace

void set_thread_init(ThreadInit f) {
    std::lock_guard lock(mu);
    hook = f ? std::make_shared<const ThreadInit>(std::move(f)) : nullptr;
}

void thread_init(const char* role) {
    std::shared_ptr<const ThreadInit> h;
    {
        std::lock_guard lock(mu);
        h = hook;
    }
    if (h) (*h)(role);
}

}  // namespace data2g::audio
