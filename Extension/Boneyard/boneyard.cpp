#include "boneyard.h"
#include "boneyard_skeleton.h"
#include "Engine/Core/Json/json.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/client_source_spawn.h"
#include "Engine/Game/Multiplayer/session_tools.h"
#include "Extension/Multiplayer/Hud/game_ui_state.h"
#include "Extension/Multiplayer/Remote/native_pose_layout.h"
#include "Extension/Settings/named_settings.h"
#include "Extension/DeathRace/death_race.h"
#include "Extension/Skater/client_source_spawn_internal.h"
#include "Extension/Skater/no_bail.h"
#include "Extension/Trainer/trainer.h"
#include <windows.h>
#include <shlobj.h>
#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <deque>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <cstring>
#include <ctime>

// Game thread only. See boneyard.h for how the overlay reaches this.
namespace dingosdk::boneyard {
namespace {
using Clock = std::chrono::steady_clock;
// The skeleton joint behind each followed point (boneyard_skeleton.h). The skull is the Head
// joint moved up: Head sits at the base of the skull, with local +X up and +Y out of the face.
constexpr std::array<std::uint16_t, point_count> point_joints{
    7, 42, 43, 44, 45, 101, 102, 103, 103,
    46, 47, 48, 49, 55,
    275, 276, 277, 278, 285,
    8, 9, 10, 11,
    341, 342, 343, 344};
constexpr Vec3 skull_offset{0.10f, 0.02f, 0.0f}; // metres from Head, in Head's frame, at scale 1
constexpr std::uint16_t root_joint = 1;          // AITrajectory: the skater's world placement
// Speeds are measured over this much time, so a hit counts the same at 30 or 144 frames per
// second: a contact the physics resolves in one or two steps lands in one window.
constexpr double window = 1.0 / 30.0;
// A skeleton that moves further than this in one tick was teleported (a respawn), not thrown.
constexpr float teleport_distance = 2.0f;
// Ground, air and grind families of the physics state (no_bail.h: 300 is a wipeout, the 500s
// are off the board).
bool riding(std::uint32_t state) { return (state >= 100 && state < 300) || (state >= 400 && state < 500); }
bool grounded(std::uint32_t state) { return (state >= 100 && state < 200) || (state >= 400 && state < 500); }
bool wiping_out(std::uint32_t state) { return state >= 300 && state < 400; }
// Effective impact, metres per second, at which a bone reaches each severity.
// On the speed a body part lost, which is at most about the skater's own (9 m/s at 33 km/h).
// Set on real bails (19 replayed with Test/probe_replay.py): an ordinary one breaks nothing or a
// bone or two, a hard slam at 40 km/h off a 5 m drop four, and only big drops shatter; then a
// touch more fragile (about 6 % lower) by request.
constexpr std::array<float, 4> severity_thresholds{8.5f, 10.4f, 12.7f, 15.0f};
// The bones the board stops along with it while the feet are on it: tibias and ankles.
constexpr std::array<std::size_t, 4> board_bones{15, 16, 17, 18};
// Hips ride about this high above the ground and lie about this low: a fall is the rest.
constexpr float riding_hips_height = 0.8f;
// A slam, whatever the game's state says: the hips lost this much speed in one hit...
constexpr float slam_hips_loss = 4.0f;
// ...and the torso then leans further from upright than this (its height over its length)...
constexpr float slam_lean = 0.5f;
// ...and, when the game's contacts can be read, a part of the body other than the feet met a
// surface at least this fast (metres per second).
constexpr float slam_body_contact = 3.0f;
// A body still in the air is no slam: its hips dropping faster than this (metres per second)...
constexpr float still_falling = 4.0f;
// ...or gaining this much downward speed in a tenth of a second (6 m/s², gravity's 9.8 with room
// to spare): flying, up or down.
constexpr float free_fall_gain = 0.6f;
// A slam is sure at once when the hips came down at least this fast (metres per second) just
// before it: a landing. Any other waits this long (seconds) to see the body stay down.
constexpr float landing_speed = 3.0f;
constexpr double slam_confirm = 0.3;

struct Settings {
    bool enabled{true};
    bool xray{true};
    bool always{}; // draw the X-ray all the time (checking where it lands)
    float sensitivity{1.0f};
    float duration{1.5f}; // seconds the result stays once the skater is up again
    bool slowmo{true};    // the game slows down as a bail starts (not in a multiplayer session)
    bool stutter{true};   // and the picture moves in jerks meanwhile
    bool glow{true};      // the X-ray's bones glow, as on a lit film
};
struct Sample {
    double time{};
    std::array<Vec3, point_count> points{};
};
struct Impact {
    double time{};
    std::array<float, bone_count> bones{};
    float hips{};  // the speed the hips lost
    float speed{}; // the hips' speed
};
// `boneyard probe on`: a reverse-engineering aid. Every tick of each bail (and the half second before
// it) it writes the skeleton and the hits measured here beside the raw memory where the game keeps
// its own per-body-part contact records, so they can be matched up offline. The skater physics
// core keeps a collision object ([[core+0x2f10]+0x1040]) with 24 records of 0x70 bytes from +0x5e0;
// the rig publisher (0x4769d20) copies them every frame to the animation's contact block
// ([[core+0x3b8]+0x30], from +0xb0+0x220), reading per-part vectors from [object+0x10c0].
struct Probe {
    bool on{};
    std::ofstream file;
    std::filesystem::path path;
    std::uint64_t frames{};
    double stop_at{};
    std::deque<std::pair<double, std::vector<char>>> before;
};
// skate 3 slows the game as a bail starts: the game's own clock (SimulationTime.TimeScale, the
// trainer's game speed) eased down to half, held while the body tumbles, then eased back, in real
// seconds. Relative to the player's own speed, never in a multiplayer session (it would change
// how the game plays there), and only while the speed is still the one set here.
constexpr std::string_view time_scale = "SimulationTime.TimeScale";
constexpr double slow_speed = 0.5, slow_in = 0.12, slow_hold = 1.2, slow_out = 0.6;
// Meanwhile the overlay shows a fresh picture only now and then (boneyard_hud.cpp).
constexpr double stutter_for = slow_in + slow_hold;
struct Slow {
    std::uint64_t serial{}; // the bail it is for
    bool active{}, warned{};
    float from{};           // the game's speed before
    std::string from_text;  // as the engine showed it, to put back exactly
    double ratio{1};        // of `from`, set now
    float set{};            // the speed set now
    unsigned changes{};
    double longest{}, spent{}; // real seconds spent changing it
};
struct State {
    bool loaded{};
    Settings settings;
    Probe probe;
    std::uintptr_t base{}, client{}, entity{}, component{};
    double next_resolve{};
    // The no-bail hook's physics state watch (no_bail.h).
    bool wipeouts_known{}, watch_valid{}, watch_warned{};
    bool contacts_valid{}, bail_contacts{}; // the game's contacts read this tick / all through the bail
    std::uint64_t seen_wipeouts{};
    std::uint32_t physics_state{}, previous_state{};
    double watch_lost_since{};
    // The skeleton this tick and the last half second of it.
    bool pose{};
    std::array<Vec3, point_count> points{};
    std::array<Vec3, point_count> previous_points{}; // the tick before's
    Vec3 root{};
    float scale{1};
    std::deque<Sample> samples;
    std::deque<Impact> impacts;                   // the last moments, to catch the hit that started a bail
    std::deque<std::pair<double, float>> heights; // hips height since the skater last rode the ground
    // The bail in progress, or the last one.
    Phase phase{};
    std::uint64_t serial{};
    double started{}, next_bail{}, demo_until{};
    double next_air_note{}; // the log's note of a slam-like hit in the air, at most once a second
    double slam_since{};    // a slam waiting to be sure the body stays down (0: none)
    double on_board_until{}; // the feet count as on the board until then
    std::array<float, bone_count> peaks{};
    float hips_peak{}, speed{}, top{}, bottom{}, drop{};
    Clock::time_point results_until{};
    std::vector<std::string> last; // `boneyard last`
    double started_real{};         // the bail's start in real seconds
    double clock{}, clock_real{};  // game time (game_time), and the real time it was last read at
    Slow slow;
};
State &state() {
    static auto *value = new State;
    return *value;
}
std::atomic<std::uintptr_t> camera_base{};

double seconds() {
    static const double frequency = [] {
        LARGE_INTEGER value;
        QueryPerformanceFrequency(&value);
        return static_cast<double>(value.QuadPart);
    }();
    LARGE_INTEGER value;
    QueryPerformanceCounter(&value);
    return static_cast<double>(value.QuadPart) / frequency;
}
// Real time, slowed down along with the game while the Boneyard slows it: speeds measured on
// the skeleton stay true in slow motion.
double game_time(State &s) {
    const double real = seconds();
    s.clock = s.clock_real ? s.clock + (real - s.clock_real) * (s.slow.active ? s.slow.ratio : 1.0) : real;
    s.clock_real = real;
    return s.clock;
}
void say(logging::Level level, const std::string &text) { logging::write(level, logging::Channel::skater, text); }
std::optional<double> number(std::string_view text) {
    double value{};
    const auto *last = text.data() + text.size();
    const auto parsed = std::from_chars(text.data(), last, value);
    if (parsed.ec != std::errc{} || parsed.ptr != last || !std::isfinite(value)) return std::nullopt;
    return value;
}
float length(const Vec3 &v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }
Vec3 minus(const Vec3 &a, const Vec3 &b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }

// ---- settings ----------------------------------------------------------------------------
std::filesystem::path reskate_folder() {
    PWSTR raw{};
    std::filesystem::path result;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw)) && raw) result = std::filesystem::path(raw) / L"ReSkate";
    if (raw) CoTaskMemFree(raw);
    return result;
}
std::filesystem::path settings_path() {
    const auto folder = reskate_folder();
    return folder.empty() ? folder : folder / L"boneyard" / L"boneyard.json";
}
void load_settings() {
    auto &s = state().settings;
    try {
        auto path = settings_path();
        // Settings kept before the feature took its name: read once, saved under the new one.
        std::error_code missing;
        if (!path.empty() && !std::filesystem::exists(path, missing)) path = reskate_folder() / L"hallofmeat" / L"hallofmeat.json";
        std::ifstream file(path, std::ios::binary);
        if (!file) return;
        const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (text.empty() || text.size() > 65536) return;
        const auto json = Json::parse(text);
        if (!json.is_object()) return;
        s.enabled = json.value("enabled", s.enabled);
        s.xray = json.value("xray", s.xray);
        s.always = json.value("always", s.always);
        s.sensitivity = std::clamp(json.value("sensitivity", s.sensitivity), 0.25f, 4.0f);
        s.duration = std::clamp(json.value("duration", s.duration), 0.5f, 30.0f);
        s.slowmo = json.value("slowmo", s.slowmo);
        s.stutter = json.value("stutter", s.stutter);
        s.glow = json.value("glow", s.glow);
    } catch (...) {
        say(logging::Level::warning, "Boneyard: boneyard.json is damaged; using the defaults.");
    }
}
void save_settings() {
    const auto &s = state().settings;
    try {
        const auto path = settings_path();
        if (path.empty()) return;
        auto json = Json::object();
        json["enabled"] = s.enabled;
        json["xray"] = s.xray;
        json["always"] = s.always;
        json["sensitivity"] = static_cast<double>(s.sensitivity);
        json["duration"] = static_cast<double>(s.duration);
        json["slowmo"] = s.slowmo;
        json["stutter"] = s.stutter;
        json["glow"] = s.glow;
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        auto temporary = path;
        temporary += L".tmp";
        {
            std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
            file << json.dump(2);
            if (!file) return;
        }
        std::filesystem::rename(temporary, path, error);
        if (error) {
            std::filesystem::remove(path, error);
            std::filesystem::rename(temporary, path, error);
        }
    } catch (...) {
        say(logging::Level::warning, "Boneyard: could not save boneyard.json.");
    }
}

