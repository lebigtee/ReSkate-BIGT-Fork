#include "death_race.h"
#include "death_race_riders.h"
#include "Engine/Core/Json/json.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Game/Abi/native_data.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Extension/Multiplayer/Hud/game_ui_state.h"
#include "Extension/Multiplayer/Remote/native_skater.h"
#include "Extension/Objects/network_object_runtime.h"
#include "Extension/Profile/local_profile_runtime.h"
#include "Extension/Settings/named_settings.h"
#include "Extension/Skater/no_bail.h"
#include "Extension/Throwdowns/native_throwdowns.h"
#include "Extension/Throwdowns/throwdown_lab.h"
#include "Extension/Throwdowns/throwdown_relay.h"
#include "Extension/Trainer/trainer.h"
#include <windows.h>
#include <shlobj.h>
#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <numbers>
#include <optional>
#include <random>
#include <tuple>

// Game thread only. See death_race.h.
namespace dingosdk::death_race {
namespace {
using Clock = std::chrono::steady_clock;
// Countdown: stopped on a start flag for this long (slower than this) starts it.
constexpr double countdown_length = 3.0;
constexpr float stopped_speed = 3.0f;   // metres per second
// A race opened as a throwdown starts with the game's own countdown: its clock starts when the
// skater sets off after the throwdown began (or after this long, for one who waits).
// The game's own GO, this long after the throwdown began (its intro, then its 3, 2, 1): the clock
// and the AI riders start then, and the skater goes onto its board on the start line just before.
constexpr double native_go = 9.7, board_before_go = 0.6;

// The throwdown's zone reaches past the race's farthest flag by this much.
constexpr float zone_margin = 25.0f;
// The skater's physics states off its board: walking, and falling.
constexpr std::uint32_t offboard_state = 504, wipeout_state = 300, ground_state = 100;
// After a fall the skater is getting over it until it has rolled on the ground this long.
constexpr double recovered_after = 0.5;
// A Death Race's queue counts down this long once it has its players (the AI riders fill it at
// once), so rivals have time to come and the host can start it sooner. The game's own setting,
// put back after.
constexpr const char *queue_timer_setting = "DingoThrowdowns.QueueStartTimer";
constexpr const char *race_queue_seconds = "30";
constexpr float teleport_distance = 8.0f; // in one tick: a teleport, not riding
// A bail in a race: the skater lies there this long, then is put back on its board where it rode
// this long before it fell (from the places it rode through, kept this long), facing the way it
// went. Its own teleport ends no race.
constexpr double respawn_after = 2.2, respawn_back = 1.0, respawn_kept = 8.0, respawn_landing = 3.0;
// The race's throwdown runs as long as a Skate Jam may (its timer kept off the screen); it ends
// when everyone has crossed the line. Everyone's score is where they finished: this much a place
// ahead of last, nothing for not finishing.
constexpr int throwdown_timer = 300, points_a_place = 100;
// The game shows its results as a throwdown ends: the race's text stays for them this long.
constexpr double dressed_after = 30.0;
// Everyone in, the race ends once the skater is up (if it fell), waiting no longer than this.
constexpr double longest_end_wait = 6.0;
// The AI riders' names, drawn anew for each race.
constexpr std::array<const char *, 16> rider_names{"Dex", "Mika", "Juno", "Kaz", "Nico", "Tess", "Rook", "Bo",
                                                   "Vince", "Lola", "Ozzy", "Remy", "Sasha", "Theo", "Wren", "Zed"};
// Their player records, by rider (0: none yet); read by the game's UI lookups (rider_info).
std::array<std::atomic<std::uint64_t>, 4> rider_records{};
// A player record (UIPlayerInfo) for a rider: what the game's leaderboards show of a player (its
// name), as ReSkate's party records are made (Extension/Multiplayer/Hud/native_party.cpp), with
// nothing else of the party's. Made once per UI model manager; `made` keeps it. Game thread.
struct RiderRecord {
    std::uintptr_t manager{};
    std::uint64_t handle{};
    std::string name;
};
std::uint64_t rider_record(std::uintptr_t base, std::uint32_t player_id, const std::string &name, RiderRecord &made) {
    namespace engine = addr::engine;
    auto &n = game::native_data().models;
    if (!n.create || !n.value || !n.publish || !game::native_data().values.assign) return 0;
    std::uintptr_t ui{}, manager{};
    if (!memory::read(base + engine::ui_manager, ui) || !ui || !memory::read(ui + 0x140, manager) || !manager) return 0;
    game::ModelWriteLock lock(manager);
    if (made.manager == manager && made.handle && made.name == name && n.value(manager, made.handle, 0, 0)) return made.handle;
    if (made.manager != manager || !made.handle || !n.value(manager, made.handle, 0, 0)) {
        // Keyed apart from any player's: "RDR" and the rider's native id.
        made.handle = n.create(manager, base + engine::player_info_type, 0x5244520000000000ULL | player_id,
                               game::native_name_hash("ReSkate.DeathRaceRider"), false, 2);
        made.manager = manager;
        if (!made.handle) return 0;
    }
    alignas(8) std::array<std::byte, 0x108> info{};
    reinterpret_cast<void (*)(void *)>(base + engine::card_info_construct)(info.data());
    const auto put = [&](std::size_t offset, auto value) { std::memcpy(info.data() + offset, &value, sizeof(value)); };
    std::uintptr_t text{};
    game::native_data().values.assign(&text, name.c_str(), static_cast<std::uint32_t>(name.size()));
    put(0x28, text);
    put(0x38, text);
    put(0x40, 0x5244520000000000ULL | player_id);
    put(0x4c, player_id);
    put(0xa0, made.handle);
    put(0x95, std::uint8_t{1});  // unlocked
    put(0xcc, std::uint32_t{1}); // not a friend
    put(0xf0, std::uint32_t{4}); // online
    put(0xf4, std::uint32_t{4}); // on Steam (no EA logo beside the name)
    put(0xc0, std::int32_t{-1}); // no party colour
    put(0xdc, std::int32_t{-1}); // no voice
    put(0x100, true);            // present
    const bool published = n.publish(manager, made.handle, base + engine::player_info_type, info.data());
    if (text) reinterpret_cast<void (*)(std::uintptr_t *)>(base + engine::native_text_release)(&text);
    if (!published || !n.value(manager, made.handle, 0, 0)) {
        made.handle = 0;
        return 0;
    }
    made.name = name;
    return made.handle;
}
constexpr double longest_race = 20 * 60.0;
constexpr auto result_shown = std::chrono::seconds(7), notice_shown = std::chrono::seconds(4);
// The tile opens the race whose start flag is this close.
constexpr float open_radius = 15.0f;
// From the tile's press, the game's throwdown placement has this long to begin.
constexpr double carrier_wait = 180.0;
// A Skate Jam placed this soon after the cursor was on the tile is the race's.
constexpr double focus_wait = 20.0;
// The AI riders' player ids in the race's throwdown: virtual participants, clear of the other
// players' (0x300 + session slot) and of coop challenges' (0x3c1 on).
constexpr std::uint32_t rider_base = 0x3e0;
// The props that stand for a race: the game's own build-kit objects (the Object Dropper's).
constexpr const char *start_prop = "own_bkprops_generic_startline_00001";
constexpr const char *finish_prop = "own_bkprops_generic_finishline_00001";
constexpr const char *checkpoint_prop = "own_bkprops_generic_slalomflag_00001";
constexpr float gate_half_width = 3.5f; // a checkpoint's two flags, either side of its point

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
State &state() {
    static auto *value = new State;
    return *value;
}
// What reaches the game thread from elsewhere: the tile's press (the game's UI) and the
// throwdown observations (its network sends and server). Drained by tick().
struct Inbox {
    std::mutex mutex;
    std::optional<std::pair<TileAction, int>> pressed;
    std::optional<int> focused;
    std::vector<std::tuple<Throwdown, std::string, std::uint32_t>> events;
};
Inbox &inbox() {
    static auto *value = new Inbox;
    return *value;
}
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
void say(const std::string &text) { logging::write(logging::Level::info, logging::Channel::skater, text); }
std::string clock_text(double s) {
    const int whole = static_cast<int>(s);
    return std::format("{}:{:02}.{:02}", whole / 60, whole % 60, static_cast<int>((s - whole) * 100));
}
float across(const Vec3 &a, const Vec3 &b) { return std::hypot(a[0] - b[0], a[2] - b[2]); }
float distance(const Vec3 &a, const Vec3 &b) { return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2])); }
bool at_flag(const Vec3 &skater, const Vec3 &flag) { return across(skater, flag) <= flag_radius && std::abs(skater[1] - flag[1]) <= flag_height; }
std::vector<Race> &races() { return state().maps[state().level]; }

