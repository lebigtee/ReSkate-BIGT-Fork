#include "death_race_riders.h"
#include "death_race_limits.h"
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

// See death_race_riders.h. The riders take the last remote-player slots, which a session's own
// players never reach, one native actor each (Extension/Multiplayer/Remote/native_skater_spawn.cpp).
namespace dingosdk::death_race {
using multiplayer::Pose;
using multiplayer::Transform;
using Vec = std::array<float, 3>;
struct Frame {
    double t{};
    Pose pose;
    bool board{};
    // What the skater was doing: plain riding (on its board on the ground, no trick, not getting
    // over a fall), in the air, pushing (the game's push request), getting over a fall; its
    // physics state.
    bool plain{}, air{}, push{}, fallen{};
    std::uint32_t state{};
};
struct Point {
    Vec p{};
    float s{}, speed{}, yaw{}; // metres along, as ridden (m/s), facing (radians, 0 = +Z)
    bool air{};                // the skater was in the air there
    float deck = std::numeric_limits<float>::quiet_NaN(); // its board's height there (NaN: not under it)
};
// A race's line: where the skater rode and how fast, nothing of what its body did (the riders'
// bodies come from the riding library, below).
struct Line {
    std::vector<Frame> frames;
    double seconds{};
    Vec side{}; // to the right of the way it sets off, across the ground
    // The course: the ride's path every half metre (its falls left out), smoothed, with the speed
    // it was ridden at and where it was in the air.
    std::vector<Point> path;
    float length{};
};
namespace {
constexpr double capture_interval = 0.05;             // 20 poses a second, as a session sends them
constexpr std::size_t longest_capture = 20 * 60 * 10; // ten minutes
constexpr float set_off_distance = 0.6f;              // metres on the board from its first pose: the ride began
// Each rider's pace on the line, and how far to the side of it it starts (metres, to the right);
// it joins the line over the first seconds of the race.
constexpr std::array<double, most_riders> paces{1.01, 0.98, 1.03, 0.95};
constexpr std::array<float, most_riders> spread{lane_spacing, -lane_spacing, 2.0f * lane_spacing, -2.0f * lane_spacing};
static_assert(most_riders == most_racers - 1);
constexpr double merge_seconds = 6.0;
// One rider's capsules are solid within this distance of the local skater.
constexpr float solid_reach = 40.0f;
// A fall: the poses kept after the local skater's begins; how long a rider lies there after it
// before it is back on its board, a little before where it fell (the distance it rode in a second,
// within these metres); no second run-in this soon after one.
constexpr double fall_seconds = 2.6, get_up_seconds = 0.4, fall_again = 1.0;
constexpr float respawn_least = 3.0f, respawn_most = 10.0f;
// A run-in (across, up, how much faster one goes than the other): from hit_speed both are pushed
// apart, the rider staggering and wobbling on its board; from crash_speed they really hit, and
// both fall. The local skater is pushed away by this much (m/s), more the harder the hit.
constexpr float hit_reach = 0.8f, hit_height = 1.8f, hit_speed = 1.2f, crash_speed = 4.0f, most_closing = 20.0f;
// Its body to run into (the game's own collision): a capsule this wide (a skater, not the retail
// proxy's metre), standing on its board: a recorded pose's root rides about a metre above the
// board, where the proxy's capsule would float at head height and above.
constexpr float body_radius = 0.36f, deck_height = 0.1f, root_above_board = 0.95f;
constexpr float nudge_least = 0.8f, nudge_most = 2.5f, crash_nudge_most = 4.0f;
// The wobble: the body swaying over its board away from the hit and back, dying away (radians at
// most, sways a second, the time it takes to settle).
constexpr double wobble_seconds = 1.4;
constexpr float wobble_most = 0.3f, wobble_hz = 2.2f, wobble_settle = 0.45f;
// A shove: out over a quarter second, back to its line over the next seconds, a stagger (turned
// away, in radians), and the time it loses while slowed.
constexpr double shove_out = 0.25, shove_back = 2.5;
constexpr float most_shove = 2.8f, most_stagger = 0.45f;
// The riders spread out again over the line's last seconds (metres of the course): they stop
// side by side.
constexpr double park_seconds = 3.0;
constexpr float park_metres = 12.0f;
// The course: points every half metre, smoothed over a metre and a half either side (speeds three
// metres); the riders ride it at its speed, never slower than this (where the skater walked) nor
// faster than a mega ramp takes it, picking up speed over the first seconds.
constexpr float path_step = 0.5f, least_speed = 6.0f, most_speed = 45.0f;
// The course is raced, whatever the skater did setting it: where it stopped, walked or fell
// (slower than riding_speed, once it had got going past launched_speed), the riders keep the
// speed it had before slowing down for it (the fastest within stall_back metres), easing into the
// speed it found again after (once back to stall_resumed of it, or stall_reach metres on). Its
// start (getting going) and its turns are raced as ridden. The ride ends where the skater last
// rode faster than riding_speed (the rest was stopping to set its finish).
constexpr float riding_speed = 3.0f, launched_speed = 6.0f, stall_back = 25.0f, stall_resumed = 0.8f, stall_reach = 80.0f;
constexpr int smooth_points = 3, smooth_speeds = 6;
constexpr double launch_seconds = 1.5;
// Each rider's own way of riding, drawn every race: its pace (around its base one), how far it
// wanders either side of the course and over how many metres, and how much its speed rises and
// falls along the way. The wander fades in and out over the course's first and last metres.
constexpr double pace_spread = 0.03;
constexpr float wander_least = 0.4f, wander_most = 1.0f, wander_fade = 15.0f;
// Sight: every few ticks a rider looks along the way it means to take with the game's own rays,
// knee and chest high, from just ahead of it to a second or so on (no less, no more than these
// metres), and keeps off whatever stands there that is no skater: its own line when that is
// clear, else where it is, else back on the course (the skater rode it), braking when nothing is.
// At most this many riders look a tick.
constexpr float sight_low = 0.4f, sight_high = 1.0f, sight_from = 0.8f, sight_least = 4.0f, sight_most = 10.0f;
constexpr double sight_seconds = 1.0, sight_every = 0.06;
constexpr int sights_a_tick = 2;
// A hit this close to a skater (across) is that skater; this close to the course, a rider rides
// as the skater did there.
constexpr float body_reach = 0.9f, on_course = 0.2f;
// Overtaking: a rider catching the local skater up keeps this far from it (across), once it is this
// far ahead, passing it this far to either side when its own way is not clear. It never shies
// from the skater beside or behind it: that one may run into it.
constexpr float pass_room = 1.3f, lead_least = 0.5f, pass_side = 1.6f;
// Steering across: how fast it eases to the side it chose (per second, and metres a second at
// most), faster when keeping off something; how far it leans into it (radians); how slow it
// brakes down to when nothing ahead is clear.
constexpr float steer_gain = 1.2f, steer_rate = 1.2f, evade_gain = 3.5f, evade_rate = 3.0f, most_lean = 0.35f;
constexpr float brake_least = 0.35f;
// Off the course it rides on the ground there, found this far ahead: never more than this above or
// below the course.
constexpr float ground_ahead = 1.5f, ground_step = 0.5f;
// A rider's board on the ground: the ground under it found by two of the game's rays this far ahead
// of and behind its middle (clear of its own capsules), every so often (at most this many riders a
// tick); eased into (a jump of more than ground_jump is taken at once). The board's middle rides
// `lift` over the ground: measured under the skater's own board as it rides on the flat (the
// middle of what is seen), first_lift until then.
constexpr float probe_reach = 0.6f, ground_jump = 0.5f, flat_ground = 0.03f, first_lift = 0.1f;
constexpr double probe_every = 0.08, lift_every = 0.5;
constexpr double ground_settle = 10.0; // a second's share of the way to what the last look found
constexpr int probes_a_tick = 2;
// Its speed swings slowly (over a cycle of these seconds, and a shorter one with less of it), by
// this share either way: riders overtake each other now and then, not all the time.
constexpr double surge_least = 0.05, surge_most = 0.08;
constexpr double swing_long_least = 25.0, swing_long_most = 60.0, swing_short_least = 15.0, swing_short_most = 30.0;
// The riding library (see Style): rolling stretches this long, pushing ones from a push's start;
// a stretch loops crossfaded into itself at its ends. Rolling: plain riding at least this fast,
// turning, rising or falling off its straight line, and the board pitching against the way it
// goes (a manual) no more than these. The best few of each from every ride, the newest kept.
constexpr double rolling_window = 2.0, pushing_window = 1.1, style_fade = 0.35;
constexpr float rolling_least_speed = 5.0f, rolling_turn = 0.35f, rolling_rise = 0.35f, rolling_pitch = 0.12f;
constexpr std::size_t styles_a_ride = 2, most_rolling = 8, most_pushing = 6, most_tucking = 6, most_flying = 6;
// Stumbles (the skater's own sketchy landings: the second after it came down off balance) and
// emotes (the skater's, on foot), put in the library from its captures. A light run-in plays a
// stumble; the riders play emotes on the start line until a moment before the go (then they are
// back on their boards), and parked past the finish, with pauses between.
constexpr std::size_t most_stumbling = 6, most_emoting = 8;
constexpr double emote_stop = 1.2, emote_pause_least = 1.0, emote_pause_most = 3.5, emote_fade = 0.35;
constexpr double stumble_in = 0.1, stumble_out = 0.25;
// Airs: the skater's own, from a little before it left the ground to a little after it came down
// (poses either side), at least this long in the air, its board never turned over (its up within
// this of the world's) nor round under its feet (its heading against the body's, radians): no
// trick, nothing gone wrong. A rider flies one through each of its own airs, stretched to fit.
constexpr std::size_t air_lead = 3;
constexpr double least_air = 0.4;
constexpr float air_board_up = 0.75f, air_board_turn = 0.6f;
// Rolling is standing on the board: the head (above the skater's placement) within this of where it
// is riding upright (the ride's tallest tenth), never below the least; tucking is crouched lower,
// held (a downhill tuck). Neither bobs more than this (pumping, a crouch before a trick).
constexpr float upright_give = 0.15f, upright_least = 0.12f, upright_bob = 0.15f;
// A rider tucks down a slope (fully from this steep, radians, starting from the gentler one) and at
// speed (some, from this fast).
constexpr float tuck_from = 0.035f, tuck_full = 0.14f, tuck_speed = 12.0f, tuck_at_speed = 0.4f;
// The skater's jumps lower than this over their way across (a flat trick, an ollie on the flat) are
// ridden on the ground; higher ones (a drop, a gap) are flown.
constexpr float hop_height = 0.7f;
// A rider pushes while slower than this, and for the first seconds of each set-off; leans into the
// course's turns this much of the way the skater would (radians at most).
constexpr float push_below = 7.0f, turn_lean_share = 0.7f, most_turn_lean = 0.3f;
constexpr double push_after = 2.0;

void say(const std::string &text) { logging::write(logging::Level::info, logging::Channel::skater, text); }
float distance(const Vec &a, const Vec &b) {
    return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}

// ---- recording -----------------------------------------------------------------------------------
struct Recording {
    bool on{};
    double started{}, next{};
    std::vector<Frame> frames;
};
Recording &recording() {
    static auto *value = new Recording;
    return *value;
}
// The local skater's last fall, from its first moment.
struct Clip {
    std::vector<Frame> frames;
    double seconds{};
};
struct Falls {
    Recording taking;
    std::shared_ptr<const Clip> clip;
};
Falls &falls() {
    static auto *value = new Falls;
    return *value;
}
// The game's push request for `entity` (its physics context +0x13c4 bit 6, as the trainer's push
// code reads it).
bool pushing(std::uintptr_t base, std::uintptr_t entity) {
    std::uintptr_t component{}, core{}, context{}, vtable{};
    std::uint32_t requests{};
    return entity && memory::peek(entity + 0x628, component) && component && memory::peek(component, vtable) &&
           vtable == base + addr::engine::skater_component_vtable && memory::peek(component + 0x70, core) && core &&
           memory::peek(core + 0x3c0, context) && context && memory::peek(context + 0x13c4, requests) && (requests & 0x40u) != 0;
}
bool capture(std::uintptr_t base, std::uintptr_t client, Recording &r, double now, const Moment &m) {
    try {
        auto frame = multiplayer::capture_local(base, client, true);
        if (!frame.ready || frame.pose.skater.empty()) return false;
        Frame f{now - r.started, std::move(frame.pose), m.board};
        f.plain = m.plain;
        f.air = m.air;
        f.fallen = m.fallen;
        f.state = m.state;
        f.push = m.plain && pushing(base, frame.entity);
        r.frames.push_back(std::move(f));
        return true;
    } catch (...) {
        return false;
    }
}

// ---- poses ---------------------------------------------------------------------------------------
using Quat = std::array<float, 4>; // x, y, z, w
Quat multiply(const Quat &a, const Quat &b) {
    return {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1], a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
            a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3], a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
}
// The way a rotation faces across the ground (radians; 0 faces +Z).
float yaw(const Quat &q) {
    const float fx = 2.0f * (q[0] * q[2] + q[3] * q[1]), fz = 1.0f - 2.0f * (q[0] * q[0] + q[1] * q[1]);
    return std::atan2(fx, fz);
}
Transform blend(const Transform &a, const Transform &b, float t) {
    Transform out;
    for (int i = 0; i < 3; ++i) {
        out.position[i] = a.position[i] + (b.position[i] - a.position[i]) * t;
        out.scale[i] = a.scale[i] + (b.scale[i] - a.scale[i]) * t;
    }
    // The shorter way round, then normalised.
    const float dot = a.rotation[0] * b.rotation[0] + a.rotation[1] * b.rotation[1] + a.rotation[2] * b.rotation[2] +
                      a.rotation[3] * b.rotation[3];
    const float sign = dot < 0 ? -1.0f : 1.0f;
    float length{};
    for (int i = 0; i < 4; ++i) {
        out.rotation[i] = a.rotation[i] + (b.rotation[i] * sign - a.rotation[i]) * t;
        length += out.rotation[i] * out.rotation[i];
    }
    length = std::sqrt(length);
    if (length > 1e-6f)
        for (auto &r : out.rotation) r /= length;
    else
        out.rotation = a.rotation;
    return out;
}
template <class Frames> std::size_t after(const Frames &f, double t) {
    return static_cast<std::size_t>(std::upper_bound(f.begin(), f.end(), t, [](double v, const Frame &x) { return v < x.t; }) - f.begin());
}
template <class Frames> void sample(const Frames &f, double t, Pose &out) {
    if (t <= f.front().t) { out = f.front().pose; return; }
    if (t >= f.back().t) { out = f.back().pose; return; }
    const auto i = after(f, t);
    const auto &b = f[i], &a = f[i - 1];
    const float k = static_cast<float>((t - a.t) / std::max(1e-6, b.t - a.t));
    out.root = blend(a.pose.root, b.pose.root, k);
    // Poses of one skater keep their bone counts; a change mid-line takes the earlier one.
    if (a.pose.skater.size() != b.pose.skater.size() || a.pose.board.size() != b.pose.board.size()) {
        out.skater = a.pose.skater;
        out.board = a.pose.board;
        return;
    }
    out.skater.resize(a.pose.skater.size());
    for (std::size_t j = 0; j < out.skater.size(); ++j) out.skater[j] = blend(a.pose.skater[j], b.pose.skater[j], k);
    out.board.resize(a.pose.board.size());
    for (std::size_t j = 0; j < out.board.size(); ++j) out.board[j] = blend(a.pose.board[j], b.pose.board[j], k);
}
Vec root_at(const Line &line, double t) {
    const auto &f = line.frames;
    if (t <= f.front().t) return f.front().pose.root.position;
    if (t >= f.back().t) return f.back().pose.root.position;
    const auto i = after(f, t);
    const auto &a = f[i - 1].pose.root.position, &b = f[i].pose.root.position;
    const float k = static_cast<float>((t - f[i - 1].t) / std::max(1e-6, f[i].t - f[i - 1].t));
    return {a[0] + (b[0] - a[0]) * k, a[1] + (b[1] - a[1]) * k, a[2] + (b[2] - a[2]) * k};
}
// Turns a pose about `from` by `turn` (radians about up) and moves it there to `to`. Only the
// world-space anchors move (offset_pose's: root, skater bone 1, the board entity and its anchor).
void place(Pose &pose, const Vec &from, const Vec &to, float turn) {
    const float s = std::sin(turn), c = std::cos(turn);
    const Quat spin{0.0f, std::sin(turn * 0.5f), 0.0f, std::cos(turn * 0.5f)};
    const auto move = [&](Transform &t) {
        const float dx = t.position[0] - from[0], dy = t.position[1] - from[1], dz = t.position[2] - from[2];
        t.position = {to[0] + dx * c + dz * s, to[1] + dy, to[2] - dx * s + dz * c};
        t.rotation = multiply(spin, t.rotation);
    };
    move(pose.root);
    if (pose.skater.size() > 1) move(pose.skater[1]);
    if (!pose.board.empty()) move(pose.board.front());
    if (pose.board.size() > 2) move(pose.board[2]);
}

// ---- the course ----------------------------------------------------------------------------------
// `v` turned by `q` (x, y, z, w).
Vec turned(const Quat &q, const Vec &v) {
    const Vec u{q[0], q[1], q[2]};
    const auto cross = [](const Vec &a, const Vec &b) { return Vec{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; };
    const auto t = cross(u, v);
    const Vec t2{t[0] * 2.0f, t[1] * 2.0f, t[2] * 2.0f};
    const auto c = cross(u, t2);
    return {v[0] + q[3] * t2[0] + c[0], v[1] + q[3] * t2[1] + c[1], v[2] + q[3] * t2[2] + c[2]};
}
// The body (the skater rig's world anchor, its bones following) leaning `angle` radians over its
// board toward `toward` (flat, unit; away from it when negative), about the root at its feet.
void lean(Pose &pose, const Vec &toward, float angle) {
    if (pose.skater.size() < 2 || std::abs(angle) < 1e-4f) return;
    // About up x toward: up turns toward `toward`.
    Vec axis{toward[2], 0.0f, -toward[0]};
    const float n = std::hypot(axis[0], axis[2]);
    if (n < 1e-4f) return;
    const float h = angle * 0.5f, s = std::sin(h) / n;
    const Quat q{axis[0] * s, 0.0f, axis[2] * s, std::cos(h)};
    auto &anchor = pose.skater[1];
    const auto &pivot = pose.root.position;
    const auto d = turned(q, {anchor.position[0] - pivot[0], anchor.position[1] - pivot[1], anchor.position[2] - pivot[2]});
    anchor.position = {pivot[0] + d[0], pivot[1] + d[1], pivot[2] + d[2]};
    anchor.rotation = multiply(q, anchor.rotation);
}
float angle_between(float from, float to) {
    float d = to - from;
    while (d > 3.14159265f) d -= 6.2831853f;
    while (d < -3.14159265f) d += 6.2831853f;
    return d;
}
// The ride's path (see Line): its falls and the getting up after them left out.
void shape(Line &line) {
    std::vector<const Frame *> f;
    for (const auto &frame : line.frames)
        if (!frame.fallen) f.push_back(&frame);
    if (f.size() < 4) return;
    // The board's height under the skater (NaN when it was not under it: carried, on foot).
    const auto deck_of = [](const Frame &frame) {
        const auto &pose = frame.pose;
        if (pose.board.empty()) return std::numeric_limits<float>::quiet_NaN();
        const auto &board = pose.board.front().position, &root = pose.root.position;
        return std::hypot(board[0] - root[0], board[2] - root[2]) < 0.7f ? board[1] : std::numeric_limits<float>::quiet_NaN();
    };
    const auto mix = [](float a, float b, float k) {
        if (std::isfinite(a) && std::isfinite(b)) return a + (b - a) * k;
        return std::isfinite(a) ? a : b;
    };
    // Every half metre along the ride, with its speed there.
    std::vector<Point> path{{f.front()->pose.root.position, 0.0f, 0.0f, 0.0f, f.front()->air, deck_of(*f.front())}};
    float along = 0.0f, next = path_step;
    for (std::size_t i = 0; i + 1 < f.size(); ++i) {
        const auto &a = f[i]->pose.root.position, &b = f[i + 1]->pose.root.position;
        const float seg = distance(a, b);
        const float speed = static_cast<float>(seg / std::max(1e-3, f[i + 1]->t - f[i]->t));
        const bool air = f[i]->air && f[i + 1]->air;
        const float deck_a = deck_of(*f[i]), deck_b = deck_of(*f[i + 1]);
        while (seg > 1e-4f && next <= along + seg) {
            const float k = (next - along) / seg;
            path.push_back({{a[0] + (b[0] - a[0]) * k, a[1] + (b[1] - a[1]) * k, a[2] + (b[2] - a[2]) * k}, next, speed, 0.0f, air,
                            mix(deck_a, deck_b, k)});
            next += path_step;
        }
        along += seg;
    }
    if (path.size() < 12) return;
    path.front().speed = path[1].speed;
    // Stops, walks and falls raced through (see riding_speed).
    {
        std::size_t j = 0;
        while (j < path.size() && path[j].speed < launched_speed) ++j;
        const auto back = static_cast<std::size_t>(stall_back / path_step), reach = static_cast<std::size_t>(stall_reach / path_step);
        while (j < path.size()) {
            if (path[j].speed >= riding_speed) {
                ++j;
                continue;
            }
            // From the fastest it went just before...
            std::size_t from = j - 1;
            for (std::size_t m = j >= back ? j - back : 0; m < j; ++m)
                if (path[m].speed > path[from].speed) from = m;
            const float before = path[from].speed;
            // ...to where it was back to most of that.
            std::size_t to = j;
            while (to < path.size() && to - j < reach && path[to].speed < before * stall_resumed) ++to;
            const float after = to < path.size() ? path[to].speed : before;
            for (std::size_t m = from + 1; m < to; ++m) {
                const float u = static_cast<float>(m - from) / static_cast<float>(to - from);
                path[m].speed = before + (after - before) * u;
            }
            j = std::max(to, j + 1);
        }
    }
    // A low jump (a flat trick) is ridden on the ground: straight from where it left it to where it
    // came down, and no air.
    for (std::size_t j = 0; j < path.size();) {
        if (!path[j].air) {
            ++j;
            continue;
        }
        std::size_t k = j;
        while (k < path.size() && path[k].air) ++k;
        const auto &from = path[j ? j - 1 : 0].p, &to = path[std::min(k, path.size() - 1)].p;
        float apex{};
        for (std::size_t m = j; m < k; ++m) {
            const float u = static_cast<float>(m - j + 1) / static_cast<float>(k - j + 1);
            apex = std::max(apex, path[m].p[1] - (from[1] + (to[1] - from[1]) * u));
        }
        if (apex < hop_height) {
            const float deck_from = path[j ? j - 1 : 0].deck, deck_to = path[std::min(k, path.size() - 1)].deck;
            for (std::size_t m = j; m < k; ++m) {
                const float u = static_cast<float>(m - j + 1) / static_cast<float>(k - j + 1);
                path[m].p[1] = from[1] + (to[1] - from[1]) * u;
                path[m].deck = mix(deck_from + (deck_to - deck_from) * u, path[m].deck, 0.0f);
                path[m].air = false;
            }
        }
        j = k;
    }
    // Smoothed: positions, then speeds; then lengths and facings again. Not through the air,
    // whose arc is kept as it was flown.
    auto smooth = path;
    for (std::size_t j = 0; j < path.size(); ++j) {
        Vec sum{};
        float speed{}, deck{};
        int n{}, m{}, decks{};
        for (int d = -smooth_speeds; d <= smooth_speeds; ++d) {
            const auto k = static_cast<std::ptrdiff_t>(j) + d;
            if (k < 0 || k >= static_cast<std::ptrdiff_t>(path.size())) continue;
            speed += path[static_cast<std::size_t>(k)].speed;
            ++m;
            if (std::abs(d) > smooth_points) continue;
            for (int a = 0; a < 3; ++a) sum[a] += path[static_cast<std::size_t>(k)].p[a];
            ++n;
            if (std::isfinite(path[static_cast<std::size_t>(k)].deck)) {
                deck += path[static_cast<std::size_t>(k)].deck;
                ++decks;
            }
        }
        for (int a = 0; a < 3; ++a) smooth[j].p[a] = sum[a] / static_cast<float>(n);
        smooth[j].deck = decks ? deck / static_cast<float>(decks) : std::numeric_limits<float>::quiet_NaN();
        if (path[j].air) {
            smooth[j].p[1] = path[j].p[1];
            smooth[j].deck = path[j].deck;
        }
        smooth[j].speed = speed / static_cast<float>(m);
    }
    smooth.front().p = path.front().p;
    float s = 0.0f;
    for (std::size_t j = 0; j < smooth.size(); ++j) {
        if (j) s += distance(smooth[j - 1].p, smooth[j].p);
        smooth[j].s = s;
        const auto &ahead = smooth[std::min(j + 6, smooth.size() - 1)].p, &behind = smooth[j >= 6 ? j - 6 : 0].p;
        smooth[j].yaw = std::atan2(ahead[0] - behind[0], ahead[2] - behind[2]);
    }
    line.path = std::move(smooth);
    line.length = line.path.back().s;
}
// The board's height `s` metres along the course, as the skater's was (NaN where it was not under it).
float deck_at(const Line &line, double s) {
    const auto &p = line.path;
    const auto i = static_cast<std::size_t>(
        std::upper_bound(p.begin(), p.end(), s, [](double v, const Point &x) { return v < x.s; }) - p.begin());
    if (i == 0) return p.front().deck;
    if (i >= p.size()) return p.back().deck;
    const auto &a = p[i - 1], &b = p[i];
    const float k = static_cast<float>((s - a.s) / std::max(1e-4f, b.s - a.s));
    if (std::isfinite(a.deck) && std::isfinite(b.deck)) return a.deck + (b.deck - a.deck) * k;
    return std::isfinite(a.deck) ? a.deck : b.deck;
}
// A rider's board on the course's (raised or lowered with all of it): whatever its body does, its
// board rides where the skater's did. Not while its board is not under it (an emote, carried).
void on_deck(Pose &pose, float deck) {
    if (!std::isfinite(deck) || pose.board.empty()) return;
    const auto &board = pose.board.front().position, &root = pose.root.position;
    if (std::hypot(board[0] - root[0], board[2] - root[2]) >= 0.7f) return;
    multiplayer::offset_pose(pose, {0.0f, deck - board[1], 0.0f});
}
// Where `s` metres along the course is, and which way it goes there.
void course_at(const Line &line, double s, Vec &at, float &facing) {
    const auto &p = line.path;
    const auto i = static_cast<std::size_t>(
        std::upper_bound(p.begin(), p.end(), s, [](double v, const Point &x) { return v < x.s; }) - p.begin());
    if (i == 0) { at = p.front().p; facing = p.front().yaw; return; }
    if (i >= p.size()) { at = p.back().p; facing = p.back().yaw; return; }
    const auto &a = p[i - 1], &b = p[i];
    const float k = static_cast<float>((s - a.s) / std::max(1e-4f, b.s - a.s));
    for (int x = 0; x < 3; ++x) at[x] = a.p[x] + (b.p[x] - a.p[x]) * k;
    facing = a.yaw + angle_between(a.yaw, b.yaw) * k;
}
float course_speed(const Line &line, double s) {
    const auto &p = line.path;
    const auto i = static_cast<std::size_t>(
        std::upper_bound(p.begin(), p.end(), s, [](double v, const Point &x) { return v < x.s; }) - p.begin());
    const float speed = p[std::min(i, p.size() - 1)].speed;
    return std::clamp(speed, least_speed, most_speed);
}
void blend_pose(const Pose &a, const Pose &b, float k, Pose &out) {
    out.root = blend(a.root, b.root, k);
    if (a.skater.size() != b.skater.size() || a.board.size() != b.board.size()) {
        if (&out != &a) {
            out.skater = a.skater;
            out.board = a.board;
        }
        return;
    }
    out.skater.resize(a.skater.size());
    for (std::size_t j = 0; j < out.skater.size(); ++j) out.skater[j] = blend(a.skater[j], b.skater[j], k);
    out.board.resize(a.board.size());
    for (std::size_t j = 0; j < out.board.size(); ++j) out.board[j] = blend(a.board[j], b.board[j], k);
}
// Whether the course was ridden in the air `s` metres along.
bool air_at(const Line &line, double s) {
    const auto &p = line.path;
    const auto i = static_cast<std::size_t>(
        std::upper_bound(p.begin(), p.end(), s, [](double v, const Point &x) { return v < x.s; }) - p.begin());
    return !p.empty() && p[std::min(i, p.size() - 1)].air && p[i ? i - 1 : 0].air;
}

// ---- the riding library --------------------------------------------------------------------------
// What the riders' bodies do. Never the skater's own ride where it rode it (its tricks, its falls):
// stretches of its plain riding, from every ride it records (kept on disk, so each race has more),
// rolling and pushing; a rider loops one of each, and crouches in the air.
struct Style {
    std::vector<Frame> frames; // from 0
    double seconds{};
    float speed{}, travel{}; // how fast it went, and which way (radians, 0 = +Z)
    float look{};            // which way the skater faced as it began (an emote, on the spot)
    float slope{};           // its board's pitch along that way (radians, nose up positive)
    std::size_t crouch{};    // its most crouched pose (the lowest head)
};
using StylePtr = std::shared_ptr<const Style>;
struct Library {
    std::vector<StylePtr> rolling, pushing, tucking, flying, stumbling, emoting; // newest first
    bool loaded{};
};
Library &library() {
    static auto *value = new Library;
    return *value;
}
// The head's height above the skater's placement (skeleton joint 1, AITrajectory), composed through
// the animation skeleton (Extension/HallOfMeat/hall_of_meat_skeleton.h): hips 7, the spine 42-45,
// the neck 101-102, the head 103. NaN for a pose of another skeleton.
float head_height(const Pose &pose) {
    constexpr std::array<std::size_t, 8> chain{7, 42, 43, 44, 45, 101, 102, 103};
    if (pose.skater.size() != 395) return std::numeric_limits<float>::quiet_NaN();
    const auto &placement = pose.skater[1];
    Quat q = placement.rotation;
    float scale = (placement.scale[0] + placement.scale[1] + placement.scale[2]) / 3.0f;
    float height = 0.0f;
    for (const auto j : chain) {
        const auto &bone = pose.skater[j];
        height += turned(q, {bone.position[0] * scale, bone.position[1] * scale, bone.position[2] * scale})[1];
        q = multiply(q, bone.rotation);
        scale *= (bone.scale[0] + bone.scale[1] + bone.scale[2]) / 3.0f;
    }
    return height;
}
// How a stretch stands: its head's mean height, and how far it bobs.
std::pair<float, float> stance(const std::vector<Frame> &f, std::size_t i, std::size_t k) {
    float sum{}, low = std::numeric_limits<float>::max(), high = -low;
    int n{};
    for (std::size_t j = i; j <= k && j < f.size(); ++j) {
        const float h = head_height(f[j].pose);
        if (!std::isfinite(h)) continue;
        sum += h;
        ++n;
        low = std::min(low, h);
        high = std::max(high, h);
    }
    if (!n) return {std::numeric_limits<float>::quiet_NaN(), 0.0f};
    return {sum / static_cast<float>(n), high - low};
}
Vec forward_of(const Quat &q) { // a rotation's +Z
    return {2.0f * (q[0] * q[2] + q[3] * q[1]), 2.0f * (q[1] * q[2] - q[3] * q[0]), 1.0f - 2.0f * (q[0] * q[0] + q[1] * q[1])};
}
// Frames i..k as a stretch to ride with: all plain (pushing too only for `push`), at least `least`
// fast; how far it is from a steady one (0: straight, level, the board flat along its way).
std::optional<float> stretch(const std::vector<Frame> &f, std::size_t i, std::size_t k, bool push, float least, float give = 1.0f,
                             float upright = std::numeric_limits<float>::quiet_NaN()) {
    if (k <= i + 4 || k >= f.size()) return std::nullopt;
    if (!push && std::isfinite(upright)) {
        // Standing (rolling) or held low (tucking), never bobbing.
        const auto [head, bob] = stance(f, i, k);
        if (!std::isfinite(head) || bob > upright_bob * give) return std::nullopt;
    }
    const auto &a = f[i].pose.root.position, &b = f[k].pose.root.position;
    const float seconds = static_cast<float>(f[k].t - f[i].t);
    float length{}, rise{}, pitch{};
    for (std::size_t j = i; j <= k; ++j) {
        if (!f[j].plain || f[j].air || f[j].fallen || (!push && f[j].push)) return std::nullopt;
        if (j < k) length += distance(f[j].pose.root.position, f[j + 1].pose.root.position);
        const float k01 = static_cast<float>((f[j].t - f[i].t) / std::max(1e-3f, seconds));
        rise = std::max(rise, std::abs(f[j].pose.root.position[1] - (a[1] + (b[1] - a[1]) * k01)));
    }
    if (seconds <= 0.2f || length / seconds < least) return std::nullopt;
    const Vec way{b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    const float n = std::sqrt(way[0] * way[0] + way[1] * way[1] + way[2] * way[2]);
    if (n < 0.5f) return std::nullopt;
    // The board's pitch against the way it goes (a manual rides one end; a trick turns it).
    for (std::size_t j = i; j <= k; ++j) {
        if (f[j].pose.board.empty()) continue;
        const auto fw = forward_of(f[j].pose.board.front().rotation);
        const float along = (fw[0] * way[0] + fw[1] * way[1] + fw[2] * way[2]) / n;
        pitch = std::max(pitch, std::abs((along >= 0 ? fw[1] : -fw[1]) - way[1] / n));
    }
    // Turning: the way it went over its first and last half.
    const auto &m = f[(i + k) / 2].pose.root.position;
    const float turn = std::abs(angle_between(std::atan2(m[0] - a[0], m[2] - a[2]), std::atan2(b[0] - m[0], b[2] - m[2])));
    if (!push && (turn > rolling_turn * give || rise > rolling_rise * give || pitch > rolling_pitch * give)) return std::nullopt;
    if (push && (rise > rolling_rise || pitch > rolling_pitch * 1.5f)) return std::nullopt;
    return turn + rise * 2.0f + pitch * 3.0f;
}
Style make_style(const std::vector<Frame> &f, std::size_t i, std::size_t k) {
    Style st;
    const double t0 = f[i].t;
    float lowest = std::numeric_limits<float>::max(), length{};
    for (std::size_t j = i; j <= k; ++j) {
        st.frames.push_back(f[j]);
        st.frames.back().t -= t0;
        if (j < k) length += distance(f[j].pose.root.position, f[j + 1].pose.root.position);
        if (const float head = head_height(f[j].pose); std::isfinite(head) && head < lowest) {
            lowest = head;
            st.crouch = j - i;
        }
    }
    st.seconds = st.frames.back().t;
    st.speed = length / static_cast<float>(std::max(1e-3, st.seconds));
    st.look = yaw(f[i].pose.root.rotation);
    const auto &a = f[i].pose.root.position, &b = f[k].pose.root.position;
    st.travel = std::atan2(b[0] - a[0], b[2] - a[2]);
    // The board's pitch along the way it went (its +Z may point either way along it).
    float rise{};
    int n{};
    for (const auto &frame : st.frames) {
        if (frame.pose.board.empty()) continue;
        const auto fw = forward_of(frame.pose.board.front().rotation);
        const float along = fw[0] * std::sin(st.travel) + fw[2] * std::cos(st.travel);
        rise += along >= 0 ? fw[1] : -fw[1];
        ++n;
    }
    if (n) st.slope = std::asin(std::clamp(rise / static_cast<float>(n), -1.0f, 1.0f));
    return st;
}
// The best stretches of a ride: rolling ones, and pushing ones from each push's start; apart.
// Frames i..k as an air to fly: nothing gone wrong, the board under the feet all the way.
bool clean_air(const std::vector<Frame> &f, std::size_t i, std::size_t k) {
    std::optional<float> held; // the board's heading against the body's
    for (std::size_t j = i; j <= k && j < f.size(); ++j) {
        const auto &pose = f[j].pose;
        if (f[j].fallen || pose.board.empty()) return false;
        const auto &board = pose.board.front().rotation;
        if (turned(board, {0.0f, 1.0f, 0.0f})[1] < air_board_up) return false;
        const float against = angle_between(yaw(pose.root.rotation), yaw(board));
        if (!held) held = against;
        else if (std::abs(angle_between(*held, against)) > air_board_turn) return false;
    }
    return true;
}
void gather(const std::vector<Frame> &f, std::vector<Style> &rolling, std::vector<Style> &pushing, std::vector<Style> &tucking,
            std::vector<Style> &flying) {
    for (std::size_t i = 0; i < f.size() && flying.size() < styles_a_ride;) {
        if (!f[i].air || f[i].fallen) {
            ++i;
            continue;
        }
        std::size_t k = i;
        while (k + 1 < f.size() && f[k + 1].air && !f[k + 1].fallen) ++k;
        const auto from = i >= air_lead ? i - air_lead : 0, to = std::min(f.size() - 1, k + air_lead);
        if (f[k].t - f[i].t >= least_air && clean_air(f, from, to)) flying.push_back(make_style(f, from, to));
        i = k + 1;
    }
    const auto end_of = [&](std::size_t i, double seconds) {
        std::size_t k = i;
        while (k < f.size() && f[k].t < f[i].t + seconds) ++k;
        return k;
    };
    // Riding upright: the head's height in the ride's tallest tenth of its plain riding.
    std::vector<float> heads;
    for (const auto &frame : f)
        if (frame.plain && !frame.air)
            if (const float h = head_height(frame.pose); std::isfinite(h)) heads.push_back(h);
    float upright = std::numeric_limits<float>::quiet_NaN();
    if (heads.size() >= 10) {
        std::sort(heads.begin(), heads.end());
        upright = heads[heads.size() * 9 / 10];
    }
    std::vector<std::pair<float, std::size_t>> found;
    for (std::size_t i = 0; i < f.size(); i += 2)
        if (const auto score = stretch(f, i, end_of(i, rolling_window), false, rolling_least_speed, 1.0f, upright))
            found.emplace_back(*score, i);
    // None steady enough: the least bad, slower, shorter, turning more (a body all the same).
    double window = rolling_window;
    if (found.empty()) {
        window = rolling_window * 0.7;
        for (std::size_t i = 0; i < f.size(); i += 2)
            if (const auto score = stretch(f, i, end_of(i, window), false, rolling_least_speed * 0.7f, 1.8f, upright))
                found.emplace_back(*score, i);
    }
    std::sort(found.begin(), found.end());
    std::vector<std::pair<std::size_t, std::size_t>> taken;
    for (const auto &[score, i] : found) {
        if (rolling.size() >= styles_a_ride && tucking.size() >= styles_a_ride) break;
        const auto k = end_of(i, window);
        if (std::any_of(taken.begin(), taken.end(), [&](const auto &x) { return i <= x.second && k >= x.first; })) continue;
        const float head = stance(f, i, k).first;
        const bool standing = !std::isfinite(upright) || head >= std::max(upright - upright_give, upright_least);
        auto &into = standing ? rolling : tucking;
        if (into.size() >= styles_a_ride) continue;
        taken.emplace_back(i, k);
        into.push_back(make_style(f, i, k));
    }
    for (std::size_t i = 1; i < f.size() && pushing.size() < styles_a_ride; ++i) {
        if (!f[i].push || f[i - 1].push) continue;
        const auto from = i >= 3 ? i - 3 : 0, k = end_of(from, pushing_window);
        if (stretch(f, from, k, true, 1.0f)) {
            pushing.push_back(make_style(f, from, k));
            i = k;
        }
    }
}
// The library on disk beside the last fall: riding.bin.
std::filesystem::path library_path();
constexpr std::uint32_t library_magic = 0x45444952; // "RIDE"
void write_frames(std::ofstream &out, const std::vector<Frame> &frames);
bool read_frames(std::ifstream &in, std::vector<Frame> &frames, std::uint32_t most);
void save_library(const Library &lib) {
    try {
        const auto path = library_path();
        if (path.empty()) return;
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        const std::uint32_t count = static_cast<std::uint32_t>(lib.rolling.size() + lib.pushing.size() + lib.tucking.size() +
                                                              lib.flying.size() + lib.stumbling.size() + lib.emoting.size());
        out.write(reinterpret_cast<const char *>(&library_magic), 4);
        out.write(reinterpret_cast<const char *>(&count), 4);
        const std::array<const std::vector<StylePtr> *, 6> kinds{&lib.rolling, &lib.pushing, &lib.tucking,
                                                                 &lib.flying, &lib.stumbling, &lib.emoting};
        for (std::uint8_t which = 0; which < kinds.size(); ++which)
            for (const auto &st : *kinds[which]) {
                out.write(reinterpret_cast<const char *>(&which), 1);
                write_frames(out, st->frames);
            }
    } catch (...) {}
}
StylePtr restyled(std::vector<Frame> frames) {
    if (frames.size() < 5) return nullptr;
    auto st = std::make_shared<Style>(make_style(frames, 0, frames.size() - 1));
    return st;
}
void load_library(Library &lib) {
    lib.loaded = true;
    try {
        std::ifstream in(library_path(), std::ios::binary);
        std::uint32_t magic{}, count{};
        in.read(reinterpret_cast<char *>(&magic), 4);
        in.read(reinterpret_cast<char *>(&count), 4);
        if (!in || magic != library_magic ||
            count > most_rolling + most_pushing + most_tucking + most_flying + most_stumbling + most_emoting)
            return;
        int moved{}, dropped{};
        for (std::uint32_t c = 0; c < count; ++c) {
            std::uint8_t which{};
            in.read(reinterpret_cast<char *>(&which), 1);
            std::vector<Frame> frames;
            if (!in || !read_frames(in, frames, 400) || which > 5) return;
            auto st = restyled(std::move(frames));
            if (!st) continue;
            if (which == 0 || which == 2) {
                // Sorted again by its stance (rolling kept before tucks existed may be a tuck): a
                // bobbing one goes.
                const auto [head, bob] = stance(st->frames, 0, st->frames.size() - 1);
                if (!std::isfinite(head) || bob > upright_bob) {
                    ++dropped;
                    continue;
                }
                const std::uint8_t now = head >= upright_least ? 0 : 2;
                moved += now != which;
                which = now;
            }
            const std::array<std::vector<StylePtr> *, 6> kinds{&lib.rolling, &lib.pushing, &lib.tucking,
                                                           &lib.flying, &lib.stumbling, &lib.emoting};
            kinds[which]->push_back(std::move(st));
        }
        if (moved || dropped) save_library(lib);
        say(std::format("Death Race: riding library: {} rolling, {} pushing, {} tucking, {} flying, {} stumbling and {} emoting "
                        "stretch(es) kept from before{}.",
                        lib.rolling.size(), lib.pushing.size(), lib.tucking.size(), lib.flying.size(), lib.stumbling.size(),
                        lib.emoting.size(),
                        moved || dropped ? std::format(" ({} re-sorted, {} dropped)", moved, dropped) : std::string()));
    } catch (...) {}
}
const Library &loaded_library() {
    auto &lib = library();
    if (!lib.loaded) load_library(lib);
    return lib;
}
// A ride's stretches into the library.
void learn(const std::vector<Frame> &frames) {
    auto &lib = library();
    if (!lib.loaded) load_library(lib);
    std::vector<Style> rolling, pushing, tucking, flying;
    gather(frames, rolling, pushing, tucking, flying);
    for (auto &st : rolling) lib.rolling.insert(lib.rolling.begin(), std::make_shared<Style>(std::move(st)));
    for (auto &st : pushing) lib.pushing.insert(lib.pushing.begin(), std::make_shared<Style>(std::move(st)));
    for (auto &st : tucking) lib.tucking.insert(lib.tucking.begin(), std::make_shared<Style>(std::move(st)));
    for (auto &st : flying) lib.flying.insert(lib.flying.begin(), std::make_shared<Style>(std::move(st)));
    if (lib.rolling.size() > most_rolling) lib.rolling.resize(most_rolling);
    if (lib.pushing.size() > most_pushing) lib.pushing.resize(most_pushing);
    if (lib.tucking.size() > most_tucking) lib.tucking.resize(most_tucking);
    if (lib.flying.size() > most_flying) lib.flying.resize(most_flying);
    if (!rolling.empty() || !pushing.empty() || !tucking.empty() || !flying.empty()) save_library(lib);
    say(std::format("Death Race: riding library: {} rolling, {} pushing, {} tucking and {} flying stretch(es) "
                    "(+{}, +{}, +{} and +{} from this ride).",
                    lib.rolling.size(), lib.pushing.size(), lib.tucking.size(), lib.flying.size(), rolling.size(), pushing.size(),
                    tucking.size(), flying.size()));
}
// A whole pose (body and board) pitched nose up by `angle` about its board, for a heading `facing`.
void pitch(Pose &pose, float facing, float angle) {
    if (std::abs(angle) < 1e-4f || pose.board.empty()) return;
    // About its right, (cos f, 0, -sin f): turning by -angle lifts the nose.
    const float h = -angle * 0.5f;
    const Quat q{std::cos(facing) * std::sin(h), 0.0f, -std::sin(facing) * std::sin(h), std::cos(h)};
    const Vec pivot = pose.board.front().position;
    const auto move = [&](Transform &x) {
        const auto d = turned(q, {x.position[0] - pivot[0], x.position[1] - pivot[1], x.position[2] - pivot[2]});
        x.position = {pivot[0] + d[0], pivot[1] + d[1], pivot[2] + d[2]};
        x.rotation = multiply(q, x.rotation);
    };
    move(pose.root);
    if (pose.skater.size() > 1) move(pose.skater[1]);
    move(pose.board.front());
    if (pose.board.size() > 2) move(pose.board[2]);
}
// A style looped at `t`, its ends crossfaded; turned so the way it went is `facing`, at `at`, its
// board along the ground's `slope` (radians, uphill positive).
void style_pose(const Style &st, double t, const Vec &at, float facing, float slope, Pose &out, Pose &scratch) {
    const auto &f = st.frames;
    const float turn = angle_between(st.travel, facing);
    const auto take = [&](double x, Pose &into) {
        sample(f, x, into);
        const auto origin = into.root.position;
        place(into, origin, at, turn);
        pitch(into, facing, slope - st.slope);
    };
    if (f.size() < 2 || st.seconds <= 0.2) {
        out = f.front().pose;
        const auto origin = out.root.position;
        place(out, origin, at, turn);
        pitch(out, facing, slope - st.slope);
        return;
    }
    const double fade = std::min(style_fade, st.seconds * 0.3), loop = st.seconds - fade;
    const double ct = std::fmod(std::max(0.0, t), loop);
    take(ct, out);
    if (ct >= fade) return;
    take(ct + loop, scratch);
    blend_pose(scratch, out, static_cast<float>(ct / fade), out);
}
// Its most crouched pose, as a rider holds it through the air.
void crouch_pose(const Style &st, const Vec &at, float facing, Pose &out) {
    out = st.frames[std::min(st.crouch, st.frames.size() - 1)].pose;
    const auto origin = out.root.position;
    place(out, origin, at, angle_between(st.travel, facing));
    pitch(out, facing, -st.slope);
}
// A stumble `t` seconds in (no loop): turned so the way it went is `facing`, at `at`, its board
// along the ground's `slope`.
void once_pose(const Style &st, double t, const Vec &at, float facing, float slope, Pose &out) {
    sample(st.frames, std::clamp(t, 0.0, st.seconds), out);
    const auto origin = out.root.position;
    place(out, origin, at, angle_between(st.travel, facing));
    pitch(out, facing, slope - st.slope);
}
// An emote `t` seconds in, on the spot: facing `facing` as the skater faced as it began, on foot
// (its root a deck lower than a rider's on its board).
void emote_pose(const Style &st, double t, const Vec &at, float facing, Pose &out) {
    sample(st.frames, std::clamp(t, 0.0, st.seconds), out);
    const auto origin = out.root.position;
    place(out, origin, {at[0], at[1] - 0.1f, at[2]}, angle_between(st.look, facing));
}
// An air at `t` seconds into it (no loop): turned so the way it went is `facing`, at `at`, its
// board as the skater's was through it.
void air_pose(const Style &st, double t, const Vec &at, float facing, Pose &out) {
    sample(st.frames, std::clamp(t, 0.0, st.seconds), out);
    const auto origin = out.root.position;
    place(out, origin, at, angle_between(st.travel, facing));
}
// The stretch of the course ridden in the air round `s` metres along (from, to).
std::pair<double, double> air_span(const Line &line, double s) {
    const auto &p = line.path;
    auto i = static_cast<std::size_t>(
        std::upper_bound(p.begin(), p.end(), s, [](double v, const Point &x) { return v < x.s; }) - p.begin());
    i = std::min(i, p.size() - 1);
    std::size_t a = i, b = i;
    while (a > 0 && p[a - 1].air) --a;
    while (b + 1 < p.size() && p[b + 1].air) ++b;
    return {p[a].s, p[b].s};
}
// The course's slope `s` metres along (radians, uphill positive), over a few metres.
float slope_at(const Line &line, double s) {
    Vec a{}, b{};
    float unused{};
    course_at(line, std::max(0.0, s - 1.5), a, unused);
    course_at(line, std::min<double>(line.length, s + 1.5), b, unused);
    const float run = std::hypot(b[0] - a[0], b[2] - a[2]);
    return run > 0.5f ? std::atan2(b[1] - a[1], run) : 0.0f;
}

// ---- the riders ----------------------------------------------------------------------------------
struct Rider {
    bool shown{};
    double retry{}, spawned{};
    unsigned dressings{};
    double delay{};              // seconds its falls cost it
    double fell = -1.0;          // when its fall began (< 0: riding)
    double fall_length{};        // how long that fall lasts, getting up included
    double next_fall{};          // no second fall before this
    Vec fell_at{};               // where it fell, and which way it faced
    float fell_facing{};
    double fell_s{};   // how far along the course it fell, and at what speed
    float fell_speed{};
    double relaunch{}; // the race's clock when it last set off (the go, or back on its board)
    // A wobble: since when (< 0: none), how far it sways first, and toward which way (world, flat).
    double wobble_at = -1.0;
    float wobble{};
    Vec wobble_toward{};
    std::shared_ptr<const Clip> clip; // the fall it plays
    std::optional<multiplayer::Appearance> outfit; // what it wears: the skater's own, each piece drawn again
    // A shove: where to (metres), when (< 0: none), its stagger, and the slowing it brings.
    Vec push{};
    double pushed = -1.0;
    float stagger{};
    double drag_until{}, drag{}, last_tick{};
    // On the course: metres along it, the time in its riding loop, when it finished (< 0: not yet).
    double s{}, cycle_t{}, finished = -1.0;
    float speed{};
    // Its body (the riding library): its rolling and pushing stretches, where it is in its push,
    // how far it is into pushing and into its crouch in the air (0 to 1), its lean into the turn.
    StylePtr rolling, pushing, tucking, flying;
    // The ground under its board (height, slope along its way; NaN: not found yet), and when next.
    float floor_offset = std::numeric_limits<float>::quiet_NaN(), floor_want{}, floor_slope{}, slope_want{};
    double probe_at{};
    // A stumble playing (since when); an emote playing (since when), when it was cut short, and
    // when the next may begin.
    StylePtr stumble, emote;
    double stumble_at = -1.0, emote_at{}, emote_next{}, emote_quit = -1.0;
    double push_t{};
    float push_k{}, air_k{}, tuck_k{}, turn_lean{};
    // Off the course (metres, right positive): where it is, how fast it moves across, the side it
    // chose when it last looked, whether that was keeping off something (and how hard it brakes
    // for it), and the ground there against the course's (metres up).
    float side{}, side_rate{}, plan{}, brake{1.0f}, braking{1.0f}, ground{}, ground_want{};
    bool evading{};
    double sight_at{};
    Vec at{}; // its root, as last shown
    // Its own way of riding this race (see pace_spread): drawn when it first shows.
    bool drawn{};
    double pace{1.0};
    float wander{}, wave1{}, phase1{};  // metres; radians a metre; radians
    double surge{}, beat1{}, beat2{}, beat_phase1{}, beat_phase2{}; // share; radians a second; radians
};
struct Riders {
    std::array<Rider, most_riders> riders{};
    std::optional<multiplayer::Appearance> look; // the local skater's own: each rider's is drawn from it
    std::mt19937 draw{std::random_device{}()};
    double next_look{};
    // The local skater's party-collision switch while the riders are out (put back after).
    std::uintptr_t core{};
    std::uint8_t core_before{};
    // The local skater's last root and when, for its speed.
    std::optional<Vec> last_me;
    double last_me_at{};
    Vec my_velocity{};
    bool reported{}, failed{};
    double clock{}; // the race's, as last shown
    bool sight_reported{}, blind_reported{}, no_body_reported{};
    Pose pose, other, extra; // scratch
};
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
// The last fall is kept on disk, so riders can fall before the skater does this session.
std::filesystem::path fall_path() {
    PWSTR raw{};
    std::filesystem::path result;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw)) && raw)
        result = std::filesystem::path(raw) / L"ReSkate" / L"deathrace" / L"fall.bin";
    if (raw) CoTaskMemFree(raw);
    return result;
}
constexpr std::uint32_t fall_magic = 0x4c4c4146; // "FALL"
void write_transform(std::ofstream &out, const Transform &t) {
    out.write(reinterpret_cast<const char *>(t.position.data()), sizeof(t.position));
    out.write(reinterpret_cast<const char *>(t.rotation.data()), sizeof(t.rotation));
    out.write(reinterpret_cast<const char *>(t.scale.data()), sizeof(t.scale));
}
bool read_transform(std::ifstream &in, Transform &t) {
    in.read(reinterpret_cast<char *>(t.position.data()), sizeof(t.position));
    in.read(reinterpret_cast<char *>(t.rotation.data()), sizeof(t.rotation));
    in.read(reinterpret_cast<char *>(t.scale.data()), sizeof(t.scale));
    return static_cast<bool>(in) && std::isfinite(t.position[0]) && std::isfinite(t.rotation[3]);
}
void write_frames(std::ofstream &out, const std::vector<Frame> &frames) {
    const std::uint32_t count = static_cast<std::uint32_t>(frames.size());
    out.write(reinterpret_cast<const char *>(&count), 4);
    for (const auto &f : frames) {
        const std::uint32_t skater = static_cast<std::uint32_t>(f.pose.skater.size()), board = static_cast<std::uint32_t>(f.pose.board.size());
        out.write(reinterpret_cast<const char *>(&f.t), sizeof(f.t));
        out.write(reinterpret_cast<const char *>(&skater), 4);
        out.write(reinterpret_cast<const char *>(&board), 4);
        write_transform(out, f.pose.root);
        for (const auto &x : f.pose.skater) write_transform(out, x);
        for (const auto &x : f.pose.board) write_transform(out, x);
    }
}
bool read_frames(std::ifstream &in, std::vector<Frame> &frames, std::uint32_t most) {
    std::uint32_t count{};
    in.read(reinterpret_cast<char *>(&count), 4);
    if (!in || count > most) return false;
    for (std::uint32_t i = 0; i < count; ++i) {
        Frame f;
        std::uint32_t skater{}, board{};
        in.read(reinterpret_cast<char *>(&f.t), sizeof(f.t));
        in.read(reinterpret_cast<char *>(&skater), 4);
        in.read(reinterpret_cast<char *>(&board), 4);
        if (!in || skater > 1024 || board > 256 || !std::isfinite(f.t) || !read_transform(in, f.pose.root)) return false;
        f.pose.skater.resize(skater);
        f.pose.board.resize(board);
        for (auto &x : f.pose.skater) if (!read_transform(in, x)) return false;
        for (auto &x : f.pose.board) if (!read_transform(in, x)) return false;
        f.plain = f.board = true;
        frames.push_back(std::move(f));
    }
    return true;
}
std::filesystem::path library_path() {
    const auto fall = fall_path();
    return fall.empty() ? fall : fall.parent_path() / L"riding.bin";
}
void save_fall(const Clip &clip) {
    try {
        const auto path = fall_path();
        if (path.empty()) return;
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        const std::uint32_t count = static_cast<std::uint32_t>(clip.frames.size());
        out.write(reinterpret_cast<const char *>(&fall_magic), 4);
        out.write(reinterpret_cast<const char *>(&count), 4);
        for (const auto &f : clip.frames) {
            const std::uint32_t skater = static_cast<std::uint32_t>(f.pose.skater.size()), board = static_cast<std::uint32_t>(f.pose.board.size());
            out.write(reinterpret_cast<const char *>(&f.t), sizeof(f.t));
            out.write(reinterpret_cast<const char *>(&skater), 4);
            out.write(reinterpret_cast<const char *>(&board), 4);
            write_transform(out, f.pose.root);
            for (const auto &t : f.pose.skater) write_transform(out, t);
            for (const auto &t : f.pose.board) write_transform(out, t);
        }
    } catch (...) {}
}
std::shared_ptr<const Clip> load_fall() {
    try {
        std::ifstream in(fall_path(), std::ios::binary);
        std::uint32_t magic{}, count{};
        in.read(reinterpret_cast<char *>(&magic), 4);
        in.read(reinterpret_cast<char *>(&count), 4);
        if (!in || magic != fall_magic || count < 10 || count > 200) return nullptr;
        auto clip = std::make_shared<Clip>();
        for (std::uint32_t i = 0; i < count; ++i) {
            Frame f;
            std::uint32_t skater{}, board{};
            in.read(reinterpret_cast<char *>(&f.t), sizeof(f.t));
            in.read(reinterpret_cast<char *>(&skater), 4);
            in.read(reinterpret_cast<char *>(&board), 4);
            if (!in || skater > 1024 || board > 256 || !read_transform(in, f.pose.root)) return nullptr;
            f.pose.skater.resize(skater);
            f.pose.board.resize(board);
            for (auto &t : f.pose.skater) if (!read_transform(in, t)) return nullptr;
            for (auto &t : f.pose.board) if (!read_transform(in, t)) return nullptr;
            clip->frames.push_back(std::move(f));
        }
        clip->seconds = clip->frames.back().t;
        return clip;
    } catch (...) {
        return nullptr;
    }
}
// The game's own clothes and board parts by kind (Own_<Kind>_...: TopShirt, ShoeSneaker,
// DeckGraphic...), from the cosmetics catalog (Extension/Customization). Faces, hair, bodies and
// whole costumes are left as they are: they go with each other.
constexpr std::array<std::string_view, 9> drawn_kinds{"top", "bottom", "shoe", "hat", "glass", "sock", "deck", "truck", "wheel"};
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
// The skater's outfit with each of its clothes and board parts drawn again among the same kind.
// What the riders wear: ReSkate's ghost skater costume, over whatever else.
constexpr const char *rider_costume = "Own_Costume_Gen_GhostSkater_00001";
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
std::shared_ptr<const Clip> fall_clip() {
    auto &f = falls();
    static bool loaded{};
    if (!f.clip && !loaded) {
        loaded = true;
        f.clip = load_fall();
        if (f.clip) say("Death Race: the AI riders fall as the skater last did (kept from before).");
    }
    return f.clip;
}
} // namespace