// ---- the skeleton ------------------------------------------------------------------------
struct Joint {
    std::array<float, 4> rotation{0, 0, 0, 1};
    Vec3 position{};
    float scale = 1;
};
Vec3 rotate(const std::array<float, 4> &q, const Vec3 &v) {
    const float tx = 2 * (q[1] * v[2] - q[2] * v[1]), ty = 2 * (q[2] * v[0] - q[0] * v[2]), tz = 2 * (q[0] * v[1] - q[1] * v[0]);
    return {v[0] + q[3] * tx + (q[1] * tz - q[2] * ty), v[1] + q[3] * ty + (q[2] * tx - q[0] * tz),
            v[2] + q[3] * tz + (q[0] * ty - q[1] * tx)};
}
// One parent-local joint {scale.xyzw, rotation.xyzw, position.xyzw} appended to its parent's
// world transform (client_first_person.cpp composes the head the same way).
Joint child(const Joint &parent, const float *bone) {
    const auto &q = parent.rotation;
    const auto offset = rotate(q, {bone[8] * parent.scale, bone[9] * parent.scale, bone[10] * parent.scale});
    Joint result;
    for (std::size_t i = 0; i < 3; ++i) result.position[i] = parent.position[i] + offset[i];
    result.rotation = {q[3] * bone[4] + q[0] * bone[7] + q[1] * bone[6] - q[2] * bone[5],
                       q[3] * bone[5] - q[0] * bone[6] + q[1] * bone[7] + q[2] * bone[4],
                       q[3] * bone[6] + q[0] * bone[5] - q[1] * bone[4] + q[2] * bone[7],
                       q[3] * bone[7] - q[0] * bone[4] - q[1] * bone[5] - q[2] * bone[6]};
    result.scale = parent.scale * (bone[0] + bone[1] + bone[2]) / 3.0f;
    return result;
}
// The local skater's skeleton in world space. The animation output pose is parent-local per
// joint and joint 1 carries the world placement, so composing from the root lands in the world.
bool read_pose(State &s) {
    s.pose = false;
    std::uintptr_t holder{};
    if (!s.component || !memory::peek(s.component + 0xa0, holder) || !holder) return false;
    multiplayer::NativePoseLayout layout;
    try {
        const auto read = [](std::uintptr_t address, void *out, std::size_t size) { return memory::peek_bytes(address, out, size); };
        layout = multiplayer::read_native_pose_layout(read, s.base, holder, 512);
    } catch (...) {
        return false;
    }
    if (!layout.buffer || layout.count != skeleton_joints) return false;
    static std::array<float, skeleton_joints * 12> raw{};
    if (!memory::peek_bytes(layout.buffer, raw.data(), sizeof(raw))) return false;
    static std::array<Joint, skeleton_joints> world{};
    for (std::size_t i = 0; i < skeleton_joints; ++i) {
        const float *bone = raw.data() + i * 12;
        for (const std::size_t lane : {0u, 1u, 2u, 4u, 5u, 6u, 7u, 8u, 9u, 10u})
            if (!std::isfinite(bone[lane]) || std::abs(bone[lane]) > 1000000.0f) return false;
        const auto parent = skeleton_parents[i];
        world[i] = child(parent < 0 ? Joint{} : world[static_cast<std::size_t>(parent)], bone);
    }
    // The hips' scale is the skater's size (first person shrinks the head it draws).
    s.scale = std::clamp(world[point_joints[hips]].scale, 0.2f, 5.0f);
    for (std::size_t p = 0; p < point_count; ++p) s.points[p] = world[point_joints[p]].position;
    const auto &head_joint = world[point_joints[head]];
    const auto up = rotate(head_joint.rotation, {skull_offset[0] * s.scale, skull_offset[1] * s.scale, skull_offset[2] * s.scale});
    for (std::size_t i = 0; i < 3; ++i) s.points[skull][i] = head_joint.position[i] + up[i];
    s.root = world[root_joint].position;
    for (const auto &point : s.points)
        for (const auto value : point)
            if (!std::isfinite(value) || std::abs(value) > 1000000.0f) return false;
    s.pose = true;
    return true;
}
// The skater entity and its animation component, looked up again every quarter second: a
// respawn makes a new skater, and out of play (menus, loading) the lookup fails.
bool resolve(State &s, double now) {
    if (now < s.next_resolve) return s.component != 0;
    s.next_resolve = now + 0.25;
    try {
        overlay::DebugModel skater;
        (void)client_source::detail::debug_skater(s.base, s.client, skater);
        if (!skater.skater_position_valid || !skater.skater_identity) throw 0;
        s.entity = skater.skater_identity;
        s.component = client_source::detail::first_person_component(s.base, s.client);
        return true;
    } catch (...) {
        // The lookup can fail for a moment (a teleport under way) while the skater stays: keep
        // the one already found for as long as its pose reads (tick drops it when it stops).
        return s.component != 0;
    }
}

