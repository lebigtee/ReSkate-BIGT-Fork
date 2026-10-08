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

// The race's line: recording the local skater's ride, its poses, and the course shaped from it
// (where the riders ride, how fast, how high its board is).
namespace dingosdk::death_race::riding {
void say(const std::string &text) { logging::write(logging::Level::info, logging::Channel::skater, text); }
float distance(const Vec &a, const Vec &b) {
    return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}
Recording &recording() {
    static auto *value = new Recording;
    return *value;
}
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
} // namespace dingosdk::death_race::riding

namespace dingosdk::death_race {
using namespace riding;

void line_start(double now) {
    auto &r = recording();
    r = {};
    r.on = true;
    r.started = now;
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
double ride_seconds(const Line &line, double pace) noexcept {
    double seconds = launch_seconds * 0.5; // picking up speed
    for (std::size_t j = 1; j < line.path.size(); ++j)
        seconds += (line.path[j].s - line.path[j - 1].s) / std::max(1.0, course_speed(line, line.path[j].s) * pace);
    return seconds;
}
} // namespace dingosdk::death_race
