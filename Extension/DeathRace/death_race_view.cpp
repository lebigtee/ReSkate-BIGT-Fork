#include "death_race.h"
#include <atomic>
#include <mutex>

// The snapshot the overlay reads. Kept apart from death_race.cpp so the overlay library links
// without the game-facing half.
namespace dingosdk::death_race {
namespace {
struct Feed {
    std::mutex mutex;
    View view;
    std::atomic<bool> racing{};
};
Feed &feed() {
    static auto *value = new Feed;
    return *value;
}
} // namespace
View view() noexcept {
    auto &f = feed();
    std::lock_guard lock(f.mutex);
    return f.view;
}
void publish(const View &next) noexcept {
    auto &f = feed();
    std::lock_guard lock(f.mutex);
    f.view = next;
}
bool racing() noexcept { return feed().racing.load(std::memory_order_acquire); }
void set_racing(bool value) noexcept { feed().racing.store(value, std::memory_order_release); }
} // namespace dingosdk::death_race
