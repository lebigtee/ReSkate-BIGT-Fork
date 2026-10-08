#pragma once
// The Death Race's state and the parts its files share (game thread). death_race.cpp holds the
// race itself, death_race_throwdown.cpp the game's throwdown that carries it, death_race_link.cpp
// races with other players, death_race_props.cpp the flags standing in the world, and
// death_race_commands.cpp the `race` commands.
#include "death_race.h"
#include "death_race_format.h"
#include "death_race_riders.h"
#include "Extension/Objects/network_object_runtime.h"
#include "Extension/Throwdowns/throwdown_relay.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace dingosdk::death_race::detail {
using Clock = std::chrono::steady_clock;
// Countdown: stopped on a start flag for this long (slower than this) starts it.
inline constexpr double countdown_length = 3.0;
inline constexpr float stopped_speed = 3.0f;   // metres per second
// A race opened as a throwdown starts with the game's own countdown: its clock starts when the
// skater sets off after the throwdown began (or after this long, for one who waits).
// The game's own GO, this long after the throwdown began (its intro, then its 3, 2, 1): the clock
// and the AI riders start then, and the skater goes onto its board on the start line just before.
inline constexpr double native_go = 9.7, board_before_go = 0.6;

// The throwdown's zone reaches past the race's farthest flag by this much.
inline constexpr float zone_margin = 25.0f;
// The skater's physics states off its board: walking, and falling.
inline constexpr std::uint32_t offboard_state = 504, wipeout_state = 300, ground_state = 100;
// After a fall the skater is getting over it until it has rolled on the ground this long.
inline constexpr double recovered_after = 0.5;
// A Death Race's queue counts down this long once it has its players (the AI riders fill it at
// once), so rivals have time to come and the host can start it sooner. The game's own setting,
// put back after.
inline constexpr const char *queue_timer_setting = "DingoThrowdowns.QueueStartTimer";
inline constexpr const char *race_queue_seconds = "30";
inline constexpr float teleport_distance = 8.0f; // in one tick: a teleport, not riding
// A bail in a race: the skater lies there this long, then is put back on its board where it rode
// this long before it fell (from the places it rode through, kept this long), facing the way it
// went. Its own teleport ends no race.
inline constexpr double respawn_after = 2.2, respawn_back = 1.0, respawn_kept = 8.0, respawn_landing = 3.0;
// The race's throwdown runs as long as a Skate Jam may (its timer kept off the screen); it ends
// when everyone has crossed the line. Everyone's score is where they finished: this much a place
// ahead of last, nothing for not finishing.
inline constexpr int throwdown_timer = 300, points_a_place = 100;
// The game shows its results as a throwdown ends: the race's text stays for them this long.
inline constexpr double dressed_after = 30.0;
// Everyone in, the race ends once the skater is up (if it fell), waiting no longer than this.
inline constexpr double longest_end_wait = 6.0;
// The AI riders' names, drawn anew for each race.
inline constexpr std::array<const char *, 16> rider_names{"Dex", "Mika", "Juno", "Kaz", "Nico", "Tess", "Rook", "Bo",
                                                          "Vince", "Lola", "Ozzy", "Remy", "Sasha", "Theo", "Wren", "Zed"};
inline constexpr double longest_race = 20 * 60.0;
inline constexpr auto result_shown = std::chrono::seconds(7), notice_shown = std::chrono::seconds(4);
// The tile opens the race whose start flag is this close.
inline constexpr float open_radius = 15.0f;
// From the tile's press, the game's throwdown placement has this long to begin.
inline constexpr double carrier_wait = 180.0;
// A Skate Jam placed this soon after the cursor was on the tile is the race's.
inline constexpr double focus_wait = 20.0;
// The AI riders' player ids in the race's throwdown: virtual participants, clear of the other
// players' (0x300 + session slot) and of coop challenges' (0x3c1 on).
inline constexpr std::uint32_t rider_base = 0x3e0;
// The props that stand for a race: the game's own build-kit objects (the Object Dropper's).
inline constexpr const char *start_prop = "own_bkprops_generic_startline_00001";
inline constexpr const char *finish_prop = "own_bkprops_generic_finishline_00001";
inline constexpr const char *checkpoint_prop = "own_bkprops_generic_slalomflag_00001";
inline constexpr float gate_half_width = 3.5f; // a checkpoint's two flags, either side of its point

// The AI riders' player records, by rider (0: none yet); read by the game's UI lookups (rider_info).
inline std::array<std::atomic<std::uint64_t>, 4> rider_records{};
// A player record (UIPlayerInfo) for a rider: what the game's leaderboards show of a player (its
// name), as ReSkate's party records are made (Extension/Multiplayer/Hud/native_party.cpp), with
// nothing else of the party's. Made once per UI model manager; `made` keeps it. Game thread.
struct RiderRecord {
    std::uintptr_t manager{};
    std::uint64_t handle{};
    std::string name;
};

