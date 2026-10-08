#pragma once
// The overlay's side of the Hall of Meat: skate 3's X-ray over the skater during a bail (a
// skeleton picture cut into bones, laid over the skater's own), and the bones it broke once the
// bail is over.
#include <cstddef>
#include <cstdint>
#include <imgui.h>

namespace dingosdk::overlay {
// Render-thread setup, before the ImGui atlas is built: reserves room in it for the bone sheet
// built into ReSkate.dll. Returns 1 when it is there.
std::size_t reserve_hall_of_meat_bones(ImFontAtlas &) noexcept;
// After the atlas is built: copies the bone sheet into it.
void fill_hall_of_meat_bones(ImFontAtlas &) noexcept;
// True while there is something to draw, so the overlay renders with the menu closed.
bool hall_of_meat_hud_pending();
void draw_hall_of_meat_hud();
// skate 3's jerky bail: for its first moments the picture only moves now and then. What the
// overlay does with this frame's picture before drawing on it, as draw_hall_of_meat_hud decided:
// keep a copy of it, or put the kept copy back in its place.
struct FrameHold {
    enum class Action : std::uint8_t { none, keep, show };
    Action action{};
    std::uint64_t picture{}; // the copy to keep, or to show (only if that one was kept)
    // In a multiplayer session, where the game's speed must not change, the slow motion is a
    // replay (hall_of_meat_replay.h): keep this frame's picture in a slot, and/or draw a kept one
    // over it. `ready`: a bail would be replayed, so have the pipeline ready before one comes.
    struct Replay {
        bool ready{};
        int keep_slot = -1, show_slot = -1;
        std::uint64_t keep_id{}, show_id{};
        float opacity{};
    } replay;
};
FrameHold hall_of_meat_frame_hold();
} // namespace dingosdk::overlay
