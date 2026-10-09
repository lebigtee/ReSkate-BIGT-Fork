// Runs the Boneyard's slow motion replay (boneyard_replay.cpp) on WARP, Windows' software
// D3D12 device, and checks the pixels it leaves: a kept picture comes back, scaled and faded, in
// each supported swapchain format, and nothing is drawn for a picture that was never kept.
// Build (x64 developer prompt, from Extension/Boneyard):
//   cl /nologo /std:c++20 /EHsc /W4 /O2 /I. Test\replay_test.cpp boneyard_replay.cpp d3d12.lib dxgi.lib
#include "boneyard_replay.h"
#include <Windows.h>
#include <dxgi1_4.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;
using dingosdk::overlay::PictureReplay;

namespace {
int failures = 0;
void check(bool condition, const char *what) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) ++failures;
}

struct Gpu {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    ComPtr<ID3D12Fence> fence;
    UINT64 value = 0;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    bool start() {
        ComPtr<IDXGIFactory4> factory;
        ComPtr<IDXGIAdapter> warp;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))) ||
            FAILED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
            return false;
        D3D12_COMMAND_QUEUE_DESC desc{};
        desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        return SUCCEEDED(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue))) &&
               SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) &&
               SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commands))) &&
               SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    }
    // Runs what was recorded and waits for it.
    void run() {
        commands->Close();
        ID3D12CommandList *lists[]{commands.Get()};
        queue->ExecuteCommandLists(1, lists);
        queue->Signal(fence.Get(), ++value);
        fence->SetEventOnCompletion(value, event);
        WaitForSingleObject(event, INFINITE);
        allocator->Reset();
        commands->Reset(allocator.Get(), nullptr);
    }
};

D3D12_RESOURCE_BARRIER transition(ID3D12Resource *resource, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = from;
    barrier.Transition.StateAfter = to;
    return barrier;
}