struct State {
    bool loaded{};
    std::map<std::string, std::vector<Race>> maps; // by level
    std::string level;
    std::uintptr_t base{};
    int selected = -1;
    // A new race being set: its start, then checkpoints.
    bool building{};
    std::vector<Vec3> draft;
    // The race in progress.
    Phase phase{};
    int active = -1;
    double phase_started{}; // real seconds: the countdown's or the race's start
    std::size_t next{};
    double time{}, previous_best{};
    bool record{};
    bool armed_away{true}; // left every start flag since the last race: a stop on one may start it
    // A race opened as a throwdown from the tile: the race, when, and how far the game's
    // throwdown got (placed, then set up). When that throwdown starts, so does the race.
    struct Carrier {
        int race = -1;
        double since{};
        bool created{}, configured{};
        std::uint32_t mmid{};
        int riders{}, riders_in{}; // AI riders it takes, and those already in its queue
        bool riders_off{};         // they stepped aside for other players who joined
        bool done{};               // its race was run to the finish
    } carrier;
    bool carried{}; // the race in progress was started by its throwdown
    int focus_race = -1; // the race the tile last offered under the cursor, and when
    double focus_at{};
    bool solo_on_stop{}; // `race solo on`: a stop on the shown race's start line counts it down alone
    int riders{};        // AI riders in the races opened (0 to 4)
    // A teleport we asked for lands a few ticks later: no arming from where the skater was.
    double no_arm_until{};
    float heading{};       // the skater's, degrees (0 = +Z)
    float draft_heading{}; // the new race's start faces this way until its next point is set
    // The props last handed over, when, and how many stand.
    std::vector<NetworkObject> props;
    double props_at{};
    bool props_live{};
    Clock::time_point result_until{}, notice_until{};
    std::string notice;
    // The skater.
    bool skater{};
    Vec3 position{};
    float speed{};
    std::optional<Vec3> last_position;
    std::uintptr_t client{};
    // The race's start teleport (0: none due) and when the skater has landed from it.
    double start_teleport_at{}, go_at{};
    // The lines the races' AI riders ride (death_race_riders.h), by map and race name; the one
    // they ride now; what is being recorded (the ride setting a finish, or a run); the go for the
    // riders (the throwdown's countdown or the skater's own set-off, whichever comes first).
    std::map<std::string, std::shared_ptr<const Line>> lines;
    std::shared_ptr<const Line> riding;
    enum class Recording { none, route, run } recording{};
    double riders_go_at{};
    std::optional<std::string> queue_timer_before; // the queue countdown setting while a race has its own
    int place{}, racers{}; // the last finish: the skater's place among how many
    std::uint32_t physics_state{};
    std::uint64_t falls_seen{}; // the skater's falls counted so far (no_bail.h)
    bool falls_known{};
    bool recovering{}; // from a fall until it rolls again (recovered_after)
    double rolling_since{};
    // The scores written to the race's throwdown (by player), and when next; its end asked for.
    std::map<std::uint32_t, std::int32_t> scores;
    double next_scores{};
    bool ending{};
    double all_in_at{}; // when everyone had crossed the line (0: not yet)
    std::uint64_t markers_refused{}; // session marker teleports refused so far (to tell of new ones)
    // Races with other players (Extension/Throwdowns/throwdown_relay.cpp). The race in progress has
    // other players in it, and its racers as the relay last told (kept for the results once it lets
    // go); the other player's race last begun here, and the last one announced; near or in another
    // player's race on offer (the Jam's text reads as the race's).
    bool linked{};
    std::optional<multiplayer::RunningRace> link;
    std::pair<std::uint64_t, std::uint32_t> begun{}, announced{};
    bool linked_dress{};
    std::map<std::pair<std::uint64_t, std::uint32_t>, bool> unreadable; // offers already said to be of no use here
    double dress_until{}; // the race's text stays this long after its throwdown ends
    std::array<std::string, 4> names; // the riders' this race
    std::array<RiderRecord, 4> records;
    double next_records{};
    // Racing: where the skater rode on its board lately (when, where, facing); a bail's respawn,
    // when due, to where and facing which way; until when its teleport is no reason to stop.
    struct Place {
        double t{};
        Vec3 at{};
        float heading{};
    };
    std::vector<Place> rode;
    double respawn_at{}, respawn_landing_until{};
    Place respawn_to;
};
// The one state, and what reaches the game thread from elsewhere: the tile's press (the game's UI)
// and the throwdown observations (its network sends and server). Drained by tick().
State &state();
struct Inbox {
    std::mutex mutex;
    std::optional<std::pair<TileAction, int>> pressed;
    std::optional<int> focused;
    std::vector<std::tuple<Throwdown, std::string, std::uint32_t>> events;
};
Inbox &inbox();

// death_race.cpp
double seconds();
void say(const std::string &text);
std::string clock_text(double s);
float across(const Vec3 &a, const Vec3 &b);
float distance(const Vec3 &a, const Vec3 &b);
bool at_flag(const Vec3 &skater, const Vec3 &flag);
std::vector<Race> &races(); // this map's
void load(State &s);
void save(const State &s);
void tell(State &s, std::string text);
void stop(State &s, const char *why);
void remove_race(State &s, int index);
void forget(State &s, int index);
std::size_t flags_of(const Race &race);
const Vec3 &flag_at(const Race &race, std::size_t next);
void advance(State &s, double now);
std::string next_name(const std::vector<Race> &list);
int start_near(State &s);
// death_race_throwdown.cpp
void write_scores(State &s, double now, bool now_or_never = false);
void end_when_everyone_is_in(State &s);
void put_back_queue_timer(State &s);
void drain(State &s, double now);
// death_race_link.cpp
int linked_index();
int add_race(State &s, Race race);
void follow_offers(State &s);
void begin_carried(State &s, double now);
void follow_running(State &s, double now);
void link_place(State &s);
int riders_for(const State &s);
// death_race_props.cpp
float facing(const Vec3 &from, const Vec3 &to, float otherwise);
Vec3 grounded(const State &s, const Vec3 &at);
void update_props(State &s, double now);
} // namespace dingosdk::death_race::detail
