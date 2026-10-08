#pragma once
// The AI riders' parts, shared by their files (game thread): death_race_line.cpp records the
// skater's ride and shapes its course, death_race_library.cpp keeps the riding library and the
// last fall, death_race_sight.cpp looks ahead and under the riders, and death_race_riders.cpp
// shows them and moves them every tick.
#include "death_race_riders.h"
#include "death_race_limits.h"
#include "Extension/Multiplayer/Net/protocol.h"
#include "Extension/Multiplayer/Remote/native_cosmetics.h"
#include "Extension/Multiplayer/Remote/native_skater.h"
#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

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
// bodies come from the riding library, death_race_library.cpp).
struct Line {
    std::vector<Frame> frames;
    double seconds{};
    Vec side{}; // to the right of the way it sets off, across the ground
    // The course: the ride's path every half metre (its falls left out), smoothed, with the speed
    // it was ridden at and where it was in the air.
    std::vector<Point> path;
    float length{};
};
} // namespace dingosdk::death_race

namespace dingosdk::death_race::riding {
inline constexpr double capture_interval = 0.05;             // 20 poses a second, as a session sends them
inline constexpr std::size_t longest_capture = 20 * 60 * 10; // ten minutes
inline constexpr float set_off_distance = 0.6f;              // metres on the board from its first pose: the ride began
// Each rider's pace on the line, and how far to the side of it it starts (metres, to the right);
// it joins the line over the first seconds of the race.
inline constexpr std::array<double, most_riders> paces{1.01, 0.98, 1.03, 0.95};
inline constexpr std::array<float, most_riders> spread{lane_spacing, -lane_spacing, 2.0f * lane_spacing, -2.0f * lane_spacing};
static_assert(most_riders == most_racers - 1);
inline constexpr double merge_seconds = 6.0;
// One rider's capsules are solid within this distance of the local skater.
inline constexpr float solid_reach = 40.0f;
// A fall: the poses kept after the local skater's begins; how long a rider lies there after it
// before it is back on its board, a little before where it fell (the distance it rode in a second,
// within these metres); no second run-in this soon after one.
inline constexpr double fall_seconds = 2.6, get_up_seconds = 0.4, fall_again = 1.0;
inline constexpr float respawn_least = 3.0f, respawn_most = 10.0f;
// A run-in (across, up, how much faster one goes than the other): from hit_speed both are pushed
// apart, the rider staggering and wobbling on its board; from crash_speed they really hit, and
// both fall. The local skater is pushed away by this much (m/s), more the harder the hit.
inline constexpr float hit_reach = 0.8f, hit_height = 1.8f, hit_speed = 1.2f, crash_speed = 4.0f, most_closing = 20.0f;
// A rider's body to run into (the game's own collision): a capsule this wide (a skater, not the retail
// proxy's metre), standing on its board: a recorded pose's root rides about a metre above the
// board, where the proxy's capsule would float at head height and above.
inline constexpr float body_radius = 0.36f, deck_height = 0.1f, root_above_board = 0.95f;
inline constexpr float nudge_least = 0.8f, nudge_most = 2.5f, crash_nudge_most = 4.0f;
// The wobble: the body swaying over its board away from the hit and back, dying away (radians at
// most, sways a second, the time it takes to settle).
inline constexpr double wobble_seconds = 1.4;
inline constexpr float wobble_most = 0.3f, wobble_hz = 2.2f, wobble_settle = 0.45f;
// A shove: out over a quarter second, back to its line over the next seconds, a stagger (turned
// away, in radians), and the time it loses while slowed.
inline constexpr double shove_out = 0.25, shove_back = 2.5;
inline constexpr float most_shove = 2.8f, most_stagger = 0.45f;
// The riders spread out again over the line's last seconds (metres of the course): they stop
// side by side.
inline constexpr double park_seconds = 3.0;
inline constexpr float park_metres = 12.0f;
// The course: points every half metre, smoothed over a metre and a half either side (speeds three
// metres); the riders ride it at its speed, never slower than this (where the skater walked) nor
// faster than a mega ramp takes it, picking up speed over the first seconds.
inline constexpr float path_step = 0.5f, least_speed = 6.0f, most_speed = 45.0f;
// The course is raced, whatever the skater did setting it: where it stopped, walked or fell
// (slower than riding_speed, once it had got going past launched_speed), the riders keep the
// speed it had before slowing down for it (the fastest within stall_back metres), easing into the
// speed it found again after (once back to stall_resumed of it, or stall_reach metres on). Its
// start (getting going) and its turns are raced as ridden. The ride ends where the skater last
// rode faster than riding_speed (the rest was stopping to set its finish).
inline constexpr float riding_speed = 3.0f, launched_speed = 6.0f, stall_back = 25.0f, stall_resumed = 0.8f, stall_reach = 80.0f;
inline constexpr int smooth_points = 3, smooth_speeds = 6;
inline constexpr double launch_seconds = 1.5;
// Each rider's own way of riding, drawn every race: its pace (around its base one), how far it
// wanders either side of the course and over how many metres, and how much its speed rises and
// falls along the way. The wander fades in and out over the course's first and last metres.
inline constexpr double pace_spread = 0.03;
inline constexpr float wander_least = 0.4f, wander_most = 1.0f, wander_fade = 15.0f;
// Sight: every few ticks a rider looks along the way it means to take with the game's own rays,
// knee and chest high, from just ahead of it to a second or so on (no less, no more than these
// metres), and keeps off whatever stands there that is no skater: its own line when that is
// clear, else where it is, else back on the course (the skater rode it), braking when nothing is.
// At most this many riders look a tick.
inline constexpr float sight_low = 0.4f, sight_high = 1.0f, sight_from = 0.8f, sight_least = 4.0f, sight_most = 10.0f;
inline constexpr double sight_seconds = 1.0, sight_every = 0.06;
inline constexpr int sights_a_tick = 2;
// A hit this close to a skater (across) is that skater; this close to the course, a rider rides
// as the skater did there.
inline constexpr float body_reach = 0.9f, on_course = 0.2f;
// Overtaking: a rider catching the local skater up keeps this far from it (across), once it is this
// far ahead, passing it this far to either side when its own way is not clear. It never shies
// from the skater beside or behind it: that one may run into it.
inline constexpr float pass_room = 1.3f, lead_least = 0.5f, pass_side = 1.6f;
// Steering across: how fast it eases to the side it chose (per second, and metres a second at
// most), faster when keeping off something; how far it leans into it (radians); how slow it
// brakes down to when nothing ahead is clear.
inline constexpr float steer_gain = 1.2f, steer_rate = 1.2f, evade_gain = 3.5f, evade_rate = 3.0f, most_lean = 0.35f;
inline constexpr float brake_least = 0.35f;
// Off the course it rides on the ground there, found this far ahead: never more than this above or
// below the course.
inline constexpr float ground_ahead = 1.5f, ground_step = 0.5f;
// A rider's board on the ground: the ground under it found by two of the game's rays this far ahead
// of and behind its middle (clear of its own capsules), every so often (at most this many riders a
// tick); eased into (a jump of more than ground_jump is taken at once). The board's middle rides
// `lift` over the ground: measured under the skater's own board as it rides on the flat (the
// middle of what is seen), first_lift until then.
inline constexpr float probe_reach = 0.6f, ground_jump = 0.5f, flat_ground = 0.03f, first_lift = 0.1f;
inline constexpr double probe_every = 0.08, lift_every = 0.5;
inline constexpr double ground_settle = 10.0; // a second's share of the way to what the last look found
inline constexpr int probes_a_tick = 2;
// A rider's speed swings slowly (over a cycle of these seconds, and a shorter one with less of it), by
// this share either way: riders overtake each other now and then, not all the time.
inline constexpr double surge_least = 0.05, surge_most = 0.08;
inline constexpr double swing_long_least = 25.0, swing_long_most = 60.0, swing_short_least = 15.0, swing_short_most = 30.0;
// The riding library (see Style): rolling stretches this long, pushing ones from a push's start;
// a stretch loops crossfaded into itself at its ends. Rolling: plain riding at least this fast,
// turning, rising or falling off its straight line, and the board pitching against the way it
// goes (a manual) no more than these. The best few of each from every ride, the newest kept.
inline constexpr double rolling_window = 2.0, pushing_window = 1.1, style_fade = 0.35;
inline constexpr float rolling_least_speed = 5.0f, rolling_turn = 0.35f, rolling_rise = 0.35f, rolling_pitch = 0.12f;
inline constexpr std::size_t styles_a_ride = 2, most_rolling = 8, most_pushing = 6, most_tucking = 6, most_flying = 6;
// Stumbles (the skater's own sketchy landings: the second after it came down off balance) and
// emotes (the skater's, on foot), put in the library from its captures. A light run-in plays a
// stumble; the riders play emotes on the start line until a moment before the go (then they are
// back on their boards), and parked past the finish, with pauses between.
inline constexpr std::size_t most_stumbling = 6, most_emoting = 8;
inline constexpr double emote_stop = 1.2, emote_pause_least = 1.0, emote_pause_most = 3.5, emote_fade = 0.35;
inline constexpr double stumble_in = 0.1, stumble_out = 0.25;
// Airs: the skater's own, from a little before it left the ground to a little after it came down
// (poses either side), at least this long in the air, its board never turned over (its up within
// this of the world's) nor round under its feet (its heading against the body's, radians): no
// trick, nothing gone wrong. A rider flies one through each of its own airs, stretched to fit.
inline constexpr std::size_t air_lead = 3;
inline constexpr double least_air = 0.4;
inline constexpr float air_board_up = 0.75f, air_board_turn = 0.6f;
// Rolling is standing on the board: the head (above the skater's placement) within this of where it
// is riding upright (the ride's tallest tenth), never below the least; tucking is crouched lower,
// held (a downhill tuck). Neither bobs more than this (pumping, a crouch before a trick).
inline constexpr float upright_give = 0.15f, upright_least = 0.12f, upright_bob = 0.15f;
// A rider tucks down a slope (fully from this steep, radians, starting from the gentler one) and at
// speed (some, from this fast).
inline constexpr float tuck_from = 0.035f, tuck_full = 0.14f, tuck_speed = 12.0f, tuck_at_speed = 0.4f;
// The skater's jumps lower than this over their way across (a flat trick, an ollie on the flat) are
// ridden on the ground; higher ones (a drop, a gap) are flown.
inline constexpr float hop_height = 0.7f;
// A rider pushes while slower than this, and for the first seconds of each set-off; leans into the
// course's turns this much of the way the skater would (radians at most).
inline constexpr float push_below = 7.0f, turn_lean_share = 0.7f, most_turn_lean = 0.3f;
inline constexpr double push_after = 2.0;
struct Recording {
    bool on{};
    double started{}, next{};
    std::vector<Frame> frames;
};
// The local skater's last fall, from its first moment.
struct Clip {
    std::vector<Frame> frames;
    double seconds{};
};
struct Falls {
    Recording taking;
    std::shared_ptr<const Clip> clip;
};
using Quat = std::array<float, 4>; // x, y, z, w
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
inline constexpr std::uint32_t library_magic = 0x45444952; // "RIDE"
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
    // The rider's body (the riding library): its rolling and pushing stretches, where it is in its push,
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
    // The rider's own way of riding this race (see pace_spread): drawn when it first shows.
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
inline constexpr std::uint32_t fall_magic = 0x4c4c4146; // "FALL"
// The game's own clothes and board parts by kind (Own_<Kind>_...: TopShirt, ShoeSneaker,
// DeckGraphic...), from the cosmetics catalog (Extension/Customization). Faces, hair, bodies and
// whole costumes are left as they are: they go with each other.
inline constexpr std::array<std::string_view, 9> drawn_kinds{"top", "bottom", "shoe", "hat", "glass", "sock", "deck", "truck", "wheel"};
// The skater's outfit with each of its clothes and board parts drawn again among the same kind.
// What the riders wear: ReSkate's ghost skater costume, over whatever else.
inline constexpr const char *rider_costume = "Own_Costume_Gen_GhostSkater_00001";
// Where a ray from `from` to `to` strikes something that is no skater (`bodies`: their roots).
struct Struck {
    bool seen{}, hit{};
    Vec at{};
};
// The way from `side` (where the rider is, `s` along) to `to`, `ahead` metres on: whether it is
// clear, knee and chest high, and if not how far on (from the rider) something stands.
struct Way {
    bool seen{}, clear{};
    float until{};
};
// How high a board's middle rides over the ground (see first_lift).
struct Lift {
    std::vector<float> seen;
    float value = first_lift;
    double next{};
};
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

// death_race_line.cpp
void say(const std::string &text);
float distance(const Vec &a, const Vec &b);
Recording &recording();
Falls &falls();
bool pushing(std::uintptr_t base, std::uintptr_t entity);
bool capture(std::uintptr_t base, std::uintptr_t client, Recording &r, double now, const Moment &m);
Quat multiply(const Quat &a, const Quat &b);
float yaw(const Quat &q);
Transform blend(const Transform &a, const Transform &b, float t);
Vec root_at(const Line &line, double t);
void place(Pose &pose, const Vec &from, const Vec &to, float turn);
Vec turned(const Quat &q, const Vec &v);
void lean(Pose &pose, const Vec &toward, float angle);
float angle_between(float from, float to);
void shape(Line &line);
float deck_at(const Line &line, double s);
void on_deck(Pose &pose, float deck);
void course_at(const Line &line, double s, Vec &at, float &facing);
float course_speed(const Line &line, double s);
void blend_pose(const Pose &a, const Pose &b, float k, Pose &out);
bool air_at(const Line &line, double s);
// death_race_library.cpp
Library &library();
float head_height(const Pose &pose);
std::pair<float, float> stance(const std::vector<Frame> &f, std::size_t i, std::size_t k);
Vec forward_of(const Quat &q);
std::optional<float> stretch(const std::vector<Frame> &f, std::size_t i, std::size_t k, bool push, float least, float give = 1.0f,
                             float upright = std::numeric_limits<float>::quiet_NaN());
Style make_style(const std::vector<Frame> &f, std::size_t i, std::size_t k);
bool clean_air(const std::vector<Frame> &f, std::size_t i, std::size_t k);
void gather(const std::vector<Frame> &f, std::vector<Style> &rolling, std::vector<Style> &pushing, std::vector<Style> &tucking,
            std::vector<Style> &flying);
void save_library(const Library &lib);
StylePtr restyled(std::vector<Frame> frames);
void load_library(Library &lib);
const Library &loaded_library();
void learn(const std::vector<Frame> &frames);
void pitch(Pose &pose, float facing, float angle);
void style_pose(const Style &st, double t, const Vec &at, float facing, float slope, Pose &out, Pose &scratch);
void crouch_pose(const Style &st, const Vec &at, float facing, Pose &out);
void once_pose(const Style &st, double t, const Vec &at, float facing, float slope, Pose &out);
void emote_pose(const Style &st, double t, const Vec &at, float facing, Pose &out);
void air_pose(const Style &st, double t, const Vec &at, float facing, Pose &out);
std::pair<double, double> air_span(const Line &line, double s);
float slope_at(const Line &line, double s);
std::filesystem::path fall_path();
void write_transform(std::ofstream &out, const Transform &t);
bool read_transform(std::ifstream &in, Transform &t);
void write_frames(std::ofstream &out, const std::vector<Frame> &frames);
bool read_frames(std::ifstream &in, std::vector<Frame> &frames, std::uint32_t most);
std::filesystem::path library_path();
void save_fall(const Clip &clip);
std::shared_ptr<const Clip> load_fall();
std::shared_ptr<const Clip> fall_clip();
// death_race_sight.cpp
Vec off_course(const Line &line, double s, float side);
Struck struck(const Vec &from, const Vec &to, const std::vector<Vec> &bodies);
float lead_in_way(const Vec &from, const std::array<Vec, 3> &way, const Vec &lead);
Way look_along(const Line &line, double s, float side, float to, float ahead, const std::vector<Vec> &bodies,
               const Vec *lead, bool rays);
std::optional<std::pair<float, float>> ground_under(const Vec &centre, float facing, const std::vector<Vec> &bodies);
Lift &lift();
void measure_lift(const Pose &pose, double now);
void snap_board(Pose &pose, Rider &rider, float facing, const std::vector<Vec> &bodies, int &probes, double now, double dt,
                float reference, float fallback);
std::optional<float> ground_at(const Vec &at, const std::vector<Vec> &bodies);
void ground(const Line &line, Rider &rider, const std::vector<Vec> &bodies);
bool look(const Line &line, Rider &rider, float want, const std::vector<Vec> &bodies, const Vec *lead, int index);
// death_race_riders.cpp
Riders &riders();
std::size_t slot_of(int rider);
std::uint64_t micros();
bool poke(std::uintptr_t address, std::uint8_t value) noexcept;
void party_collision(std::uintptr_t base, std::uintptr_t entity, bool on);
void take_away(std::uintptr_t base, int index);
std::string kind_of(std::string_view asset);
bool drawn(const std::string &kind);
const std::map<std::string, std::vector<std::string>> &wardrobe();
multiplayer::Appearance dress(const multiplayer::Appearance &look, std::mt19937 &draw, std::uintptr_t base, std::uintptr_t entity);
void emote(Riders &r, Rider &rider, double now, bool wanted, const Vec &at, float facing);
void draw_ride(Riders &r, Rider &rider, int i);
void get_up(Rider &rider, const Line &line, double clock);
void ride_on(Rider &rider, const Tick &tick, Step &step);
void wait_pose(Riders &r, Rider &rider, Tick &tick, const Step &step);
void look_ahead(Riders &r, Rider &rider, Tick &tick, const Step &step, float want, float facing);
float steer(Rider &rider, double dt);
bool body_pose(Riders &r, Rider &rider, const Tick &tick, const Step &step, const Vec &at, float facing, float tilt);
void course_pose(Riders &r, Rider &rider, Tick &tick, const Step &step);
void move_aside(Riders &r, Rider &rider, const Tick &tick, const Step &step);
void run_in(Riders &r, Rider &rider, const Tick &tick, const Step &step);
void present(Riders &r, Rider &rider, Tick &tick, int i);

// Sampling frames recorded at `t` (death_race_line.cpp records them).
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
} // namespace dingosdk::death_race::riding