// ---- the game's own contacts ---------------------------------------------------------------
// The skater physics core keeps a record per ragdoll part (24: head, neck, each arm's collarbone,
// upper arm, forearm and hand, four spine parts, each leg's thigh, shin, foot and toes, the
// pelvis, and one that is not on the body), and the rig publisher (0x4769d20) copies them every
// frame into the animation's contact block ([[core+0x3b8]+0x30], record i at +0x2d0 + i*0x80):
// the part's position (+0x00), whether it touches something (+0x73), and for each of its two
// contact slots how fast it meets the surface: |velocity . normal| (+0x30, +0x60) and the
// contact's own speed (+0x34, +0x64). Read off `boneyard probe` recordings of this game build
// (Test/probe_contacts.py). A part is told by where it is: the skeleton point it sits on.
constexpr std::size_t contact_parts = 24, contact_records = 0x2d0, contact_stride = 0x80;
constexpr float contact_reach = 0.12f; // metres from its joint a part's position may be
// The bone a part sitting on each skeleton point stands for.
constexpr std::array<std::size_t, point_count> point_bone{6, 5, 5, 4, 4, 1, 1, 0, 0, 2, 7, 9, 11, 11, 3, 8, 10, 12, 12,
                                                         13, 15, 17, 17, 14, 16, 18, 18};
struct Contacts {
    bool valid{};             // the parts sit on the skeleton: this is the layout found
    std::size_t parts{}, touching{};
    std::array<float, bone_count> bones{}; // the hardest contact on each bone this tick, m/s
};
Contacts read_contacts(const State &s) {
    Contacts result;
    std::uintptr_t core{}, exported{}, block{};
    if (!s.component || !memory::peek(s.component + 0x70, core) || !memory::peek(core + 0x3b8, exported) ||
        !memory::peek(exported + 0x30, block))
        return result;
    static std::array<char, contact_parts * contact_stride> raw{};
    if (!memory::peek_bytes(block + contact_records, raw.data(), raw.size())) return result;
    for (std::size_t i = 0; i < contact_parts; ++i) {
        const char *record = raw.data() + i * contact_stride;
        Vec3 at{};
        std::memcpy(at.data(), record, sizeof(at));
        if (!std::isfinite(at[0]) || !std::isfinite(at[1]) || !std::isfinite(at[2])) continue;
        std::size_t nearest = point_count;
        float best = contact_reach;
        for (std::size_t p = 0; p < point_count; ++p) {
            if (p == skull) continue; // not a joint
            const float d = length(minus(at, s.points[p]));
            if (d < best) {
                best = d;
                nearest = p;
            }
        }
        if (nearest == point_count) continue;
        ++result.parts;
        if (!record[0x73]) continue;
        float hit{};
        for (const std::size_t offset : {0x30u, 0x34u, 0x60u, 0x64u}) {
            float speed{};
            std::memcpy(&speed, record + offset, sizeof(speed));
            if (std::isfinite(speed) && speed > hit && speed < 30.0f) hit = speed; // past 30 m/s: not a body
        }
        ++result.touching;
        auto &bone = result.bones[point_bone[nearest]];
        bone = std::max(bone, hit);
    }
    // Nearly every part sits on its joint when the layout is the one found; otherwise (another
    // game build) the skeleton's own estimate stays in use.
    result.valid = result.parts >= 18;
    return result;
}