void line_start(double now) {
    auto &r = recording();
    r = {};
    r.on = true;
    r.started = now;
}
namespace {
void measure_lift(const Pose &pose, double now);
}
void line_capture(std::uintptr_t base, std::uintptr_t client, double now, const Moment &moment) {
    auto &r = recording();
    if (!r.on || now < r.next) return;
    r.next = now + capture_interval;
    if (r.frames.size() >= longest_capture) {
        r.on = false;
        say("Death Race: the ride is too long to record any further.");
        return;
    }
    if (capture(base, client, r, now, moment) && moment.plain && !moment.air && !r.frames.empty()) measure_lift(r.frames.back().pose, now);
}
std::shared_ptr<const Line> line_stop() {
    auto &r = recording();
    r.on = false;
    auto frames = std::move(r.frames);
    r.frames.clear();
    if (frames.size() < 20) return nullptr;
    // From the last pose on the board before it set off: the riders wait on their boards.
    std::size_t board = 0;
    while (board < frames.size() && !frames[board].board) ++board;
    if (board == frames.size()) board = 0;
    std::size_t first = frames.size();
    for (std::size_t i = board + 1; i < frames.size(); ++i)
        if (distance(frames[i].pose.root.position, frames[board].pose.root.position) > set_off_distance) {
            first = i - 1;
            break;
        }
    if (first + 20 > frames.size()) return nullptr;
    // Where it last rode (on its board, rolling): what came after was stopping to set its finish.
    std::size_t last = frames.size() - 1;
    while (last > first + 20) {
        const auto &a = frames[last - 1], &b = frames[last];
        const double dt = std::max(1e-3, b.t - a.t);
        if (b.board && !b.fallen && distance(a.pose.root.position, b.pose.root.position) / dt > riding_speed) break;
        --last;
    }
    frames.resize(last + 1);
    auto line = std::make_shared<Line>();
    const double t0 = frames[first].t;
    for (std::size_t i = first; i < frames.size(); ++i) {
        frames[i].t -= t0;
        line->frames.push_back(std::move(frames[i]));
    }
    line->seconds = line->frames.back().t;
    // The way it sets off: from the start to where it is a few metres on.
    const auto &start = line->frames.front().pose.root.position;
    Vec ahead = line->frames.back().pose.root.position;
    for (const auto &f : line->frames)
        if (distance(f.pose.root.position, start) > 4.0f) {
            ahead = f.pose.root.position;
            break;
        }
    const float dx = ahead[0] - start[0], dz = ahead[2] - start[2], n = std::hypot(dx, dz);
    if (n > 1e-3f) line->side = {dz / n, 0.0f, -dx / n};
    shape(*line);
    // What the skater did on the way, by physics state (for the record), and how much was plain.
    std::map<std::uint32_t, int> states;
    int plain{}, air{}, push{}, fallen{};
    for (const auto &f : line->frames) {
        ++states[f.state];
        plain += f.plain;
        air += f.air;
        push += f.push;
        fallen += f.fallen;
    }
    std::string seen;
    for (const auto &[state, poses] : states) seen += std::format(" {}x{}", state, poses);
    say(std::format("Death Race: recorded a line of {:.1f} s ({} poses: {} plain, {} in the air, {} pushing, {} falling; states{}); course {:.0f} m.",
                    line->seconds, line->frames.size(), plain, air, push, fallen, seen, line->length));
    learn(line->frames);
    if (line->path.empty()) return nullptr;
    return line;
}
double line_seconds(const Line &line) noexcept { return line.seconds; }

