#include "hall_of_meat_replay.h"
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

namespace dingosdk::overlay {
namespace {
#include "hall_of_meat_replay_shaders.h"

using Microsoft::WRL::ComPtr;

D3D12_RESOURCE_BARRIER transition(ID3D12Resource *resource, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = from;
    barrier.Transition.StateAfter = to;
    return barrier;
}
std::string failed(const char *what, HRESULT result) {
    char text[96];
    std::snprintf(text, sizeof text, "%s failed (0x%08lx)", what, static_cast<unsigned long>(result));
    return text;
}
UINT pixel_bytes(DXGI_FORMAT format) { return format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 : 4; }
} // namespace

bool PictureReplay::prepare(ID3D12Device *device, const D3D12_RESOURCE_DESC &buffer, std::string &error) {
    const auto width = static_cast<UINT>(buffer.Width);
    if (pipeline_) {
        if (device == device_.Get() && buffer.Format == format_ && width == width_ && buffer.Height == height_) return true;
        error = "the picture changed size or format";
        return false;
    }
    if (!device || buffer.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || !width || !buffer.Height || buffer.SampleDesc.Count != 1) {
        error = "unsupported picture";
        return false;
    }
    release();
    // The game links d3d12.dll; so does the test.
    const auto serialize = reinterpret_cast<PFN_D3D12_SERIALIZE_ROOT_SIGNATURE>(
        reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "D3D12SerializeRootSignature")));
    if (!serialize) {
        error = "D3D12SerializeRootSignature is unavailable";
        return false;
    }
    // One picture in, filtered; the blend factor fades it over the target.
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    D3D12_ROOT_PARAMETER parameter{};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable.NumDescriptorRanges = 1;
    parameter.DescriptorTable.pDescriptorRanges = &range;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC signature{};
    signature.NumParameters = 1;
    signature.pParameters = &parameter;
    signature.NumStaticSamplers = 1;
    signature.pStaticSamplers = &sampler;
    ComPtr<ID3DBlob> blob, message;
    auto result = serialize(&signature, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &message);
    if (FAILED(result)) {
        error = failed("root signature serialization", result);
        return false;
    }
    result = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&signature_));
    if (FAILED(result)) {
        error = failed("CreateRootSignature", result);
        return false;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{};
    pipeline.pRootSignature = signature_.Get();
    pipeline.VS = {replay_vs, sizeof replay_vs};
    pipeline.PS = {replay_ps, sizeof replay_ps};
    auto &blend = pipeline.BlendState.RenderTarget[0];
    blend.BlendEnable = TRUE;
    blend.SrcBlend = D3D12_BLEND_BLEND_FACTOR;
    blend.DestBlend = D3D12_BLEND_INV_BLEND_FACTOR;
    blend.BlendOp = D3D12_BLEND_OP_ADD;
    blend.SrcBlendAlpha = D3D12_BLEND_ZERO;
    blend.DestBlendAlpha = D3D12_BLEND_ONE;
    blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.LogicOp = D3D12_LOGIC_OP_NOOP;
    // Colour only: the target's alpha stays as the game left it.
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE;
    pipeline.SampleMask = UINT_MAX;
    pipeline.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pipeline.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pipeline.RasterizerState.DepthClipEnable = TRUE;
    pipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pipeline.NumRenderTargets = 1;
    pipeline.RTVFormats[0] = buffer.Format;
    pipeline.SampleDesc.Count = 1;
    result = device->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(&pipeline_));
    if (FAILED(result)) {
        release();
        error = failed("CreateGraphicsPipelineState", result);
        return false;
    }
    D3D12_DESCRIPTOR_HEAP_DESC heap{};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors = static_cast<UINT>(1 + slots);
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    result = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&shader_views_));
    if (SUCCEEDED(result)) {
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap.NumDescriptors = static_cast<UINT>(slots);
        heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        result = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&targets_));
    }
    if (FAILED(result)) {
        release();
        error = failed("CreateDescriptorHeap", result);
        return false;
    }
    // The full-size copy of the buffer, to scale down from.
    D3D12_HEAP_PROPERTIES memory{};
    memory.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC full{};
    full.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    full.Width = width;
    full.Height = buffer.Height;
    full.DepthOrArraySize = 1;
    full.MipLevels = 1;
    full.Format = buffer.Format;
    full.SampleDesc.Count = 1;
    result = device->CreateCommittedResource(&memory, D3D12_HEAP_FLAG_NONE, &full, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&full_));
    if (FAILED(result)) {
        release();
        error = failed("CreateCommittedResource (full picture)", result);
        return false;
    }
    device->CreateShaderResourceView(full_.Get(), nullptr, shader_views_->GetCPUDescriptorHandleForHeapStart());
    device_ = device;
    format_ = buffer.Format;
    width_ = width;
    height_ = buffer.Height;
    small_height_ = (std::min)(height_, most_height);
    small_width_ = (std::max)(1u, static_cast<UINT>(std::lround(static_cast<double>(width_) * small_height_ / height_)));
    shader_view_size_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    target_size_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    return true;
}