// ---- the races file ------------------------------------------------------------------------
std::filesystem::path races_path() {
    PWSTR raw{};
    std::filesystem::path result;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw)) && raw)
        result = std::filesystem::path(raw) / L"ReSkate" / L"deathrace" / L"races.json";
    if (raw) CoTaskMemFree(raw);
    return result;
}
Json point_json(const Vec3 &p) { return Json::array({p[0], p[1], p[2]}); }
std::optional<Vec3> point_of(const Json &j) {
    if (!j.is_array() || j.size() != 3) return std::nullopt;
    Vec3 p{};
    for (std::size_t i = 0; i < 3; ++i) {
        if (!j.at(i).is_number()) return std::nullopt;
        p[i] = j.at(i).get<float>();
        if (!std::isfinite(p[i])) return std::nullopt;
    }
    return p;
}
void load(State &s) {
    if (s.loaded) return;
    s.loaded = true;
    try {
        std::ifstream file(races_path(), std::ios::binary);
        if (!file) return;
        const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (text.empty() || text.size() > 4 * 1024 * 1024) return;
        const auto json = Json::parse(text);
        if (!json.is_object() || !json.contains("maps") || !json.at("maps").is_object()) return;
        std::size_t count{};
        for (const auto &[level, list] : json.at("maps").items()) {
            if (!list.is_array()) continue;
            auto &into = s.maps[level];
            for (std::size_t i = 0; i < list.size() && into.size() < 100; ++i) {
                const auto &r = list.at(i);
                if (!r.is_object() || !r.contains("start") || !r.contains("finish")) continue;
                const auto start = point_of(r.at("start")), finish = point_of(r.at("finish"));
                if (!start || !finish) continue;
                Race race;
                race.name = r.value("name", std::string("Race"));
                if (race.name.empty() || race.name.size() > 64) race.name = "Race";
                race.start = *start;
                race.finish = *finish;
                if (r.contains("checkpoints") && r.at("checkpoints").is_array())
                    for (std::size_t c = 0; c < r.at("checkpoints").size() && race.checkpoints.size() < 32; ++c)
                        if (const auto p = point_of(r.at("checkpoints").at(c))) race.checkpoints.push_back(*p);
                race.best = std::max(0.0, r.value("best", 0.0));
                if (r.contains("heading") && r.at("heading").is_number()) {
                    race.heading = r.at("heading").get<float>();
                    race.has_heading = std::isfinite(race.heading);
                }
                into.push_back(std::move(race));
                ++count;
            }
        }
        say(std::format("Death Race: {} race(s) loaded.", count));
    } catch (...) {
        say("Death Race: races.json is damaged; it is left as it is and not read.");
        s.maps.clear();
    }
}
void save(const State &s) {
    try {
        const auto path = races_path();
        if (path.empty()) return;
        auto maps = Json::object();
        for (const auto &[level, list] : s.maps) {
            if (list.empty()) continue;
            auto out = Json::array();
            for (const auto &race : list) {
                if (race.from) continue; // another player's
                auto r = Json::object();
                r["name"] = race.name;
                r["start"] = point_json(race.start);
                auto checkpoints = Json::array();
                for (const auto &c : race.checkpoints) checkpoints.push_back(point_json(c));
                r["checkpoints"] = std::move(checkpoints);
                r["finish"] = point_json(race.finish);
                r["best"] = race.best;
                if (race.has_heading) r["heading"] = race.heading;
                out.push_back(std::move(r));
            }
            maps[level] = std::move(out);
        }
        auto json = Json::object();
        json["schema"] = 1;
        json["maps"] = std::move(maps);
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
        say("Death Race: could not save races.json.");
    }
}

// ---- the race ------------------------------------------------------------------------------
void tell(State &s, std::string text) {
    s.notice = std::move(text);
    s.notice_until = Clock::now() + notice_shown;
    say("Death Race: " + s.notice);
}
void stop(State &s, const char *why) {
    if (s.phase == Phase::countdown || s.phase == Phase::racing) {
        const auto name = s.active >= 0 && s.active < static_cast<int>(races().size()) ? races()[s.active].name : std::string("the race");
        tell(s, std::format("{} cancelled ({}).", name, why));
    }
    s.phase = Phase::idle;
    s.active = -1;
    s.carried = false;
    s.start_teleport_at = 0;
    if (s.recording == State::Recording::run) {
        line_stop();
        s.recording = State::Recording::none;
    }
    set_racing(false);
}
void write_scores(State &s, double now, bool now_or_never = false);
// Everyone has crossed the line: the race's throwdown ends for everyone.
void end_when_everyone_is_in(State &s) {
    if (!s.carried || s.phase != Phase::finished || s.ending) return;
    bool in{};
    if (s.linked && s.link) {
        // With other players: each has crossed the line, quit, or left the session.
        in = std::all_of(s.link->racers.begin(), s.link->racers.end(),
                         [](const multiplayer::RaceRacer &r) { return r.local || r.finish > 0 || r.out; });
    } else {
        const auto times = rider_finishes(s.carrier.riders_in);
        in = std::none_of(times.begin(), times.end(), [](double t) { return t < 0; });
    }
    if (!in) {
        s.all_in_at = 0;
        return;
    }
    // Not while the skater lies on the ground (its bodies against the riders, the finish arch):
    // once it is up again, or after a while all the same.
    const double now = seconds();
    if (!s.all_in_at) s.all_in_at = now;
    const bool down = s.recovering || s.physics_state == wipeout_state;
    if (down && now - s.all_in_at < longest_end_wait) return;
    s.ending = true;
    // Everyone's final score first (the last one in may not have its own yet): the results show them.
    write_scores(s, seconds(), true);
    // The skater leaves it first (as Quit does), then it ends as one of its riders. Every "you
    // left" that comes of it is kept from the skater: the race's own result says how it went.
    multiplayer::quiet_leave_confirmations(3000);
    if (s.linked && s.link) {
        // With other players, as one of them: each machine ends its own copy once all are in.
        multiplayer::throwdown_relay_end_race();
        say("Death Race: everyone has crossed the line; the race's throwdown ends here.");
        return;
    }
    const auto id = multiplayer::local_native_player_id();
    const bool left = id && multiplayer::queue_challenge_leave(id);
    const bool ended = s.carrier.riders_in > 0 ? multiplayer::queue_throwdown_destroy_event(rider_base + 1)
                                               : id && multiplayer::queue_throwdown_destroy_event(id);
    say(std::format("Death Race: everyone has crossed the line; the throwdown ends ({}{}).", left ? "the skater left it" : "the skater could not leave it",
                    ended ? ", ended" : ", could not be ended"));
}
// A race over (its throwdown ended): it goes, flags and all, as if never set; the tile offers a
// new one.
void remove_race(State &s, int index) {
    auto &list = races();
    if (index < 0 || index >= static_cast<int>(list.size())) return;
    s.lines.erase(s.level + "/" + list[index].name);
    list.erase(list.begin() + index);
    const auto shift = [&](int &i) {
        if (i == index) i = -1;
        else if (i > index) --i;
    };
    shift(s.selected);
    shift(s.focus_race);
    shift(s.active);
    if (s.carrier.race == index) s.carrier = {};
    else if (s.carrier.race > index) --s.carrier.race;
}
void forget(State &s, int index) {
    if (index < 0 || index >= static_cast<int>(races().size())) return;
    const auto name = races()[index].name;
    remove_race(s, index);
    s.riding.reset();
    save(s);
    say(std::format("Death Race: {} is over; its flags come down.", name));
}

