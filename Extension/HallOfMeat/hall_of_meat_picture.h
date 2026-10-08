#pragma once
// What the Hall of Meat does with the game's picture before the overlay draws on it
// (hall_of_meat_picture.cpp): skate 3's jerky bail, a kept picture shown again over the next few,
// and in a multiplayer session its slow motion replay (hall_of_meat_replay.h). Both as
// draw_hall_of_meat_hud planned (hall_of_meat_frame_hold). Render thread only.
#include <d3d12.h>

namespace dingosdk::overlay {
// This frame: the overlay's device and its open command list, the picture (state PRESENT, left
// so) and its render target view, and whether the GPU has finished all the overlay submitted (so
// what the Hall of Meat holds may be let go).
struct HallOfMeatFrame {
    ID3D12Device *device{};
    ID3D12GraphicsCommandList *commands{};
    ID3D12Resource *buffer{};
    D3D12_CPU_DESCRIPTOR_HANDLE target{};
    bool gpu_idle{};
};
// Before the overlay draws on the picture.
void hall_of_meat_picture(const HallOfMeatFrame &frame);
// When the overlay lets its graphics go (a resize, a lost device): these go too.
void release_hall_of_meat_pictures();
} // namespace dingosdk::overlay