bool PictureReplay::keep(ID3D12GraphicsCommandList *commands, ID3D12Resource *buffer, std::size_t slot, std::uint64_t id,
                         std::string &error) {
    if (!pipeline_ || !commands || !buffer || slot >= slots || !id) {
        error = "not prepared";
        return false;
    }
    auto &s = slots_[slot];
    auto target = targets_->GetCPUDescriptorHandleForHeapStart();
    target.ptr += slot * target_size_;
    if (!s.picture) {
        D3D12_HEAP_PROPERTIES memory{};
        memory.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC kept{};
        kept.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        kept.Width = small_width_;
        kept.Height = small_height_;
        kept.DepthOrArraySize = 1;
        kept.MipLevels = 1;
        kept.Format = format_;
        kept.SampleDesc.Count = 1;
        kept.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        const auto result = device_->CreateCommittedResource(&memory, D3D12_HEAP_FLAG_NONE, &kept, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                             nullptr, IID_PPV_ARGS(&s.picture));
        if (FAILED(result)) {
            s.picture.Reset();
            error = failed("CreateCommittedResource (kept picture)", result);
            return false;
        }
        auto view = shader_views_->GetCPUDescriptorHandleForHeapStart();
        view.ptr += (1 + slot) * shader_view_size_;
        device_->CreateShaderResourceView(s.picture.Get(), nullptr, view);
        device_->CreateRenderTargetView(s.picture.Get(), nullptr, target);
    }
    s.id = 0;
    // The buffer into the full-size copy...
    auto barrier = transition(buffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    commands->ResourceBarrier(1, &barrier);
    commands->CopyResource(full_.Get(), buffer);
    const D3D12_RESOURCE_BARRIER before[]{
        transition(buffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT),
        transition(full_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
        transition(s.picture.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET)};
    commands->ResourceBarrier(3, before);
    // ...then drawn small into the slot.
    ID3D12DescriptorHeap *heaps[]{shader_views_.Get()};
    commands->SetDescriptorHeaps(1, heaps);
    commands->SetGraphicsRootSignature(signature_.Get());
    commands->SetPipelineState(pipeline_.Get());
    commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const D3D12_VIEWPORT viewport{0, 0, static_cast<float>(small_width_), static_cast<float>(small_height_), 0, 1};
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(small_width_), static_cast<LONG>(small_height_)};
    commands->RSSetViewports(1, &viewport);
    commands->RSSetScissorRects(1, &scissor);
    commands->OMSetRenderTargets(1, &target, FALSE, nullptr);
    commands->SetGraphicsRootDescriptorTable(0, shader_views_->GetGPUDescriptorHandleForHeapStart());
    const float whole[]{1, 1, 1, 1};
    commands->OMSetBlendFactor(whole);
    commands->DrawInstanced(3, 1, 0, 0);
    const D3D12_RESOURCE_BARRIER after[]{
        transition(full_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST),
        transition(s.picture.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)};
    commands->ResourceBarrier(2, after);
    s.id = id;
    return true;
}

bool PictureReplay::show(ID3D12GraphicsCommandList *commands, ID3D12Resource *buffer, D3D12_CPU_DESCRIPTOR_HANDLE target, std::size_t slot,
                         std::uint64_t id, float opacity) {
    if (!pipeline_ || !commands || !buffer || slot >= slots || !id || slots_[slot].id != id || !slots_[slot].picture) return false;
    opacity = std::clamp(opacity, 0.0f, 1.0f);
    if (opacity <= 0) return true;
    auto barrier = transition(buffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    commands->ResourceBarrier(1, &barrier);
    ID3D12DescriptorHeap *heaps[]{shader_views_.Get()};
    commands->SetDescriptorHeaps(1, heaps);
    commands->SetGraphicsRootSignature(signature_.Get());
    commands->SetPipelineState(pipeline_.Get());
    commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const D3D12_VIEWPORT viewport{0, 0, static_cast<float>(width_), static_cast<float>(height_), 0, 1};
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_)};
    commands->RSSetViewports(1, &viewport);
    commands->RSSetScissorRects(1, &scissor);
    commands->OMSetRenderTargets(1, &target, FALSE, nullptr);
    auto view = shader_views_->GetGPUDescriptorHandleForHeapStart();
    view.ptr += (1 + slot) * shader_view_size_;
    commands->SetGraphicsRootDescriptorTable(0, view);
    const float factor[]{opacity, opacity, opacity, opacity};
    commands->OMSetBlendFactor(factor);
    commands->DrawInstanced(3, 1, 0, 0);
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    commands->ResourceBarrier(1, &barrier);
    return true;
}

void PictureReplay::release_pictures() {
    for (auto &s : slots_) s = {};
}

void PictureReplay::release() {
    release_pictures();
    full_.Reset();
    targets_.Reset();
    shader_views_.Reset();
    pipeline_.Reset();
    signature_.Reset();
    device_.Reset();
    format_ = {};
    width_ = height_ = small_width_ = small_height_ = 0;
}

std::size_t PictureReplay::bytes() const {
    std::size_t total = full_ ? static_cast<std::size_t>(width_) * height_ * pixel_bytes(format_) : 0;
    for (const auto &s : slots_)
        if (s.picture) total += static_cast<std::size_t>(small_width_) * small_height_ * pixel_bytes(format_);
    return total;
}
} // namespace dingosdk::overlay