// ---- impacts -----------------------------------------------------------------------------
const Sample *at_or_before(const State &s, double time) {
    for (auto it = s.samples.rbegin(); it != s.samples.rend(); ++it)
        if (it->time <= time) return &*it;
    return nullptr;
}
// How much speed each point lost over the last two windows: a body part that hits something
// stops dead, or bounces back slower, within a window or two. A limb the ragdoll only whips
// round keeps its speed: no hit. A speed no body part reaches is a glitch, not a hit.
constexpr float plausible_speed = 30.0f; // metres per second
std::optional<Impact> measure(const State &s, double now) {
    const auto *a = at_or_before(s, now - window);
    const auto *b = a ? at_or_before(s, a->time - window) : nullptr;
    if (!a || !b) return std::nullopt;
    const double ta = now - a->time, tb = a->time - b->time;
    if (ta < window * 0.5 || ta > window * 3 || tb < window * 0.5 || tb > window * 3) return std::nullopt; // a hitch
    const auto &current = s.samples.back();
    std::array<float, point_count> change{};
    Impact impact;
    impact.time = now;
    for (std::size_t p = 0; p < point_count; ++p) {
        Vec3 velocity{}, before{};
        for (std::size_t i = 0; i < 3; ++i) {
            velocity[i] = static_cast<float>((current.points[p][i] - a->points[p][i]) / ta);
            before[i] = static_cast<float>((a->points[p][i] - b->points[p][i]) / tb);
        }
        const float speed = length(velocity), previous = length(before);
        change[p] = speed > plausible_speed || previous > plausible_speed ? 0.0f : std::max(0.0f, previous - speed);
        if (p == hips) impact.speed = speed;
    }
    for (std::size_t k = 0; k < bone_count; ++k) impact.bones[k] = (change[bones[k].from] + change[bones[k].to]) * 0.5f;
    impact.hips = change[hips];
    return impact;
}
Severity severity_of(std::size_t bone, float impact, float sensitivity) {
    const float effective = impact * bones[bone].fragility * sensitivity;
    auto result = Severity::none;
    for (std::size_t level = 0; level < severity_thresholds.size(); ++level)
        if (effective >= severity_thresholds[level]) result = static_cast<Severity>(level + 1);
    return result;
}
std::array<Severity, bone_count> severities(const State &s) {
    std::array<Severity, bone_count> result{};
    for (std::size_t k = 0; k < bone_count; ++k) result[k] = severity_of(k, s.peaks[k], s.settings.sensitivity);
    return result;
}

// ---- the bail ----------------------------------------------------------------------------
void start_bail(State &s, double now, const char *why) {
    s.phase = Phase::bailing;
    ++s.serial;
    s.started = now;
    s.slam_since = 0;
    s.started_real = seconds();
    s.peaks = {};
    s.hips_peak = s.speed = 0;
    s.bail_contacts = s.contacts_valid;
    // The hit that started the bail came just before it was seen as one.
    for (const auto &impact : s.impacts) {
        if (now - impact.time > 0.4) continue;
        for (std::size_t k = 0; k < bone_count; ++k) s.peaks[k] = std::max(s.peaks[k], impact.bones[k]);
        s.hips_peak = std::max(s.hips_peak, impact.hips);
        s.speed = std::max(s.speed, impact.speed);
    }
    s.top = s.bottom = s.points[hips][1];
    for (const auto &[time, height] : s.heights) s.top = std::max(s.top, height);
    say(logging::Level::info, std::format("Boneyard: bail {} ({}) at {:.1f} km/h, physics state {}.", s.serial, why,
                                          s.speed * 3.6f, s.watch_valid ? std::to_string(s.physics_state) : std::string("unknown")));
}
void finish_bail(State &s, double now, const char *why) {
    const auto result = severities(s);
    const float fall = s.top - s.bottom - riding_hips_height;
    s.drop = fall >= 1.0f ? fall : 0.0f;
    s.phase = Phase::results;
    s.next_bail = now + 0.8; // the body settling after this one is not another
    s.results_until = Clock::now() + std::chrono::milliseconds(static_cast<int>(s.settings.duration * 1000.0f));
    // The log line and `boneyard last`: every bone, so the thresholds can be checked against real bails.
    std::vector<std::size_t> order(bone_count);
    for (std::size_t k = 0; k < bone_count; ++k) order[k] = k;
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return s.peaks[a] > s.peaks[b]; });
    std::string broken_list, peaks;
    std::size_t count{};
    s.last.clear();
    s.last.push_back(std::format("Bail {} ({:.1f} s, ended: {}): {:.0f} km/h, {:.1f} m fall, hips hit {:.1f} m/s, sensitivity {:.2f}.",
                                 s.serial, now - s.started, why, s.speed * 3.6f, s.drop, s.hips_peak, s.settings.sensitivity));
    for (const auto k : order) {
        peaks += std::format("{}{} {:.1f}", peaks.empty() ? "" : ", ", bones[k].label, s.peaks[k]);
        s.last.push_back(std::format("  {:<22} {:>5.1f} m/s  {}", bones[k].name, s.peaks[k], severity_name(result[k])));
        if (!broken(result[k])) continue;
        ++count;
        broken_list += std::format("{}{} ({})", broken_list.empty() ? "" : ", ", bones[k].name, severity_name(result[k]));
    }
    say(logging::Level::info, std::format("Boneyard: bail {} ended ({}) after {:.1f} s: {:.1f} km/h, drop {:.1f} m, hips {:.1f} m/s; "
                                          "hits from {}; {} broken{}{}; peaks (m/s): {}.",
                                          s.serial, why, now - s.started, s.speed * 3.6f, s.drop, s.hips_peak,
                                          s.bail_contacts ? "the game's contacts" : "the skeleton", count, count ? ": " : "", broken_list, peaks));
}