// ---- races with other players (Extension/Throwdowns/throwdown_relay.cpp) -------------------------
// Another player's race stands last in this map's list (Race::from) while it is shown or run here.
int linked_index() {
    const auto &list = races();
    for (std::size_t i = 0; i < list.size(); ++i)
        if (list[i].from) return static_cast<int>(i);
    return -1;
}
// A new race of the player's own goes ahead of it.
int add_race(State &s, Race race) {
    auto &list = races();
    const int linked = linked_index();
    const int at = linked >= 0 ? linked : static_cast<int>(list.size());
    list.insert(list.begin() + at, std::move(race));
    const auto shift = [&](int &i) {
        if (i >= at) ++i;
    };
    shift(s.selected);
    shift(s.focus_race);
    shift(s.active);
    shift(s.carrier.race);
    return at;
}
// A racer's place across the start line: the host in the middle, the others either side.
float lane_offset(std::size_t lane) {
    if (!lane) return 0.0f;
    const float step = lane_spacing * static_cast<float>((lane + 1) / 2);
    return lane % 2 ? step : -step;
}
// A race for the other players: the map, its name and its flags.
constexpr std::uint8_t race_format = 1;
std::vector<std::uint8_t> encode_race(const std::string &level, const Race &race) {
    std::vector<std::uint8_t> out{race_format};
    const auto text = [&](const std::string &value, std::size_t most) {
        const auto n = std::min(value.size(), most);
        out.push_back(static_cast<std::uint8_t>(n));
        out.insert(out.end(), value.begin(), value.begin() + static_cast<std::ptrdiff_t>(n));
    };
    const auto number = [&](float value) {
        std::uint32_t bits{};
        std::memcpy(&bits, &value, sizeof(bits));
        for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(bits >> (8 * i)));
    };
    const auto point = [&](const Vec3 &p) {
        for (const auto v : p) number(v);
    };
    text(level, 255);
    text(race.name, 64);
    point(race.start);
    point(race.finish);
    number(race.heading);
    out.push_back(race.has_heading ? 1 : 0);
    const auto count = std::min<std::size_t>(race.checkpoints.size(), 32);
    out.push_back(static_cast<std::uint8_t>(count));
    for (std::size_t i = 0; i < count; ++i) point(race.checkpoints[i]);
    return out;
}
std::optional<std::pair<std::string, Race>> decode_race(const std::vector<std::uint8_t> &bytes) {
    std::size_t at{};
    bool ok = true;
    const auto byte = [&]() -> std::uint8_t {
        if (at >= bytes.size()) {
            ok = false;
            return 0;
        }
        return bytes[at++];
    };
    const auto text = [&](std::size_t most) {
        const std::size_t n = byte();
        if (!ok || n > most || n > bytes.size() - at) {
            ok = false;
            return std::string();
        }
        std::string value(reinterpret_cast<const char *>(bytes.data() + at), n);
        at += n;
        return value;
    };
    const auto number = [&]() {
        std::uint32_t bits{};
        for (int i = 0; i < 4; ++i) bits |= std::uint32_t{byte()} << (8 * i);
        float value{};
        std::memcpy(&value, &bits, sizeof(value));
        if (!std::isfinite(value) || std::abs(value) > 1e6f) ok = false;
        return value;
    };
    const auto point = [&]() {
        Vec3 p{};
        for (auto &v : p) v = number();
        return p;
    };
    if (byte() != race_format) return std::nullopt;
    std::pair<std::string, Race> out;
    out.first = text(255);
    auto &race = out.second;
    race.name = text(64);
    race.start = point();
    race.finish = point();
    race.heading = number();
    race.has_heading = byte() != 0;
    const std::size_t count = byte();
    if (count > 32) return std::nullopt;
    for (std::size_t i = 0; i < count && ok; ++i) race.checkpoints.push_back(point());
    if (!ok || at != bytes.size() || race.name.empty()) return std::nullopt;
    return out;
}
// Another player's races on offer: the one the player joined, else the nearest within this
// reach of its start, stands here (its flags, its name; the Jam's text reads as a race's near
// its flag), unless the player is busy with a race of its own.
constexpr float offer_reach = 150.0f;
void follow_offers(State &s) {
    if (s.phase != Phase::idle || s.carried || s.carrier.created || s.building) {
        s.linked_dress = false;
        return;
    }
    const auto offers = multiplayer::throwdown_relay_race_offers();
    const multiplayer::RaceOffer *chosen = nullptr;
    std::optional<Race> race;
    float nearest = offer_reach;
    for (const auto &offer : offers) {
        auto decoded = decode_race(offer.race);
        if (!decoded || decoded->first != s.level) {
            if (!s.unreadable[{offer.leader, offer.id}])
                say(std::format("Death Race: {}'s race {} (map {}).", offer.leader_name, decoded ? "is on another map" : "could not be read",
                                decoded ? decoded->first : std::string("?")));
            s.unreadable[{offer.leader, offer.id}] = true;
            continue;
        }
        const bool had_joined = chosen && chosen->joined;
        const float d = across(s.position, decoded->second.start);
        if (offer.joined ? had_joined : (had_joined || d >= nearest)) continue;
        chosen = &offer;
        nearest = offer.joined ? 0.0f : d;
        race = std::move(decoded->second);
    }
    auto &list = races();
    int index = linked_index();
    if (index >= 0 && (!chosen || list[index].from != chosen->leader || list[index].from_id != chosen->id)) {
        remove_race(s, index);
        index = -1;
    }
    if (!chosen) {
        s.linked_dress = false;
        return;
    }
    if (index < 0) {
        race->from = chosen->leader;
        race->from_id = chosen->id;
        race->best = 0;
        race->name = std::format("{}'s {}", chosen->leader_name, race->name);
        list.push_back(std::move(*race));
        index = static_cast<int>(list.size()) - 1;
        s.selected = index;
        const std::pair key{chosen->leader, chosen->id};
        if (s.announced != key) {
            s.announced = key;
            tell(s, std::format("{} is open: join it at its flag to race.", list[index].name));
        }
    }
    s.linked_dress = chosen->joined || across(s.position, list[index].start) <= open_radius * 2;
}
// The race of the carrier's throwdown counts down: on foot through the game's intro and countdown
// (no rolling away down a slope), on the board on the start line just before its GO, which starts
// the clock and the riders.
void begin_carried(State &s, double now) {
    if (s.phase == Phase::countdown || s.phase == Phase::racing) stop(s, "restarted");
    s.phase = Phase::countdown;
    s.active = s.carrier.race;
    s.selected = s.carrier.race;
    s.carried = true;
    s.phase_started = now;
    s.armed_away = false;
    s.go_at = now + native_go;
    s.start_teleport_at = s.go_at - board_before_go;
    s.riders_go_at = s.go_at;
    s.linked = false;
    s.link.reset();
    set_racing(true);
    say(std::format("Death Race: {} counting down (its throwdown started).", races()[s.active].name));
}
// The relay's running race: the racers of the race in progress (the local player's own, or another
// player's, which starts here as its throwdown's copy does).
void follow_running(State &s, double now) {
    auto running = multiplayer::throwdown_relay_running_race();
    if (!running) {
        s.begun = {};
        return;
    }
    const std::pair key{running->leader, running->id};
    const bool leader = !running->racers.empty() && running->racers.front().local;
    if (s.carried && (leader || s.begun == key)) {
        s.link = std::move(running);
        s.linked = s.link->racers.size() >= 2;
        return;
    }
    if (leader || s.begun == key) return;
    s.begun = key;
    const int index = linked_index();
    if (index < 0 || races()[index].from != running->leader || races()[index].from_id != running->id) {
        say("Death Race: another player's race started here, but its flags never came: it is a plain Skate Jam here.");
        return;
    }
    s.carrier = {};
    s.carrier.race = index;
    s.carrier.since = now;
    s.carrier.created = true;
    s.carrier.configured = true;
    s.riding.reset(); // no AI riders in another player's race
    begin_carried(s, now);
    s.link = std::move(running);
    s.linked = true;
    say(std::format("Death Race: racing {} with {} player(s).", races()[index].name, s.link->racers.size()));
}
// With other players: the place among them, by their times as they come in.
void link_place(State &s) {
    if (!s.linked || !s.link || s.phase != Phase::finished) return;
    const auto mine = static_cast<std::int32_t>(std::lround(s.time * 1000.0));
    s.racers = static_cast<int>(s.link->racers.size());
    s.place = 1;
    for (const auto &racer : s.link->racers)
        if (!racer.local && racer.finish > 0 && racer.finish < mine) ++s.place;
}
// The AI riders a race takes: in a session with other players, room stays for them (one who joins
// never fills the queue with the riders, which would start it at once; see queue_players).
int riders_for(const State &s) {
    return multiplayer::throwdown_relay_linked() ? std::min(s.riders, most_racers - 3) : s.riders;
}
std::size_t flags_of(const Race &race) { return race.checkpoints.size() + 1; }
const Vec3 &flag_at(const Race &race, std::size_t next) { return next < race.checkpoints.size() ? race.checkpoints[next] : race.finish; }

