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

// What the riders see: the game's collision rays ahead of them, the ground under them, and how
// high the skater's board rides above its feet.
namespace dingosdk::death_race::riding {
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
Struck struck(const Vec &from, const Vec &to, const std::vector<Vec> &bodies) {
    const auto ray = cast_world_ray(from, to);
    if (!ray.cast) return {};
    if (!ray.hit) return {true};
    for (const auto &b : bodies)
        if (std::hypot(ray.at[0] - b[0], ray.at[2] - b[2]) < body_reach && ray.at[1] > b[1] - 0.3f && ray.at[1] < b[1] + 2.0f)
            return {true};
    return {true, true, ray.at};
}
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
} // namespace dingosdk::death_race::riding
