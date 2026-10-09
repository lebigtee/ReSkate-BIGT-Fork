#include "boneyard.h"
#include <atomic>
#include <mutex>

// The snapshot the overlay reads. Kept apart from boneyard.cpp so the overlay library links
// without the game-facing half.
namespace dingosdk::boneyard {
namespace {
struct Feed {
    std::mutex mutex;
    View view;
    std::atomic<LiveCamera> camera{};
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
void set_live_camera(LiveCamera reader) noexcept { feed().camera.store(reader, std::memory_order_release); }
bool live_camera(GameView &view) noexcept {
    const auto reader = feed().camera.load(std::memory_order_acquire);
    return reader && reader(view);
}
bool live_view(View &view) noexcept {
    auto camera = view.camera;
    if (!live_camera(camera)) return false;
    if (camera.world == view.camera.world) view.points = view.previous_points;
    view.camera = camera;
    return true;
}
} // namespace dingosdk::boneyard
