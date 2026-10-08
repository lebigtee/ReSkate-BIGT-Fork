#pragma once
// Death Races: skate's downhill races, made by the player. Set a start flag where you stand,
// checkpoints if you like, ride to the finish and set its flag; then stop on a start flag and
// race: 3, 2, 1, go, through every checkpoint in turn, to the finish, against the clock and your
// best time. Everything is on the controller: the DEATH RACE tile in the radial menu's
// Throwdowns page (native_throwdown_tile.cpp) sets a race's flags and opens it as a throwdown
// whose flag rivals join at, and the pause menu's Custom Stuff page (native_tools_view.h) lists
// and manages the races. A race run to its finish through its throwdown is over: the result
// shows, the throwdown ends and the race goes, flags and all, so the tile offers a new one. Every
// action is also a `race` console command.
//
// With other players in a multiplayer session, a race is theirs too when they run this build: its
// throwdown is linked through the throwdown relay (Extension/Throwdowns/throwdown_relay.h), which
// carries the race's flags and each racer's finish, under a series other ReSkate builds ignore.
// Each machine runs the race on its own copy of the throwdown and ranks everyone the same way; the
// AI riders step aside as soon as another player joins.
//
// The game thread (death_race.cpp) follows the local skater through the trainer's telemetry and
// keeps the races, per map, in %LOCALAPPDATA%\ReSkate\deathrace\races.json. The overlay
// (death_race_hud.cpp) draws the flags in the world, the countdown, the clock and the result
// from the snapshot published here (death_race_view.cpp).
#include "death_race_limits.h"
#include "Engine/Game/UI/game_view.h"
#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace dingosdk::death_race {
using Vec3 = std::array<float, 3>;

// A flag's reach: the skater is at it within this distance across the ground and this height.
inline constexpr float flag_radius = 5.0f, flag_height = 4.0f;

struct Race {
    std::string name;
    Vec3 start{};
    std::vector<Vec3> checkpoints;
    Vec3 finish{};
    double best{}; // seconds; 0: never finished
    // The way the skater faced setting the start (degrees, 0 = +Z): the race starts facing it.
    float heading{};
    bool has_heading{};
    // Another player's race, run here through their linked throwdown (their Steam ID and their
    // drop's number): never saved, gone with their drop.
    std::uint64_t from{};
    std::uint32_t from_id{};
};

enum class Phase : std::uint8_t {
    idle,      // riding around: start flags shown
    countdown, // stopped on a start flag: 3, 2, 1
    racing,    // the clock runs
    finished,  // the result shows for a moment
};

// What the overlay draws. Published every client tick.
struct View {
    Phase phase{};
    bool hidden{};                // a game menu is up
    std::vector<Race> races;      // this map's
    int active = -1;              // the race being counted down, raced or just finished
    // The one race that stands in the world (its props): the one selected, made or opened this
    // session. A relaunch shows none until one is picked again.
    int shown = -1;
    bool props_live{};            // the game's own props stand for it (else the overlay draws flags)
    bool carried{};               // the race in progress started with its throwdown
    int place{}, racers{};        // the last finish: the skater's place, among how many (with the AI riders)
    // Building a new race: the points set so far (start, then checkpoints).
    bool building{};
    std::vector<Vec3> draft;
    // Counting down and racing.
    double countdown{};           // seconds left before go
    double elapsed{};             // seconds since go
    std::size_t next{};           // the next flag: 0..checkpoints-1, then the finish
    Vec3 target{};                // where that flag is
    float distance{};             // metres to it
    // The result.
    double time{}, previous_best{};
    bool record{};
    std::string notice;           // a line to show for a moment (a false start, a cancelled race)
    std::chrono::steady_clock::time_point notice_until{}, result_until{}, published{};
    Vec3 skater{};
    bool skater_known{};
    GameView camera;              // the client's camera this tick
};
// Thread-safe snapshot (death_race_view.cpp, part of the overlay).
View view() noexcept;
void publish(const View &) noexcept;
// True while a race is counted down or raced (any thread): other features stay out of the way
// (the Hall of Meat's slow motion would stretch the clock).
bool racing() noexcept;
void set_racing(bool) noexcept;