float facing(const Vec3 &from, const Vec3 &to, float otherwise);
Vec3 grounded(const State &s, const Vec3 &at);

void advance(State &s, double now) {
    auto &list = races();
    switch (s.phase) {
    case Phase::idle: {
        // Races start with their throwdown (the tile opens them). A stop on the start line
        // counting one down alone is kept for the console's `race solo on`.
        if (!s.solo_on_stop) break;
        int on = -1;
        if (s.selected >= 0 && s.selected < static_cast<int>(list.size()) && at_flag(s.position, list[s.selected].start)) on = s.selected;
        if (on < 0) {
            if (now >= s.no_arm_until) s.armed_away = true;
            break;
        }
        if (!s.armed_away || s.building || s.speed > stopped_speed || s.carrier.race >= 0) break;
        s.phase = Phase::countdown;
        s.active = on;
        s.selected = on;
        s.phase_started = now;
        s.armed_away = false;
        set_racing(true);
        say(std::format("Death Race: {} counting down.", list[on].name));
        break;
    }
    case Phase::countdown: {
        if (s.active < 0 || s.active >= static_cast<int>(list.size())) return stop(s, "gone");
        // A throwdown's start puts the riders where its flag is, near the race's own.
        if (!s.carried && !at_flag(s.position, list[s.active].start)) {
            tell(s, "False start: wait for GO on the start flag.");
            s.phase = Phase::idle;
            s.active = -1;
            set_racing(false);
            break;
        }
        // Opened as a throwdown, the game counts down and calls everyone to the start; then the
        // skater is put on its board on the race's own start line, facing the way. The clock
        // starts when it sets off.
        if (s.carried && s.start_teleport_at && now >= s.start_teleport_at) {
            s.start_teleport_at = 0;
            const auto &race = list[s.active];
            const auto &first = race.checkpoints.empty() ? race.finish : race.checkpoints.front();
            // As the skater faced setting the start (older races: towards their first flag).
            const float heading = race.has_heading ? race.heading : facing(race.start, first, s.heading);
            try {
                if (const auto frame = multiplayer::capture_local(s.base, s.client, false); frame.ready)
                    keep_teleports_on_board(s.client, frame.entity);
            } catch (...) {}
            // With other players, each on its own lane, in the order every machine shares.
            Vec3 at = race.start;
            std::size_t lane{};
            if (s.linked && s.link)
                for (std::size_t i = 0; i < s.link->racers.size(); ++i)
                    if (s.link->racers[i].local) lane = i;
            if (lane) {
                const float r = heading * std::numbers::pi_v<float> / 180.0f, side = lane_offset(lane);
                at = grounded(s, {at[0] + std::cos(r) * side, at[1], at[2] - std::sin(r) * side});
            }
            const bool sent = teleport_local_skater_facing({at[0], at[1] + 0.3f, at[2]}, heading);
            say(std::format("Death Race: onto the start line{}, facing {:.0f} degrees{}.", lane ? std::format(" (lane {})", lane + 1) : "",
                            heading, sent ? "" : " (the teleport is unavailable)"));
        }
        const bool go = s.carried ? now >= s.go_at : now - s.phase_started >= countdown_length;
        if (go) {
            s.phase = Phase::racing;
            s.phase_started = now;
            s.next = 0;
            if (!s.riders_go_at || s.riders_go_at > now) s.riders_go_at = now;
            // The run, for the AI riders: a faster one becomes their line.
            line_start(now);
            s.recording = State::Recording::run;
            say(std::format("Death Race: {} started.", list[s.active].name));
        }
        break;
    }
    case Phase::racing: {
        if (s.active < 0 || s.active >= static_cast<int>(list.size())) return stop(s, "gone");
        auto &race = list[s.active];
        const double elapsed = now - s.phase_started;
        if (elapsed > longest_race) return stop(s, "too long");
        if (!at_flag(s.position, flag_at(race, s.next))) break;
        if (s.next + 1 < flags_of(race)) {
            ++s.next;
            break;
        }
        // The finish.
        s.time = elapsed;
        s.previous_best = race.best;
        s.record = race.best <= 0 || elapsed < race.best;
        if (s.record) {
            race.best = elapsed;
            save(s);
        }
        s.phase = Phase::finished;
        s.result_until = Clock::now() + result_shown;
        set_racing(false);
        if (s.carried) s.carrier.done = true;
        // With other players: they are told (the place is among them: link_place).
        if (s.linked) multiplayer::throwdown_relay_race_finished(static_cast<std::int32_t>(std::lround(elapsed * 1000.0)));
        // The place among the AI riders, by the times their line gives them.
        s.racers = 1;
        s.place = 1;
        if (s.riding && s.carrier.riders > 0 && !s.linked) {
            for (const auto t : rider_times(*s.riding, s.carrier.riders)) {
                ++s.racers;
                if (t < elapsed) ++s.place;
            }
        }
        link_place(s);
        if (s.recording == State::Recording::run) {
            s.recording = State::Recording::none;
            const auto key = s.level + "/" + race.name;
            if (auto run = line_stop(); run && (!s.lines[key] || line_seconds(*run) < line_seconds(*s.lines[key]))) {
                s.lines[key] = std::move(run);
                say(std::format("Death Race: {}'s AI riders now ride this run.", race.name));
            }
        }
        say(std::format("Death Race: {} finished in {}{}.", race.name, clock_text(elapsed),
                        s.record ? " (a record)" : std::format(" (best {})", clock_text(s.previous_best))));
        break;
    }
    case Phase::finished:
        // A throwdown's race ends with it (handle_event); alone, after its result.
        if (!s.carried && Clock::now() >= s.result_until) {
            s.phase = Phase::idle;
            s.active = -1;
        }
        break;
    }
}

void publish_view(State &s) {
    View v;
    v.phase = s.phase;
    v.hidden = s.base && multiplayer::sample_game_ui_state(s.base).in_menu;
    if (s.loaded && !s.level.empty()) v.races = races();
    v.active = s.active;
    v.shown = s.phase != Phase::idle && s.active >= 0 ? s.active : s.selected;
    v.props_live = s.props_live;
    v.carried = s.carried;
    v.place = s.place;
    v.racers = s.racers;
    v.building = s.building;
    v.draft = s.draft;
    const double now = seconds();
    if (s.phase == Phase::countdown) v.countdown = std::max(0.0, countdown_length - (now - s.phase_started));
    if (s.phase == Phase::racing) v.elapsed = now - s.phase_started;
    if (s.phase == Phase::finished) v.elapsed = s.time;
    if (s.active >= 0 && s.active < static_cast<int>(v.races.size())) {
        const auto &race = v.races[s.active];
        v.next = s.phase == Phase::racing ? s.next : 0;
        v.target = s.phase == Phase::racing ? flag_at(race, s.next) : race.start;
        v.distance = s.skater ? distance(s.position, v.target) : 0.0f;
    }
    v.time = s.time;
    v.previous_best = s.previous_best;
    v.record = s.record;
    v.notice = s.notice;
    v.notice_until = s.notice_until;
    v.result_until = s.result_until;
    v.skater = s.position;
    v.skater_known = s.skater;
    if (const auto camera = latest_game_view()) v.camera = *camera;
    v.published = Clock::now();
    publish(v);
}

std::string next_name(const std::vector<Race> &list) {
    for (int n = 1;; ++n) {
        const auto name = std::format("Race {}", n);
        if (std::none_of(list.begin(), list.end(), [&](const Race &r) { return r.name == name; })) return name;
    }
}
// The shown race, when the skater is within open_radius of its start; -1 otherwise.
int start_near(State &s) {
    const auto &list = races();
    if (s.selected < 0 || s.selected >= static_cast<int>(list.size())) return -1;
    const auto &start = list[s.selected].start;
    return across(s.position, start) <= open_radius && std::abs(s.position[1] - start[1]) <= open_radius ? s.selected : -1;
}

