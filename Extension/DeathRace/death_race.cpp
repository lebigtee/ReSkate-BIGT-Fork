#include "death_race_internal.h"
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

// Game thread only. See death_race.h; the files sharing the state are listed in death_race_internal.h.
namespace dingosdk::death_race::detail {
State &state() {
    static auto *value = new State;
    return *value;
}
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
void load(State &s) {
    if (s.loaded) return;
    s.loaded = true;
    try {
        std::ifstream file(races_path(), std::ios::binary);
        if (!file) return;
        const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (text.empty() || text.size() > 4 * 1024 * 1024) return;
        s.maps = races_from_json(text);
        std::size_t count{};
        for (const auto &[level, list] : s.maps) count += list.size();
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
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        auto temporary = path;
        temporary += L".tmp";
        {
            std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
            file << races_to_json(s.maps);
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

std::size_t flags_of(const Race &race) { return race.checkpoints.size() + 1; }
const Vec3 &flag_at(const Race &race, std::size_t next) { return next < race.checkpoints.size() ? race.checkpoints[next] : race.finish; }

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

namespace {
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
} // namespace
} // namespace dingosdk::death_race::detail

namespace dingosdk::death_race {
using namespace detail;

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

} // namespace dingosdk::death_race
