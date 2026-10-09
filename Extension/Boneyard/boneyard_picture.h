#pragma once
// What the Boneyard does with the game's picture before the overlay draws on it
// (boneyard_picture.cpp): skate 3's jerky bail, a kept picture shown again over the next few,
// and in a multiplayer session its slow motion replay (boneyard_replay.h). Both as
// draw_boneyard_hud planned (boneyard_frame_hold). Render thread only.
#include <d3d12.h>

namespace dingosdk::overlay {
// This frame: the overlay's device and its open command list, the picture (state PRESENT, left
// so) and its render target view, and whether the GPU has finished all the overlay submitted (so
// what the Boneyard holds may be let go).
struct BoneyardFrame {
    ID3D12Device *device{};
    ID3D12GraphicsCommandList *commands{};
    ID3D12Resource *buffer{};
    D3D12_CPU_DESCRIPTOR_HANDLE target{};
    bool gpu_idle{};
};
// Before the overlay draws on the picture.
void boneyard_picture(const BoneyardFrame &frame);
// When the overlay lets its graphics go (a resize, a lost device): these go too.
void release_boneyard_pictures();
} // namespace dingosdk::overlay
