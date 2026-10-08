#pragma once
#include <cstdint>

// The DEATH RACE tile in the radial menu's Throwdowns page (the throwdown mode select), beside
// S.K.A.T.E., Spot Battle and Skate Jam: a copy of the Spot Battle tile with its own title,
// description, icon and action (Extension/DeathRace decides what it says and does).
namespace dingosdk::multiplayer {
// The mode select is filling its tiles (native_throwdowns.cpp's mode check; any thread): the
// next ticks look for its list.
void throwdown_mode_select_opened() noexcept;
// Client tick (game thread), outside a level transition.
void tick_throwdown_tile(std::uintptr_t base) noexcept;
// Before a level transition (game thread): takes the tile out and releases its models while the
// widget assets they reference are still loaded.
void release_throwdown_tile(std::uintptr_t base) noexcept;
} // namespace dingosdk::multiplayer