// The no-bail hook's watch on the skater's physics state: how many wipeouts it saw since the
// last tick, and the state chosen last (no_bail.h).
std::uint64_t observe_watch(State &s, double now) {
    watch_physics_state(s.client, s.entity);
    const auto watch = watched_physics_state();
    s.watch_valid = watch.valid;
    if (!watch.valid) {
        if (!s.watch_lost_since) s.watch_lost_since = now;
        if (!s.watch_warned && now - s.watch_lost_since > 3.0) {
            s.watch_warned = true;
            say(logging::Level::info, "Boneyard: the physics state watch is unavailable; bails are seen from the skeleton alone.");
        }
        return 0;
    }
    s.watch_lost_since = 0;
    s.previous_state = s.physics_state;
    s.physics_state = watch.state;
    if (riding(s.physics_state)) s.on_board_until = now + 0.15;
    if (s.physics_state != s.previous_state && logging::enabled(logging::Level::debug))
        say(logging::Level::debug, std::format("Boneyard: physics state {} -> {}.", s.previous_state, s.physics_state));
    if (!s.wipeouts_known) {
        s.wipeouts_known = true;
        s.seen_wipeouts = watch.wipeouts;
        return 0;
    }
    const auto fresh = watch.wipeouts - s.seen_wipeouts;
    s.seen_wipeouts = watch.wipeouts;
    return fresh;
}
// The hips' rise over the `span` seconds before `end` (metres per second, below 0 falling), from
// the samples; nothing when they do not reach that far.
std::optional<float> hips_rise(const State &s, double end, double span) {
    const auto *b = at_or_before(s, end);
    const auto *a = b ? at_or_before(s, end - span) : nullptr;
    if (!a || !b || b->time - a->time < span * 0.5) return std::nullopt;
    return static_cast<float>((b->points[hips][1] - a->points[hips][1]) / (b->time - a->time));
}
// A body in the air: its hips still dropping fast, or dropping and gaining speed as gravity makes
// a falling body gain it. Lying, sliding or stopped where it hit, neither.
bool airborne(const State &s, double now, float &falling) {
    const auto recent = hips_rise(s, now, 0.1), earlier = hips_rise(s, now - 0.1, 0.1);
    falling = recent ? std::max(0.0f, -*recent) : 0.0f;
    if (!recent) return false;
    if (*recent < -still_falling) return true;
    return earlier && *recent - *earlier < -free_fall_gain;
}
// Why a bail starts this tick, or nothing.
const char *bail_trigger(State &s, std::uint64_t fresh_wipeouts, double now) {
    if (fresh_wipeouts) return "wipeout";
    if (s.watch_valid && wiping_out(s.physics_state) && !wiping_out(s.previous_state)) return "physics state";
    // A slam the state did not show: a hard hit to the hips, then a torso far from upright...
    float hardest{}, body{};
    for (const auto &impact : s.impacts) {
        if (now - impact.time > 0.6) continue;
        hardest = std::max(hardest, impact.hips);
        for (std::size_t k = 0; k < bone_count; ++k)
            if (k != 17 && k != 18) body = std::max(body, impact.bones[k]); // not the ankles: the feet land
    }
    const auto torso = minus(s.points[spine3], s.points[hips]);
    // ...but not a skater still on the board, nor a deep landing on the feet: with the game's
    // contacts, some part of the body other than the feet must have hit something (a drop landed
    // folded double looks like a slam to the skeleton, and touches nothing).
    const bool slam_like = hardest >= slam_hips_loss && torso[1] < slam_lean * length(torso) &&
                           !(s.watch_valid && riding(s.physics_state)) && !(s.contacts_valid && body < slam_body_contact);
    // ...nor a body in the air. Leaving the board into a dive (a spread eagle) looks like a slam to
    // the skeleton as the skater goes: the bail is the landing, once the fall has stopped.
    float falling{};
    const bool in_air = airborne(s, now, falling);
    if (!slam_like || in_air) {
        if (slam_like && now >= s.next_air_note) {
            s.next_air_note = now + 1.0;
            say(logging::Level::info, std::format("Boneyard: a slam-like hit in the air (hips lost {:.1f} m/s, falling at {:.1f} m/s{}); "
                                                  "no bail before the fall stops.",
                                                  hardest, falling, s.slam_since ? ", after leaving the ground" : ""));
        }
        s.slam_since = 0;
        return nullptr;
    }
    // A landing (the hips came down fast just before, and have stopped) is a slam at once; any
    // other waits a moment to see the body stay down rather than fly off into a dive.
    const auto came = hips_rise(s, now - 0.1, 0.15);
    const bool landing = came && *came < -landing_speed;
    if (!landing) {
        if (!s.slam_since) s.slam_since = now;
        if (now - s.slam_since < slam_confirm) return nullptr;
    }
    s.slam_since = 0;
    say(logging::Level::info, std::format("Boneyard: slam ({}): hips lost {:.1f} m/s, hardest body contact {:.1f} m/s{}.",
                                          landing ? "a landing" : "stayed down", hardest, body, s.contacts_valid ? "" : " (skeleton)"));
    return "slam";
}

// The camera as it is now, for the overlay: the same camera object the client tick saw, still
// a camera, with a sound matrix (custom_nametags.cpp reads it the same way).
bool read_live_camera(GameView &view) noexcept {
    const auto base = camera_base.load(std::memory_order_acquire);
    if (!base || !view.camera || (view.camera & 7)) return false;
    std::uintptr_t vtable{};
    if (!memory::peek(view.camera, vtable) ||
        (vtable != base + addr::engine::camera_vtable && vtable != base + addr::client_source_spawn::free_camera_vtable))
        return false;
    std::array<float, 16> world{};
    float fov{};
    if (!memory::peek(view.camera + 0x50, world) || !memory::peek(view.camera + 0xac, fov)) return false;
    if (!std::isfinite(fov) || fov <= 1 || fov >= 175) return false;
    for (const auto value : world)
        if (!std::isfinite(value) || std::abs(value) > 1e7f) return false;
    view.world = world;
    view.vertical_fov = fov;
    return true;
}