// ---- the throwdown's scores ---------------------------------------------------------------------
// Each second while a throwdown's race runs: everyone's score is where they finished (the game's
// own results rank by it), the skater's tricks counting for nothing.
void write_scores(State &s, double now, bool now_or_never) {
    if (!s.carried || (s.phase != Phase::racing && s.phase != Phase::finished) || (now < s.next_scores && !now_or_never)) return;
    s.next_scores = now + 1.0;
    std::vector<std::pair<double, std::uint32_t>> finished; // time, player
    std::vector<std::uint32_t> players;
    const auto me = multiplayer::local_native_player_id();
    if (s.linked && s.link) {
        // With other players: everyone's place, from the finishes every machine has (the same).
        for (const auto &racer : s.link->racers) {
            const auto id = racer.local ? me : racer.id;
            if (!id) continue;
            players.push_back(id);
            if (racer.local ? s.phase == Phase::finished : racer.finish > 0)
                finished.emplace_back(racer.local ? s.time : racer.finish / 1000.0, id);
        }
    } else {
        const int count = s.carrier.riders_in;
        if (me) {
            players.push_back(me);
            if (s.phase == Phase::finished) finished.emplace_back(s.time, me);
        }
        const auto times = rider_finishes(count);
        for (int i = 0; i < count; ++i) {
            const auto id = rider_base + 1 + static_cast<std::uint32_t>(i);
            players.push_back(id);
            if (i < static_cast<int>(times.size()) && times[static_cast<std::size_t>(i)] >= 0)
                finished.emplace_back(times[static_cast<std::size_t>(i)], id);
        }
    }
    std::sort(finished.begin(), finished.end());
    const auto racers = static_cast<std::int32_t>(players.size());
    for (const auto id : players) {
        std::int32_t points{};
        for (std::size_t rank = 0; rank < finished.size(); ++rank)
            if (finished[rank].second == id) points = (racers - static_cast<std::int32_t>(rank)) * points_a_place;
        // The skater's own each time (its tricks add to it); the riders' when it changes.
        const auto known = s.scores.find(id);
        if (id != me && known != s.scores.end() && known->second == points) continue;
        if (multiplayer::queue_throwdown_score(id, points)) s.scores[id] = points;
    }
}

// ---- a bail's respawn -------------------------------------------------------------------------
// While racing: the places the skater rides through on its board; a bail puts it back on its
// board a moment before it, facing the way it went, once it has lain there a little.
void respawn(State &s, double now, bool on_board, bool fell) {
    if (s.phase != Phase::racing) {
        s.rode.clear();
        s.respawn_at = 0;
        return;
    }
    if (on_board && !s.respawn_at) {
        s.rode.push_back({now, s.position, s.heading});
        const auto old = std::find_if(s.rode.begin(), s.rode.end(), [&](const State::Place &p) { return now - p.t <= respawn_kept; });
        s.rode.erase(s.rode.begin(), old);
    }
    if (fell && !s.respawn_at) {
        // The last place ridden a moment before the fall (the oldest kept, when it came sooner).
        const auto before = std::find_if(s.rode.rbegin(), s.rode.rend(), [&](const State::Place &p) { return now - p.t >= respawn_back; });
        if (before != s.rode.rend()) s.respawn_to = *before;
        else if (!s.rode.empty()) s.respawn_to = s.rode.front();
        else return;
        s.respawn_at = now + respawn_after;
        say(std::format("Death Race: a bail; back on the board in {:.1f} s.", respawn_after));
    }
    if (!s.respawn_at || now < s.respawn_at) return;
    s.respawn_at = 0;
    s.rode.clear();
    try {
        if (const auto frame = multiplayer::capture_local(s.base, s.client, false); frame.ready)
            keep_teleports_on_board(s.client, frame.entity);
    } catch (...) {}
    const auto &to = s.respawn_to;
    const bool sent = teleport_local_skater_facing({to.at[0], to.at[1] + 0.3f, to.at[2]}, to.heading);
    s.respawn_landing_until = now + respawn_landing;
    s.last_position.reset();
    say(std::format("Death Race: back on the board after the bail{}.", sent ? "" : " (the teleport is unavailable)"));
}

// ---- the props ---------------------------------------------------------------------------------
// Facing from `from` to `to` (degrees, the trainer's heading), or `otherwise` when they meet.
float facing(const Vec3 &from, const Vec3 &to, float otherwise) {
    const float dx = to[0] - from[0], dz = to[2] - from[2];
    if (std::hypot(dx, dz) < 0.5f) return otherwise;
    return std::atan2(dx, dz) * 180.0f / std::numbers::pi_v<float>;
}
// A flag's place on the ground: flags are set where the skater stood (on its board, above the
// ground), so the game's rays find the ground under it, once per place (before its prop stands
// there to be found instead): the middle of what a few rays round it strike, leaving out the
// skater's own body.
Vec3 grounded(const State &s, const Vec3 &at) {
    static std::map<std::array<int, 3>, float> found; // by place, to the centimetre
    const std::array<int, 3> key{static_cast<int>(std::lround(at[0] * 100.0f)), static_cast<int>(std::lround(at[1] * 100.0f)),
                                 static_cast<int>(std::lround(at[2] * 100.0f))};
    if (const auto known = found.find(key); known != found.end()) return {at[0], known->second, at[2]};
    std::vector<float> heights;
    bool asked{};
    for (const auto &[dx, dz] : {std::pair{0.0f, 0.0f}, {0.8f, 0.0f}, {-0.8f, 0.0f}, {0.0f, 0.8f}, {0.0f, -0.8f}}) {
        const auto ray = cast_world_ray({at[0] + dx, at[1] + 1.0f, at[2] + dz}, {at[0] + dx, at[1] - 3.0f, at[2] + dz});
        asked = asked || ray.cast;
        if (!ray.hit) continue;
        const bool body = s.skater && std::hypot(ray.at[0] - s.position[0], ray.at[2] - s.position[2]) < 0.6f &&
                          ray.at[1] > s.position[1] - 0.3f;
        if (!body) heights.push_back(ray.at[1]);
    }
    if (!asked) return at; // no rays yet: as set, and asked again
    float height = at[1];
    if (!heights.empty()) {
        std::sort(heights.begin(), heights.end());
        height = heights[heights.size() / 2];
    }
    found.emplace(key, height);
    if (!heights.empty() && std::abs(height - at[1]) > 0.02f)
        say(std::format("Death Race: a flag set {:.2f} m {} the ground stands on it.", std::abs(at[1] - height),
                        at[1] > height ? "above" : "below"));
    return {at[0], height, at[2]};
}
NetworkObject prop(std::uint64_t id, const char *item, const Vec3 &at, float heading) {
    NetworkObject o;
    o.id = id;
    o.item = item;
    o.position = at;
    const float half = heading * std::numbers::pi_v<float> / 360.0f;
    o.rotation = {0.0f, std::sin(half), 0.0f, std::cos(half)}; // a turn about +Y
    return o;
}
// What stands: the start line facing the first flag, a pair of slalom flags across each
// checkpoint, the finish arch facing the way in. Ids stay with their flag as a race grows.
std::vector<NetworkObject> props_for(const State &s) {
    std::vector<NetworkObject> out;
    std::vector<Vec3> points;
    bool finished{};
    float start_heading = s.draft_heading;
    if (s.building) {
        points = s.draft;
    } else {
        const int index = (s.phase != Phase::idle && s.active >= 0) ? s.active : s.selected;
        const auto it = s.maps.find(s.level);
        if (it == s.maps.end() || index < 0 || index >= static_cast<int>(it->second.size())) return out;
        const auto &race = it->second[index];
        points.push_back(race.start);
        points.insert(points.end(), race.checkpoints.begin(), race.checkpoints.end());
        points.push_back(race.finish);
        finished = true;
        start_heading = race.has_heading ? race.heading : facing(race.start, points[1], 0.0f);
    }
    if (points.empty()) return out;
    // The start line faces the way the skater faced setting it.
    out.push_back(prop(1, start_prop, grounded(s, points[0]), start_heading));
    const std::size_t gates = finished ? points.size() - 1 : points.size();
    for (std::size_t i = 1; i < gates; ++i) {
        const float h = facing(points[i - 1], points[i], 0.0f);
        const float r = h * std::numbers::pi_v<float> / 180.0f;
        // Across the way in: the right of a rider heading `h` is (cos h, -sin h) in x, z.
        const Vec3 side{std::cos(r) * gate_half_width, 0.0f, -std::sin(r) * gate_half_width};
        const auto &p = points[i];
        out.push_back(prop(10 + i * 2, checkpoint_prop, grounded(s, {p[0] + side[0], p[1], p[2] + side[2]}), h));
        out.push_back(prop(11 + i * 2, checkpoint_prop, grounded(s, {p[0] - side[0], p[1], p[2] - side[2]}), h));
    }
    if (finished)
        out.push_back(prop(2, finish_prop, grounded(s, points.back()), facing(points[points.size() - 2], points.back(), 0.0f)));
    return out;
}
void update_props(State &s, double now) {
    auto wanted = props_for(s);
    // Every second as well: a world change clears them, and the handover puts them back.
    if (wanted == s.props && now - s.props_at < 1.0) return;
    s.props_at = now;
    try {
        set_race_props(wanted);
        s.props_live = !wanted.empty() && race_props_live() > 0;
    } catch (...) {
        s.props_live = false;
    }
    s.props = std::move(wanted);
}
std::string without_error(const std::string &text) { return text.starts_with("error: ") ? text.substr(7) : text; }
std::string command_here(std::string_view verb, const std::vector<std::string> &arguments = {});

