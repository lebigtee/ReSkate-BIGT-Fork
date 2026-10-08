#include "runtime_internal.h"
#include "Extension/Objects/local_placements_runtime.h"
#include "Extension/Objects/ParkEditor/park_editor_runtime.h"
#include "Extension/Objects/ParkEditor/park_editor_surface.h"
#include "Extension/Customization/local_customization_runtime.h"
#include "Extension/World/local_world_layers.h"

namespace dingosdk {
using namespace profile_runtime;
ObjectPersistenceModel local_profile_object_persistence() {
    std::lock_guard lock(local_runtime().native_mutex);
    auto& r = placements_runtime();
    ObjectPersistenceModel model;
    model.available = local_runtime().active.load() && r.store && !r.failed && !r.save_failed;
    model.enabled = r.enabled; model.map = r.map; model.clearing = r.clearing;
    if (const auto it = r.document.maps.find(r.map); it != r.document.maps.end()) model.count = it->second.size();
    model.busy = r.clearing || r.inflight.has_value();
    for (const auto& row : r.rows) model.rows.push_back({row.token, row.object.item, row.object.position, row.saved_id != 0, row.spawned});
    model.can_clear = model.available && world_layers_runtime().model.ready && !r.map.empty() &&
        !model.busy && model.count != 0;
    model.status = r.status;
    if (r.save_failed) model.status = "Object save failed. The previous file is retained; restart after checking objects.sqlite3.";
    if (!r.store || r.failed) model.status = "Object persistence is unavailable. Check objects.sqlite3 and the runtime log.";
    return model;
}
bool set_local_object_persistence(bool enabled) {
    std::lock_guard lock(local_runtime().native_mutex);
    auto& r = placements_runtime();
    if (!local_runtime().active || !r.store || r.failed || r.save_failed || park_editor_owns_placements()) return false;
    try {
        local_runtime().store->set_bool_option("ReSkate.ObjectPersistence", enabled);
        r.enabled = enabled;
        r.status = enabled ? "Object persistence enabled." : "Object persistence disabled. Saved layouts are retained.";
        return true;
    } catch (...) { r.status = "Could not save the object persistence option."; return false; }
}
bool clear_local_persisted_objects(std::string_view map) {
    std::lock_guard lock(local_runtime().native_mutex);
    auto& r = placements_runtime();
    if (!local_runtime().active || !placement_session_ready() || r.map != map) return false;
    std::set<std::uint64_t> tokens;
    for (const auto& row : r.rows) if (row.saved_id) tokens.insert(row.token);
    return begin_placement_delete(tokens);
}
bool delete_local_placed_object(std::string_view map, std::uint64_t token) {
    std::lock_guard lock(local_runtime().native_mutex);
    auto& r = placements_runtime();
    if (!local_runtime().active || !placement_session_ready() || r.map != map ||
        std::none_of(r.rows.begin(), r.rows.end(), [&](const auto& row) { return row.token == token; })) return false;
    return begin_placement_delete({token});
}
bool teleport_local_skater(const std::array<float, 3>& position) {
    std::lock_guard lock(local_runtime().native_mutex);
    auto& r = placements_runtime();
    if (!local_runtime().active || !r.teleport || !r.transition_ctor || !r.transition_destroy) return false;
    for (const auto v : position)
        if (!std::isfinite(v) || std::abs(v) > 1e6f) return false;
    r.position_teleport = position;
    r.position_teleport_heading.reset();
    r.position_teleport_at = GetTickCount64();
    return true;
}
bool teleport_local_skater_facing(const std::array<float, 3>& position, float heading) {
    if (!std::isfinite(heading) || !teleport_local_skater(position)) return false;
    std::lock_guard lock(local_runtime().native_mutex);
    placements_runtime().position_teleport_heading = heading;
    return true;
}
WorldRayHit cast_world_ray(const std::array<float, 3>& from, const std::array<float, 3>& to) {
    std::lock_guard lock(local_runtime().native_mutex);
    auto& r = placements_runtime();
    const auto& world = world_layers_runtime();
    // The current client context is the calling thread's: only the update thread's is the client's.
    if (!local_runtime().active || !r.context || cosmetic_runtime().update_thread != GetCurrentThreadId() ||
        !world.model.ready || !world.context) return {};
    for (int a = 0; a < 3; ++a)
        if (!std::isfinite(from[a]) || !std::isfinite(to[a]) || std::abs(from[a]) > 100000 || std::abs(to[a]) > 100000)
            return {};
    static const auto api = editor::surface_api(local_runtime().base);
    if (!api.ready) return {};
    std::array<std::uintptr_t, 2> context{};
    r.context(context.data());
    if (!context[0]) return {};
    const auto physics = api.world(context[0]);
    if (!physics) return {};
    std::vector<editor::SurfaceBody> discovered;
    const auto hit = editor::cast_surface_once(api, physics, from, to, nullptr, {}, nullptr, {}, discovered, nullptr);
    WorldRayHit result{true, hit.has_value()};
    if (hit) result.at = *hit;
    return result;
}
bool teleport_to_local_placed_object(std::string_view map, std::uint64_t token) {
    std::lock_guard lock(local_runtime().native_mutex);
    auto& r = placements_runtime();
    if (!local_runtime().active || !placement_session_ready() || r.map != map || r.clearing ||
        std::none_of(r.rows.begin(), r.rows.end(), [&](const auto& row) { return row.token == token; })) return false;
    r.teleport_token = token; r.teleport_queued_at = GetTickCount64();
    r.status = "Teleport queued...";
    return true;
}
}
