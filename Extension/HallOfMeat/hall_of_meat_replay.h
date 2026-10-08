#pragma once
// skate 3's slow motion where the game's own speed must not change (a multiplayer session): the
// bail's first moments are kept as small pictures on the GPU, then drawn back over the game's
// picture at half speed and faded out (hall_of_meat_hud.cpp decides which and when). Recorded on
// the overlay's command list before it draws; render thread only. Needs nothing but D3D12, so
// Test/replay_test.cpp runs it on WARP.
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <cstdint>
#include <string>

namespace dingosdk::overlay {
class PictureReplay {
public:
    static constexpr std::size_t slots = 24;
    static constexpr UINT most_height = 720; // kept pictures are at most this tall

    // Creates what it needs for pictures of this size and format (again if they change). The
    // pictures themselves are made as they are first kept. False, with `error`, when it cannot.
    bool prepare(ID3D12Device *device, const D3D12_RESOURCE_DESC &buffer, std::string &error);
    // Keeps the picture in `buffer` (state PRESENT, left so) in `slot`, as picture `id`.
    bool keep(ID3D12GraphicsCommandList *commands, ID3D12Resource *buffer, std::size_t slot, std::uint64_t id, std::string &error);
    // Draws the picture kept as `id` in `slot` over `buffer` (PRESENT, left so) through `target`,
    // at `opacity`. False when that picture is not there.
    bool show(ID3D12GraphicsCommandList *commands, ID3D12Resource *buffer, D3D12_CPU_DESCRIPTOR_HANDLE target, std::size_t slot,
              std::uint64_t id, float opacity);
    // Lets everything go, or only the kept pictures: only once the GPU is done with them.
    void release();
    void release_pictures();
    bool prepared() const { return pipeline_ != nullptr; }
    std::size_t bytes() const; // the GPU memory it holds now

private:
    struct Slot {
        Microsoft::WRL::ComPtr<ID3D12Resource> picture;
        std::uint64_t id{};
    };
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> signature_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> shader_views_, targets_; // [0] the full picture, then a slot each / a slot each
    Microsoft::WRL::ComPtr<ID3D12Resource> full_;                         // the buffer's copy, to scale down from
    std::array<Slot, slots> slots_{};
    DXGI_FORMAT format_{};
    UINT width_{}, height_{}, small_width_{}, small_height_{};
    UINT shader_view_size_{}, target_size_{};
};
} // namespace dingosdk::overlay
