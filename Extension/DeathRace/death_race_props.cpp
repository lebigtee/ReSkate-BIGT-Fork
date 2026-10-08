#include "death_race_internal.h"
#include "Engine/Core/Json/json.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Game/Abi/native_data.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Extension/Multiplayer/Hud/game_ui_state.h"
#include "Extension/Multiplayer/Remote/native_skater.h"
#include "Extension/Objects/network_object_runtime.h"
#include "Extension/Profile/local_profile_runtime.h"
#include "Extension/Settings/named_settings.h"
#include "Extension/Skater/no_bail.h"
#include "Extension/Throwdowns/native_throwdowns.h"
#include "Extension/Throwdowns/throwdown_lab.h"
#include "Extension/Throwdowns/throwdown_relay.h"
#include "Extension/Trainer/trainer.h"
#include <windows.h>
#include <shlobj.h>
#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <numbers>
#include <optional>
#include <random>
#include <tuple>

namespace dingosdk::death_race::detail {
namespace {
NetworkObject prop(std::uint64_t id, const char *item, const Vec3 &at, float heading) {
    NetworkObject o;
    o.id = id;
    o.item = item;
    o.position = at;
    const float half = heading * std::numbers::pi_v<float> / 360.0f;
    o.rotation = {0.0f, std::sin(half), 0.0f, std::cos(half)}; // a turn about +Y
    return o;
}
// What stands: the start line facing the first flag, a pair of slalom flags across each
// checkpoint, the finish arch facing the way in. Ids stay with their flag as a race grows.
std::vector<NetworkObject> props_for(const State &s) {
    std::vector<NetworkObject> out;
    std::vector<Vec3> points;
    bool finished{};
    float start_heading = s.draft_heading;
    if (s.building) {
        points = s.draft;
    } else {
        const int index = (s.phase != Phase::idle && s.active >= 0) ? s.active : s.selected;
        const auto it = s.maps.find(s.level);
        if (it == s.maps.end() || index < 0 || index >= static_cast<int>(it->second.size())) return out;
        const auto &race = it->second[index];
        points.push_back(race.start);
        points.insert(points.end(), race.checkpoints.begin(), race.checkpoints.end());
        points.push_back(race.finish);
        finished = true;
        start_heading = race.has_heading ? race.heading : facing(race.start, points[1], 0.0f);
    }
    if (points.empty()) return out;
    // The start line faces the way the skater faced setting it.
    out.push_back(prop(1, start_prop, grounded(s, points[0]), start_heading));
    const std::size_t gates = finished ? points.size() - 1 : points.size();
    for (std::size_t i = 1; i < gates; ++i) {
        const float h = facing(points[i - 1], points[i], 0.0f);
        const float r = h * std::numbers::pi_v<float> / 180.0f;
        // Across the way in: the right of a rider heading `h` is (cos h, -sin h) in x, z.
        const Vec3 side{std::cos(r) * gate_half_width, 0.0f, -std::sin(r) * gate_half_width};
        const auto &p = points[i];
        out.push_back(prop(10 + i * 2, checkpoint_prop, grounded(s, {p[0] + side[0], p[1], p[2] + side[2]}), h));
        out.push_back(prop(11 + i * 2, checkpoint_prop, grounded(s, {p[0] - side[0], p[1], p[2] - side[2]}), h));
    }
    if (finished)
        out.push_back(prop(2, finish_prop, grounded(s, points.back()), facing(points[points.size() - 2], points.back(), 0.0f)));
    return out;
}
} // namespace

// ---- the props ---------------------------------------------------------------------------------
// Facing from `from` to `to` (degrees, the trainer's heading), or `otherwise` when they meet.
float facing(const Vec3 &from, const Vec3 &to, float otherwise) {
    const float dx = to[0] - from[0], dz = to[2] - from[2];
    if (std::hypot(dx, dz) < 0.5f) return otherwise;
    return std::atan2(dx, dz) * 180.0f / std::numbers::pi_v<float>;
}
// A flag's place on the ground: flags are set where the skater stood (on its board, above the
// ground), so the game's rays find the ground under it, once per place (before its prop stands
// there to be found instead): the middle of what a few rays round it strike, leaving out the
// skater's own body.
Vec3 grounded(const State &s, const Vec3 &at) {
    static std::map<std::array<int, 3>, float> found; // by place, to the centimetre
    const std::array<int, 3> key{static_cast<int>(std::lround(at[0] * 100.0f)), static_cast<int>(std::lround(at[1] * 100.0f)),
                                 static_cast<int>(std::lround(at[2] * 100.0f))};
    if (const auto known = found.find(key); known != found.end()) return {at[0], known->second, at[2]};
    std::vector<float> heights;
    bool asked{};
    for (const auto &[dx, dz] : {std::pair{0.0f, 0.0f}, {0.8f, 0.0f}, {-0.8f, 0.0f}, {0.0f, 0.8f}, {0.0f, -0.8f}}) {
        const auto ray = cast_world_ray({at[0] + dx, at[1] + 1.0f, at[2] + dz}, {at[0] + dx, at[1] - 3.0f, at[2] + dz});
        asked = asked || ray.cast;
        if (!ray.hit) continue;
        const bool body = s.skater && std::hypot(ray.at[0] - s.position[0], ray.at[2] - s.position[2]) < 0.6f &&
                          ray.at[1] > s.position[1] - 0.3f;
        if (!body) heights.push_back(ray.at[1]);
    }
    if (!asked) return at; // no rays yet: as set, and asked again
    float height = at[1];
    if (!heights.empty()) {
        std::sort(heights.begin(), heights.end());
        height = heights[heights.size() / 2];
    }
    found.emplace(key, height);
    if (!heights.empty() && std::abs(height - at[1]) > 0.02f)
        say(std::format("Death Race: a flag set {:.2f} m {} the ground stands on it.", std::abs(at[1] - height),
                        at[1] > height ? "above" : "below"));
    return {at[0], height, at[2]};
}
void update_props(State &s, double now) {
    auto wanted = props_for(s);
    // Every second as well: a world change clears them, and the handover puts them back.
    if (wanted == s.props && now - s.props_at < 1.0) return;
    s.props_at = now;
    try {
        set_race_props(wanted);
        s.props_live = !wanted.empty() && race_props_live() > 0;
    } catch (...) {
        s.props_live = false;
    }
    s.props = std::move(wanted);
}

} // namespace dingosdk::death_race::detail
