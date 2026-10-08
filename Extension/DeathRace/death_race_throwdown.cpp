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


// The race as the game's throwdown carries it (a Skate Jam): the tile's press, the throwdown's
// events, the AI riders in its queue, everyone's score, and its end once everyone is in.
namespace dingosdk::death_race::detail {
namespace {
std::string without_error(const std::string &text) { return text.starts_with("error: ") ? text.substr(7) : text; }
void handle_press(State &s, TileAction action, int race, double now) {
    say(std::format("Death Race: tile pressed ({}).", action == TileAction::create ? "new race" : action == TileAction::finish ? "finish"
                                                   : action == TileAction::open ? "open" : "busy"));
    switch (action) {
    case TileAction::create: {
        const auto result = command("new", {});
        tell(s, result.starts_with("error: ") ? without_error(result)
                                              : "Start flag set. Ride to the finish, then press SET FINISH HERE in Throwdowns.");
        break;
    }
    case TileAction::finish: {
        const auto result = command("finish-here", {});
        if (result.starts_with("error: ")) {
            tell(s, without_error(result));
            break;
        }
        const auto name = s.selected >= 0 && s.selected < static_cast<int>(races().size()) ? races()[s.selected].name : std::string("The race");
        command("go", {});
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
void handle_event(State &s, Throwdown what, const std::string &series, std::uint32_t mmid, double now);
} // namespace

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
void put_back_queue_timer(State &s) {
    if (!s.queue_timer_before) return;
    say("Death Race: queue countdown back: " + change_named_setting(queue_timer_setting, *s.queue_timer_before, false));
    s.queue_timer_before.reset();
    multiplayer::throwdown_relay_queue_timer_changed();
}
namespace {
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
} // namespace

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
    // The race's flag is up: its AI riders join its queue, as players, so it can start. Until they
    // can (the game's event types are found anew in each level), the player is told after a while.
    if (c.configured && c.mmid && c.riders_in < c.riders) {
        if (!c.joining_since) c.joining_since = now;
        if (!c.joining_told && now - c.joining_since >= riders_slow_to_join) {
            c.joining_told = true;
            say(std::format("Death Race: the AI riders have not joined the race's queue after {:.0f} s ({}/{} in): {}.", now - c.joining_since,
                            c.riders_in, c.riders, multiplayer::throwdown_injection_state()));
            tell(s, "The AI riders are on their way to the start: a moment...");
        }
    }
    if (c.configured && c.mmid && c.riders_in < c.riders && multiplayer::prepare_throwdown_injection()) {
        const int before = c.riders_in;
        if (!before) {
            // The AI riders' names this race: drawn apart from each other.
            std::vector<const char *> pool(rider_names.begin(), rider_names.end());
            static std::mt19937 draw{std::random_device{}()};
            std::shuffle(pool.begin(), pool.end(), draw);
            for (std::size_t i = 0; i < s.names.size(); ++i) s.names[i] = pool[i];
            for (auto &record : rider_records) record.store(0, std::memory_order_release);
        }
        while (c.riders_in < c.riders && multiplayer::add_throwdown_rider(rider_base + 1 + c.riders_in, "JamSession", c.mmid)) ++c.riders_in;
        if (c.riders_in > before)
            say(std::format("Death Race: {} AI rider(s) joined the race's queue{}.", c.riders_in,
                            c.joining_told ? std::format(" after {:.0f} s", now - c.joining_since) : ""));
        else if (!c.joining_told && now - c.joining_since >= riders_slow_to_join)
            say("Death Race: the game turned the AI riders away from the race's queue.");
    }
}

} // namespace dingosdk::death_race::detail