void handle_press(State &s, TileAction action, int race, double now) {
    say(std::format("Death Race: tile pressed ({}).", action == TileAction::create ? "new race" : action == TileAction::finish ? "finish"
                                                   : action == TileAction::open ? "open" : "busy"));
    switch (action) {
    case TileAction::create: {
        const auto result = command_here("new");
        tell(s, result.starts_with("error: ") ? without_error(result)
                                              : "Start flag set. Ride to the finish, then press SET FINISH HERE in Throwdowns.");
        break;
    }
    case TileAction::finish: {
        const auto result = command_here("finish-here");
        if (result.starts_with("error: ")) {
            tell(s, without_error(result));
            break;
        }
        const auto name = s.selected >= 0 && s.selected < static_cast<int>(races().size()) ? races()[s.selected].name : std::string("The race");
        command_here("go");
        // Back on the start: not straight into a race of your own.
        s.armed_away = false;
        s.no_arm_until = now + 3.0;
        tell(s, name + " saved. You are on its start: press DEATH RACE in Throwdowns to open it to rivals.");
        break;
    }
    case TileAction::open: {
        if (race < 0 || race >= static_cast<int>(races().size())) {
            tell(s, "Ride to a race's start flag first.");
            break;
        }
        if (races()[race].from) {
            tell(s, races()[race].name + ": join it at its flag to race.");
            break;
        }
        s.carrier = {};
        s.carrier.race = race;
        s.carrier.since = now;
        s.selected = race;
        tell(s, std::format("Opening {}: set it up, then start it from its flag. Rivals join there.", races()[race].name));
        break;
    }
    case TileAction::busy:
        if (s.carrier.created) tell(s, "Your race flag is up: start the race from it.");
        break;
    }
}
void put_back_queue_timer(State &s) {
    if (!s.queue_timer_before) return;
    say("Death Race: queue countdown back: " + change_named_setting(queue_timer_setting, *s.queue_timer_before, false));
    s.queue_timer_before.reset();
    multiplayer::throwdown_relay_queue_timer_changed();
}
void handle_event(State &s, Throwdown what, const std::string &series, std::uint32_t mmid, double now) {
    static const char *names[] = {"created", "configured", "cancelled", "started", "entered", "ended"};
    say(std::format("Death Race: throwdown {} (series {}, MMID {:#x}){}.", names[static_cast<int>(what)], series.empty() ? "-" : series,
                    mmid, s.carrier.race >= 0 ? std::format("; race {} opened", s.carrier.race + 1) : ""));
    const bool opened = s.carrier.race >= 0 && s.carrier.race < static_cast<int>(races().size());
    switch (what) {
    case Throwdown::created: {
        // The game's placement for the race the tile opened (it carries the race as a Skate Jam).
        if (series != "JamSession" || s.carrier.created) break;
        int race = -1;
        if (opened && now - s.carrier.since <= carrier_wait) race = s.carrier.race;
        else if (s.focus_race >= 0 && now - s.focus_at <= focus_wait) race = s.focus_race;
        if (race < 0 || race >= static_cast<int>(races().size())) break;
        s.carrier = {};
        s.carrier.race = race;
        s.carrier.since = now;
        s.carrier.created = true;
        s.selected = race;
        s.focus_race = -1;
        tell(s, std::format("Opening {}: set it up, then start it from its flag. Rivals join there.", races()[race].name));
        break;
    }
    case Throwdown::configured:
        if (!opened || !s.carrier.created) break;
        s.carrier.configured = true;
        s.carrier.mmid = mmid;
        s.carrier.riders = riders_for(s);
        if (s.carrier.riders < s.riders)
            tell(s, std::format("With other players around, room stays for them: {} AI rider(s).", s.carrier.riders));
        s.riding = s.lines[s.level + "/" + races()[s.carrier.race].name];
        if (s.riders > 0 && !s.riding) tell(s, "No AI line for this race yet: ride it once and its AI riders will follow you.");
        say(std::format("Death Race: {} opened with {} AI rider(s) (MMID {:#x}).", races()[s.carrier.race].name, s.riders, mmid));
        tell(s, "Race flag up: rivals join at it. Start the race from the flag.");
        break;
    case Throwdown::cancelled:
        if (!s.carrier.created) break;
        s.carrier = {};
        tell(s, "Race flag taken down.");
        break;
    case Throwdown::started:
        break;
    case Throwdown::entered:
        // The throwdown started: so does the race, for everyone at the flag.
        if (!opened || !s.carrier.created) break;
        begin_carried(s, now);
        break;
    case Throwdown::ended:
        if (!s.carrier.created) break;
        if (s.carried && (s.phase == Phase::countdown || s.phase == Phase::racing || s.phase == Phase::finished)) {
            // Over: run to the finish, or the time ran out. The game shows its results.
            const int done = s.active;
            s.dress_until = seconds() + dressed_after; // the race's text through the game's results
            if (s.phase == Phase::racing) tell(s, "Time's up!");
            if (s.phase != Phase::finished) stop(s, "the throwdown ended");
            s.phase = Phase::idle;
            s.active = -1;
            s.carried = false;
            s.riders_go_at = 0;
            s.scores.clear();
            s.ending = false;
            s.all_in_at = 0;
            s.linked = false;
            s.link.reset();
            s.carrier = {};
            forget(s, done);
            break;
        }
        s.carrier = {};
        s.carried = false;
        s.riders_go_at = 0;
        s.linked = false;
        s.link.reset();
        break;
    }
}
void drain(State &s, double now) {
    std::optional<std::pair<TileAction, int>> pressed;
    std::optional<int> focused;
    std::vector<std::tuple<Throwdown, std::string, std::uint32_t>> events;
    {
        auto &in = inbox();
        std::lock_guard lock(in.mutex);
        pressed.swap(in.pressed);
        focused.swap(in.focused);
        events.swap(in.events);
    }
    if (focused) {
        s.focus_race = *focused;
        s.focus_at = now;
    }
    for (const auto &[what, series, mmid] : events) handle_event(s, what, series, mmid, now);
    if (pressed) handle_press(s, pressed->first, pressed->second, now);
    // The tile was pressed but the game's throwdown never came: the host backed out.
    if (s.carrier.race >= 0 && !s.carrier.created && now - s.carrier.since > carrier_wait) s.carrier = {};
    // Other players joined the race's queue: its AI riders step aside (back if they all leave).
    auto &c = s.carrier;
    if (c.configured && !s.carried) {
        const bool joined = multiplayer::throwdown_relay_hosted_joiners() > 0;
        if (joined && (c.riders > 0 || c.riders_in > 0)) {
            for (int i = 0; i < c.riders_in; ++i) multiplayer::queue_throwdown_remove(rider_base + 1 + static_cast<std::uint32_t>(i));
            if (c.riders_in) tell(s, "A rival joined: the AI riders step aside.");
            say(std::format("Death Race: another player joined; {} AI rider(s) left the queue.", c.riders_in));
            c.riders = 0;
            c.riders_in = 0;
            c.riders_off = true;
        } else if (!joined && c.riders_off) {
            c.riders_off = false;
            c.riders = riders_for(s);
        }
    }
    // The race's flag is up: its AI riders join its queue, as players, so it can start.
    if (c.configured && c.mmid && c.riders_in < c.riders && multiplayer::prepare_throwdown_injection()) {
        const int before = c.riders_in;
        if (!before) {
            // Their names this race: drawn apart from each other.
            std::vector<const char *> pool(rider_names.begin(), rider_names.end());
            static std::mt19937 draw{std::random_device{}()};
            std::shuffle(pool.begin(), pool.end(), draw);
            for (std::size_t i = 0; i < s.names.size(); ++i) s.names[i] = pool[i];
            for (auto &record : rider_records) record.store(0, std::memory_order_release);
        }
        while (c.riders_in < c.riders && multiplayer::add_throwdown_rider(rider_base + 1 + c.riders_in, "JamSession", c.mmid)) ++c.riders_in;
        if (c.riders_in > before) say(std::format("Death Race: {} AI rider(s) joined the race's queue.", c.riders_in));
    }
}

