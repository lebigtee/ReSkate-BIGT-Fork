#pragma once
#include <cstdint>

namespace dingosdk {
bool start_no_bail(std::uintptr_t image_base) noexcept;
bool no_bail_available() noexcept;
// The teleport to the skater's session marker (the d-pad's way back to it; a throwdown's start):
// refused while blocked. Publish from the client tick; the block lapses half a second after the
// last. How many were refused since the process started.
bool start_session_marker_guard(std::uintptr_t image_base) noexcept;
void block_session_marker_teleports(bool blocked) noexcept;
std::uint64_t refused_session_marker_teleports() noexcept;
// Publish from the validated local client tick. Returns owner availability even
// when both controls are off. Manual protection expires if ticks stop arriving.
bool update_no_bail(std::uintptr_t client, std::uintptr_t entity, bool manual,
    bool flying, std::uint64_t flight_expires) noexcept;
void clear_no_bail() noexcept;
// S.K.A.T.E.: while `locked`, the local skater cannot get back on the board once it is off
// (its mount request is dropped). Publish from the client tick; it expires if ticks stop.
// Releasing it also leaves the skater's teleport option on the board again, since a turn's
// teleport may have set it off (skater component +0xc0) and the SDK's own teleports keep it.
void update_board_lock(std::uintptr_t client, std::uintptr_t entity, bool locked) noexcept;
// The local skater's next teleport puts it on its board (a throwdown's own teleport may have
// left that option off). False while the skater cannot be resolved (mid-teleport).
bool keep_teleports_on_board(std::uintptr_t client, std::uintptr_t entity) noexcept;
// Knocks the local skater off its board as a collision would (the native impact request and its
// cause), on its next physics step while it rides, unless No Bail protects it. False when the
// skater cannot be resolved.
bool knock_local_skater(std::uintptr_t client, std::uintptr_t entity) noexcept;
// The physics state the local skater's selector last chose, for the trainer (air time, bail
// markers). Publish the skater to watch from the client tick; it expires if ticks stop.
struct PhysicsStateWatch {
    bool valid{};
    std::uint32_t state{};
    std::uint32_t previous{}; // the state before this one
    float previous_seconds{}; // how long that one lasted
    std::uint64_t changes{}, wipeouts{}; // counted since the process started
};
void watch_physics_state(std::uintptr_t client, std::uintptr_t entity) noexcept;
PhysicsStateWatch watched_physics_state() noexcept;
// Stopping flight must not discard the independent manual preference.
void clear_no_bail_flight() noexcept;
}
