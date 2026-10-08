// The Death Race's races as data (death_race_format.h), without the game: the races file, a race
// as the other players receive it, and the start line's lanes.
#include "Extension/DeathRace/death_race_format.h"
#include <cmath>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>

namespace {
using namespace dingosdk::death_race;

void check(bool value, const char *why) {
    if (!value) throw std::runtime_error(why);
}
bool same(const Race &a, const Race &b) {
    return a.name == b.name && a.start == b.start && a.finish == b.finish && a.checkpoints == b.checkpoints && a.best == b.best &&
           a.has_heading == b.has_heading && (!a.has_heading || a.heading == b.heading);
}
Race race(std::string name, float x) {
    Race r;
    r.name = std::move(name);
    r.start = {x, 10.0f, -4.5f};
    r.finish = {x + 120.0f, -3.25f, 80.0f};
    r.checkpoints = {{x + 30.0f, 5.0f, 10.0f}, {x + 60.0f, 1.5f, 40.0f}};
    r.best = 83.25;
    r.heading = 271.5f;
    r.has_heading = true;
    return r;
}

void races_file() {
    RaceMaps maps;
    maps["Levels/BAM"] = {race("Hill bomb", 100.0f), race("Plaza", -50.0f)};
    maps["Levels/BAM"][1].has_heading = false;
    maps["Levels/BAM"][1].best = 0;
    auto theirs = race("Their race", 0.0f);
    theirs.from = 76561198000000011ULL;
    theirs.from_id = 3;
    maps["Levels/BAM"].push_back(theirs);
    maps["Levels/Grom"] = {race("Pier", 5.0f)};
    maps["Levels/Empty"] = {};

    const auto back = races_from_json(races_to_json(maps));
    check(back.size() == 2, "races file: a map with no race is not written");
    check(back.at("Levels/BAM").size() == 2, "races file: another player's race is not the player's to keep");
    check(same(back.at("Levels/BAM")[0], maps["Levels/BAM"][0]), "races file: a race comes back as it was");
    check(same(back.at("Levels/BAM")[1], maps["Levels/BAM"][1]), "races file: a race without a heading or a best comes back so");
    check(same(back.at("Levels/Grom")[0], maps["Levels/Grom"][0]), "races file: every map's races come back");
}

void damaged_races() {
    const auto maps = races_from_json(R"({"schema": 1, "maps": {
        "A": [
            {"name": "", "start": [1, 2, 3], "finish": [4, 5, 6], "best": -5},
            {"name": "no finish", "start": [1, 2, 3]},
            {"name": "bad start", "start": [1, "2", 3], "finish": [4, 5, 6]},
            {"name": "short start", "start": [1, 2], "finish": [4, 5, 6]},
            {"name": "bad checkpoint", "start": [0, 0, 0], "finish": [1, 1, 1], "checkpoints": [[1, 2], [7, 8, 9]], "heading": "north"},
            "not a race"
        ],
        "B": "not a list"
    }})");
    check(maps.size() == 1 && maps.at("A").size() == 2, "damaged races: unreadable races and maps are left out");
    const auto &unnamed = maps.at("A")[0];
    check(unnamed.name == "Race" && unnamed.best == 0.0, "damaged races: a race without a name is called Race, a negative best is none");
    const auto &partial = maps.at("A")[1];
    check(partial.checkpoints.size() == 1 && partial.checkpoints[0] == Vec3{7, 8, 9}, "damaged races: unreadable checkpoints are left out");
    check(!partial.has_heading, "damaged races: a heading that is no number is none");

    std::string many = R"({"maps": {"A": [)";
    for (int i = 0; i < 120; ++i) many += std::string(i ? "," : "") + R"({"start": [0, 0, 0], "finish": [1, 1, 1], "checkpoints": [)" +
                                          [] {
                                              std::string c;
                                              for (int k = 0; k < 40; ++k) c += std::string(k ? "," : "") + "[1, 2, 3]";
                                              return c;
                                          }() +
                                          "]}";
    many += "]}}";
    const auto capped = races_from_json(many);
    check(capped.at("A").size() == 100, "damaged races: at most 100 races a map");
    check(capped.at("A")[0].checkpoints.size() == 32, "damaged races: at most 32 checkpoints a race");

    for (const char *text : {"", "[]", R"({"maps": []})", R"({"races": {}})", "{not json"}) {
        bool threw = false;
        try {
            (void)races_from_json(text);
        } catch (...) {
            threw = true;
        }
        check(threw, "damaged races: what is not a races file is refused");
    }
}

void offered_race() {
    auto r = race("Hill bomb", 100.0f);
    r.from = 76561198000000011ULL;
    const auto bytes = encode_race("Levels/BAM", r);
    const auto back = decode_race(bytes);
    auto sent = r;
    sent.best = 0; // the player's own best stays with them
    check(back && back->first == "Levels/BAM" && same(back->second, sent), "offered race: a race comes back as it was sent");
    check(!back->second.from && !back->second.best, "offered race: neither its owner nor its best travel with it");

    auto crowded = r;
    for (int i = 0; i < 40; ++i) crowded.checkpoints.push_back({static_cast<float>(i), 0.0f, 0.0f});
    const auto trimmed = decode_race(encode_race("L", crowded));
    check(trimmed && trimmed->second.checkpoints.size() == 32, "offered race: at most 32 checkpoints are sent");

    check(!decode_race({}), "offered race: nothing is no race");
    auto wrong = bytes;
    wrong[0] = 2;
    check(!decode_race(wrong), "offered race: another format is refused");
    for (std::size_t n : {std::size_t{1}, bytes.size() / 2, bytes.size() - 1})
        check(!decode_race(std::vector<std::uint8_t>(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(n))), "offered race: a cut race is refused");
    auto longer = bytes;
    longer.push_back(0);
    check(!decode_race(longer), "offered race: bytes after the race are refused");
    auto unnamed = r;
    unnamed.name.clear();
    check(!decode_race(encode_race("L", unnamed)), "offered race: a race without a name is refused");
    auto far = r;
    far.start[0] = 2e6f;
    check(!decode_race(encode_race("L", far)), "offered race: a flag out of any map is refused");
    auto broken = r;
    broken.finish[1] = std::numeric_limits<float>::quiet_NaN();
    check(!decode_race(encode_race("L", broken)), "offered race: a flag that is no number is refused");
}

void start_lanes() {
    check(lane_offset(0) == 0.0f, "start lanes: the host stands in the middle");
    check(lane_offset(1) > 0.0f && lane_offset(2) < 0.0f, "start lanes: the others stand either side in turn");
    check(std::abs(lane_offset(1) - lane_spacing) < 1e-6f && std::abs(lane_offset(2) + lane_spacing) < 1e-6f, "start lanes: lane_spacing apart");
    std::set<float> lanes;
    for (std::size_t i = 0; i < static_cast<std::size_t>(most_racers); ++i) lanes.insert(lane_offset(i));
    check(lanes.size() == static_cast<std::size_t>(most_racers), "start lanes: every racer has a lane of their own");
    check(std::abs(lane_offset(3)) == 2.0f * lane_spacing && std::abs(lane_offset(4)) == 2.0f * lane_spacing, "start lanes: the line fills from the middle out");
}
} // namespace

int main() {
    try {
        races_file();
        damaged_races();
        offered_race();
        start_lanes();
        std::cout << "Death Race: races file, damaged races, offered race and start lanes passed.\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