void fall_capture(std::uintptr_t base, std::uintptr_t client, double now, bool fell) {
    auto &f = falls();
    auto &r = f.taking;
    if (fell && !r.on) {
        r = {};
        r.on = true;
        r.started = now;
    }
    if (!r.on || now < r.next) return;
    r.next = now + capture_interval;
    capture(base, client, r, now, Moment{});
    if (now - r.started < fall_seconds) return;
    r.on = false;
    if (r.frames.size() < 10) return;
    auto clip = std::make_shared<Clip>();
    clip->frames = std::move(r.frames);
    clip->seconds = clip->frames.back().t;
    save_fall(*clip);
    f.clip = std::move(clip);
    say("Death Race: recorded a fall for the AI riders.");
}

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
double ride_seconds(const Line &line, double pace) noexcept {
    double seconds = launch_seconds * 0.5; // picking up speed
    for (std::size_t j = 1; j < line.path.size(); ++j)
        seconds += (line.path[j].s - line.path[j - 1].s) / std::max(1.0, course_speed(line, line.path[j].s) * pace);
    return seconds;
}
std::size_t riders_shown() noexcept {
    const auto &r = riders();
    return static_cast<std::size_t>(std::count_if(r.riders.begin(), r.riders.end(), [](const Rider &x) { return x.shown; }));
}

