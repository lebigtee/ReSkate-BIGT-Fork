#include "boneyard_picture.h"
#include "boneyard_hud.h"
#include "boneyard_replay.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Profiling/profiler.h"
#include <wrl/client.h>
#include <chrono>
#include <cstdint>
#include <string>
#include <utility>

namespace dingosdk::overlay {
namespace {
using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;

// The jerky bail's copy of a picture: which one it holds, and when it was last used.
struct Held {
    ComPtr<ID3D12Resource> picture;
    std::uint64_t id{};
    Clock::time_point used{};
};
Held &held() {
    static auto *value = new Held;
    return *value;
}
// The slow motion replay, and when it was last used.
PictureReplay &picture_replay() {
    static auto *value = new PictureReplay;
    return *value;
}
Clock::time_point picture_replay_used{};

// Keeps a copy of this frame's picture, or puts the kept copy back in its place. Two copies of
// the screen on the GPU at most; the game itself runs on at full speed.
void hold_picture(const BoneyardFrame &frame, const FrameHold &hold) {
    DINGO_PROFILE_ZONE("overlay/boneyard picture");
    using Action = FrameHold::Action;
    auto &h = held();
    const auto now = Clock::now();
    if (hold.action == Action::none) {
        // Unused for a while: let the copy go, once the GPU is done with it.
        if (h.picture && now - h.used > std::chrono::seconds(3) && frame.gpu_idle) h = {};
        return;
    }
    const auto desc = frame.buffer->GetDesc();
    if (h.picture) {
        // A resize recreates the overlay's graphics, and this copy, first.
        const auto kept = h.picture->GetDesc();
        if (kept.Width != desc.Width || kept.Height != desc.Height || kept.Format != desc.Format) return;
    } else {
        if (hold.action != Action::keep) return;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        auto picture = desc;
        picture.Flags = D3D12_RESOURCE_FLAG_NONE;
        const auto started = Clock::now();
        const auto created = frame.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &picture, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                                   IID_PPV_ARGS(&h.picture));
        const auto took = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
        logging::printf(logging::Level::info, logging::Channel::graphics, "Boneyard: picture copy %llux%u %s in %.2f ms.",
                        static_cast<unsigned long long>(desc.Width), desc.Height, SUCCEEDED(created) ? "created" : "unavailable", took);
        if (FAILED(created)) {
            h.picture.Reset();
            return;
        }
    }
    const bool keep = hold.action == Action::keep;
    if (!keep && h.id != hold.picture) return; // that one was never kept: the live picture stays
    D3D12_RESOURCE_BARRIER barriers[2]{};
    for (auto &barrier : barriers) {
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    barriers[0].Transition.pResource = frame.buffer;
    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barriers[0].Transition.StateAfter = keep ? D3D12_RESOURCE_STATE_COPY_SOURCE : D3D12_RESOURCE_STATE_COPY_DEST;
    // The copy rests in COPY_DEST, ready to keep the next picture.
    barriers[1].Transition.pResource = h.picture.Get();
    barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    const UINT count = keep ? 1 : 2;
    frame.commands->ResourceBarrier(count, barriers);
    if (keep) frame.commands->CopyResource(h.picture.Get(), frame.buffer);
    else frame.commands->CopyResource(frame.buffer, h.picture.Get());
    for (auto &barrier : barriers) std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    frame.commands->ResourceBarrier(count, barriers);
    if (keep) h.id = hold.picture;
    h.used = now;
}

// Keeps this frame's picture and draws a kept one over it, as the HUD planned. The replay's
// pipeline is made while a bail would be replayed (a multiplayer session), before one comes; the
// kept pictures are let go 10 s after the last replay, and everything once no bail would be
// replayed.
void replay_picture(const BoneyardFrame &frame, const FrameHold::Replay &plan) {
    DINGO_PROFILE_ZONE("overlay/boneyard replay");
    auto &replay = picture_replay();
    const auto now = Clock::now();
    const bool busy = plan.keep_slot >= 0 || plan.show_slot >= 0;
    if (busy) picture_replay_used = now;
    if (!busy && replay.prepared() && now - picture_replay_used > std::chrono::seconds(10) && frame.gpu_idle) {
        if (replay.bytes()) {
            logging::printf(logging::Level::info, logging::Channel::graphics, "Boneyard: replay let go of %.1f MB of pictures.",
                            replay.bytes() / 1048576.0);
            replay.release_pictures();
        }
        if (!plan.ready) replay.release();
        picture_replay_used = now;
    }
    if (!busy && !plan.ready) return;
    std::string error;
    static bool warned = false;
    if (!replay.prepared()) {
        const auto started = Clock::now();
        const bool ok = replay.prepare(frame.device, frame.buffer->GetDesc(), error);
        const auto took = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
        if (ok) logging::printf(logging::Level::info, logging::Channel::graphics, "Boneyard: slow motion replay ready in %.2f ms.", took);
        else if (!warned) {
            warned = true;
            logging::printf(logging::Level::warning, logging::Channel::graphics, "Boneyard: no slow motion replay: %s.", error.c_str());
        }
        if (!ok) return;
    }
    if (plan.keep_slot >= 0 && !replay.keep(frame.commands, frame.buffer, static_cast<std::size_t>(plan.keep_slot), plan.keep_id, error) &&
        !warned) {
        warned = true;
        logging::printf(logging::Level::warning, logging::Channel::graphics, "Boneyard: replay picture not kept: %s.", error.c_str());
    }
    if (plan.show_slot >= 0)
        (void)replay.show(frame.commands, frame.buffer, frame.target, static_cast<std::size_t>(plan.show_slot), plan.show_id, plan.opacity);
}
} // namespace

void boneyard_picture(const BoneyardFrame &frame) {
    const auto hold = boneyard_frame_hold();
    hold_picture(frame, hold);
    replay_picture(frame, hold.replay);
}

void release_boneyard_pictures() {
    held() = {};
    picture_replay().release();
}
} // namespace dingosdk::overlay