// A stand-in for a swapchain buffer: a render target resting in PRESENT.
struct Buffer {
    ComPtr<ID3D12Resource> resource;
    ComPtr<ID3D12DescriptorHeap> heap;
    D3D12_CPU_DESCRIPTOR_HANDLE target{};
    ComPtr<ID3D12Resource> readback;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    DXGI_FORMAT format{};
    UINT width{}, height{};
};
Buffer make_buffer(Gpu &gpu, DXGI_FORMAT format, UINT width, UINT height) {
    Buffer b;
    b.format = format;
    b.width = width;
    b.height = height;
    D3D12_HEAP_PROPERTIES memory{};
    memory.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    gpu.device->CreateCommittedResource(&memory, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_PRESENT, nullptr, IID_PPV_ARGS(&b.resource));
    D3D12_DESCRIPTOR_HEAP_DESC heap{};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heap.NumDescriptors = 1;
    gpu.device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&b.heap));
    b.target = b.heap->GetCPUDescriptorHandleForHeapStart();
    gpu.device->CreateRenderTargetView(b.resource.Get(), nullptr, b.target);
    UINT64 total{};
    gpu.device->GetCopyableFootprints(&desc, 0, 1, 0, &b.footprint, nullptr, nullptr, &total);
    memory.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bytes{};
    bytes.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bytes.Width = total;
    bytes.Height = bytes.DepthOrArraySize = bytes.MipLevels = 1;
    bytes.SampleDesc.Count = 1;
    bytes.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    gpu.device->CreateCommittedResource(&memory, D3D12_HEAP_FLAG_NONE, &bytes, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&b.readback));
    return b;
}
// Paints the left half one colour and the right half another, as a game frame would be drawn.
void paint(Gpu &gpu, Buffer &b, const float (&left)[4], const float (&right)[4]) {
    auto barrier = transition(b.resource.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    gpu.commands->ResourceBarrier(1, &barrier);
    const D3D12_RECT l{0, 0, static_cast<LONG>(b.width / 2), static_cast<LONG>(b.height)};
    const D3D12_RECT r{static_cast<LONG>(b.width / 2), 0, static_cast<LONG>(b.width), static_cast<LONG>(b.height)};
    gpu.commands->ClearRenderTargetView(b.target, left, 1, &l);
    gpu.commands->ClearRenderTargetView(b.target, right, 1, &r);
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    gpu.commands->ResourceBarrier(1, &barrier);
}
// The colour (0..1, red green blue) at a pixel, read back.
struct Colour {
    float r, g, b;
};
float half_to_float(std::uint16_t h) {
    const int exponent = (h >> 10) & 0x1f, mantissa = h & 0x3ff;
    const float sign = (h & 0x8000) ? -1.0f : 1.0f;
    if (!exponent) return sign * std::ldexp(static_cast<float>(mantissa), -24);
    return sign * std::ldexp(static_cast<float>(mantissa + 1024), exponent - 25);
}
Colour read(Gpu &gpu, Buffer &b, UINT x, UINT y) {
    auto barrier = transition(b.resource.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    gpu.commands->ResourceBarrier(1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION to{}, from{};
    to.pResource = b.readback.Get();
    to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    to.PlacedFootprint = b.footprint;
    from.pResource = b.resource.Get();
    from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    gpu.commands->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    gpu.commands->ResourceBarrier(1, &barrier);
    gpu.run();
    void *data{};
    b.readback->Map(0, nullptr, &data);
    const auto *row = static_cast<const unsigned char *>(data) + b.footprint.Footprint.RowPitch * y;
    Colour c{};
    if (b.format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
        std::uint16_t px[4];
        std::memcpy(px, row + x * 8, 8);
        c = {half_to_float(px[0]), half_to_float(px[1]), half_to_float(px[2])};
    } else if (b.format == DXGI_FORMAT_R10G10B10A2_UNORM) {
        std::uint32_t px;
        std::memcpy(&px, row + x * 4, 4);
        c = {(px & 0x3ff) / 1023.0f, ((px >> 10) & 0x3ff) / 1023.0f, ((px >> 20) & 0x3ff) / 1023.0f};
    } else {
        const auto *px = row + x * 4;
        c = {px[0] / 255.0f, px[1] / 255.0f, px[2] / 255.0f};
        if (b.format == DXGI_FORMAT_B8G8R8A8_UNORM) std::swap(c.r, c.b);
    }
    D3D12_RANGE nothing{};
    b.readback->Unmap(0, &nothing);
    return c;
}
bool close_to(const Colour &c, float r, float g, float b, float tolerance = 0.02f) {
    return std::abs(c.r - r) <= tolerance && std::abs(c.g - g) <= tolerance && std::abs(c.b - b) <= tolerance;
}

void run_format(Gpu &gpu, DXGI_FORMAT format, const char *name) {
    std::printf("-- %s, 2560x1440\n", name);
    auto b = make_buffer(gpu, format, 2560, 1440);
    PictureReplay replay;
    std::string error;
    const float red[]{1, 0, 0, 1}, green[]{0, 1, 0, 1}, blue[]{0, 0, 1, 1}, white[]{1, 1, 1, 1};
    check(replay.prepare(gpu.device.Get(), b.resource->GetDesc(), error), ("prepare " + error).c_str());
    // Keep a red | green picture as picture 1 in slot 0, and a white | blue one as picture 2 in the last slot.
    paint(gpu, b, red, green);
    check(replay.keep(gpu.commands.Get(), b.resource.Get(), 0, 1, error), "keep picture 1");
    paint(gpu, b, white, blue);
    check(replay.keep(gpu.commands.Get(), b.resource.Get(), PictureReplay::slots - 1, 2, error), "keep picture 2 in the last slot");
    gpu.run();
    // Over a blue frame: picture 1 in full.
    paint(gpu, b, blue, blue);
    check(replay.show(gpu.commands.Get(), b.resource.Get(), b.target, 0, 1, 1.0f), "show picture 1");
    gpu.run();
    check(close_to(read(gpu, b, 200, 700), 1, 0, 0), "its left half is red again");
    check(close_to(read(gpu, b, 2400, 700), 0, 1, 0), "its right half is green again");
    check(close_to(read(gpu, b, 0, 0), 1, 0, 0) && close_to(read(gpu, b, 2559, 1439), 0, 1, 0), "it reaches the corners");
    // Half faded over blue.
    paint(gpu, b, blue, blue);
    replay.show(gpu.commands.Get(), b.resource.Get(), b.target, 0, 1, 0.5f);
    gpu.run();
    check(close_to(read(gpu, b, 200, 700), 0.5f, 0, 0.5f), "at half opacity it is half red, half blue");
    // The other slot.
    paint(gpu, b, red, red);
    replay.show(gpu.commands.Get(), b.resource.Get(), b.target, PictureReplay::slots - 1, 2, 1.0f);
    gpu.run();
    check(close_to(read(gpu, b, 200, 700), 1, 1, 1) && close_to(read(gpu, b, 2400, 700), 0, 0, 1), "picture 2 comes back from the last slot");
    // A picture never kept, or kept over: nothing drawn.
    paint(gpu, b, red, red);
    check(!replay.show(gpu.commands.Get(), b.resource.Get(), b.target, 0, 2, 1.0f), "picture 2 is not in slot 0");
    check(!replay.show(gpu.commands.Get(), b.resource.Get(), b.target, 5, 1, 1.0f), "slot 5 holds nothing");
    gpu.run();
    check(close_to(read(gpu, b, 200, 700), 1, 0, 0), "and the frame is left alone");
    // Kept over: slot 0 now holds picture 3.
    paint(gpu, b, green, green);
    replay.keep(gpu.commands.Get(), b.resource.Get(), 0, 3, error);
    paint(gpu, b, red, red);
    check(!replay.show(gpu.commands.Get(), b.resource.Get(), b.target, 0, 1, 1.0f), "picture 1 is gone once slot 0 is kept over");
    check(replay.show(gpu.commands.Get(), b.resource.Get(), b.target, 0, 3, 1.0f), "picture 3 is there");
    gpu.run();
    check(close_to(read(gpu, b, 1280, 700), 0, 1, 0), "and shows green");
    std::printf("     GPU memory held: %.1f MB\n", replay.bytes() / 1048576.0);
    replay.release();
    check(!replay.prepared() && replay.bytes() == 0, "released");
}
} // namespace

int main() {
    Gpu gpu;
    if (!gpu.start()) {
        std::printf("FAIL no WARP device\n");
        return 1;
    }
    run_format(gpu, DXGI_FORMAT_R8G8B8A8_UNORM, "R8G8B8A8_UNORM");
    run_format(gpu, DXGI_FORMAT_B8G8R8A8_UNORM, "B8G8R8A8_UNORM");
    run_format(gpu, DXGI_FORMAT_R10G10B10A2_UNORM, "R10G10B10A2_UNORM");
    run_format(gpu, DXGI_FORMAT_R16G16B16A16_FLOAT, "R16G16B16A16_FLOAT");
    check(gpu.device->GetDeviceRemovedReason() == S_OK, "the device is still healthy");
    std::printf(failures ? "\n%d check(s) failed.\n" : "\nAll replay checks passed.\n", failures);
    return failures ? 1 : 0;
}
