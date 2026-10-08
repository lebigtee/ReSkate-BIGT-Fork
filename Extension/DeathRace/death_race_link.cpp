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

namespace dingosdk::death_race::detail {
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
} // namespace dingosdk::death_race::detail
