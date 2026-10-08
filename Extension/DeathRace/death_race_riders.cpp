#include "death_race_riders_internal.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/no_bail.h"
#include "Engine/Game/Build/20260929/remote_collision.h"
#include "Extension/Multiplayer/Remote/native_cosmetics.h"
#include "Extension/Multiplayer/Remote/native_skater.h"
#include "Extension/Multiplayer/Remote/remote_collision.h"
#include "Extension/Multiplayer/Session/peer_slots.h"
#include "Extension/Customization/local_customization_runtime.h"
#include "Extension/Profile/local_profile_runtime.h"
#include "Extension/Profile/runtime_internal.h"
#include "Extension/Skater/client_source_spawn.h"
#include "Extension/Skater/no_bail.h"
#include <windows.h>
#include <shlobj.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <limits>
#include <optional>
#include <random>
#include <string>

// The riders themselves: shown as the game's own skaters, dressed, run into, falling and
// getting up, and placed every tick (see death_race_riders.h). They take the last remote-player
// slots, which a session's own players never reach, one native actor each
// (Extension/Multiplayer/Remote/native_skater_spawn.cpp).
namespace dingosdk::death_race::riding {
Riders &riders() {
    static auto *value = new Riders;
    return *value;
}
std::size_t slot_of(int rider) { return multiplayer::max_remote_players - 1 - static_cast<std::size_t>(rider); }
std::uint64_t micros() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
bool poke(std::uintptr_t address, std::uint8_t value) noexcept {
    if (address < 0x10000 || address > memory::highest_user_address) return false;
    __try {
        *reinterpret_cast<volatile std::uint8_t *>(address) = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// The riders' capsules meet the local skater only while its party collision is on (the game's
// "Enable Party Collision" and its player-collision setting): on for the race, then as it was.
void party_collision(std::uintptr_t base, std::uintptr_t entity, bool on) {
    auto &r = riders();
    if (!on) {
        if (r.core) poke(r.core + addr::remote_collision::core_party_collision, r.core_before);
        r.core = 0;
        return;
    }
    std::uintptr_t component{}, core{}, vtable{};
    if (!memory::peek(entity + 0x628, component) || !component || !memory::peek(component, vtable) ||
        vtable != base + addr::engine::skater_component_vtable || !memory::peek(component + 0x70, core) || !core ||
        !memory::peek(core, vtable) || vtable != base + addr::no_bail::bail_core_vtable)
        return;
    if (r.core && r.core != core) party_collision(base, 0, false); // another skater (a respawn)
    std::uint8_t now{};
    if (!memory::peek(core + addr::remote_collision::core_party_collision, now)) return;
    if (!r.core) {
        r.core = core;
        r.core_before = now;
    }
    if (!now) poke(core + addr::remote_collision::core_party_collision, 1);
}
void take_away(std::uintptr_t base, int index) {
    auto &rider = riders().riders[static_cast<std::size_t>(index)];
    if (rider.shown) {
        multiplayer::PeerScope scope(slot_of(index));
        multiplayer::remove_remote(base);
    }
    rider = {};
}
// Lower case: the catalog's keys and an outfit's assets need not agree on it.
std::string kind_of(std::string_view asset) {
    std::string text(asset);
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (text.size() < 5 || !text.starts_with("own_")) return {};
    const auto end = text.find('_', 4);
    return text.substr(4, end == std::string::npos ? std::string::npos : end - 4);
}
bool drawn(const std::string &kind) {
    return std::any_of(drawn_kinds.begin(), drawn_kinds.end(), [&](std::string_view k) { return kind.starts_with(k); });
}
const std::map<std::string, std::vector<std::string>> &wardrobe() {
    static std::map<std::string, std::vector<std::string>> kinds;
    static std::uint64_t generation = ~std::uint64_t{};
    std::lock_guard lock(profile_runtime::local_runtime().native_mutex);
    const auto &catalog = profile_runtime::cosmetic_runtime();
    if (generation == catalog.items_generation && !kinds.empty()) return kinds;
    kinds.clear();
    for (const auto &[key, info] : catalog.items)
        if (!info.build_kit)
            if (const auto kind = kind_of(key); !kind.empty() && drawn(kind)) kinds[kind].push_back(key);
    generation = catalog.items_generation;
    return kinds;
}
multiplayer::Appearance dress(const multiplayer::Appearance &look, std::mt19937 &draw, std::uintptr_t base, std::uintptr_t entity) {
    auto outfit = look;
    static bool told{};
    const bool costume = multiplayer::wear_cosmetic(base, entity, outfit.skater, rider_costume);
    if (!told) {
        told = true;
        say(costume ? std::string("Death Race: the AI riders wear the ghost skater costume.")
                    : std::string("Death Race: the ghost skater costume does not fit here; the riders' clothes are drawn instead."));
    }
    const auto &kinds = wardrobe();
    static bool explained{};
    std::string kept, changed;
    for (auto *recipe : {&outfit.skater, &outfit.board})
        for (auto &item : recipe->items) {
            const auto kind = kind_of(item.asset);
            const auto found = kinds.find(kind);
            if (kind.empty() || !drawn(kind) || found == kinds.end() || found->second.size() < 2) {
                if (!explained && kept.size() < 600) kept += " " + item.asset;
                continue;
            }
            item.asset = found->second[std::uniform_int_distribution<std::size_t>(0, found->second.size() - 1)(draw)];
            item.parameters.clear(); // the new piece's own colours
            if (!explained && changed.size() < 600) changed += " " + kind;
        }
    if (!explained) {
        explained = true;
        say(std::format("Death Race: rider outfits draw{} (from {} kinds); kept:{}", changed.empty() ? " nothing" : changed,
                        kinds.size(), kept.empty() ? " nothing" : kept));
    }
    outfit.hide_items = true;
    return outfit;
}
} // namespace dingosdk::death_race::riding

namespace dingosdk::death_race {
using namespace riding;

std::vector<double> rider_times(const Line &line, int count) {
    std::vector<double> times;
    const auto &r = riders();
    const bool course = !line.path.empty();
    for (int i = 0; i < std::min(count, most_riders); ++i) {
        const auto &rider = r.riders[static_cast<std::size_t>(i)];
        const double pace = rider.drawn ? rider.pace : paces[static_cast<std::size_t>(i)];
        if (rider.finished >= 0) times.push_back(rider.finished);
        else if (course) times.push_back(r.clock + (line.length - rider.s) / std::max(3.0, course_speed(line, rider.s) * pace));
        else times.push_back(line.seconds / pace + rider.delay);
    }
    return times;
}
std::vector<double> rider_finishes(int count) {
    std::vector<double> out;
    const auto &r = riders();
    for (int i = 0; i < std::min(count, most_riders); ++i) out.push_back(r.riders[static_cast<std::size_t>(i)].finished);
    return out;
}
std::size_t riders_shown() noexcept {
    const auto &r = riders();
    return static_cast<std::size_t>(std::count_if(r.riders.begin(), r.riders.end(), [](const Rider &x) { return x.shown; }));
}
} // namespace dingosdk::death_race

namespace dingosdk::death_race::riding {
// Emotes while `wanted` (waiting on the start line, parked past the finish): one of the skater's,
// then a pause, then another; each eased in and out over r.pose, and cut short (eased out) once no
// longer wanted (a rider about to go is back on its board in time).
void emote(Riders &r, Rider &rider, double now, bool wanted, const Vec &at, float facing) {
    const auto &emotes = loaded_library().emoting;
    const auto uniform = [&](double low, double high) { return std::uniform_real_distribution<double>(low, high)(r.draw); };
    if (rider.emote) {
        const double since = now - rider.emote_at, left = rider.emote->seconds - since;
        if (!wanted && rider.emote_quit < 0) rider.emote_quit = now;
        const double quit_left = rider.emote_quit >= 0 ? emote_fade - (now - rider.emote_quit) : emote_fade;
        if (left <= 0 || quit_left <= 0) {
            rider.emote.reset();
            rider.emote_quit = -1.0;
            rider.emote_next = now + uniform(emote_pause_least, emote_pause_most);
            return;
        }
        const double k = std::clamp(std::min({since, left, quit_left}) / emote_fade, 0.0, 1.0);
        emote_pose(*rider.emote, since, at, facing, r.extra);
        blend_pose(r.pose, r.extra, static_cast<float>(k), r.pose);
        return;
    }
    if (!wanted || emotes.empty()) return;
    if (rider.emote_next <= 0) rider.emote_next = now + uniform(0.3, emote_pause_most); // not all at once
    if (now < rider.emote_next) return;
    rider.emote = emotes[r.draw() % emotes.size()];
    rider.emote_at = now;
    rider.emote_quit = -1.0;
}
// A rider's own way of riding this race (see pace_spread): drawn when it first shows.
void draw_ride(Riders &r, Rider &rider, int i) {
    rider.drawn = true;
    const auto uniform = [&](double low, double high) { return std::uniform_real_distribution<double>(low, high)(r.draw); };
    constexpr double tau = 6.283185307179586;
    rider.pace = paces[static_cast<std::size_t>(i)] * (1.0 + uniform(-pace_spread, pace_spread));
    rider.wander = static_cast<float>(uniform(wander_least, wander_most));
    rider.wave1 = static_cast<float>(tau / uniform(40.0, 80.0));
    rider.phase1 = static_cast<float>(uniform(0.0, tau));
    rider.surge = uniform(surge_least, surge_most);
    rider.beat1 = tau / uniform(swing_long_least, swing_long_most);
    rider.beat2 = tau / uniform(swing_short_least, swing_short_most);
    rider.beat_phase1 = uniform(0.0, tau);
    rider.beat_phase2 = uniform(0.0, tau);
    // The rider's own stretches of riding: other ones than the rider before it, where there are.
    const auto &lib = loaded_library();
    const auto pick = [&](const std::vector<StylePtr> &kind) {
        return kind.empty() ? StylePtr{} : kind[(static_cast<std::size_t>(i) + r.draw()) % kind.size()];
    };
    rider.rolling = pick(lib.rolling);
    rider.pushing = pick(lib.pushing);
    rider.tucking = pick(lib.tucking);
    rider.flying = pick(lib.flying);
    rider.cycle_t = uniform(0.0, 10.0);
}
// Up again after a fall: on its board a little before where it fell, on the course, setting off.
void get_up(Rider &rider, const Line &line, double clock) {
    rider.fell = -1.0;
    if (line.path.empty()) return;
    rider.s = std::max(0.0, rider.fell_s - std::clamp(rider.fell_speed, respawn_least, respawn_most));
    rider.side = rider.plan = rider.side_rate = 0.0f;
    rider.ground = rider.ground_want = 0.0f;
    rider.brake = rider.braking = 1.0f;
    rider.evading = false;
    rider.relaunch = std::max(0.0, clock);
    rider.pushed = -1.0;
    rider.wobble_at = -1.0;
}
// On its way at its own speed: along the course (the line's path), or with none through the ride's
// replay (step.t). Its finish once at the end.
void ride_on(Rider &rider, const Tick &tick, Step &step) {
    const auto &line = tick.line;
    const double clock = tick.clock, dt = step.dt;
    if (!step.course) {
        // Slowed by a shove: its line runs behind for a moment (no jump back along it).
        if (tick.now < rider.drag_until) rider.delay += dt * rider.drag;
        step.t = clock < 0 ? 0.0 : std::clamp((clock - rider.delay) * rider.pace, 0.0, line.seconds);
        if (clock >= 0 && step.t >= line.seconds && rider.finished < 0) rider.finished = clock;
        return;
    }
    if (clock < 0) rider.s = -step.lead; // waiting, still, on the start line
    if (clock < 0 || step.falling || rider.s >= line.length) return;
    const double launch = std::clamp((clock - rider.relaunch) / launch_seconds, 0.25, 1.0);
    const double slowed = tick.now < rider.drag_until ? 1.0 - rider.drag : 1.0;
    // The rider's speed rises and falls a little along the way: overtaking, and being overtaken.
    const double surge = 1.0 + rider.surge * (0.6 * std::sin(clock * rider.beat1 + rider.beat_phase1) +
                                              0.4 * std::sin(clock * rider.beat2 + rider.beat_phase2));
    // Braking for something ahead it cannot keep off.
    rider.braking += (rider.brake - rider.braking) * static_cast<float>(std::min(1.0, dt * 3.0));
    rider.speed = static_cast<float>(course_speed(line, std::max(0.0, rider.s)) * rider.pace * launch * slowed * surge * rider.braking);
    rider.s = std::min<double>(line.length, rider.s + rider.speed * dt);
    rider.cycle_t += dt * std::clamp(rider.speed / std::max(1.0f, rider.rolling->speed), 0.6f, 1.6f);
    if (rider.s >= line.length && rider.finished < 0) rider.finished = clock;
}
// On the start line and the way onto the course: the pose it waits in, emoting until just before
// the go.
void wait_pose(Riders &r, Rider &rider, Tick &tick, const Step &step) {
    const auto &line = tick.line;
    const float k = static_cast<float>((rider.s + step.lead) / step.lead);
    Vec at{};
    for (int a = 0; a < 3; ++a) at[a] = (*tick.start)[a] + (step.course_start[a] - (*tick.start)[a]) * k;
    const float facing = line.path.front().yaw;
    rider.cycle_t += step.dt * 0.3; // standing on its board, balancing
    style_pose(*rider.rolling, rider.cycle_t, at, facing, slope_at(line, 0.0), r.pose, r.other);
    snap_board(r.pose, rider, facing, tick.bodies, tick.probes, tick.now, step.dt, at[1], deck_at(line, 0.0) + (at[1] - step.course_start[1]));
    emote(r, rider, tick.now, tick.clock < -emote_stop, at, facing);
    rider.side = rider.plan = step.lineup;
    rider.side_rate = 0.0f;
}
// Every few ticks (within the tick's budget) it looks along its way with the game's rays and picks
// the side it steers to (see look), overtaking the local skater around it when catching it up;
// parked past the finish, it only finds the ground it stands on. Before the go: its place on the
// start line.
void look_ahead(Riders &r, Rider &rider, Tick &tick, const Step &step, float want, float facing) {
    const auto &line = tick.line;
    if (tick.clock < 0) {
        rider.side = rider.plan = step.lineup;
        rider.side_rate = 0.0f;
        return;
    }
    if (step.falling || tick.sights >= sights_a_tick || tick.now < rider.sight_at) return;
    ++tick.sights;
    if (rider.s >= line.length) {
        rider.sight_at = tick.now + sight_every * 4.0;
        ground(line, rider, tick.bodies);
        return;
    }
    rider.sight_at = tick.now + sight_every;
    const auto &me = tick.me;
    const float fx = std::sin(facing), fz = std::cos(facing);
    const float along = (me[0] - rider.at[0]) * fx + (me[2] - rider.at[2]) * fz;
    const float my_forward = r.my_velocity[0] * fx + r.my_velocity[2] * fz;
    const bool catching = rider.shown && along > lead_least && along < sight_from + sight_most && rider.speed > my_forward + 0.5f;
    if (look(line, rider, want, tick.bodies, catching ? &me : nullptr, step.index)) {
        if (!r.sight_reported) {
            r.sight_reported = true;
            say("Death Race: the AI riders look ahead with the game's own rays.");
        }
        return;
    }
    // Blind: on the course, where the skater rode.
    rider.plan = step.lineup;
    rider.evading = false;
    rider.brake = 1.0f;
    rider.ground_want = 0.0f;
    if (!r.blind_reported) {
        r.blind_reported = true;
        logging::write(logging::Level::warning, logging::Channel::skater, "Death Race: the game's rays are unavailable; the AI riders keep to the course.");
    }
}
// Eased across toward the side it picked (faster when keeping off something), never a jump
// sideways; onto the ground it found there. Returns how far it leans into it (radians).
float steer(Rider &rider, double dt) {
    const float gain = rider.evading ? evade_gain : steer_gain, most = rider.evading ? evade_rate : steer_rate;
    const float rate = std::clamp((rider.plan - rider.side) * gain, -most, most);
    rider.side_rate += (rate - rider.side_rate) * static_cast<float>(std::min(1.0, dt * 6.0));
    rider.side += rider.side_rate * static_cast<float>(dt);
    rider.ground += (rider.ground_want - rider.ground) * static_cast<float>(std::min(1.0, dt * 5.0));
    return std::clamp(std::atan2(rider.side_rate, std::max(rider.speed, 3.0f)), -most_lean, most_lean);
}
// The rider's body at `at`, heading `facing` leant `tilt`: rolling; pushing while slow and as it sets off;
// tucked down a slope and at speed; in the air where the skater flew; leaning into the course's
// turns; a stumble over it all. Eased between. Returns whether it is in the air.
bool body_pose(Riders &r, Rider &rider, const Tick &tick, const Step &step, const Vec &at, float facing, float tilt) {
    const auto &line = tick.line;
    const double clock = tick.clock, dt = step.dt;
    const double s = std::max(0.0, rider.s);
    const bool air = air_at(line, s);
    const bool push = rider.pushing && clock >= 0 && !air && rider.s < line.length &&
                      (rider.speed < push_below || clock - rider.relaunch < push_after);
    if (push && rider.push_k <= 0.0f) rider.push_t = 0.0;
    rider.push_t += dt;
    const auto ease = [&](float &k, bool on, double rate) { k = std::clamp(k + static_cast<float>((on ? 1.0 : -1.0) * dt * rate), 0.0f, 1.0f); };
    ease(rider.push_k, push, 4.0);
    ease(rider.air_k, air, 7.0);
    // Tucked down a slope and at speed, as far as they ask.
    const float slope = slope_at(line, s);
    const float tuck = rider.tucking && clock >= 0 && rider.s < line.length
                           ? std::max(std::clamp((-slope - tuck_from) / (tuck_full - tuck_from), 0.0f, 1.0f),
                                      rider.speed > tuck_speed ? tuck_at_speed : 0.0f)
                           : 0.0f;
    rider.tuck_k += std::clamp(tuck - rider.tuck_k, static_cast<float>(-dt * 1.5), static_cast<float>(dt * 1.5));
    style_pose(*rider.rolling, rider.cycle_t, at, facing + tilt, slope, r.pose, r.other);
    if (rider.tuck_k > 0.0f && rider.tucking) {
        style_pose(*rider.tucking, rider.cycle_t, at, facing + tilt, slope, r.extra, r.other);
        blend_pose(r.pose, r.extra, rider.tuck_k, r.pose);
    }
    if (rider.push_k > 0.0f && rider.pushing) {
        style_pose(*rider.pushing, rider.push_t, at, facing + tilt, slope, r.extra, r.other);
        blend_pose(r.pose, r.extra, rider.push_k, r.pose);
    }
    if (rider.air_k > 0.0f) {
        if (rider.flying) {
            // One of the skater's own airs, from its pop to its landing, stretched over this one (its
            // lead in and out fall either side of it).
            const auto [from, to] = air_span(line, s);
            const auto &fly = *rider.flying;
            const double edge = std::min(static_cast<double>(air_lead) * capture_interval, fly.seconds * 0.25);
            const double k = to > from ? std::clamp((rider.s - from) / (to - from), 0.0, 1.0) : 0.5;
            air_pose(fly, edge + k * std::max(0.0, fly.seconds - 2.0 * edge), at, facing + tilt, r.extra);
        } else {
            // None yet: gathered up, its tuck's lowest pose (else its rolling one's).
            crouch_pose(rider.tucking ? *rider.tucking : *rider.rolling, at, facing + tilt, r.extra);
        }
        blend_pose(r.pose, r.extra, rider.air_k, r.pose);
    }
    // Into the turn: as far over as the speed and the turn ask (v^2 k / g), some of it.
    Vec unused{};
    float behind{}, ahead{};
    course_at(line, std::max(0.0, rider.s - 2.0), unused, behind);
    course_at(line, std::min<double>(line.length, rider.s + 2.0), unused, ahead);
    const float bend = angle_between(behind, ahead) / 4.0f; // radians a metre, + to the right
    const float want_lean =
        std::clamp(std::atan(rider.speed * rider.speed * bend / 9.81f) * turn_lean_share, -most_turn_lean, most_turn_lean);
    rider.turn_lean += (want_lean - rider.turn_lean) * static_cast<float>(std::min(1.0, dt * 3.0));
    lean(r.pose, {std::cos(facing), 0.0f, -std::sin(facing)}, rider.turn_lean);
    // A stumble: eased in fast, out a little slower.
    if (rider.stumble) {
        const double since = tick.now - rider.stumble_at, length = rider.stumble->seconds;
        if (since >= length) {
            rider.stumble.reset();
        } else {
            const float k = static_cast<float>(std::clamp(std::min(since / stumble_in, (length - since) / stumble_out), 0.0, 1.0));
            once_pose(*rider.stumble, since, at, facing + tilt, slope, r.extra);
            blend_pose(r.pose, r.extra, k, r.pose);
        }
    }
    return air;
}
// On the course: its own line a little either side of it in long curves (side by side just after
// the go and at the finish), keeping off whatever it sees ahead, leaning into it; its body; its
// board on the ground under it (the game's rays; in the air, where the skater's was); emotes once
// parked past the finish.
void course_pose(Riders &r, Rider &rider, Tick &tick, const Step &step) {
    const auto &line = tick.line;
    Vec at{};
    float facing{};
    course_at(line, std::max(0.0, rider.s), at, facing);
    const float s = static_cast<float>(rider.s);
    const float fade = std::clamp(std::min(s, line.length - park_metres - s) / wander_fade, 0.0f, 1.0f);
    const float want = step.lineup + rider.wander * fade * std::sin(s * rider.wave1 + rider.phase1);
    look_ahead(r, rider, tick, step, want, facing);
    const float tilt = steer(rider, step.dt);
    // The rider's right, for a heading `facing` (0 = +Z): (cos, 0, -sin).
    at[0] += std::cos(facing) * rider.side;
    at[2] -= std::sin(facing) * rider.side;
    at[1] += rider.ground;
    const bool air = body_pose(r, rider, tick, step, at, facing, tilt);
    const float deck = deck_at(line, std::max(0.0, rider.s)) + rider.ground;
    if (air) on_deck(r.pose, deck);
    else snap_board(r.pose, rider, facing + tilt, tick.bodies, tick.probes, tick.now, step.dt, at[1], deck);
    emote(r, rider, tick.now, rider.s >= line.length || tick.clock < -emote_stop, at, facing);
}
// Moved aside: across the start line on the way onto the course, or replaying the ride as it
// starts and finishes (on the course, its side does that; the replay also comes from the race's
// start line); out with a shove and back, staggering; swaying over its board after a light hit.
void move_aside(Riders &r, Rider &rider, const Tick &tick, const Step &step) {
    const auto &line = tick.line;
    const float off = step.course && !(rider.s < 0 && step.lead > 0.01f) ? 0.0f : step.lineup;
    Vec move{line.side[0] * off, 0.0f, line.side[2] * off};
    if (tick.start && !step.course) {
        const auto &first = line.frames.front().pose.root.position;
        for (int a = 0; a < 3; ++a) move[a] += ((*tick.start)[a] - first[a]) * step.early;
    }
    if (rider.pushed >= 0) {
        const double since = tick.now - rider.pushed;
        const float k = static_cast<float>(since < shove_out ? since / shove_out : std::max(0.0, 1.0 - (since - shove_out) / shove_back));
        if (since >= shove_out + shove_back) rider.pushed = -1.0;
        for (int a = 0; a < 3; ++a) move[a] += rider.push[a] * k;
        const auto root = r.pose.root.position;
        place(r.pose, root, root, rider.stagger * k);
    }
    multiplayer::offset_pose(r.pose, move);
    // A wobble: swaying over its board, away from the hit first.
    if (rider.wobble_at >= 0) {
        const double since = tick.now - rider.wobble_at;
        if (since >= wobble_seconds) {
            rider.wobble_at = -1.0;
        } else {
            constexpr double tau = 6.283185307179586;
            const float sway = rider.wobble * static_cast<float>(std::exp(-since / wobble_settle) * std::sin(since * wobble_hz * tau));
            lean(r.pose, rider.wobble_toward, sway);
        }
    }
}
// A run-in with the local skater (racing only, once in a while): both pushed apart, the rider
// stumbling on its board; hard enough, both fall, then are back on their boards a little before.
void run_in(Riders &r, Rider &rider, const Tick &tick, const Step &step) {
    const auto &line = tick.line;
    const double now = tick.now;
    const bool under_way = step.course ? rider.s < line.length : step.t < line.seconds;
    if (tick.clock < 0 || step.falling || now < rider.next_fall || !under_way) return;
    const auto at = r.pose.root.position;
    const auto &me = tick.me;
    const float across = std::hypot(at[0] - me[0], at[2] - me[2]);
    if (across >= hit_reach || std::abs(at[1] - me[1]) >= hit_height) return;
    Vec its{};
    if (step.course) {
        // Along its heading, and across as it steers.
        const float facing = yaw(r.pose.root.rotation);
        its = {std::sin(facing) * rider.speed + std::cos(facing) * rider.side_rate, 0.0f,
               std::cos(facing) * rider.speed - std::sin(facing) * rider.side_rate};
    } else {
        const auto ahead = root_at(line, std::min(step.t + 0.1, line.seconds));
        its = {(ahead[0] - at[0]) * 10.0f * static_cast<float>(rider.pace), 0.0f, (ahead[2] - at[2]) * 10.0f * static_cast<float>(rider.pace)};
    }
    // A jump in the skater's own position (a teleport) is no run into anyone.
    const float closing = std::hypot(r.my_velocity[0] - its[0], r.my_velocity[2] - its[2]);
    if (closing > most_closing || closing <= hit_speed) return;
    // From the skater to the rider: the way each is pushed (the skater the other way).
    float nx = at[0] - me[0], nz = at[2] - me[2];
    const float n = std::hypot(nx, nz);
    if (n > 1e-3f) {
        nx /= n;
        nz /= n;
    } else {
        nx = line.side[0];
        nz = line.side[2];
    }
    const auto clip = fall_clip();
    const bool crash = closing > crash_speed;
    // The skater: pushed away from the rider; a crash floors it too.
    const float push = std::clamp(nudge_least + closing * 0.35f, nudge_least, crash ? crash_nudge_most : nudge_most);
    const bool nudged = nudge_local_skater(tick.client, tick.local.entity, {-nx * push, 0.0f, -nz * push});
    const bool knocked = crash && knock_local_skater(tick.client, tick.local.entity);
    rider.next_fall = now + fall_again;
    say(std::format("Death Race: run-in with AI rider {} ({:.1f} m/s): {}{}{}.", step.index + 1, closing,
                    crash ? (clip ? "both fall" : "the skater falls, the rider staggers") : "pushed apart",
                    nudged ? "" : " (the skater's push is unavailable)", crash && !knocked ? " (the skater's fall is unavailable)" : ""));
    const float facing = yaw(r.pose.root.rotation);
    if (crash && clip) {
        // It falls, the way it was hit, partly.
        rider.clip = clip;
        rider.fell = now;
        rider.fall_length = clip->seconds + get_up_seconds;
        rider.delay += rider.fall_length;
        rider.next_fall = now + rider.fall_length + fall_again;
        rider.fell_at = at;
        rider.fell_facing = facing + 0.5f * angle_between(facing, std::atan2(nx, nz));
        rider.fell_s = rider.s;
        rider.fell_speed = rider.speed;
        rider.pushed = -1.0;
        rider.wobble_at = -1.0;
        return;
    }
    // Shoved away from the skater, staggering, stumbling, slowed for a moment; never into a wall or
    // a prop: only as far as there is room (past its own capsule).
    float size = std::min(most_shove, 0.6f + closing * 0.3f);
    const Vec out{at[0] + nx * 0.45f, at[1] + 0.6f, at[2] + nz * 0.45f};
    const auto hit = struck(out, {out[0] + nx * (size + 0.4f), out[1], out[2] + nz * (size + 0.4f)}, tick.bodies);
    if (hit.hit) size = std::clamp(std::hypot(hit.at[0] - out[0], hit.at[2] - out[2]) + 0.05f, 0.0f, size);
    rider.push = {nx * size, 0.0f, nz * size};
    rider.pushed = now;
    // Turned away from the side it was hit on.
    const float side = std::sin(facing) * nz - std::cos(facing) * nx; // its right . push
    rider.stagger = std::clamp(closing * 0.05f, 0.1f, most_stagger) * (side >= 0 ? -1.0f : 1.0f);
    // Off balance: one of the skater's own sketchy landings; without any, a wobble.
    const auto &stumbles = loaded_library().stumbling;
    if (!stumbles.empty()) {
        rider.stumble = stumbles[r.draw() % stumbles.size()];
        rider.stumble_at = now;
        rider.wobble_at = -1.0;
    } else {
        rider.wobble_at = now;
        rider.wobble = std::clamp(0.1f + closing * 0.06f, 0.1f, wobble_most);
        rider.wobble_toward = {nx, 0.0f, nz};
    }
    rider.drag = std::min(0.7, 0.08 * closing);
    rider.drag_until = now + 1.5;
}
// Shown as a remote actor in its slot (its spawn and its outfit within the tick's one native pass;
// the outfit put on a few times while the new actor settles, as the first can come too early), its
// capsules solid near the local skater, their foot at its board.
void present(Riders &r, Rider &rider, Tick &tick, int i) {
    const double now = tick.now;
    rider.at = r.pose.root.position;
    multiplayer::PeerScope scope(slot_of(i));
    const bool spawning = !rider.shown;
    std::string detail;
    if (!multiplayer::show_remote(tick.base, tick.client, tick.local, r.pose, detail)) {
        if (rider.shown) take_away(tick.base, i);
        rider.retry = now + 3.0;
        logging::write(logging::Level::warning, logging::Channel::skater, std::format("Death Race: AI rider {} could not be shown ({}).", i + 1, detail));
        return;
    }
    if (spawning) {
        tick.native_pass = true;
        rider.shown = true;
        rider.spawned = now;
        rider.dressings = 0;
        if (!r.reported) {
            r.reported = true;
            say("Death Race: AI riders shown.");
        }
    }
    if (r.look && !rider.outfit) {
        try {
            rider.outfit = dress(*r.look, r.draw, tick.base, tick.local.entity);
        } catch (...) {
            rider.outfit = r.look;
        }
    }
    if (rider.outfit && rider.dressings < 4 && now - rider.spawned >= 0.5 + rider.dressings * 1.5 && !tick.native_pass) {
        tick.native_pass = true;
        std::string why;
        multiplayer::update_remote_cosmetics(tick.base, tick.local, *rider.outfit, why);
        ++rider.dressings;
    }
    const bool close = distance(r.pose.root.position, tick.me) < solid_reach;
    // The rider's capsule's foot: under its root, at its board when the board is under it (else where the
    // board would be).
    Vec feet = r.pose.root.position;
    feet[1] -= root_above_board + deck_height;
    if (!r.pose.board.empty()) {
        const auto &board = r.pose.board.front().position;
        if (std::hypot(board[0] - feet[0], board[2] - feet[2]) < 0.7f) feet[1] = board[1] - deck_height;
    }
    multiplayer::update_remote_collision(tick.base, tick.local.context, r.pose, close, tick.us, &feet, body_radius);
}
} // namespace dingosdk::death_race::riding

namespace dingosdk::death_race {
using namespace riding;

void show_riders(std::uintptr_t base, std::uintptr_t client, const Line *line, int count, double clock, double now,
                 const std::array<float, 3> *start) {
    auto &r = riders();
    if (!line || line->frames.empty()) count = 0;
    count = std::clamp(count, 0, most_riders);
    try {
        if (!count) {
            for (int i = 0; i < most_riders; ++i) take_away(base, i);
            party_collision(base, 0, false);
            r.look.reset();
            r.last_me.reset();
            r.failed = false;
            return;
        }
        if (r.failed) return;
        std::string detail;
        if (!multiplayer::install_entity_hooks(base, detail)) {
            r.failed = true;
            logging::write(logging::Level::warning, logging::Channel::skater, "Death Race: AI riders unavailable (" + detail + ").");
            return;
        }
        auto local = multiplayer::capture_local(base, client, false);
        if (!local.ready) return;
        party_collision(base, local.entity, true);
        if (!r.look && now >= r.next_look) {
            r.next_look = now + 2.0;
            try {
                std::string why;
                r.look = multiplayer::capture_cosmetics(base, multiplayer::capture_local(base, client, true), why);
            } catch (...) {}
        }
        // The local skater's speed, smoothed over a few ticks.
        const auto me = local.pose.root.position;
        if (clock >= 0) r.clock = clock;
        if (r.last_me && now > r.last_me_at) {
            const float dt = static_cast<float>(now - r.last_me_at);
            for (int a = 0; a < 3; ++a) r.my_velocity[a] = r.my_velocity[a] * 0.6f + ((me[a] - (*r.last_me)[a]) / dt) * 0.4f;
        }
        r.last_me = me;
        r.last_me_at = now;
        Tick tick{base, client, *line, start, clock, now, std::move(local), me, {me}, micros()};
        for (int i = 0; i < count; ++i)
            if (r.riders[static_cast<std::size_t>(i)].shown) tick.bodies.push_back(r.riders[static_cast<std::size_t>(i)].at);
        for (int i = 0; i < most_riders; ++i) {
            auto &rider = r.riders[static_cast<std::size_t>(i)];
            if (i >= count) {
                take_away(base, i);
                continue;
            }
            if (!rider.shown && (tick.native_pass || now < rider.retry)) continue;
            if (!rider.drawn) draw_ride(r, rider, i);
            if (!rider.rolling) {
                // Nothing of the skater's plain riding yet: no body to ride with.
                if (!r.no_body_reported) {
                    r.no_body_reported = true;
                    logging::write(logging::Level::warning, logging::Channel::skater,
                                   "Death Race: no plain riding recorded yet to give the AI riders a body; ride the course once without tricks.");
                }
                take_away(base, i);
                continue;
            }
            Step step{i};
            step.dt = rider.last_tick ? std::clamp(now - rider.last_tick, 0.0, 0.1) : 0.0;
            rider.last_tick = now;
            step.falling = rider.fell >= 0 && now - rider.fell < rider.fall_length;
            if (rider.fell >= 0 && !step.falling) get_up(rider, *line, clock);
            // On the course (the line's path; with none, the old replay of the ride). The way from
            // the race's start line onto it (the ride began a little past it) is ridden too, so
            // nobody starts ahead.
            step.course = !line->path.empty();
            if (step.course) {
                step.course_start = line->path.front().p;
                if (start) step.lead = std::hypot((*start)[0] - step.course_start[0], (*start)[2] - step.course_start[2]);
            }
            rider.speed = 0.0f;
            ride_on(rider, tick, step);
            // Side by side on the race's start line before the go, onto the line over the first
            // seconds, and side by side again at the finish.
            step.early = static_cast<float>(clock < 0 ? 1.0 : std::max(0.0, 1.0 - clock / merge_seconds));
            const float late = step.course
                                   ? std::clamp((static_cast<float>(rider.s) - (line->length - park_metres)) / park_metres, 0.0f, 1.0f)
                                   : static_cast<float>(std::clamp((step.t - (line->seconds - park_seconds)) / park_seconds, 0.0, 1.0));
            step.lineup = spread[static_cast<std::size_t>(i)] * std::max(step.early, late);
            if (step.falling && rider.clip) {
                // The rider's fall: the local skater's own, turned and moved to where it fell.
                sample(rider.clip->frames, std::min(now - rider.fell, rider.clip->seconds), r.pose);
                const auto &origin = rider.clip->frames.front().pose.root;
                place(r.pose, origin.position, rider.fell_at, rider.fell_facing - yaw(origin.rotation));
            } else {
                if (!step.course) sample(line->frames, step.t, r.pose);
                else if (rider.s < 0 && step.lead > 0.01f) wait_pose(r, rider, tick, step);
                else course_pose(r, rider, tick, step);
                move_aside(r, rider, tick, step);
            }
            run_in(r, rider, tick, step);
            present(r, rider, tick, i);
        }
    } catch (const std::exception &e) {
        r.failed = true;
        logging::write(logging::Level::warning, logging::Channel::skater, std::string("Death Race: AI riders stopped: ") + e.what());
    } catch (...) {
        r.failed = true;
    }
}
} // namespace dingosdk::death_race