// ---- probe -------------------------------------------------------------------------------
// One frame, little endian: "HOMP", version 1, time (double), phase, physics state (~0 unknown),
// bail serial; 27 points (3 floats each); 19 bone hits, hips hit, hips speed (floats); the core,
// collision object and contact block addresses (u64); then three memory blocks, each a u32 size
// (0: unreadable) and its bytes: the collision object from +0x380 to +0x1110, the contact block's
// first 0x1000 bytes, and 0x50 per-part vectors from [object+0x10c0] + 0xd2 * 16.
constexpr std::uint32_t probe_magic = 0x504d4f48; // "HOMP"
constexpr std::size_t probe_object_from = 0x380, probe_object_size = 0x1110 - 0x380, probe_contacts_size = 0x1000;
template <class T> void put(std::vector<char> &out, const T &value) {
    const auto *bytes = reinterpret_cast<const char *>(&value);
    out.insert(out.end(), bytes, bytes + sizeof(T));
}
void put_memory(std::vector<char> &out, std::uintptr_t address, std::size_t size) {
    const auto at = out.size();
    out.resize(at + 4 + size);
    const std::uint32_t read = address && memory::peek_bytes(address, out.data() + at + 4, size) ? static_cast<std::uint32_t>(size) : 0;
    std::memcpy(out.data() + at, &read, 4);
    if (!read) out.resize(at + 4);
}
std::vector<char> probe_frame(const State &s, double now, const std::optional<Impact> &impact) {
    std::vector<char> out;
    out.reserve(12288);
    put(out, probe_magic);
    put(out, std::uint32_t{1});
    put(out, now);
    put(out, static_cast<std::uint32_t>(s.phase));
    put(out, s.watch_valid ? s.physics_state : 0xffffffffu);
    put(out, static_cast<std::uint32_t>(s.serial));
    put(out, s.points);
    put(out, impact ? impact->bones : std::array<float, bone_count>{});
    put(out, impact ? impact->hips : 0.0f);
    put(out, impact ? impact->speed : 0.0f);
    std::uintptr_t core{}, physics{}, object{}, exported{}, contacts{}, vectors{};
    if (s.component) (void)memory::peek(s.component + 0x70, core);
    if (core) {
        (void)memory::peek(core + 0x2f10, physics);
        (void)memory::peek(core + 0x3b8, exported);
    }
    if (physics) (void)memory::peek(physics + 0x1040, object);
    if (exported) (void)memory::peek(exported + 0x30, contacts);
    if (object) (void)memory::peek(object + 0x10c0, vectors);
    put(out, static_cast<std::uint64_t>(core));
    put(out, static_cast<std::uint64_t>(object));
    put(out, static_cast<std::uint64_t>(contacts));
    put_memory(out, object ? object + probe_object_from : 0, probe_object_size);
    put_memory(out, contacts, probe_contacts_size);
    put_memory(out, vectors ? vectors + 0xd2 * 16 : 0, 0x50 * 16);
    return out;
}
void probe_close(State &s) {
    auto &p = s.probe;
    if (!p.file.is_open()) return;
    p.file.close();
    say(logging::Level::info, std::format("Boneyard: probe recorded {} frames to {}.", p.frames, p.path.string()));
    p.frames = 0;
    p.stop_at = 0;
}
// Client tick, after the bail's state is settled for this tick.
void probe_tick(State &s, double now, const std::optional<Impact> &impact) {
    auto &p = s.probe;
    if (!p.on) return;
    auto frame = probe_frame(s, now, impact);
    if (!p.file.is_open() && s.phase == Phase::bailing) {
        std::error_code error;
        const auto folder = settings_path().parent_path() / L"probe";
        std::filesystem::create_directories(folder, error);
        p.path = folder / std::format("bail_{}_{}.bin", s.serial, static_cast<long long>(std::time(nullptr)));
        p.file.open(p.path, std::ios::binary | std::ios::trunc);
        p.frames = 0;
        p.stop_at = 0;
        for (const auto &[time, before] : p.before) {
            p.file.write(before.data(), static_cast<std::streamsize>(before.size()));
            ++p.frames;
        }
        p.before.clear();
    }
    if (p.file.is_open()) {
        p.file.write(frame.data(), static_cast<std::streamsize>(frame.size()));
        ++p.frames;
        // A little past the end of the bail, then the file is done.
        if (s.phase != Phase::bailing) {
            if (!p.stop_at) p.stop_at = now + 0.3;
            if (now >= p.stop_at) probe_close(s);
        }
        return;
    }
    p.before.emplace_back(now, std::move(frame));
    while (!p.before.empty() && now - p.before.front().first > 0.6) p.before.pop_front();
}

// ---- slow motion -------------------------------------------------------------------------
std::optional<float> speed_of(const std::optional<std::string> &text) {
    float value{};
    if (!text) return std::nullopt;
    const auto *last = text->data() + text->size();
    const auto parsed = std::from_chars(text->data(), last, value);
    if (parsed.ec != std::errc{} || parsed.ptr != last || !std::isfinite(value)) return std::nullopt;
    return value;
}
// Sets the game's speed (a few times a bail: the engine's setter, timed for the log).
bool set_speed(Slow &w, std::string_view value) {
    const double from = seconds();
    const auto result = change_named_setting(time_scale, value, false);
    const double spent = seconds() - from;
    ++w.changes;
    w.longest = std::max(w.longest, spent);
    w.spent += spent;
    if (!result.starts_with("error")) return true;
    if (!w.warned) {
        w.warned = true;
        say(logging::Level::info, "Boneyard: no slow motion: " + result);
    }
    return false;
}
// Puts the player's speed back, unless something else changed it meanwhile.
void end_slow(State &s, const char *why) {
    auto &w = s.slow;
    if (!w.active) return;
    w.active = false;
    const auto now = speed_of(observe_named_setting(time_scale));
    const bool ours = now && std::abs(*now - w.set) < 1e-4f;
    if (ours && w.ratio != 1.0) (void)set_speed(w, w.from_text);
    w.ratio = 1;
    say(logging::Level::info, std::format("Boneyard: slow motion of bail {} over ({}{}): {} speed change(s), longest {:.3f} ms, "
                                          "{:.3f} ms in all.",
                                          w.serial, why, ours ? "" : "; the speed was changed elsewhere, left as it is", w.changes,
                                          w.longest * 1000.0, w.spent * 1000.0));
}
// Each tick: the speed the bail is at, in steps of a tenth (a handful of changes, not one a frame).
void update_slow(State &s) {
    auto &w = s.slow;
    // Not in a Death Race: slowing the game would stretch the clock.
    const bool wanted = s.settings.enabled && s.settings.slowmo && s.phase == Phase::bailing && !multiplayer_session_active() &&
                        !death_race::racing();
    if (w.active && (!wanted || w.serial != s.serial)) end_slow(s, wanted ? "a new bail" : s.phase == Phase::bailing ? "turned off" : "bail over");
    if (!wanted || (!w.active && w.serial == s.serial)) return;
    const double t = seconds() - s.started_real;
    double target = 1.0;
    if (t < slow_in) target = 1.0 - (1.0 - slow_speed) * t / slow_in;
    else if (t < slow_in + slow_hold) target = slow_speed;
    else if (t < slow_in + slow_hold + slow_out) target = slow_speed + (1.0 - slow_speed) * (t - slow_in - slow_hold) / slow_out;
    target = std::round(target * 10.0) / 10.0;
    if (!w.active) {
        // The bail's start: what the speed is now (the player may have slowed it in the trainer).
        const bool warned = w.warned;
        w = {};
        w.serial = s.serial;
        w.warned = warned;
        if (t > slow_in) return;
        const auto text = observe_named_setting(time_scale);
        const auto speed = speed_of(text);
        if (!speed || *speed < 0.05f) return; // unreadable, or paused
        w.active = true;
        w.from = w.set = *speed;
        w.from_text = *text;
    }
    if (t >= slow_in + slow_hold + slow_out) return end_slow(s, "done");
    // Still all but full speed: the very start, or the last moment of the way back.
    if (target >= 1.0 || target == w.ratio) return;
    if (w.ratio != 1.0) {
        const auto now = speed_of(observe_named_setting(time_scale));
        if (!now || std::abs(*now - w.set) >= 1e-4f) return end_slow(s, "changed elsewhere");
    }
    const float value = static_cast<float>(w.from * target);
    if (!set_speed(w, std::format("{}", value))) return end_slow(s, "refused");
    w.ratio = target;
    w.set = value;
}

