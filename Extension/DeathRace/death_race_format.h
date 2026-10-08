#pragma once
// The Death Race's races as data, with nothing of the game (death_race_format.cpp; Test/
// death_race_tests.cpp checks it): the races file, a race as it is offered to the other players,
// and where each racer stands on the start line.
#include "death_race.h"
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dingosdk::death_race {
// Every map's races, by level name.
using RaceMaps = std::map<std::string, std::vector<Race>>;
// races.json's text: each map's races (at most 100 a map, 32 checkpoints a race); a race without
// a readable start and finish is left out. Throws when the text is not a races file.
RaceMaps races_from_json(std::string_view text);
// races.json's text for these maps. Another player's races (Race::from) are not the player's to
// keep: they are left out.
std::string races_to_json(const RaceMaps &maps);

// A race offered to the other players (the relay carries it in its drop): its map, its name and
// its flags. Decoding fails on anything that was not encoded so.
std::vector<std::uint8_t> encode_race(const std::string &level, const Race &race);
std::optional<std::pair<std::string, Race>> decode_race(const std::vector<std::uint8_t> &bytes);

// A racer's place across the start line, in metres to the right: the first (the host) in the
// middle, the others either side in turn, lane_spacing apart.
float lane_offset(std::size_t lane);
} // namespace dingosdk::death_race