std::optional<int> index_of(const std::vector<std::string> &arguments) {
    if (arguments.empty()) return std::nullopt;
    int value{};
    const auto &text = arguments[0];
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value < 1) return std::nullopt;
    return value - 1;
}
} // namespace

Status status() noexcept {
    auto &s = state();
    Status out;
    try {
        out.ready = s.loaded && !s.level.empty() && s.skater;
        if (s.loaded && !s.level.empty()) out.races = races();
        // Another player's race (last) is not the player's to manage.
        std::erase_if(out.races, [](const Race &r) { return r.from != 0; });
        out.selected = s.selected < static_cast<int>(out.races.size()) ? s.selected : -1;
        out.building = s.building;
        out.draft_points = s.draft.size();
        out.phase = s.phase;
    } catch (...) {}
    return out;
}

Tile tile() noexcept {
    Tile t;
    try {
        auto &s = state();
        t.title = "DEATH RACE";
        t.action = TileAction::busy;
        if (!s.loaded || s.level.empty() || !s.skater) {
            t.description = "Ride in the world to set or open a race.";
        } else if (s.phase == Phase::countdown || s.phase == Phase::racing) {
            t.description = "A race is on: ride to the finish!";
        } else if (s.carrier.created) {
            t.description = "Your race flag is up: rivals join at it. Start the race from the flag.";
        } else if (s.building) {
            t.action = TileAction::finish;
            t.title = "SET FINISH HERE";
            t.description = "Your new race ends where you stand. You are then put back on its start.";
        } else if (const int theirs = start_near(s); theirs >= 0 && races()[theirs].from) {
            t.description = races()[theirs].name + ": join it at its flag to race.";
        } else if (const int nearby = start_near(s); nearby >= 0) {
            const auto &race = races()[nearby];
            t.action = TileAction::open;
            t.race = nearby;
            t.navigation = "JamSession";
            t.description = std::format("Open {} here as a throwdown: rivals join at its flag, first to the finish wins. {}", race.name,
                                        race.best > 0 ? "Best " + clock_text(race.best) + "." : std::string("No time yet."));
        } else {
            t.action = TileAction::create;
            t.description = "First to the [font effect=AttentionOutline color=Attention]finish line[/font] wins. "
                            "Knock your rivals off their boards!";
        }
    } catch (...) {}
    return t;
}

int riders() noexcept { return state().riders; }

int queue_players() noexcept { return multiplayer::throwdown_relay_linked() ? most_racers : most_racers + 1; }

int throwdown_seconds() noexcept { return throwdown_timer; }

float zone_reach(const std::array<float, 3> &from) noexcept {
    try {
        auto &s = state();
        const int race = s.carrier.race >= 0 ? s.carrier.race : s.selected;
        const auto it = s.maps.find(s.level);
        if (it == s.maps.end() || race < 0 || race >= static_cast<int>(it->second.size())) return 0;
        const auto &r = it->second[race];
        float reach = across(from, r.start);
        for (const auto &c : r.checkpoints) reach = std::max(reach, across(from, c));
        reach = std::max(reach, across(from, r.finish));
        return reach + zone_margin;
    } catch (...) {
        return 0;
    }
}
void set_riders(int riders) noexcept { state().riders = std::clamp(riders, 0, 4); }

bool is_rider(std::uint32_t player_id) noexcept { return player_id > rider_base && player_id <= rider_base + most_riders; }

std::vector<std::uint8_t> hosted_race(std::uint32_t mmid) noexcept {
    try {
        const auto &s = state();
        const auto &c = s.carrier;
        if (!mmid || !c.configured || c.mmid != mmid || s.level.empty()) return {};
        const auto it = s.maps.find(s.level);
        if (it == s.maps.end() || c.race < 0 || c.race >= static_cast<int>(it->second.size())) return {};
        const auto &race = it->second[static_cast<std::size_t>(c.race)];
        if (race.from) return {};
        return encode_race(s.level, race);
    } catch (...) {
        return {};
    }
}

bool dressed() noexcept {
    const auto &s = state();
    return s.carrier.created || s.carried || s.linked_dress || (s.focus_race >= 0 && seconds() - s.focus_at <= focus_wait) ||
           seconds() < s.dress_until;
}

std::uint64_t rider_info(std::uint32_t player_id) noexcept {
    if (player_id <= rider_base || player_id > rider_base + rider_records.size()) return 0;
    return rider_records[player_id - rider_base - 1].load(std::memory_order_acquire);
}

void throwdown_modes_opened() noexcept {
    // The game's results are gone: the Skate Jam's text is its own again (its tile among them).
    state().dress_until = 0;
}

bool race_on() noexcept {
    const auto &s = state();
    return s.carried && (s.phase == Phase::countdown || s.phase == Phase::racing || s.phase == Phase::finished);
}

bool setting_up() noexcept {
    const auto &s = state();
    return s.carrier.created && !s.carrier.configured;
}

void tile_pressed(TileAction action, int race) noexcept {
    try {
        auto &in = inbox();
        std::lock_guard lock(in.mutex);
        in.pressed.emplace(action, race);
    } catch (...) {}
}

void tile_focused(int race) noexcept {
    try {
        auto &in = inbox();
        std::lock_guard lock(in.mutex);
        in.focused = race;
    } catch (...) {}
}

void throwdown_event(Throwdown what, std::string_view series, std::uint32_t mmid) noexcept {
    try {
        auto &in = inbox();
        std::lock_guard lock(in.mutex);
        if (in.events.size() < 64) in.events.emplace_back(what, std::string(series), mmid);
    } catch (...) {}
}