void publish_view(State &s) {
    View v;
    v.phase = s.phase;
    v.serial = s.serial;
    // The X-ray shows during the bail only: once the skater is up, it is gone at once.
    v.xray = s.settings.xray && (s.phase == Phase::bailing || s.settings.always);
    v.glow = s.settings.glow;
    v.hidden = multiplayer::sample_game_ui_state(s.base).in_menu;
    v.pose = s.pose;
    v.points = s.points;
    v.previous_points = s.previous_points;
    v.scale = s.scale;
    v.severity = severities(s);
    v.impact = s.peaks;
    v.speed = s.speed;
    if (s.phase == Phase::bailing) {
        // The result is shown all through the bail: the fall so far.
        const float fall = s.top - s.bottom - riding_hips_height;
        v.drop = fall >= 1.0f ? fall : 0.0f;
    } else {
        v.drop = s.drop;
    }
    v.stutter = s.settings.stutter && s.phase == Phase::bailing && seconds() - s.started_real < stutter_for;
    // Where the game's speed must stay (a multiplayer session), the overlay replays the bail slowly.
    v.replay = s.settings.enabled && s.settings.slowmo && multiplayer_session_active() && !death_race::racing();
    if (const auto camera = latest_game_view()) {
        v.camera = *camera;
        // As the overlay reads it (the camera object's own matrix), so it can tell whether the
        // game has moved the camera since this tick.
        (void)read_live_camera(v.camera);
    }
    v.published = Clock::now();
    v.results_until = s.results_until;
    publish(v);
}
void load(State &s) {
    if (s.loaded) return;
    s.loaded = true;
    load_settings();
    set_live_camera(&read_live_camera);
}
} // namespace

void tick(std::uintptr_t base, std::uintptr_t client, bool playing) noexcept {
    try {
        auto &s = state();
        load(s);
        camera_base.store(base, std::memory_order_release);
        s.base = base;
        if (s.client != client) {
            s.client = client;
            s.component = 0;
        }
        const double now = game_time(s);
        bool skater = false;
        const auto before = s.points;
        const bool had_pose = s.pose;
        if (s.settings.enabled && playing && resolve(s, now)) {
            skater = read_pose(s);
            if (!skater) s.component = 0; // looked up again at the next resolve
        }
        // The skeleton of the tick before, for the overlay to pair with the camera of its moment.
        s.previous_points = skater && had_pose ? before : s.points;
        if (!skater) {
            s.samples.clear();
            s.impacts.clear();
            s.heights.clear();
            if (s.phase == Phase::bailing) finish_bail(s, now, "skater lost");
            if (s.phase == Phase::results && Clock::now() >= s.results_until) s.phase = Phase::idle;
            probe_close(s);
            s.probe.before.clear();
            s.pose = false;
            update_slow(s);
            publish_view(s);
            return;
        }
        const auto fresh_wipeouts = observe_watch(s, now);
        // A respawn or a teleport moves the whole skeleton at once: nothing hit anything.
        const bool teleported = !s.samples.empty() && length(minus(s.samples.back().points[hips], s.points[hips])) > teleport_distance;
        if (teleported) {
            s.samples.clear();
            s.impacts.clear();
            s.heights.clear();
        }
        s.samples.push_back({now, s.points});
        while (s.samples.size() > 2 && now - s.samples.front().time > 0.5) s.samples.pop_front();
        auto impact = measure(s, now);
        // The game's own contacts say which part hit and how hard: they replace the skeleton's
        // estimate whenever their layout is the one found (the hips' hit and speed, which start
        // a bail, stay the skeleton's).
        const auto contacts = read_contacts(s);
        s.contacts_valid = contacts.valid;
        if (contacts.valid) {
            if (!impact) {
                impact = Impact{};
                impact->time = now;
            }
            impact->bones = contacts.bones;
        } else {
            // From the skeleton: the feet ride the board until the bail throws them off, and when
            // the board hits something they stop with it: the board's knock, not the ankles'. Leg
            // hits count once the skater has been off the board for a moment (from the physics
            // state; without it, from the bail's start).
            const bool on_board = s.watch_valid ? now <= s.on_board_until : s.phase != Phase::bailing || now - s.started < 0.15;
            if (impact && on_board)
                for (const auto k : board_bones) impact->bones[k] = 0.0f;
        }
        if (impact) {
            s.impacts.push_back(*impact);
            while (!s.impacts.empty() && now - s.impacts.front().time > 0.6) s.impacts.pop_front();
        }
        // The highest the hips have been since the skater last rode the ground: a fall's top.
        if (s.watch_valid && grounded(s.physics_state)) s.heights.clear();
        s.heights.emplace_back(now, s.points[hips][1]);
        while (!s.heights.empty() && now - s.heights.front().first > 4.0) s.heights.pop_front();

        if (s.phase != Phase::bailing && now >= s.next_bail)
            if (const auto why = bail_trigger(s, fresh_wipeouts, now)) start_bail(s, now, why);
        if (s.phase == Phase::bailing) {
            s.bail_contacts = s.bail_contacts && s.contacts_valid;
            if (impact) {
                for (std::size_t k = 0; k < bone_count; ++k) s.peaks[k] = std::max(s.peaks[k], impact->bones[k]);
                s.hips_peak = std::max(s.hips_peak, impact->hips);
                if (logging::enabled(logging::Level::debug)) {
                    const auto top = std::max_element(impact->bones.begin(), impact->bones.end());
                    if (*top >= 2.0f)
                        say(logging::Level::debug, std::format("Boneyard: +{:.2f} s, hardest {} {:.1f} m/s, hips {:.1f} m/s at {:.1f} m/s.",
                                                               now - s.started, bones[static_cast<std::size_t>(top - impact->bones.begin())].label,
                                                               *top, impact->hips, impact->speed));
                }
            }
            s.bottom = std::min(s.bottom, s.points[hips][1]);
            const double elapsed = now - s.started;
            // Back on their feet: hips well above where the body lay, and the spine upright. A
            // tumbling body can bounce as high, but not standing straight.
            const auto torso = minus(s.points[spine3], s.points[hips]);
            const bool upright = s.points[hips][1] - s.bottom > 0.5f && torso[1] > 0.85f * length(torso);
            // The bail, and everything on screen, lasts until the skater is up again: on their
            // feet, back on the board, or teleported (a session marker, a respawn). Lying still is
            // still the bail. `boneyard demo` holds its made-up one for a few seconds.
            if (now < s.demo_until) {
            } else if (s.demo_until) {
                s.demo_until = 0;
                finish_bail(s, now, "demo");
            } else if (teleported) finish_bail(s, now, "teleported");
            else if (s.watch_valid && riding(s.physics_state) && elapsed > 0.4) finish_bail(s, now, "back on the board");
            else if (upright && elapsed > 1.0) finish_bail(s, now, "got up");
            else if (elapsed > 60.0) finish_bail(s, now, "60 s");
        } else if (s.phase == Phase::results && Clock::now() >= s.results_until) {
            s.phase = Phase::idle;
        }
        probe_tick(s, now, impact);
        update_slow(s);
        publish_view(s);
    } catch (...) {
        // Nothing here may stop the client tick.
    }
}

