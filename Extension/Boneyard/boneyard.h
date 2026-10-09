#pragma once
// The Boneyard: skate 3's bail mini-game. Bail, and see which bones you broke.
//
// The game thread (boneyard.cpp) watches the local skater. The no-bail hook's physics state
// watch, or the skeleton itself (a hard hit, then a body no longer upright), says when a bail
// starts; the animation pose (the 395-joint AnimBase skeleton, composed into world space the way
// first person does) says how hard each part of the body hit something: the speed it lost at
// once. The overlay (boneyard_hud.cpp) draws skate 3's X-ray over the skater from the
// snapshot published here (boneyard_view.cpp); every setting is a `boneyard` console command.
#include "Engine/Game/UI/game_view.h"
#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace dingosdk::boneyard {
using Vec3 = std::array<float, 3>;

// The points of the body that are followed: skeleton joints, and the top of the skull (a point
// above the Head joint, which sits at the base of the skull).
enum Point : std::uint8_t {
    hips, spine, spine1, spine2, spine3, neck, neck1, head, skull,
    right_clavicle, right_shoulder, right_elbow, right_wrist, right_knuckles,
    left_clavicle, left_shoulder, left_elbow, left_wrist, left_knuckles,
    right_hip, right_knee, right_ankle, right_toes,
    left_hip, left_knee, left_ankle, left_toes,
    point_count
};

// A bone that can break: the segment between two points, drawn on the X-ray. Its impact is the
// mean of the speed changes at its two ends, so the end that hit the ground counts, and a bone
// whose both ends stopped dead (a flat landing) counts in full.
struct Bone {
    const char *name;  // in the result list
    const char *label; // short name, in the log
    Point from, to;
    float fragility;   // above 1 breaks more easily
};
// Fragility is about the bone alone now that the hits come from the game's own contacts (the
// speed each part meets a surface with): the skull and the collarbones give first, the femur
// last. The skeleton's estimate, used when the contacts cannot be read, overrates the hands and
// feet that whip around in a ragdoll; it shares these values.
inline constexpr std::array<Bone, 19> bones{{
    {"Skull", "SKULL", neck1, skull, 1.15f},
    {"Neck", "NECK", neck, neck1, 0.9f},
    {"Right collarbone", "R. COLLARBONE", right_clavicle, right_shoulder, 1.0f},
    {"Left collarbone", "L. COLLARBONE", left_clavicle, left_shoulder, 1.0f},
    {"Ribs", "RIBS", spine2, spine3, 1.0f},
    {"Spine", "SPINE", spine, spine2, 0.85f},
    {"Pelvis", "PELVIS", right_hip, left_hip, 0.9f},
    {"Right humerus", "R. HUMERUS", right_shoulder, right_elbow, 0.9f},
    {"Left humerus", "L. HUMERUS", left_shoulder, left_elbow, 0.9f},
    {"Right forearm", "R. FOREARM", right_elbow, right_wrist, 0.95f},
    {"Left forearm", "L. FOREARM", left_elbow, left_wrist, 0.95f},
    {"Right wrist", "R. WRIST", right_wrist, right_knuckles, 1.0f},
    {"Left wrist", "L. WRIST", left_wrist, left_knuckles, 1.0f},
    {"Right femur", "R. FEMUR", right_hip, right_knee, 0.75f},
    {"Left femur", "L. FEMUR", left_hip, left_knee, 0.75f},
    {"Right tibia", "R. TIBIA", right_knee, right_ankle, 0.9f},
    {"Left tibia", "L. TIBIA", left_knee, left_ankle, 0.9f},
    {"Right ankle", "R. ANKLE", right_ankle, right_toes, 0.95f},
    {"Left ankle", "L. ANKLE", left_ankle, left_toes, 0.95f},
}};
inline constexpr std::size_t bone_count = bones.size();
inline constexpr std::size_t skull_bone = 0;

enum class Severity : std::uint8_t { none, crack, fracture, open, shattered };
inline constexpr const char *severity_name(Severity severity) noexcept {
    switch (severity) {
    case Severity::crack: return "Cracked";
    case Severity::fracture: return "Fractured";
    case Severity::open: return "Compound fracture";
    case Severity::shattered: return "Shattered";
    default: return "Intact";
    }
}
// Anything but intact: a bone is whole or broken, nothing in between.
inline constexpr bool broken(Severity severity) noexcept { return severity >= Severity::crack; }

enum class Phase : std::uint8_t { idle, bailing, results };

// What the overlay draws. Published every client tick while a bail or its result is on screen.
struct View {
    Phase phase{};
    std::uint64_t serial{}; // bails seen this session
    bool xray{true};        // draw the skeleton over the skater
    bool glow{true};        // with a glow round its bones, as on a lit film
    bool hidden{};          // a game menu is up
    bool pose{};            // `points` hold this tick's skeleton
    std::array<Vec3, point_count> points{};
    // The skeleton of the tick before. Each frame the game moves the skeleton, then runs the client
    // tick (which reads it and the camera), then moves the camera: until the camera has moved, the
    // camera the overlay sees is the frame before's, which goes with these points.
    std::array<Vec3, point_count> previous_points{};
    float scale{1};         // the skater's size, for the skull
    std::array<Severity, bone_count> severity{};
    std::array<float, bone_count> impact{}; // the peak per bone, metres per second
    float speed{};          // the skater's speed as the bail started, metres per second
    float drop{};           // how far the skater fell, metres (0: not a fall)
    bool stutter{};         // the bail's first moments: the overlay shows the picture in jerks
    bool replay{};          // in a multiplayer session: the slow motion is the overlay's replay
    GameView camera;        // the client's camera this tick, as the camera object holds it
    std::chrono::steady_clock::time_point published, results_until;
};
// Thread-safe snapshot (boneyard_view.cpp, part of the overlay).
View view() noexcept;
void publish(const View &) noexcept;
// The game moves its camera after the client tick the view was taken in, so the overlay reads
// the camera again when it draws. The game side supplies the reader.
using LiveCamera = bool (*)(GameView &) noexcept;
void set_live_camera(LiveCamera) noexcept;
bool live_camera(GameView &) noexcept;
// The camera read again now, with the skeleton of its own moment: the tick's (`points`) once the
// game has moved the camera since the tick, the tick before's (`previous_points`) until then. Drawn
// with a camera of one frame and a skeleton of another, a skater moving fast was a frame's travel
// from the bones laid over it, more or less each frame as the overlay drew earlier or later: they
// shook. False (the view as it was) when the camera cannot be read.
bool live_view(View &) noexcept;

// Game thread (boneyard.cpp).
// Each client tick; `playing` while a local skater can exist.
void tick(std::uintptr_t base, std::uintptr_t client, bool playing) noexcept;
// Runs one `boneyard` command and returns the line to print. Only from the console dispatcher.
std::string command(std::string_view verb, const std::vector<std::string> &arguments);
// The settings the game's pause menu shows (CUSTOM STUFF > PLAYER, native_tools_view.h).
struct Status {
    bool enabled{}, slowmo{}, stutter{}, glow{};
};
Status status() noexcept;
} // namespace dingosdk::boneyard