void tick(std::uintptr_t base, std::uintptr_t client, bool playing, const std::string &level) noexcept {
    try {
        auto &s = state();
        load(s);
        s.base = base;
        s.client = client;
        if (level != s.level) {
            stop(s, "the map changed");
            s.level = level;
            s.selected = -1;
            s.building = false;
            s.draft.clear();
            s.last_position.reset();
            s.carrier = {};
        }
        // The d-pad's way back to the session marker (a throwdown's start) is no way to the finish:
        // refused from the start line until the race is over.
        const bool on_the_line = s.phase == Phase::racing || s.phase == Phase::finished ||
                                 (s.phase == Phase::countdown && s.carried && !s.start_teleport_at);
        block_session_marker_teleports(on_the_line);
        if (const auto refused = refused_session_marker_teleports(); refused != s.markers_refused) {
            tell(s, "No going back to the start during the race!");
            s.markers_refused = refused;
        }
        const auto t = trainer::telemetry();
        s.skater = playing && t.skater;
        if (!s.skater) {
            stop(s, "no skater");
            s.last_position.reset();
            show_riders(base, client, nullptr, 0, 0, seconds());
            publish_view(s);
            return;
        }
        s.position = t.position;
        s.speed = t.speed;
        s.heading = t.heading;
        s.physics_state = t.physics_state;
        // A teleport (a marker, a respawn) ends a race: the clock would not mean anything.
        if (s.last_position && distance(*s.last_position, s.position) > teleport_distance && s.phase == Phase::racing &&
            seconds() >= s.respawn_landing_until)
            stop(s, "teleported");
        s.last_position = s.position;
        if (!s.level.empty()) {
            const double now = seconds();
            drain(s, now);
            follow_offers(s);
            follow_running(s, now);
            link_place(s);
            advance(s, now);
            update_props(s, now);
            const bool on_board = s.physics_state && s.physics_state != offboard_state && s.physics_state != wipeout_state;
            // The skater's falls: the riders fall the same way when run into. From a fall until it
            // rolls on its board on the ground again for a moment, it is getting over it.
            bool fell{};
            if (const auto watch = watched_physics_state(); watch.valid) {
                fell = s.falls_known && watch.wipeouts != s.falls_seen;
                s.falls_seen = watch.wipeouts;
                s.falls_known = true;
            }
            const bool rolling = s.physics_state == ground_state && !t.airborne && s.speed > 3.0f;
            if (fell) {
                s.recovering = true;
                s.rolling_since = 0;
            } else if (s.recovering) {
                if (!rolling) s.rolling_since = 0;
                else if (!s.rolling_since) s.rolling_since = now;
                else if (now - s.rolling_since >= recovered_after) s.recovering = false;
            }
            Moment moment;
            moment.board = on_board && !s.recovering;
            moment.plain = s.physics_state == ground_state && !t.airborne && !s.recovering;
            moment.air = t.airborne;
            moment.fallen = s.recovering;
            moment.state = s.physics_state;
            if (s.recording != State::Recording::none) line_capture(base, client, now, moment);
            // A race's queue counts down before the start (the AI riders fill it at once): the queue
            // takes the setting when the game makes it, so it is set from the cursor on the tile on.
            // Not with other players in the session: the relay's own timer lets the leader start.
            const bool own_queue = dressed() && !s.linked_dress && !multiplayer::throwdown_relay_linked();
            if (own_queue && !s.queue_timer_before) {
                s.queue_timer_before = observe_named_setting(queue_timer_setting).value_or("1");
                say("Death Race: queue countdown " + change_named_setting(queue_timer_setting, race_queue_seconds, false));
            } else if (!own_queue && s.queue_timer_before) {
                put_back_queue_timer(s);
            }
            fall_capture(base, client, now, fell);
            respawn(s, now, on_board && !s.recovering, fell);
            write_scores(s, now);
            end_when_everyone_is_in(s);
            // The riders' player records (their names on the leaderboards), made again now and then
            // (a new UI model manager drops them).
            if (s.carrier.configured && now >= s.next_records) {
                s.next_records = now + 2.0;
                for (int i = 0; i < s.carrier.riders_in && i < static_cast<int>(rider_records.size()); ++i) {
                    const auto n = static_cast<std::size_t>(i);
                    std::uint64_t record{};
                    try {
                        record = rider_record(base, rider_base + 1 + static_cast<std::uint32_t>(i), s.names[n], s.records[n]);
                    } catch (...) {}
                    if (record && record != rider_records[n].exchange(record))
                        say(std::format("Death Race: AI rider {} is {}.", i + 1, s.names[n]));
                }
            }
            // The AI riders: waiting on the start from the race's flag going up, on their way from
            // the go, and through the result.
            const bool out = s.carrier.configured && s.riding && (s.phase != Phase::idle || (!s.carried && !s.carrier.done));
            // From the go (before it, how long until it; with none due, long before).
            const double clock = s.riders_go_at ? now - s.riders_go_at : -1e6;
            const Vec3 *line_up = nullptr;
            if (s.carrier.race >= 0 && s.carrier.race < static_cast<int>(races().size())) line_up = &races()[s.carrier.race].start;
            show_riders(base, client, out ? s.riding.get() : nullptr, out ? s.carrier.riders : 0, s.carried ? clock : -1e6, now, line_up);
        }
        publish_view(s);
    } catch (...) {
        // Nothing here may stop the client tick.
    }
}

namespace {
std::string command_here(std::string_view verb, const std::vector<std::string> &arguments) { return command(verb, arguments); }
} // namespace

std::string command(std::string_view verb, const std::vector<std::string> &arguments) {
    auto &s = state();
    load(s);
    auto &list = races();
    const auto need_skater = [&]() -> std::string {
        if (s.level.empty() || !s.skater) return "error: no skater in play.";
        return {};
    };
    if (verb == "status" || verb == "list") {
        std::string text = std::format("Death Race: {} race(s) on this map{}.", list.size(),
                                       s.building ? std::format("; building one ({} point(s) set)", s.draft.size()) : "");
        for (std::size_t i = 0; i < list.size(); ++i)
            text += std::format("\n  {}. {} - {} checkpoint(s), best {}{}", i + 1, list[i].name, list[i].checkpoints.size(),
                                list[i].best > 0 ? clock_text(list[i].best) : std::string("none"), list[i].from ? " (another player's)" : "");
        return text;
    }
    if (verb == "new" || verb == "start-here") {
        if (auto e = need_skater(); !e.empty()) return e;
        if (s.phase == Phase::countdown || s.phase == Phase::racing) return "error: finish or abort the race first.";
        s.building = true;
        s.draft = {s.position};
        s.draft_heading = s.heading;
        // The ride to the finish is the race's first AI line.
        line_start(seconds());
        s.recording = State::Recording::route;
        return "Start flag set. Add checkpoints if you like, then ride to the finish and set it.";
    }
    if (verb == "checkpoint") {
        if (auto e = need_skater(); !e.empty()) return e;
        if (!s.building || s.draft.empty()) return "error: set a start first.";
        if (s.draft.size() > 32) return "error: 32 checkpoints at most.";
        if (across(s.position, s.draft.back()) < flag_radius * 2) return "error: too close to the last flag.";
        s.draft.push_back(s.position);
        return std::format("Checkpoint {} set.", s.draft.size() - 1);
    }
    if (verb == "finish-here") {
        if (auto e = need_skater(); !e.empty()) return e;
        if (!s.building || s.draft.empty()) return "error: set a start first.";
        if (across(s.position, s.draft.back()) < flag_radius * 2) return "error: too close to the last flag.";
        Race race;
        race.name = next_name(list);
        race.start = s.draft.front();
        race.checkpoints.assign(s.draft.begin() + 1, s.draft.end());
        race.finish = s.position;
        race.heading = s.draft_heading;
        race.has_heading = true;
        s.selected = add_race(s, std::move(race));
        s.building = false;
        s.draft.clear();
        s.armed_away = false; // not straight away, if the finish is near a start
        if (s.recording == State::Recording::route) {
            s.recording = State::Recording::none;
            if (auto line = line_stop()) s.lines[s.level + "/" + list[s.selected].name] = std::move(line);
        }
        save(s);
        return std::format("{} saved. Stop on its start flag to race it.", list[s.selected].name);
    }
    if (verb == "cancel") {
        s.building = false;
        s.draft.clear();
        if (s.recording == State::Recording::route) {
            line_stop();
            s.recording = State::Recording::none;
        }
        return "New race cancelled.";
    }
    if (verb == "solo") {
        const auto choice = arguments.empty() ? std::string("status") : arguments[0];
        if (choice == "on" || choice == "off") s.solo_on_stop = choice == "on";
        return std::string("Racing alone by stopping on the start line: ") + (s.solo_on_stop ? "on." : "off.");
    }
    if (verb == "hide") {
        if (s.phase == Phase::countdown || s.phase == Phase::racing) return "error: finish or abort the race first.";
        s.selected = -1;
        return "Race props put away.";
    }
    if (verb == "select") {
        const auto i = index_of(arguments);
        if (!i || *i >= static_cast<int>(list.size())) return "error: no such race.";
        s.selected = *i;
        return std::format("{} selected.", list[*i].name);
    }
    if (verb == "go") {
        if (auto e = need_skater(); !e.empty()) return e;
        const auto i = arguments.empty() ? std::optional<int>(s.selected) : index_of(arguments);
        if (!i || *i < 0 || *i >= static_cast<int>(list.size())) return "error: no such race.";
        if (s.phase == Phase::countdown || s.phase == Phase::racing) stop(s, "restarted");
        s.selected = *i;
        s.armed_away = true;
        const auto &start = list[*i].start;
        // The trainer's teleport keeps a session's teleport rules.
        return trainer::command("tp", {std::format("{:.2f}", start[0]), std::format("{:.2f}", start[1] + 0.5f), std::format("{:.2f}", start[2])});
    }
    if (verb == "delete") {
        const auto i = arguments.empty() ? std::optional<int>(s.selected) : index_of(arguments);
        if (!i || *i < 0 || *i >= static_cast<int>(list.size())) return "error: no such race.";
        if (list[*i].from) return "error: that race is another player's.";
        if (s.active == *i) stop(s, "deleted");
        const auto name = list[*i].name;
        list.erase(list.begin() + *i);
        s.selected = -1;
        if (s.active > *i) --s.active;
        save(s);
        return name + " deleted.";
    }
    if (verb == "abort") {
        if (s.phase != Phase::countdown && s.phase != Phase::racing) return "No race in progress.";
        stop(s, "aborted");
        return "Race aborted.";
    }
    return "error: unknown command.";
}
} // namespace dingosdk::death_race