namespace {
// ---- sight ---------------------------------------------------------------------------------------
// The course `s` metres along, `side` metres to its right (for a heading f, (cos f, 0, -sin f)).
Vec off_course(const Line &line, double s, float side) {
    Vec at{};
    float facing{};
    course_at(line, std::clamp(s, 0.0, static_cast<double>(line.length)), at, facing);
    at[0] += std::cos(facing) * side;
    at[2] -= std::sin(facing) * side;
    return at;
}
// Where a ray from `from` to `to` strikes something that is no skater (`bodies`: their roots).
struct Struck {
    bool seen{}, hit{};
    Vec at{};
};
Struck struck(const Vec &from, const Vec &to, const std::vector<Vec> &bodies) {
    const auto ray = cast_world_ray(from, to);
    if (!ray.cast) return {};
    if (!ray.hit) return {true};
    for (const auto &b : bodies)
        if (std::hypot(ray.at[0] - b[0], ray.at[2] - b[2]) < body_reach && ray.at[1] > b[1] - 0.3f && ray.at[1] < b[1] + 2.0f)
            return {true};
    return {true, true, ray.at};
}
// The way from `side` (where the rider is, `s` along) to `to`, `ahead` metres on: whether it is
// clear, knee and chest high, and if not how far on (from the rider) something stands.
struct Way {
    bool seen{}, clear{};
    float until{};
};
// How far along the way from `from` through `way` the skater `lead` stands in it (across, within
// pass_room; no nearer than lead_least), or -1.
float lead_in_way(const Vec &from, const std::array<Vec, 3> &way, const Vec &lead) {
    float gone = 0.0f;
    Vec a = from;
    for (const auto &b : way) {
        const float dx = b[0] - a[0], dz = b[2] - a[2], length = std::hypot(dx, dz);
        const float k = length > 1e-3f ? std::clamp(((lead[0] - a[0]) * dx + (lead[2] - a[2]) * dz) / (length * length), 0.0f, 1.0f) : 0.0f;
        const float along = gone + k * length;
        if (std::hypot(a[0] + dx * k - lead[0], a[2] + dz * k - lead[2]) < pass_room && along >= lead_least &&
            std::abs(lead[1] - (a[1] + (b[1] - a[1]) * k)) < 2.0f)
            return along;
        gone += length;
        a = b;
    }
    return -1.0f;
}
// `rays`: false on the course, where the skater rode (over whatever it jumped); `lead`: the local
// skater, when the rider is catching it up.
Way look_along(const Line &line, double s, float side, float to, float ahead, const std::vector<Vec> &bodies,
               const Vec *lead, bool rays) {
    const std::array<Vec, 3> way{off_course(line, s + sight_from, side),
                                 off_course(line, s + sight_from + ahead * 0.5, (side + to) * 0.5f),
                                 off_course(line, s + sight_from + ahead, to)};
    float until = lead ? lead_in_way(off_course(line, s, side), way, *lead) : -1.0f;
    float gone = sight_from;
    for (std::size_t k = 0; rays && k + 1 < way.size(); ++k) {
        float nearest = -1.0f;
        for (const float h : {sight_low, sight_high}) {
            const auto hit = struck({way[k][0], way[k][1] + h, way[k][2]}, {way[k + 1][0], way[k + 1][1] + h, way[k + 1][2]}, bodies);
            if (!hit.seen) return {};
            if (!hit.hit) continue;
            const float d = std::hypot(hit.at[0] - way[k][0], hit.at[2] - way[k][2]);
            if (nearest < 0 || d < nearest) nearest = d;
        }
        if (nearest >= 0) {
            if (until < 0 || gone + nearest < until) until = gone + nearest;
            break;
        }
        gone += distance(way[k], way[k + 1]);
    }
    if (until >= 0) return {true, false, until};
    return {true, true, sight_from + ahead};
}
// The ground under a board's middle `centre` on a heading `facing`: two rays down, probe_reach
// ahead of and behind it (from a metre above it to two below): the height under the middle, and
// its slope along the way (radians, uphill positive).
std::optional<std::pair<float, float>> ground_under(const Vec &centre, float facing, const std::vector<Vec> &bodies) {
    const float fx = std::sin(facing) * probe_reach, fz = std::cos(facing) * probe_reach;
    const auto down = [&](float dx, float dz) -> std::optional<float> {
        const auto hit = struck({centre[0] + dx, centre[1] + 1.0f, centre[2] + dz}, {centre[0] + dx, centre[1] - 2.0f, centre[2] + dz}, bodies);
        if (!hit.hit) return std::nullopt;
        return hit.at[1];
    };
    const auto ahead = down(fx, fz), behind = down(-fx, -fz);
    if (!ahead || !behind) return std::nullopt;
    return std::pair{(*ahead + *behind) * 0.5f, std::atan2(*ahead - *behind, 2.0f * probe_reach)};
}
// How high a board's middle rides over the ground (see first_lift).
struct Lift {
    std::vector<float> seen;
    float value = first_lift;
    double next{};
};
Lift &lift() {
    static auto *value = new Lift;
    return *value;
}
// The skater's own board, riding on the flat: how high its middle is over the ground.
void measure_lift(const Pose &pose, double now) {
    auto &l = lift();
    if (now < l.next || pose.board.empty()) return;
    l.next = now + lift_every;
    const auto &board = pose.board.front();
    const auto &root = pose.root.position;
    if (std::hypot(board.position[0] - root[0], board.position[2] - root[2]) >= 0.7f) return;
    const auto fw = forward_of(board.rotation);
    const auto under = ground_under(board.position, std::atan2(fw[0], fw[2]), {});
    if (!under || std::abs(under->second) > flat_ground) return;
    const float height = board.position[1] - under->first;
    if (height < -0.05f || height > 0.4f) return;
    l.seen.push_back(height);
    if (l.seen.size() > 200) l.seen.erase(l.seen.begin());
    auto sorted = l.seen;
    std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(sorted.size() / 2), sorted.end());
    const float before = l.value;
    l.value = sorted[sorted.size() / 2];
    if (l.seen.size() == 5 || std::abs(l.value - before) > 0.02f)
        say(std::format("Death Race: a board's middle rides {:.3f} m over the ground ({} look(s)).", l.value, l.seen.size()));
}
// A rider's board on the ground under it, its body with it: pitched to the ground's slope along
// `facing`, then raised or lowered so its wheels are on it. The course (`reference`, the height its
// root is placed at) carries it smoothly from one look to the next; each look (now and then,
// within the tick's budget) only finds how far the ground is from it there, which the rider eases
// into (a jump of more than ground_jump at once). Not while its board is not under it (an emote,
// carried); without any ground found yet, the board where the skater's was (`fallback`).
void snap_board(Pose &pose, Rider &rider, float facing, const std::vector<Vec> &bodies, int &probes, double now, double dt,
                float reference, float fallback) {
    if (pose.board.empty()) return;
    const auto &board = pose.board.front();
    const auto &root = pose.root.position;
    if (std::hypot(board.position[0] - root[0], board.position[2] - root[2]) >= 0.7f) return;
    if (probes < probes_a_tick && now >= rider.probe_at) {
        ++probes;
        rider.probe_at = now + probe_every;
        if (const auto under = ground_under(board.position, facing, bodies)) {
            const auto [y, slope] = *under;
            rider.floor_want = y + lift().value - reference;
            rider.slope_want = slope;
            if (!std::isfinite(rider.floor_offset) || std::abs(rider.floor_want - rider.floor_offset) > ground_jump) {
                rider.floor_offset = rider.floor_want;
                rider.floor_slope = slope;
            }
        }
    }
    if (!std::isfinite(rider.floor_offset)) {
        on_deck(pose, fallback);
        return;
    }
    const float k = static_cast<float>(std::min(1.0, dt * ground_settle));
    rider.floor_offset += (rider.floor_want - rider.floor_offset) * k;
    rider.floor_slope += (rider.slope_want - rider.floor_slope) * k;
    // Pitched to the ground: from the board's own pitch along the way.
    const auto fw = forward_of(board.rotation);
    const float along = fw[0] * std::sin(facing) + fw[2] * std::cos(facing);
    const float now_pitch = std::asin(std::clamp(along >= 0 ? fw[1] : -fw[1], -1.0f, 1.0f));
    pitch(pose, facing, rider.floor_slope - now_pitch);
    multiplayer::offset_pose(pose, {0.0f, reference + rider.floor_offset - pose.board.front().position[1], 0.0f});
}
// The ground's height under `at` (from a metre above it to a metre below), when it is no skater.
std::optional<float> ground_at(const Vec &at, const std::vector<Vec> &bodies) {
    const auto hit = struck({at[0], at[1] + 1.0f, at[2]}, {at[0], at[1] - 1.0f, at[2]}, bodies);
    if (!hit.hit) return std::nullopt;
    return hit.at[1];
}
// The ground where a rider is heading (or stands, parked past the finish) against the course's
// there: its board on the kerb, not under it.
void ground(const Line &line, Rider &rider, const std::vector<Vec> &bodies) {
    const double s = std::clamp(rider.s + (rider.s < line.length ? ground_ahead : 0.0), 0.0, static_cast<double>(line.length));
    if (std::abs(rider.side) < on_course) {
        rider.ground_want = 0.0f;
        return;
    }
    const auto there = ground_at(off_course(line, s, rider.side), bodies);
    const auto course = ground_at(off_course(line, s, 0.0f), bodies);
    if (there && course) rider.ground_want = std::clamp(*there - *course, -ground_step, ground_step);
}
// A rider on the course, `s` along at `speed`, looks ahead: the side it means to ride (`want`),
// where it is, or the course itself, whichever way is clear first; when none is, the one clear the
// furthest, braking for it. False when the world could not be asked.
bool look(const Line &line, Rider &rider, float want, const std::vector<Vec> &bodies, const Vec *lead, int index) {
    const float ahead = std::clamp(rider.speed * static_cast<float>(sight_seconds), sight_least, sight_most);
    const double s = std::max(0.0, rider.s);
    std::array<float, 5> choices{want, rider.side, 0.0f, rider.side + pass_side, rider.side - pass_side};
    float best = 0.0f, best_until = -1.0f;
    bool chosen{}, kept_off{}; // kept_off: its own line was not clear
    for (std::size_t c = 0; c < choices.size() && !chosen; ++c) {
        const float to = choices[c];
        bool repeat{};
        for (std::size_t d = 0; d < c; ++d) repeat = repeat || std::abs(choices[d] - to) < 0.1f;
        if (repeat) continue;
        // On the course: where the skater rode (over whatever it jumped), unless it is ahead there.
        const bool course = std::abs(to) < on_course && std::abs(rider.side) < on_course;
        const auto way = look_along(line, s, rider.side, to, ahead, bodies, lead, !course);
        if (!way.seen) return false;
        if (way.clear) {
            best = to;
            chosen = true;
        } else {
            kept_off = kept_off || c == 0;
            if (way.until > best_until) {
                best = to;
                best_until = way.until;
            }
        }
    }
    const bool was = rider.evading;
    rider.plan = best;
    rider.evading = kept_off || !chosen;
    rider.brake = chosen ? 1.0f : std::clamp(best_until / (ahead + sight_from), brake_least, 1.0f);
    if (rider.evading && !was)
        say(std::format("Death Race: AI rider {} sees something ahead; keeping off it ({:.1f} m across{}).", index + 1, best,
                        chosen ? "" : std::format(", braking: {:.1f} m to go", best_until)));
    ground(line, rider, bodies);
    return true;
}
} // namespace

