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

// The riding library: stretches of the skater's own riding (rolling, pushing, tucking, flying,
// sketchy landings, emotes) the riders' bodies are made of, kept between sessions; and the
// skater's last fall, which the riders fall the same way.
namespace dingosdk::death_race::riding {
Library &library() {
    static auto *value = new Library;
    return *value;
}
// The head's height above the skater's placement (skeleton joint 1, AITrajectory), composed through
// the animation skeleton (Extension/Boneyard/boneyard_skeleton.h): hips 7, the spine 42-45,
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
std::optional<float> stretch(const std::vector<Frame> &f, std::size_t i, std::size_t k, bool push, float least, float give,
                             float upright) {
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
// A style's most crouched pose, as a rider holds it through the air.
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
// The last fall is kept on disk, so riders can fall before the skater does this session.
std::filesystem::path fall_path() {
    PWSTR raw{};
    std::filesystem::path result;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw)) && raw)
        result = std::filesystem::path(raw) / L"ReSkate" / L"deathrace" / L"fall.bin";
    if (raw) CoTaskMemFree(raw);
    return result;
}
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
// The library on disk beside the last fall: riding.bin.
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
} // namespace dingosdk::death_race::riding
