#include "camera_light.h"
#include "camera_light_state.h"
#include "Engine/Core/Json/json.h"
#include "Engine/Core/Log/logging.h"
#include <windows.h>
#include <shlobj.h>
#include <algorithm>
#include <charconv>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <optional>

namespace dingosdk::camera_light {
namespace {
constexpr float least_gain = 0.25f, most_gain = 4.0f;
// The settings' version: the gain meant something else before the light was one of the game's.
constexpr int settings_version = 3;

std::filesystem::path settings_path() {
    PWSTR raw{};
    std::filesystem::path result;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw)) && raw)
        result = std::filesystem::path(raw) / L"ReSkate" / L"cameralight.json";
    if (raw) CoTaskMemFree(raw);
    return result;
}
std::uint32_t packed(int red, int green, int blue) {
    return static_cast<std::uint32_t>(red) << 16 | static_cast<std::uint32_t>(green) << 8 | static_cast<std::uint32_t>(blue);
}
void load() {
    static std::once_flag once;
    std::call_once(once, [] {
        try {
            std::ifstream file(settings_path(), std::ios::binary);
            if (!file) return;
            const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            if (text.empty() || text.size() > 4096) return;
            const auto json = Json::parse(text);
            if (!json.is_object()) return;
            state::on = json.value("on", false);
            if (json.value("version", 0) == settings_version)
                state::gain = std::clamp(json.value("gain", state::default_gain), least_gain, most_gain);
            // Added after version 3, without changing what the rest means: white when it is missing.
            if (json.contains("color") && json.at("color").is_array() && json.at("color").size() == 3) {
                const auto &color = json.at("color");
                std::array<int, 3> rgb{};
                for (std::size_t i = 0; i < 3; ++i) rgb[i] = std::clamp(color[i].is_number() ? color[i].get<int>() : 255, 0, 255);
                state::color = packed(rgb[0], rgb[1], rgb[2]);
            }
        } catch (...) {}
    });
}
void save() {
    try {
        const auto path = settings_path();
        if (path.empty()) return;
        auto json = Json::object();
        json["version"] = settings_version;
        json["on"] = state::on.load();
        json["gain"] = state::gain.load();
        const auto rgb = color();
        json["color"] = Json::array({rgb[0], rgb[1], rgb[2]});
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << json.dump(2);
    } catch (...) {
        logging::write(logging::Level::warning, logging::Channel::graphics, "Camera light: could not save its settings.");
    }
}
template <class T> std::optional<T> number(const std::string &text) {
    T value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) return std::nullopt;
    return value;
}
std::string describe() {
    const auto rgb = color();
    return std::format("Camera light {} (gain {:.2f}, color {} {} {}).", state::on ? "on" : "off", state::gain.load(), rgb[0], rgb[1], rgb[2]);
}
} // namespace

bool enabled() noexcept {
    load();
    return state::on.load();
}
float gain() noexcept {
    load();
    return state::gain.load();
}
std::array<int, 3> color() noexcept {
    load();
    const auto value = state::color.load();
    return {static_cast<int>(value >> 16 & 0xff), static_cast<int>(value >> 8 & 0xff), static_cast<int>(value & 0xff)};
}

std::string command(std::string_view verb, const std::vector<std::string> &arguments) {
    load();
    if (verb == "on" || verb == "off") {
        state::on = verb == "on";
        save();
        return describe();
    }
    if (verb == "gain") {
        const auto value = arguments.empty() ? std::nullopt : number<float>(arguments[0]);
        if (!value || *value < least_gain || *value > most_gain) return std::format("error: a gain of {} to {}.", least_gain, most_gain);
        state::gain = *value;
        save();
        return describe();
    }
    if (verb == "color") {
        std::array<int, 3> rgb{};
        bool valid = arguments.size() == 3;
        for (std::size_t i = 0; valid && i < 3; ++i) {
            const auto value = number<int>(arguments[i]);
            valid = value && *value >= 0 && *value <= 255;
            if (valid) rgb[i] = *value;
        }
        if (!valid) return "error: a color of three numbers from 0 to 255 (red, green, blue).";
        state::color = packed(rgb[0], rgb[1], rgb[2]);
        save();
        return describe();
    }
    if (verb == "status") return describe();
    return "error: unknown command.";
}
} // namespace dingosdk::camera_light
