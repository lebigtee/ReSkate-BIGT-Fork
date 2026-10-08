#pragma once
// A Death Race's AI riders. A race keeps a line: the local skater's own ride from its start to its
// finish, recorded pose by pose (the ride that set the finish, then the fastest run since). Its AI
// riders ride that line's path again, each at a pace of its own that swells and ebbs, but never as
// the skater's body did there (its tricks, its falls): their bodies roll, push, tuck and fly with
// stretches of the skater's plain riding and airs from all its rides (the riding library,
// %LOCALAPPDATA%\ReSkate\deathrace\riding.bin), and wait on the start line and past the finish
// with its emotes. They are shown as the game's own skaters, as other players' skaters are in a
// multiplayer session (Extension/Multiplayer/Remote), with the same collision capsules.
//
// Each wanders a little either side of the course, looking ahead with the game's own collision
// rays and keeping off walls and props (back to the course, which the skater rode, or braking when
// nothing ahead is clear); its board follows the ground under it. A light run-in pushes both
// apart, the rider stumbling with one of the skater's own sketchy landings; a hard one floors
// both: the rider plays the local skater's own last fall where it is, gets up and rides on that
// much later, and the local skater bails as a native collision makes it (Extension/Skater/no_bail.h).
//
// Game thread only (the client tick).
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace dingosdk::death_race {
struct Line;
// What the local skater is doing as a pose is taken: on its board (not on foot, not falling);
// riding plainly (on its board on the ground, no trick, not getting over a fall); in the air;
// getting over a fall (from the fall until it rides plainly again); its physics state.
struct Moment {
    bool board{}, plain{}, air{}, fallen{};
    std::uint32_t state{};
};
// Recording the local skater, 20 poses a second, until line_stop; the line starts on the board.
// The skater's plain riding also goes into the riders' riding library (death_race_library.cpp).
void line_start(double now);
void line_capture(std::uintptr_t base, std::uintptr_t client, double now, const Moment &moment);
// The ride from the moment the skater set off on its board; null when too short (or nothing
// was recorded).
std::shared_ptr<const Line> line_stop();
double line_seconds(const Line &line) noexcept;
// The local skater's falls, which the riders fall the same way: `fell` when a new one began.
void fall_capture(std::uintptr_t base, std::uintptr_t client, double now, bool fell);
// Most AI riders a race takes.
inline constexpr int most_riders = 4;
// `count` AI riders on `line`: side by side on `start` (the race's start line) while `clock` < 0,
// then each on its way at its own pace, `clock` seconds after the go. A count of 0 (or no line)
// takes them all away.
void show_riders(std::uintptr_t base, std::uintptr_t client, const Line *line, int count, double clock, double now,
                 const std::array<float, 3> *start = nullptr);
std::size_t riders_shown() noexcept;
// The time each of `count` riders takes to the finish of `line`, in rider order: the line at its
// pace, plus what its falls cost it.
std::vector<double> rider_times(const Line &line, int count);
// The time each of `count` riders finished in (the race's clock), or -1 while still on its way.
std::vector<double> rider_finishes(int count);
// About how long a rider at `pace` (its share of the skater's speed along the course) takes over
// `line`'s course, from the go.
double ride_seconds(const Line &line, double pace) noexcept;
} // namespace dingosdk::death_race