// What the pause menu shows (game thread, like the menu's own tick).
struct Status {
    bool ready{};                 // a map and a skater
    std::vector<Race> races;
    int selected = -1;
    bool building{};
    std::size_t draft_points{};   // the start and checkpoints set so far
    Phase phase{};
};
Status status() noexcept;

// The DEATH RACE tile in the throwdown mode select. What it does depends on where the skater is:
// away from every race it sets a new race's start; while one is being set, its finish; on a
// race's start it opens that race as a throwdown (the game's own flag, which rivals join at, and
// the host's settings), carried by the game's Skate Jam: when that throwdown starts, so does the
// race.
enum class TileAction : std::uint8_t { create, finish, open, busy };
struct Tile {
    TileAction action{};
    int race = -1; // open: the race it opens
    std::string title, description;
    // The mode select's own navigation the tile also sends: "JamSession" opens the game's
    // throwdown flow; empty, the tile is ours alone.
    std::string navigation;
};
// Game thread (the native menu's tick).
Tile tile() noexcept;
// How many AI riders race the races opened (0 to 4): the setup page's slider sets it.
int riders() noexcept;
// The players its throwdown's queue takes. A full queue starts at once, so alone the AI riders
// never fill it (the queue's countdown stays); with other players in the session it holds
// most_racers, and its AI riders (who step aside when one joins) leave room for them.
int queue_players() noexcept;
// The race's throwdown's timer (seconds): as long as a Skate Jam lasts. It is no limit: the race
// ends its throwdown when everyone has crossed the line.
int throwdown_seconds() noexcept;
void set_riders(int riders) noexcept;
// How far from `from` (the throwdown's flag) its zone must reach to take in every flag of the race
// being set up, across the ground; 0 when there is none.
float zone_reach(const std::array<float, 3> &from) noexcept;
// The race being opened or run as a Skate Jam: the game's Jam text reads as the Death Race's
// (Extension/Throwdowns), from the cursor on its tile until its throwdown ends.
bool dressed() noexcept;
// The race's host is on its throwdown's setup page: the Jam's own settings are taken out of it.
bool setting_up() noexcept;
// A race of a throwdown is on: counting down, racing, or finished and waiting for the others.
bool race_on() noexcept;
// The throwdown modes page opened: a finished race's text (kept for the game's results) goes.
void throwdown_modes_opened() noexcept;
// The player record (UIPlayerInfo) of AI rider `player_id` (its name on the throwdown's
// leaderboards), or 0: not a rider, or its record is not made yet.
std::uint64_t rider_info(std::uint32_t player_id) noexcept;
// Whether a server participant id is one of the AI riders (no relayed player).
bool is_rider(std::uint32_t player_id) noexcept;
// Races with other players (Extension/Throwdowns/throwdown_relay.cpp; game thread): the race the
// local player's drop `mmid` runs, encoded for the others (empty: none).
std::vector<std::uint8_t> hosted_race(std::uint32_t mmid) noexcept;
// The tile was pressed while it showed `action` (the game's UI thread; handled on the next tick).
void tile_pressed(TileAction action, int race) noexcept;
// The cursor is on the tile while it opens `race` in the Skate Jam's place: a Skate Jam placed
// right after is that race's (the tile's press is the game's own then; game thread).
void tile_focused(int race) noexcept;
// What the local player's throwdowns do (throwdown_lab.cpp's observations; any thread).
enum class Throwdown : std::uint8_t { created, configured, cancelled, started, entered, ended };
void throwdown_event(Throwdown what, std::string_view series, std::uint32_t mmid) noexcept;

// Game thread (death_race.cpp).
// Each client tick, after the trainer's (whose telemetry it reads); `level` the loaded map.
void tick(std::uintptr_t base, std::uintptr_t client, bool playing, const std::string &level) noexcept;
// Runs one `race` command and returns the line to print. Only from the console dispatcher.
std::string command(std::string_view verb, const std::vector<std::string> &arguments);
} // namespace dingosdk::death_race
