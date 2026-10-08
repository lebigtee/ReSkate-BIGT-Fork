#pragma once
// The camera light's lamp in the game (camera_light_native.cpp): a spot light of the game's own,
// created while the light is on and placed on the camera every client tick.
#include <cstdint>

namespace dingosdk::camera_light {
// Client thread, after the native client tick; `ready` while a local skater can exist.
void tick(std::uintptr_t base, std::uintptr_t client, bool ready) noexcept;
} // namespace dingosdk::camera_light