namespace {
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
} // namespace

namespace {
// ---- each tick ------------------------------------------------------------------------------------
// What every rider's steps share in one tick (show_riders).
struct Tick {
    std::uintptr_t base{}, client{};
    const Line &line;
    const std::array<float, 3> *start{}; // the race's start line, when known
    double clock{}, now{};
    multiplayer::NativeFrame local;
    Vec me{};                // the local skater's root
    std::vector<Vec> bodies; // the skaters the riders' rays may strike (no obstacle): the local one and the riders
    std::uint64_t us{};
    int sights{}, probes{};  // looks ahead and ground probes taken this tick (each has a budget)
    bool native_pass{};      // one spawn or outfit a tick: each stalls the frame a little
};
// One rider's tick: since its last, whether it lies fallen, how far into the ride's replay it is
// (no course), and how it lines up across the start line (on it, `lead` metres before the course
// begins, until `early` falls to 0 after the go; again at the finish).
struct Step {
    int index{};
    double dt{}, t{};
    bool falling{}, course{};
    Vec course_start{};
    float lead{}, early{}, lineup{};
};

// Its own way of riding this race (see pace_spread): drawn when it first shows.
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
    // Its own stretches of riding: another one than the rider before it, where there are.
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
    // Its speed rises and falls a little along the way: overtaking, and being overtaken.
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
// Its body at `at`, heading `facing` leant `tilt`: rolling; pushing while slow and as it sets off;
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
    // Its right, for a heading `facing` (0 = +Z): (cos, 0, -sin).
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
    // Its capsule's foot: under its root, at its board when the board is under it (else where the
    // board would be).
    Vec feet = r.pose.root.position;
    feet[1] -= root_above_board + deck_height;
    if (!r.pose.board.empty()) {
        const auto &board = r.pose.board.front().position;
        if (std::hypot(board[0] - feet[0], board[2] - feet[2]) < 0.7f) feet[1] = board[1] - deck_height;
    }
    multiplayer::update_remote_collision(tick.base, tick.local.context, r.pose, close, tick.us, &feet, body_radius);
}
} // namespace

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
                // Its fall: the local skater's own, turned and moved to where it fell.
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
