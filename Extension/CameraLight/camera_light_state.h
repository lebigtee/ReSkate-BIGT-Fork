#pragma once
#include <atomic>

// The camera light's settings: set by its commands (camera_light.cpp), read by the client tick that
// keeps the light (camera_light_native.cpp). Any thread.
namespace dingosdk::camera_light::state {
inline std::atomic<bool> on{false};
// How bright the light is, times its usual brightness.
inline constexpr float default_gain = 1.0f;
inline std::atomic<float> gain{default_gain};
} // namespace dingosdk::camera_light::state