Status status() noexcept {
    auto &s = state();
    load(s);
    return {s.settings.enabled, s.settings.slowmo, s.settings.stutter, s.settings.glow};
}

std::string command(std::string_view verb, const std::vector<std::string> &arguments) {
    auto &s = state();
    load(s);
    auto &settings = s.settings;
    const auto on_off = [&](bool &flag, const char *what) -> std::string {
        if (!arguments.empty()) {
            if (arguments[0] == "1" || arguments[0] == "on") flag = true;
            else if (arguments[0] == "0" || arguments[0] == "off") flag = false;
            else return std::string("error: ") + what + " takes 0 or 1.";
            save_settings();
        }
        return std::format("{} : {}.", what, flag ? "on" : "off");
    };
    if (verb == "status") {
        return std::format("Boneyard {}; X-ray {}{}, glow {}; slow motion {}; jerky picture {}; sensitivity {:.2f}; result shown {:.1f} s; "
                           "{} bail(s) this session; physics state {}; skater {}; hits from {}.",
                           settings.enabled ? "on" : "off", settings.xray ? "on" : "off", settings.always ? " (always)" : "", settings.glow ? "on" : "off",
                           settings.slowmo ? "on" : "off", settings.stutter ? "on" : "off", settings.sensitivity, settings.duration, s.serial,
                           s.watch_valid ? std::to_string(s.physics_state) : std::string("unavailable (bails seen from the skeleton alone)"),
                           s.pose ? "tracked" : "not found",
                           s.contacts_valid ? "the game's contacts" : "the skeleton (the game's contacts are not readable)");
    }
    if (verb == "on" || verb == "off") {
        settings.enabled = verb == "on";
        save_settings();
        return settings.enabled ? "Boneyard on." : "Boneyard off.";
    }
    if (verb == "xray") {
        if (!arguments.empty() && arguments[0] == "always") {
            settings.xray = settings.always = true;
            save_settings();
            return "X-ray always shown (to check it sits on the skater; `boneyard xray 1` goes back to bails only).";
        }
        settings.always = false;
        return on_off(settings.xray, "X-ray");
    }
    if (verb == "slowmo") return on_off(settings.slowmo, "Slow motion as a bail starts (a replay on your screen in a multiplayer session)");
    if (verb == "stutter") return on_off(settings.stutter, "Jerky picture as a bail starts");
    if (verb == "glow") return on_off(settings.glow, "X-ray glow");
    if (verb == "sensitivity") {
        if (arguments.empty()) return std::format("Sensitivity: {:.2f} (1 is normal, higher breaks bones more easily).", settings.sensitivity);
        const auto value = number(arguments[0]);
        if (!value || *value < 0.25 || *value > 4.0) return "error: sensitivity is between 0.25 and 4.";
        settings.sensitivity = static_cast<float>(*value);
        save_settings();
        return std::format("Sensitivity: {:.2f}.", settings.sensitivity);
    }
    if (verb == "duration") {
        if (arguments.empty()) return std::format("The result stays {:.1f} s after the skater is up again.", settings.duration);
        const auto value = number(arguments[0]);
        if (!value || *value < 0.5 || *value > 30.0) return "error: between 0.5 and 30 seconds.";
        settings.duration = static_cast<float>(*value);
        save_settings();
        return std::format("The result stays {:.1f} s after the skater is up again.", settings.duration);
    }
    if (verb == "drop") {
        if (!s.pose) return "error: no skater in play.";
        float height = 12.0f;
        if (!arguments.empty()) {
            const auto value = number(arguments[0]);
            if (!value || *value < 2.0 || *value > 80.0) return "error: height is between 2 and 80 metres.";
            height = static_cast<float>(*value);
        }
        // The trainer's teleport keeps a session's teleport rules.
        return trainer::command("tp", {std::format("{:.2f}", s.root[0]), std::format("{:.2f}", s.root[1] + height),
                                       std::format("{:.2f}", s.root[2])});
    }
    if (verb == "probe") {
        auto &p = s.probe;
        const auto folder = settings_path().parent_path() / L"probe";
        if (!arguments.empty() && (arguments[0] == "on" || arguments[0] == "1")) p.on = true;
        else if (!arguments.empty() && (arguments[0] == "off" || arguments[0] == "0")) {
            p.on = false;
            probe_close(s);
            p.before.clear();
        }
        std::size_t files{};
        std::error_code error;
        for (const auto &entry : std::filesystem::directory_iterator(folder, error))
            if (entry.path().extension() == ".bin") ++files;
        return std::format("Probe {}: every bail is recorded to {} ({} file(s) so far).", p.on ? "on" : "off", folder.string(), files);
    }
    if (verb == "last") {
        if (s.last.empty()) return "No bail yet.";
        std::string text;
        for (const auto &line : s.last) text += (text.empty() ? "" : "\n") + line;
        return text;
    }
    if (verb == "demo") {
        // A made-up bail on the current pose: shows the X-ray and the result without falling.
        if (!s.pose) return "error: no skater in play.";
        const double now = game_time(s);
        s.phase = Phase::bailing;
        ++s.serial;
        s.started = now;
        s.started_real = seconds();
        s.peaks = {};
        s.peaks[skull_bone] = 12.0f; // compound fracture
        s.peaks[11] = 11.5f;         // right wrist, fractured
        s.peaks[3] = 9.5f;           // left clavicle, cracked
        s.peaks[14] = 22.0f;         // left femur, shattered
        s.peaks[4] = 4.0f;           // ribs, whole
        s.speed = 11.0f;
        s.top = s.points[hips][1] + 4.5f;
        s.bottom = s.points[hips][1];
        s.hips_peak = 5.0f;
        s.demo_until = now + 5.0; // shown as a bail for this long, then as its result
        return "Demo: a made-up bail on the current pose for 5 s.";
    }
    return "error: unknown command.";
}
} // namespace dingosdk::boneyard
