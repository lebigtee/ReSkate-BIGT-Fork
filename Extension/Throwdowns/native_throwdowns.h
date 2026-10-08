#pragma once
#include <cstdint>

namespace dingosdk::multiplayer {
// Offline stand-ins for the answers throwdowns normally get from the backend
// (throwdowns_v1 mode catalogue) and the online roster (host player lookup).
void initialize_native_throwdowns(std::uintptr_t base) noexcept;
// Game thread, every client tick: adds the throwdown text retail never shipped (the
// S.K.A.T.E. title showed as ID_ACTIVITY_SKATE_TITLE, Spot Battle's details description as
// ID_ACTIVITY_SPOTBATTLE_DESC) to the game's own string-override store, the one the
// backend's client_strings_v1 fills, once the string database is loaded. Rechecked every
// 2 s, since a language change or a backend refresh clears that store.
void apply_throwdown_strings(std::uintptr_t base) noexcept;
// Game thread, every client tick: while `death_race` (a Death Race carried by a Skate Jam is
// being opened or run, Extension/DeathRace), the Skate Jam's title and description read as the
// Death Race's, through the same store; the game's own text is put back after.
void dress_death_race_strings(std::uintptr_t base, bool death_race) noexcept;
// For the next `milliseconds`, no leave confirmation reaches the local player: a Death Race ends
// its throwdown by having the local player leave it, and shows its own result instead of the
// game's "you left" message.
void quiet_leave_confirmations(unsigned milliseconds) noexcept;

// True while a client ThrowdownRegistration graph is asking IsOnline (0) or
// IsSessionReady (1) in hosted offline play. Every other graph stays native.
bool native_throwdown_ready(unsigned predicate) noexcept;
// Call after an expression returns: delivers a parameter request that the
// absent throwdowns_v1 service left pending for that expression.
void complete_native_throwdown_parameters(std::uintptr_t vm) noexcept;
// Native player id of the hosted offline server's only real player, or zero.
std::uint32_t local_native_player_id() noexcept;
// True from the moment the local player creates or joins a throwdown until it
// ends, is left or destroyed. Retail mirrors this onto UIPlayerInfo, where the
// throwdown Quit action reads it; offline the SDK owns that record.
bool local_throwdown_active() noexcept;
// True while that throwdown is the local player's own (it created it).
bool local_throwdown_host() noexcept;
}
