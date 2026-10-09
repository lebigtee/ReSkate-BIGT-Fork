#pragma once
// The camera light: as if a lamp on the camera lit the skater at night, the way a filmer's does. A
// light of the game's own: one spot light of the game's character light rig, created through the
// engine and placed on the camera every client tick (camera_light_native.cpp). Settings kept in
// %LOCALAPPDATA%\ReSkate\cameralight.json.
#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace dingosdk::camera_light {
// Runs one `light` command (on, off, status, gain <0.25-4>, color <red> <green> <blue>, 0-255 each)
// and returns the line to print. Game thread (the console dispatcher).
std::string command(std::string_view verb, const std::vector<std::string> &arguments);
// Whether it is on, how bright (times its usual brightness) and its colour (red, green, blue,
// 0-255), for the pause menu. Any thread.
bool enabled() noexcept;
float gain() noexcept;
std::array<int, 3> color() noexcept;
} // namespace dingosdk::camera_light
