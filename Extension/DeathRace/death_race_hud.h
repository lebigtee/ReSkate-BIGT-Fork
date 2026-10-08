#pragma once
// The overlay's side of the Death Race: flags in the world, countdown, clock and result.
namespace dingosdk::overlay {
// True while there is something to draw, so the overlay renders with the menu closed.
bool death_race_hud_pending();
void draw_death_race_hud();
} // namespace dingosdk::overlay
