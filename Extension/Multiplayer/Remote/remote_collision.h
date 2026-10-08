#pragma once
#include "Extension/Multiplayer/Net/protocol.h"
#include <array>
#include <cstdint>
#include <string>

// Other players as solid bodies for the local skater, as the game's party collision is between
// party members (analysis/party-collision.md). Retail builds each remote skater a keyframed
// capsule in the RemoteCharacterProxy category, and the local skater's bodies collide with
// that category while "Enable Party Collision" is on. A puppet has no physics of its own, so
// this gives each nearby player the same capsule, plus a thin one along their skateboard,
// placed from the pose shown for them.
namespace dingosdk::multiplayer {
// The local skater has the game's party collision on: "Enable Party Collision" together with
// DingoAdvanceSettings.PlayerVsPlayerCollisionEnabled (the SDK's playercollision feature).
bool local_allows_player_collision(std::uintptr_t base, std::uintptr_t local_entity) noexcept;
// Client thread, for the current PeerScope slot, each frame the player's pose is shown:
// places the slot's capsules at `pose`, or switches them off when `solid` is false. `context`
// is the local skater's game context; a new one (a level load) means a new physics world.
// `feet`: where the skater's capsule stands, when not at the pose's root (the retail proxy's
// place); `radius`: its radius, when not the retail proxy's (both kept as the slot's capsule was
// first made with them).
void update_remote_collision(std::uintptr_t base, std::uintptr_t context, const Pose &pose, bool solid,
                             std::uint64_t now_us, const std::array<float, 3> *feet = nullptr, float radius = 0.0f) noexcept;
// Client thread, for the current PeerScope slot: the player's skater is gone. Its capsules stop
// colliding and wait, still allocated, for the slot's next player.
void clear_remote_collision(std::uintptr_t base) noexcept;
// Client thread, every tick: capsules whose player was not shown for a moment (loading,
// travelling, the session paused its rendering) stop colliding.
void expire_remote_collision(std::uintptr_t base, std::uint64_t now_us) noexcept;
// Local switch for the whole feature (on by default); off makes every capsule inert.
void set_remote_collision_enabled(bool enabled) noexcept;
bool remote_collision_enabled() noexcept;
// One console line (any thread): availability, the local switch, the local party-collision
// setting as last read and how many players are solid.
std::string remote_collision_status();
} // namespace dingosdk::multiplayer
