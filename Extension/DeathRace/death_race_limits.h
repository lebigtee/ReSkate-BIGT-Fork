#pragma once
// How many race together and how they line up (death_race.h, death_race_riders.cpp).
namespace dingosdk::death_race {
// Most racers a race takes, its host included (AI riders or other players), and how far apart
// they line up on its start line (metres, side by side: the host in the middle).
inline constexpr int most_racers = 5;
inline constexpr float lane_spacing = 1.3f;
} // namespace dingosdk::death_race
