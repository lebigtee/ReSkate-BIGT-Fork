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
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << json.dump(2);
    } catch (...) {
        logging::write(logging::Level::warning, logging::Channel::graphics, "Camera light: could not save its settings.");
    }
}
std::optional<float> number(const std::vector<std::string> &arguments) {
    if (arguments.empty()) return std::nullopt;
    float value{};
    const auto &text = arguments[0];
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) return std::nullopt;
    return value;
}
std::string describe() {
    return std::format("Camera light {} (gain {:.2f}).", state::on ? "on" : "off", state::gain.load());
}
} // namespace

bool enabled() noexcept {
    load();
    return state::on.load();
}

std::string command(std::string_view verb, const std::vector<std::string> &arguments) {
    load();
    if (verb == "on" || verb == "off") {
        state::on = verb == "on";
        save();
        return describe();
    }
    if (verb == "gain") {
        const auto value = number(arguments);
        if (!value || *value < least_gain || *value > most_gain) return std::format("error: a gain of {} to {}.", least_gain, most_gain);
        state::gain = *value;
        save();
        return describe();
    }
    if (verb == "status") return describe();
    return "error: unknown command.";
}
} // namespace dingosdk::camera_light
