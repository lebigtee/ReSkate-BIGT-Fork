#include "hall_of_meat_hud.h"
#include "hall_of_meat.h"
#include "hall_of_meat_replay.h"
#include "Extension/UI/Overlay/overlay_internal.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Profiling/profiler.h"
#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <format>
#include <span>
#include <string>
#include <utility>
#include <vector>

// Presentation thread: reads the Hall of Meat's snapshot (hall_of_meat_view.cpp) and draws skate
// 3's X-ray over the skater: a skeleton picture cut into bones, each laid between the skater's
// own joints, tinted pink, and red where it broke. The skater stays fully visible underneath.
namespace dingosdk::overlay {
namespace {
namespace meat = hall_of_meat;
using Clock = std::chrono::steady_clock;
using meat::Point;
using meat::Severity;
using Microsoft::WRL::ComPtr;

// ---- the pictures --------------------------------------------------------------------------
// Built into ReSkate.dll (cmake/Runtime.cmake) by Test/build_assets.ps1 from the artist's
// drawings, and added to ImGui's atlas at startup like the chat emotes:
//  * HOM_BONES, assets/bones.png: the skeleton from the front, from the back and in profile (the
//    profile split in two: its arm alone, and the rest without the arm drawn over it), white on a
//    transparent background for the tint to colour. The X-ray draws the front, with the back's skull
//    when the skater is seen from behind (the profile looked jumpy as the skater turned). The sprites below are measured on the drawings as drawn (1024 x 1536);
//    each drawing's crop and place in the sheet maps them onto it.
//  * HOM_PANEL, assets/panel.png: the result panel's artwork.
struct Image {
    const wchar_t *resource;
    std::vector<unsigned char> rgba;
    UINT width{}, height{};
    ImFontAtlas *atlas{};
    int rect = -1;
    bool filled{};
};
std::array<Image, 2> &images() {
    static std::array<Image, 2> value{{{L"HOM_BONES"}, {L"HOM_PANEL"}}};
    return value;
}
Image &sheet() { return images()[0]; }
Image &panel_art() { return images()[1]; }
// The picture's corners in the atlas, once it is there.
bool atlas_uv(const Image &image, ImVec2 &uv0, ImVec2 &uv1) {
    if (!image.filled || !image.atlas || image.atlas != ImGui::GetIO().Fonts || !image.atlas->TexID) return false;
    const auto *rect = image.atlas->GetCustomRectByIndex(image.rect);
    if (!rect || !rect->IsPacked()) return false;
    image.atlas->CalcCustomRectUV(rect, &uv0, &uv1);
    return true;
}
// Where each drawing went in the sheet (build_assets.ps1 prints these): its crop on the drawing,
// and the crop's corner in the sheet, all scaled by picture_scale.
constexpr float sheet_width = 1230.0f, sheet_height = 917.0f, picture_scale = 0.6f;
struct Picture {
    float crop_x, crop_y, at_x, at_y;
};
constexpr std::array<Picture, 4> pictures{{{105, 5, 0, 0}, {130, 20, 492, 0}, {405, 78, 954, 0}, {405, 78, 1094, 0}}};
ImVec2 on_sheet(std::uint8_t picture, ImVec2 p) {
    const auto &c = pictures[picture];
    return ImVec2(c.at_x + (p.x - c.crop_x) * picture_scale, c.at_y + (p.y - c.crop_y) * picture_scale);
}
std::span<const unsigned char> embedded(const wchar_t *name) {
    HMODULE module{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&embedded), &module))
        return {};
    const auto resource = FindResourceW(module, name, MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    const auto handle = resource ? LoadResource(module, resource) : nullptr;
    const auto *data = handle ? static_cast<const unsigned char *>(LockResource(handle)) : nullptr;
    return data ? std::span<const unsigned char>(data, SizeofResource(module, resource)) : std::span<const unsigned char>{};
}
// Straight-alpha RGBA, as the atlas keeps it (chat_emotes.cpp decodes its pack the same way).
bool decode(std::span<const unsigned char> bytes, std::vector<unsigned char> &pixels, UINT &width, UINT &height) {
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    if (bytes.empty() || FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory(const_cast<BYTE *>(bytes.data()), static_cast<DWORD>(bytes.size()))) ||
        FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) || FAILED(frame->GetSize(&width, &height)) || !width || !height || width > 4096 ||
        height > 4096)
        return false;
    if (FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0,
                                     WICBitmapPaletteTypeCustom)))
        return false;
    pixels.resize(static_cast<std::size_t>(width) * height * 4);
    return SUCCEEDED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.size()), pixels.data()));
}

float dot(const meat::Vec3 &a, const meat::Vec3 &b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
float metres_between(const meat::Vec3 &a, const meat::Vec3 &b) {
    const meat::Vec3 d{a[0] - b[0], a[1] - b[1], a[2] - b[2]};
    return std::sqrt(dot(d, d));
}
float length(ImVec2 v) { return std::sqrt(v.x * v.x + v.y * v.y); }
meat::Vec3 minus(const meat::Vec3 &a, const meat::Vec3 &b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
meat::Vec3 cross(const meat::Vec3 &a, const meat::Vec3 &b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
// The camera's pinhole, as the nametags place names: rows right, up, back and position.
struct Projector {
    meat::Vec3 right{}, up{}, back{}, origin{};
    float focal{};
    ImVec2 centre;
    bool operator()(const meat::Vec3 &point, ImVec2 &out, float &depth) const {
        const meat::Vec3 delta{point[0] - origin[0], point[1] - origin[1], point[2] - origin[2]};
        depth = -dot(delta, back);
        if (depth < 0.1f) return false;
        out = ImVec2(centre.x + dot(delta, right) * focal / depth, centre.y - dot(delta, up) * focal / depth);
        return true;
    }
};
// The X-ray's own pinkish white, then each severity's colour.
std::array<int, 3> severity_rgb(Severity severity) {
    switch (severity) {
    case Severity::crack: return {255, 145, 85};
    case Severity::fracture: return {240, 52, 40};
    case Severity::open: return {222, 22, 32};
    case Severity::shattered: return {176, 0, 24};
    default: return {255, 222, 218};
    }
}
ImU32 rgba(int r, int g, int b, float alpha) { return IM_COL32(r, g, b, static_cast<int>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f)); }
ImU32 colour(Severity severity, float alpha) {
    const auto rgb = severity_rgb(severity);
    return rgba(rgb[0], rgb[1], rgb[2], alpha);
}
// A severity's colour lifted towards white, to read on the panel's dark blue.
ImU32 lifted(Severity severity, float alpha) {
    auto rgb = severity_rgb(severity);
    for (auto &c : rgb) c += (255 - c) / 5;
    return rgba(rgb[0], rgb[1], rgb[2], alpha);
}
void text(ImDrawList *draw, ImFont *font, float size, ImVec2 at, ImU32 fill, ImU32 shadow, const std::string &value) {
    draw->AddText(font, size, ImVec2(at.x + size * 0.06f, at.y + size * 0.06f), shadow, value.c_str());
    draw->AddText(font, size, at, fill, value.c_str());
}
float width(ImFont *font, float size, const std::string &value) { return font->CalcTextSizeA(size, FLT_MAX, 0, value.c_str()).x; }

// ---- the bones ---------------------------------------------------------------------------
// How a bone's breadth on screen follows the body: a limb is round, so its breadth is its size
// at that distance; the ribcage and the skull are flat-ish, so they narrow as the skater turns
// side-on (by how much of the shoulders' width the camera sees).
enum class Breadth : std::uint8_t { round, torso };
// One bone of a drawing: the axis between two of its points, which is laid over the axis between
// the skater's points `from` and `to`; how far the picture runs past them along the axis, and to
// each side of it (`left` is to the left of the axis as it runs on the drawing).
struct Sprite {
    Point from, to;
    std::size_t bone;     // whose severity tints it
    std::uint8_t picture; // which drawing (pictures)
    ImVec2 a, b;          // on that drawing, as drawn
    float before, after, left, right;
    Breadth breadth;
    bool twin; // the other side's bone too: mirrored from the front and back, the same art in profile
};
// Measured on the drawings with Test/skeleton_spec.py, which prints these tables. From the front
// the drawing's left is the skater's right; from the back, their left; the profile faces right.
constexpr std::array<Sprite, 12> front_sprites{{
    {meat::right_hip, meat::left_hip, 6, 0, {420, 772}, {604, 772}, 32, 32, 68, 134, Breadth::round, false}, // pelvis
    {meat::spine1, meat::spine, 5, 0, {512, 530}, {512, 655}, 2, 15, 38, 38, Breadth::round, false}, // lumbar
    {meat::spine3, meat::spine1, 4, 0, {512, 290}, {512, 515}, 32, 55, 132, 132, Breadth::torso, false}, // ribcage
    {meat::neck, meat::head, 1, 0, {512, 262}, {512, 202}, 6, 4, 28, 28, Breadth::round, false}, // neck
    {meat::head, meat::skull, 0, 0, {512, 200}, {512, 116}, 6, 108, 72, 70, Breadth::torso, false}, // skull
    {meat::right_hip, meat::right_knee, 13, 0, {420, 772}, {426, 1042}, 22, 4, 42, 32, Breadth::round, true}, // femur
    {meat::right_knee, meat::right_ankle, 15, 0, {425, 1052}, {428, 1384}, 6, 6, 50, 33, Breadth::round, true}, // tibia
    {meat::right_ankle, meat::right_toes, 17, 0, {428, 1392}, {422, 1465}, 9, 69, 54, 49, Breadth::round, true}, // foot
    {meat::right_clavicle, meat::right_shoulder, 2, 0, {468, 296}, {348, 293}, 4, 16, 15, 14, Breadth::round, true}, // clavicle
    {meat::right_shoulder, meat::right_elbow, 7, 0, {340, 322}, {272, 558}, 20, 10, 30, 30, Breadth::round, true}, // humerus
    {meat::right_elbow, meat::right_wrist, 9, 0, {270, 572}, {212, 740}, 6, 2, 30, 22, Breadth::round, true}, // forearm
    {meat::right_wrist, meat::right_knuckles, 11, 0, {197, 760}, {178, 812}, 6, 102, 68, 64, Breadth::round, true}, // hand
}};
// From behind, the body keeps the front drawing (the skater's own joints turn it); only the skull
// becomes the back of the head, so a skater seen from behind does not show a face.
constexpr Sprite back_skull{meat::head, meat::skull, 0, 1, {512, 175}, {512, 91}, 5, 67, 70, 70, Breadth::torso, false};
// The other side's point or bone.
Point other_side(Point p) {
    switch (p) {
    case meat::right_clavicle: return meat::left_clavicle;
    case meat::right_shoulder: return meat::left_shoulder;
    case meat::right_elbow: return meat::left_elbow;
    case meat::right_wrist: return meat::left_wrist;
    case meat::right_knuckles: return meat::left_knuckles;
    case meat::right_hip: return meat::left_hip;
    case meat::right_knee: return meat::left_knee;
    case meat::right_ankle: return meat::left_ankle;
    case meat::right_toes: return meat::left_toes;
    case meat::left_clavicle: return meat::right_clavicle;
    case meat::left_shoulder: return meat::right_shoulder;
    case meat::left_elbow: return meat::right_elbow;
    case meat::left_wrist: return meat::right_wrist;
    case meat::left_knuckles: return meat::right_knuckles;
    case meat::left_hip: return meat::right_hip;
    case meat::left_knee: return meat::right_knee;
    case meat::left_ankle: return meat::right_ankle;
    case meat::left_toes: return meat::right_toes;
    default: return p;
    }
}
std::size_t other_bone(std::size_t bone) {
    if (bone == 2 || bone == 3) return 5 - bone; // the collarbones
    return bone >= 7 ? (bone % 2 ? bone + 1 : bone - 1) : bone;
}
// The other side's twin: the front drawing mirrored about its middle (x 512).
Sprite twin_of(const Sprite &s) {
    constexpr float axis = 1024.0f; // a drawing's mirror line, doubled
    Sprite t = s;
    t.from = other_side(s.from);
    t.to = other_side(s.to);
    t.bone = other_bone(s.bone);
    t.a.x = axis - s.a.x;
    t.b.x = axis - s.b.x;
    std::swap(t.left, t.right);
    return t;
}
// Drawing order, back to front: the trunk, the legs, then the arms over the ribs. The skull is
// fifth (index 4).
constexpr std::size_t skull_sprite = 4;
const std::vector<Sprite> &front() {
    static const std::vector<Sprite> all = [] {
        std::vector<Sprite> result(front_sprites.begin(), front_sprites.begin() + 5);
        for (std::size_t i = 5; i < front_sprites.size(); ++i) {
            result.push_back(front_sprites[i]);
            if (front_sprites[i].twin) result.push_back(twin_of(front_sprites[i]));
        }
        return result;
    }();
    return all;
}
// Whether the camera is behind the skater: its chest faces away (with some give, so the skull
// does not flicker as the skater turns side-on). Which way cross(spine, shoulders) points, out of
// the chest or out of the back, is the engine's handedness: learned whenever front and back are
// plain to see, from which way the shoulders run on screen against the spine.
bool seen_from_behind(const meat::View &view, const Projector &project, float torso_breadth) {
    static bool behind{};
    static float chest_sign = 0.0f;
    ImVec2 hips_at, top_at, right_at, left_at;
    float depth{};
    if (!project(view.points[meat::hips], hips_at, depth) || !project(view.points[meat::spine3], top_at, depth) ||
        !project(view.points[meat::right_shoulder], right_at, depth) || !project(view.points[meat::left_shoulder], left_at, depth))
        return behind;
    const ImVec2 up(top_at.x - hips_at.x, top_at.y - hips_at.y), across(right_at.x - left_at.x, right_at.y - left_at.y);
    const float turn = up.x * across.y - up.y * across.x; // below 0: the chest faces the camera
    const auto &hips = view.points[meat::hips];
    const auto chest = cross(minus(view.points[meat::spine3], hips), minus(view.points[meat::right_shoulder], view.points[meat::left_shoulder]));
    const auto camera = minus(project.origin, hips);
    if (torso_breadth > 0.6f) chest_sign = (turn < 0) == (dot(chest, camera) > 0) ? 1.0f : -1.0f;
    const float size = std::sqrt(dot(chest, chest) * dot(camera, camera));
    if (!chest_sign || size < 1e-6f) {
        behind = turn >= 0;
        return behind;
    }
    const float facing = chest_sign * dot(chest, camera) / size; // 1: the chest faces the camera
    if (behind ? facing > 0.2f : facing < -0.2f) behind = !behind;
    return behind;
}

// When each bone broke, for the flash it gets. Per bail.
struct Flashes {
    std::uint64_t serial{};
    std::array<Severity, meat::bone_count> shown{};
    std::array<double, meat::bone_count> at{};
    double results_from{};
    meat::Phase phase{};
};
Flashes &flashes() {
    static Flashes value;
    return value;
}

// One sprite's corners on screen, and on the sheet. False when it is behind the camera.
bool place(const Sprite &sprite, const meat::View &view, const Projector &project, float torso_breadth,
           std::array<ImVec2, 4> &screen, std::array<ImVec2, 4> &sheet_points, ImVec2 &middle, float &thickness) {
    ImVec2 a, b;
    float depth_a{}, depth_b{};
    if (!project(view.points[sprite.from], a, depth_a) || !project(view.points[sprite.to], b, depth_b)) return false;
    const ImVec2 axis(sprite.b.x - sprite.a.x, sprite.b.y - sprite.a.y);
    const float sheet_length = length(axis);
    const ImVec2 d(axis.x / sheet_length, axis.y / sheet_length), n(-d.y, d.x);
    // Screen pixels per sheet pixel: along the bone, its projected length (foreshortened when it
    // points at the camera); across it, its real size at that distance.
    const float metres = metres_between(view.points[sprite.from], view.points[sprite.to]);
    const float depth = (depth_a + depth_b) * 0.5f;
    float across = project.focal / depth * metres / sheet_length;
    if (sprite.breadth == Breadth::torso) across *= torso_breadth;
    ImVec2 along(b.x - a.x, b.y - a.y);
    const float screen_length = length(along);
    along = screen_length > 0.5f ? ImVec2(along.x / screen_length, along.y / screen_length) : ImVec2(0, 1);
    const float stretch = std::max(screen_length / sheet_length, across * 0.25f);
    const ImVec2 normal(-along.y, along.x);
    const std::array<std::pair<float, float>, 4> corners{{{-sprite.before, sprite.left}, {sheet_length + sprite.after, sprite.left},
                                                          {sheet_length + sprite.after, -sprite.right}, {-sprite.before, -sprite.right}}};
    for (std::size_t i = 0; i < 4; ++i) {
        const auto [u, v] = corners[i];
        sheet_points[i] = on_sheet(sprite.picture, ImVec2(sprite.a.x + d.x * u + n.x * v, sprite.a.y + d.y * u + n.y * v));
        screen[i] = ImVec2(a.x + along.x * u * stretch + normal.x * v * across, a.y + along.y * u * stretch + normal.y * v * across);
    }
    middle = ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
    thickness = std::clamp(across * 30.0f, 1.5f, 14.0f);
    return true;
}

void draw_xray(ImDrawList *draw, const meat::View &view, const Projector &project, double now, float fade) {
    using namespace meat;
    const auto &s = sheet();
    ImVec2 uv0, uv1;
    if (!atlas_uv(s, uv0, uv1)) return;
    const auto uv = [&](ImVec2 p) {
        return ImVec2(uv0.x + (uv1.x - uv0.x) * std::clamp(p.x / sheet_width, 0.0f, 1.0f),
                      uv0.y + (uv1.y - uv0.y) * std::clamp(p.y / sheet_height, 0.0f, 1.0f));
    };
    // How much of the shoulders' width the camera sees: 1 facing it, less side-on.
    float torso_breadth = 1.0f;
    {
        ImVec2 r, l;
        float dr{}, dl{};
        const float real = metres_between(view.points[right_shoulder], view.points[left_shoulder]);
        if (real > 0.01f && project(view.points[right_shoulder], r, dr) && project(view.points[left_shoulder], l, dl))
            torso_breadth = std::clamp(length(ImVec2(r.x - l.x, r.y - l.y)) / (project.focal / ((dr + dl) * 0.5f) * real), 0.35f, 1.0f);
    }
    const bool behind = seen_from_behind(view, project, torso_breadth);
    const auto &sprites = front();
    auto &f = flashes();
    const float pulse = 0.78f + 0.22f * static_cast<float>(std::sin(now * 9.0));
    for (std::size_t i = 0; i < sprites.size(); ++i) {
        const auto &sprite = behind && i == skull_sprite ? back_skull : sprites[i];
        std::array<ImVec2, 4> screen, sheet_points;
        ImVec2 middle;
        float thick{};
        if (!place(sprite, view, project, torso_breadth, screen, sheet_points, middle, thick)) continue;
        const auto severity = view.severity[sprite.bone];
        const float alpha = (broken(severity) ? 0.95f * pulse : 0.72f) * fade;
        draw->AddImageQuad(s.atlas->TexID, screen[0], screen[1], screen[2], screen[3], uv(sheet_points[0]), uv(sheet_points[1]),
                           uv(sheet_points[2]), uv(sheet_points[3]), colour(severity, alpha));
        if (!broken(severity)) continue;
        // A ring bursting out as it breaks.
        const float age = static_cast<float>(now - f.at[sprite.bone]);
        if (f.at[sprite.bone] > 0 && age < 0.45f) {
            const float t = age / 0.45f;
            draw->AddCircle(middle, thick * (2.0f + 10.0f * t), colour(severity, (1.0f - t) * fade), 20, thick * 0.5f);
        }
    }
}

// Soft gradients ImGui has no call for: bands between closed rings of as many points, each point
// with its own colour (a ring of one point repeated makes a fan).
struct Vertex {
    ImVec2 at;
    ImU32 colour;
};
using Ring = std::vector<Vertex>;
void draw_bands(ImDrawList *draw, const std::vector<Ring> &rings) {
    if (rings.size() < 2 || rings[0].size() < 3) return;
    const std::size_t n = rings[0].size();
    const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
    draw->PrimReserve(static_cast<int>((rings.size() - 1) * n * 6), static_cast<int>(rings.size() * n));
    const std::size_t base = draw->_VtxCurrentIdx; // after the reserve, which may start a new vertex block
    for (const auto &ring : rings)
        for (const auto &v : ring) draw->PrimWriteVtx(v.at, uv, v.colour);
    for (std::size_t r = 0; r + 1 < rings.size(); ++r)
        for (std::size_t i = 0; i < n; ++i) {
            const std::size_t a = base + r * n + i, b = base + r * n + (i + 1) % n;
            for (const auto index : {a, b, b + n, a, b + n, a + n}) draw->PrimWriteIdx(static_cast<ImDrawIdx>(index));
        }
}

// skate 3's red Hall of Meat screen: a faint red over everything, deepening towards the edges and
// most in the corners. Elliptic rings around the centre, the last one past the corners.
void draw_vignette(ImDrawList *draw, const ImVec2 &display, float alpha) {
    if (alpha <= 0) return;
    const auto red = [&](float a) { return rgba(140, 6, 16, a * alpha); };
    draw->PushClipRect(ImVec2(0, 0), display, true);
    draw->AddRectFilled(ImVec2(0, 0), display, red(0.10f));
    constexpr int segments = 96;
    constexpr std::array<std::pair<float, float>, 4> rings{{{0.55f, 0.0f}, {0.8f, 0.28f}, {1.0f, 0.66f}, {1.45f, 0.9f}}}; // size, red
    const ImVec2 centre(display.x * 0.5f, display.y * 0.5f);
    std::vector<Ring> mesh;
    for (const auto &[size, amount] : rings) {
        auto &ring = mesh.emplace_back();
        for (int i = 0; i < segments; ++i) {
            const float angle = 6.2831853f * static_cast<float>(i) / segments;
            ring.push_back({ImVec2(centre.x + std::cos(angle) * centre.x * size, centre.y + std::sin(angle) * centre.y * size), red(amount)});
        }
    }
    draw_bands(draw, mesh);
    draw->PopClipRect();
}

// The panel's wear: the same for a whole bail (seeded by it), and kept in place as the panel grows
// upwards (everything is placed from its bottom-left corner).
float noise(std::uint64_t seed, std::uint32_t key) {
    std::uint64_t h = seed * 0x9E3779B97F4A7C15ull + key * 0xBF58476D1CE4E5B9ull;
    h ^= h >> 31;
    h *= 0x94D049BB133111EBull;
    h ^= h >> 29;
    return static_cast<float>(h >> 40) / 16777216.0f;
}

// Text that reads over any picture: a dark edge all round, then the colour.
void outlined(ImDrawList *draw, ImFont *font, float size, ImVec2 at, ImU32 fill, float alpha, const std::string &value) {
    const ImU32 edge = rgba(2, 8, 20, 0.9f * alpha);
    const float o = std::max(1.0f, size * 0.07f);
    const std::array<ImVec2, 8> around{{{-o, 0}, {o, 0}, {0, -o}, {0, o}, {-o, -o}, {o, -o}, {-o, o}, {o, o}}};
    for (const auto &d : around) draw->AddText(font, size, ImVec2(at.x + d.x, at.y + d.y), edge, value.c_str());
    draw->AddText(font, size, at, fill, value.c_str());
}

// The result over the artist's panel (assets/panel.png), bottom right, the size of its artwork
// for the fullest result: the title, the count big, then each bone over its severity, on soft dark
// plates so the X-rays behind stay seen. False when the artwork is not there.
bool draw_panel_art(ImDrawList *draw, const meat::View &view, const ImVec2 &display, float scale, float alpha,
                    const std::vector<std::size_t> &broken_bones, const std::string &count_label, const std::string &numbers) {
    using namespace meat;
    const auto &art = panel_art();
    ImVec2 uv0, uv1;
    if (!art.height || !atlas_uv(art, uv0, uv1)) return false;
    auto &s = detail::state();
    ImFont *title_font = s.menu.title ? s.menu.title : ImGui::GetFont();
    ImFont *bold = s.menu.bold ? s.menu.bold : ImGui::GetFont();
    ImFont *body = s.menu.body ? s.menu.body : ImGui::GetFont();
    const float panel_width = 300.0f * scale, panel_height = panel_width * static_cast<float>(art.height) / static_cast<float>(art.width);
    const ImVec2 end(std::max(display.x - display.y * 0.09f, panel_width + 16.0f * scale), display.y * 0.955f);
    const ImVec2 origin(end.x - panel_width, end.y - panel_height);
    draw->AddImage(art.atlas->TexID, origin, end, uv0, uv1, rgba(255, 255, 255, alpha));

    constexpr std::size_t most = 5;
    const float inset = 24.0f * scale, gap = 4.0f * scale, inner = panel_width - inset * 2;
    const float title_size = std::min(34.0f * scale, inner / std::max(width(title_font, 1.0f, "HALL OF MEAT"), 0.01f));
    const float count_size = 52.0f * scale, label_size = 15.0f * scale, detail_size = 14.0f * scale;
    const float name_size = 15.0f * scale, severity_size = 13.0f * scale, row_height = name_size + severity_size + 7.0f * scale;
    const std::size_t rows = std::min(broken_bones.size(), most);
    const bool more = broken_bones.size() > most;
    const ImU32 plate = rgba(2, 8, 22, 0.42f * alpha);
    float y = origin.y + inset;
    // The heading on its plate.
    const float heading = title_size * 0.95f + gap + count_size + label_size + gap * 2 + detail_size;
    draw->AddRectFilled(ImVec2(origin.x + inset - 8.0f * scale, y - 6.0f * scale), ImVec2(end.x - inset + 8.0f * scale, y + heading + 6.0f * scale),
                        plate, 6.0f * scale);
    const auto centred = [&](ImFont *font, float size, const std::string &value, ImU32 fill, float advance) {
        outlined(draw, font, size, ImVec2(origin.x + (panel_width - width(font, size, value)) * 0.5f, y), fill, alpha, value);
        y += advance;
    };
    centred(title_font, title_size, "HALL OF MEAT", rgba(242, 248, 255, alpha), title_size * 0.95f + gap);
    centred(title_font, count_size, std::to_string(broken_bones.size()), rgba(242, 248, 255, alpha), count_size);
    centred(bold, label_size, count_label, rgba(150, 210, 255, alpha), label_size + gap * 2);
    centred(body, detail_size, numbers, rgba(190, 215, 240, alpha), detail_size);
    if (!rows) return true;
    // The bones on theirs.
    y += gap * 4;
    const float list = rows * row_height + (more ? name_size + gap : 0.0f);
    draw->AddRectFilled(ImVec2(origin.x + inset - 8.0f * scale, y - 6.0f * scale), ImVec2(end.x - inset + 8.0f * scale, y + list + 4.0f * scale),
                        plate, 6.0f * scale);
    for (std::size_t i = 0; i < rows; ++i) {
        const auto k = broken_bones[i];
        draw->AddRectFilled(ImVec2(origin.x + inset, y + scale), ImVec2(origin.x + inset + 3.0f * scale, y + name_size + severity_size),
                            lifted(view.severity[k], 0.95f * alpha));
        outlined(draw, body, name_size, ImVec2(origin.x + inset + 9.0f * scale, y), rgba(236, 244, 252, alpha), alpha, bones[k].name);
        outlined(draw, bold, severity_size, ImVec2(origin.x + inset + 9.0f * scale, y + name_size + scale), lifted(view.severity[k], alpha), alpha,
                 severity_name(view.severity[k]));
        y += row_height;
    }
    if (more)
        outlined(draw, body, name_size, ImVec2(origin.x + inset + 9.0f * scale, y), rgba(170, 200, 230, alpha), alpha,
                 std::format("+ {} more", broken_bones.size() - most));
    return true;
}

void draw_result(ImDrawList *draw, const meat::View &view, const ImVec2 &display, float scale, float alpha) {
    using namespace meat;
    auto &s = detail::state();
    ImFont *title_font = s.menu.title ? s.menu.title : ImGui::GetFont();
    ImFont *bold = s.menu.bold ? s.menu.bold : ImGui::GetFont();
    ImFont *body = s.menu.body ? s.menu.body : ImGui::GetFont();
    std::vector<std::size_t> broken_bones;
    for (std::size_t k = 0; k < bone_count; ++k)
        if (broken(view.severity[k])) broken_bones.push_back(k);
    std::sort(broken_bones.begin(), broken_bones.end(), [&](std::size_t a, std::size_t b) {
        return view.severity[a] != view.severity[b] ? view.severity[a] > view.severity[b] : view.impact[a] > view.impact[b];
    });
    const std::string count = std::to_string(broken_bones.size());
    const std::string count_label = broken_bones.size() == 1 ? "BROKEN BONE" : "BROKEN BONES";
    std::string detail = std::format("{:.0f} km/h", view.speed * 3.6f);
    if (view.drop > 0) detail += std::format("  \xC2\xB7  {:.1f} m fall", view.drop);
    if (draw_panel_art(draw, view, display, scale, alpha, broken_bones, count_label, detail)) return;

    // Without the artwork: a narrow film in the bottom right corner, as far in from the right and as high as the game's
    // own score is from the left (the game lays its HUD out by the picture's height). The title
    // stacked, the count big, then each bone over its severity.
    constexpr std::size_t most = 5;
    const float panel_width = 250.0f * scale, pad = 14.0f * scale, gap = 4.0f * scale, inner = panel_width - pad * 2;
    const float title_size = std::min(44.0f * scale, inner / std::max(width(title_font, 1.0f, "HALL OF"), 0.01f));
    const float count_size = 58.0f * scale, label_size = 16.0f * scale, detail_size = 15.0f * scale;
    const float name_size = 16.0f * scale, severity_size = 14.0f * scale, row_height = name_size + severity_size + 8.0f * scale;
    const std::size_t rows = std::min(broken_bones.size(), most);
    const bool more = broken_bones.size() > most;
    const float panel_height = pad * 2 + title_size * 1.9f + gap + count_size + label_size + gap * 2 + detail_size +
                               (rows ? gap * 4 + rows * row_height + (more ? name_size + gap : 0.0f) : 0.0f);
    const ImVec2 end(std::max(display.x - display.y * 0.09f, panel_width + 16.0f * scale), display.y * 0.955f);
    const ImVec2 origin(end.x - panel_width, end.y - panel_height);
    const std::uint64_t seed = view.serial + 1;
    const auto wear = [&](std::uint32_t key) { return noise(seed, key); };
    const float most_height = 700.0f * scale; // the wear is laid out this high, and shows as far as the panel goes

    // The film's edge: a little ragged, its corners chipped.
    std::vector<ImVec2> outline;
    {
        float chip[4];
        for (std::uint32_t c = 0; c < 4; ++c) chip[c] = (3.0f + 7.0f * wear(c)) * scale;
        const float step = 14.0f * scale, rag = 2.5f * scale;
        // Along the top and the bottom, from the left; up the sides, from the bottom.
        const auto across = [&](float y, float inward, bool rightwards, std::uint32_t key, float left_chip, float right_chip) {
            const float left = origin.x + left_chip, right = end.x - right_chip;
            const int steps = std::max(2, static_cast<int>((right - left) / step));
            for (int i = 0; i <= steps; ++i) {
                const int k = rightwards ? i : steps - i;
                const float in = k == 0 || k == steps ? 0.0f : rag * wear(key + static_cast<std::uint32_t>(k));
                outline.emplace_back(left + (right - left) * static_cast<float>(k) / static_cast<float>(steps), y + inward * in);
            }
        };
        const auto upright = [&](float x, float inward, bool downwards, std::uint32_t key, float top_chip, float bottom_chip) {
            const float bottom = end.y - bottom_chip, top = origin.y + top_chip;
            std::vector<ImVec2> side{ImVec2(x, bottom)};
            for (int k = 1; bottom - k * step > top + step * 0.5f; ++k)
                side.emplace_back(x + inward * rag * wear(key + static_cast<std::uint32_t>(k)), bottom - k * step);
            side.emplace_back(x, top);
            if (downwards) std::reverse(side.begin(), side.end());
            outline.insert(outline.end(), side.begin(), side.end());
        };
        across(origin.y, 1.0f, true, 100, chip[0], chip[1]);
        upright(end.x, -1.0f, true, 300, chip[1], chip[2]);
        across(end.y, -1.0f, false, 500, chip[3], chip[2]);
        upright(origin.x, 1.0f, false, 700, chip[0], chip[3]);
    }
    // An X-ray film on a lightbox: deep blue, lit from behind in the middle, darker at the top and
    // bottom, burnt at the edges.
    const ImVec2 middle((origin.x + end.x) * 0.5f, (origin.y + end.y) * 0.5f);
    const float burn = 16.0f * scale, half_w = panel_width * 0.5f, half_h = panel_height * 0.5f;
    const auto film = [&](ImVec2 at, float light) {
        const float t = std::clamp((at.y - origin.y) / panel_height, 0.0f, 1.0f);
        const auto channel = [&](float top, float bottom) { return std::clamp(static_cast<int>((top + (bottom - top) * t) * light), 0, 255); };
        return rgba(channel(30, 8), channel(70, 22), channel(118, 48), (0.86f + 0.06f * t) * alpha);
    };
    {
        Ring centre, lit, edge;
        for (const auto &p : outline) {
            const ImVec2 in(middle.x + (p.x - middle.x) * (half_w - burn) / half_w, middle.y + (p.y - middle.y) * (half_h - burn) / half_h);
            centre.push_back({middle, film(middle, 1.3f)});
            lit.push_back({in, film(in, 1.0f)});
            edge.push_back({p, film(p, 0.5f)});
        }
        draw_bands(draw, {centre, lit, edge});
    }
    // Stains: blotches lighter and darker.
    for (std::uint32_t k = 0; k < 14; ++k) {
        const float r = (12.0f + 30.0f * wear(1000 + k * 4)) * scale;
        const ImVec2 at(origin.x + r + (panel_width - 2 * r) * wear(1001 + k * 4), end.y - r - most_height * wear(1002 + k * 4));
        if (at.y - r < origin.y + 2.0f * scale) continue;
        const bool dark = wear(1003 + k * 4) < 0.6f;
        draw->AddCircleFilled(at, r, dark ? rgba(0, 4, 14, 0.12f * alpha) : rgba(130, 180, 230, 0.06f * alpha), 24);
    }
    // A crease across the film.
    {
        const float from = 0.25f + 0.5f * wear(1100), to = 0.25f + 0.5f * wear(1101);
        const ImVec2 a(origin.x + 3.0f * scale, end.y - std::min(from * most_height * 0.5f, panel_height - 6.0f * scale));
        const ImVec2 b(end.x - 3.0f * scale, end.y - std::min(to * most_height * 0.5f, panel_height - 6.0f * scale));
        draw->AddLine(a, b, rgba(170, 210, 245, 0.10f * alpha), std::max(1.0f, scale));
        draw->AddLine(ImVec2(a.x, a.y + scale), ImVec2(b.x, b.y + scale), rgba(0, 0, 0, 0.22f * alpha), std::max(1.0f, scale));
    }

    // The text, centred and then down the left.
    const auto shadow = rgba(0, 8, 24, 0.8f * alpha);
    float y = origin.y + pad;
    const auto centred = [&](ImFont *font, float size, const std::string &value, ImU32 fill, float advance, bool ghost = false) {
        const ImVec2 at(origin.x + (panel_width - width(font, size, value)) * 0.5f, y);
        // A print slightly out of register, as worn film labels are.
        if (ghost) draw->AddText(font, size, ImVec2(at.x + 1.5f * scale, at.y + 0.5f * scale), rgba(90, 170, 255, 0.35f * alpha), value.c_str());
        text(draw, font, size, at, fill, shadow, value);
        y += advance;
    };
    centred(title_font, title_size, "HALL OF", rgba(242, 248, 255, alpha), title_size * 0.95f, true);
    centred(title_font, title_size, "MEAT", rgba(242, 248, 255, alpha), title_size * 0.95f + gap, true);
    centred(title_font, count_size, count, rgba(242, 248, 255, alpha), count_size, true);
    centred(bold, label_size, count_label, rgba(140, 205, 255, alpha), label_size + gap * 2);
    centred(body, detail_size, detail, rgba(178, 205, 232, alpha), detail_size + gap);
    if (rows) {
        y += gap;
        draw->AddLine(ImVec2(origin.x + pad, y), ImVec2(end.x - pad, y), rgba(140, 190, 235, 0.45f * alpha), std::max(1.0f, scale));
        y += gap * 2;
        for (std::size_t i = 0; i < rows; ++i) {
            const auto k = broken_bones[i];
            // A mark in the severity's colour beside the bone's name and, under it, the severity.
            draw->AddRectFilled(ImVec2(origin.x + pad, y + scale), ImVec2(origin.x + pad + 3.0f * scale, y + name_size + severity_size),
                                lifted(view.severity[k], 0.9f * alpha));
            text(draw, body, name_size, ImVec2(origin.x + pad + 9.0f * scale, y), rgba(232, 242, 252, alpha), shadow, bones[k].name);
            text(draw, bold, severity_size, ImVec2(origin.x + pad + 9.0f * scale, y + name_size + scale), lifted(view.severity[k], alpha), shadow,
                 severity_name(view.severity[k]));
            y += row_height;
        }
        if (more)
            text(draw, body, name_size, ImVec2(origin.x + pad + 9.0f * scale, y), rgba(160, 192, 224, alpha), shadow,
                 std::format("+ {} more", broken_bones.size() - most));
    }

    // Over everything, the text too: grain and scratches.
    for (std::uint32_t k = 0; k < 700; ++k) {
        const ImVec2 at(origin.x + 3.0f * scale + (panel_width - 6.0f * scale) * wear(2000 + k * 4), end.y - 3.0f * scale - most_height * wear(2001 + k * 4));
        if (at.y < origin.y + 3.0f * scale) continue;
        const float size = (0.8f + 1.4f * wear(2002 + k * 4)) * scale, shade = wear(2003 + k * 4);
        draw->AddRectFilled(at, ImVec2(at.x + size, at.y + size),
                            shade < 0.55f ? rgba(0, 0, 0, (0.22f + 0.3f * shade) * alpha) : rgba(215, 232, 255, (0.06f + 0.22f * (shade - 0.55f)) * alpha));
    }
    for (std::uint32_t k = 0; k < 7; ++k) {
        const float x = origin.x + 6.0f * scale + (panel_width - 12.0f * scale) * wear(3000 + k * 5);
        const float bottom = end.y - 4.0f * scale - most_height * wear(3001 + k * 5);
        const float top = std::max(bottom - (30.0f + 150.0f * wear(3002 + k * 5)) * scale, origin.y + 4.0f * scale);
        if (bottom - top < 10.0f * scale) continue;
        const float lean = (wear(3003 + k * 5) - 0.5f) * 14.0f * scale, bend = (wear(3004 + k * 5) - 0.5f) * 6.0f * scale;
        const ImVec2 scratch[]{ImVec2(x, bottom), ImVec2(x + lean * 0.5f + bend, (top + bottom) * 0.5f), ImVec2(x + lean, top)};
        draw->AddPolyline(scratch, 3, rgba(215, 235, 255, (0.10f + 0.14f * wear(3005 + k * 5)) * alpha), 0, std::max(1.0f, 0.8f * scale));
    }
    // The border, worn through in places.
    for (std::size_t i = 0; i < outline.size(); ++i) {
        const auto &a = outline[i], &b = outline[(i + 1) % outline.size()];
        const ImVec2 mid((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
        const auto key = 4000 + static_cast<std::uint32_t>((mid.x - origin.x) / (7.0f * scale)) * 131 + static_cast<std::uint32_t>((end.y - mid.y) / (7.0f * scale));
        const float worn = wear(key);
        if (worn < 0.18f) continue;
        draw->AddLine(a, b, rgba(160, 205, 245, (0.2f + 0.45f * worn) * alpha), std::max(1.0f, 1.4f * scale));
    }
}

// The jerky picture: a fresh one this often, the kept one in between. The skeleton is held with
// it, so it stays on the skater in the picture.
constexpr double stutter_period = 1.0 / 40.0;
// The slow motion replay (a multiplayer session): from the bail's start, in real seconds, the
// picture slows to half speed over `replay_in`, plays on at that speed, and fades out to the live
// game from `replay_fade` to `replay_end`. It moves on as often as the jerky picture does, so it
// needs the game's pictures kept twice as often (80 a second at most); at most 22 of the `slots`
// wait to be shown, even at 240 frames a second (Test/replay_plan.py), in 720p.
constexpr double replay_in = 0.12, replay_fade = 0.72, replay_end = 1.02, replay_speed = 0.5;
constexpr double replay_keep_every = stutter_period * replay_speed;
// The moment of the bail on screen `t` seconds into it.
double replay_moment(double t) {
    if (t <= replay_in) return t - (1.0 - replay_speed) * t * t / (2.0 * replay_in);
    return replay_in * (1.0 + replay_speed) / 2.0 + replay_speed * (t - replay_in);
}
struct Replay {
    std::uint64_t serial{}; // the bail replayed
    bool running{};
    double started{}, last_kept{}, on_screen{}; // on_screen: the moment of the picture shown
    std::uint64_t ids{};
    struct Kept {
        std::uint64_t id{};
        double moment{};
        meat::View view; // the skeleton in that picture
    };
    std::array<Kept, PictureReplay::slots> slots{};
};
struct Hold {
    FrameHold frame;     // this frame's
    std::uint64_t picture{};
    double kept_at{};
    bool kept{};
    meat::View view;     // the skeleton as it was in the kept picture
    Replay replay;
    // What the HUD costs while the picture is jerky (its busiest moment): logged as it ends.
    bool jerky{};
    std::uint64_t serial{};
    struct {
        std::uint64_t serial{};
        unsigned frames{}, kept{}, shown{};
        double total{}, longest{}; // microseconds
    } cost;
};
Hold &hold() {
    static Hold value;
    return value;
}
} // namespace

FrameHold hall_of_meat_frame_hold() { return hold().frame; }
void draw_hud();

std::size_t reserve_hall_of_meat_bones(ImFontAtlas &atlas) noexcept {
    std::size_t reserved{};
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    for (auto &image : images()) {
        const auto *resource = image.resource;
        image = {resource};
        try {
            if (!decode(embedded(resource), image.rgba, image.width, image.height)) continue;
            atlas.TexDesiredWidth = std::max(atlas.TexDesiredWidth, 2048);
            atlas.Flags |= ImFontAtlasFlags_NoPowerOfTwoHeight;
            image.rect = atlas.AddCustomRectRegular(static_cast<int>(image.width), static_cast<int>(image.height));
            image.atlas = &atlas;
            ++reserved;
        } catch (...) {
            image = {resource};
        }
    }
    if (SUCCEEDED(com)) CoUninitialize();
    // No skeleton, no X-ray: the panel alone does not count.
    return sheet().atlas ? reserved : 0;
}

void fill_hall_of_meat_bones(ImFontAtlas &atlas) noexcept {
    for (auto &image : images()) {
        if (image.atlas != &atlas || image.rect < 0 || image.rgba.empty()) continue;
        try {
            unsigned char *pixels{};
            int width{}, height{};
            atlas.GetTexDataAsRGBA32(&pixels, &width, &height);
            const auto *rect = atlas.GetCustomRectByIndex(image.rect);
            if (!pixels || !rect || !rect->IsPacked() || rect->X + static_cast<int>(image.width) > width ||
                rect->Y + static_cast<int>(image.height) > height)
                continue;
            for (UINT y = 0; y < image.height; ++y)
                std::memcpy(pixels + ((static_cast<std::size_t>(rect->Y) + y) * width + rect->X) * 4,
                            image.rgba.data() + static_cast<std::size_t>(y) * image.width * 4, static_cast<std::size_t>(image.width) * 4);
            atlas.TexPixelsUseColors = true;
            image.filled = true;
            image.rgba = {};
        } catch (...) {
            image.filled = false;
        }
    }
}

bool hall_of_meat_hud_pending() {
    const auto view = meat::view();
    if (view.hidden || Clock::now() - view.published > std::chrono::milliseconds(300)) return false;
    return (view.xray && view.pose) || view.phase == meat::Phase::bailing ||
           (view.phase == meat::Phase::results && Clock::now() < view.results_until);
}

void draw_hall_of_meat_hud() {
    DINGO_PROFILE_ZONE("overlay/hall of meat");
    auto &h = hold();
    const bool was_jerky = h.jerky;
    const auto from = Clock::now();
    draw_hud();
    const double spent = std::chrono::duration<double, std::micro>(Clock::now() - from).count();
    auto &c = h.cost;
    if (h.jerky) {
        if (c.serial != h.serial) c = {h.serial};
        ++c.frames;
        c.kept += h.frame.action == FrameHold::Action::keep;
        c.shown += h.frame.action == FrameHold::Action::show;
        c.total += spent;
        c.longest = std::max(c.longest, spent);
    } else if (was_jerky && c.frames) {
        logging::printf(logging::Level::info, logging::Channel::graphics,
                        "Hall of Meat: bail %llu jerky picture over: %u frames, %u fresh, %u held; the HUD took %.0f us a frame on average, "
                        "%.0f us at most.",
                        static_cast<unsigned long long>(c.serial), c.frames, c.kept, c.shown, c.total / c.frames, c.longest);
        c = {};
    }
}

// This frame's part of the replay: which picture to keep, which to show, and how strongly.
// Returns the one shown, for its skeleton.
const Replay::Kept *plan_replay(Hold &h, const meat::View &view, double now) {
    auto &r = h.replay;
    auto &plan = h.frame.replay;
    plan.ready = view.replay;
    const bool wanted = view.replay && view.phase == meat::Phase::bailing;
    if (wanted && view.serial != r.serial) {
        r = {};
        r.serial = view.serial;
        r.running = true;
        r.started = now;
        r.last_kept = -1;
    }
    if (r.running && (!wanted || view.serial != r.serial || now - r.started >= replay_end)) r.running = false;
    if (!r.running) return nullptr;
    const double t = now - r.started;
    // Keep this frame's picture while the replay is yet to reach it, in a slot it is done with.
    if (t <= replay_moment(replay_end) + replay_keep_every && t - r.last_kept >= replay_keep_every * 0.9) {
        for (std::size_t k = 0; k < r.slots.size(); ++k) {
            auto &slot = r.slots[k];
            if (slot.id && slot.moment >= r.on_screen) continue;
            slot = {++r.ids, t, view};
            (void)meat::live_camera(slot.view.camera);
            plan.keep_slot = static_cast<int>(k);
            plan.keep_id = slot.id;
            r.last_kept = t;
            break;
        }
    }
    // On screen: the latest picture kept by the moment the replay is at, moving on in jerks.
    const double moment = replay_moment(std::floor(t / stutter_period) * stutter_period) + 1e-6;
    const Replay::Kept *shown = nullptr;
    int shown_slot = -1;
    for (std::size_t k = 0; k < r.slots.size(); ++k) {
        const auto &slot = r.slots[k];
        if (slot.id && slot.moment <= moment && (!shown || slot.moment > shown->moment)) {
            shown = &slot;
            shown_slot = static_cast<int>(k);
        }
    }
    if (!shown) return nullptr;
    r.on_screen = shown->moment;
    plan.show_slot = shown_slot;
    plan.show_id = shown->id;
    plan.opacity = t < replay_fade ? 1.0f : static_cast<float>(1.0 - (t - replay_fade) / (replay_end - replay_fade));
    return shown;
}

void draw_hud() {
    auto &h = hold();
    h.frame = {};
    h.jerky = false;
    const auto view = meat::view();
    const auto steady = Clock::now();
    if (view.hidden || steady - view.published > std::chrono::milliseconds(300)) {
        h.kept = false;
        return;
    }
    const double now = ImGui::GetTime();
    // In a multiplayer session the replay moves in jerks itself: no held picture on top.
    const bool jerky = view.stutter && !view.replay;
    const Replay::Kept *replayed = plan_replay(h, view, now);
    h.jerky = jerky || h.replay.running;
    h.serial = view.serial;
    if (!jerky) h.kept = false;
    else if (!h.kept || now - h.kept_at >= stutter_period) {
        h.kept = true;
        h.kept_at = now;
        h.frame.action = FrameHold::Action::keep;
        h.frame.picture = ++h.picture;
    } else {
        h.frame.action = FrameHold::Action::show;
        h.frame.picture = h.picture;
    }
    // Note when each bone broke (for its flash) and when the result came up (for its fade).
    auto &f = flashes();
    if (view.serial != f.serial) {
        f = {};
        f.serial = view.serial;
    }
    for (std::size_t k = 0; k < meat::bone_count; ++k) {
        if (meat::broken(view.severity[k]) && view.severity[k] > f.shown[k]) f.at[k] = now;
        f.shown[k] = std::max(f.shown[k], view.severity[k]);
    }
    if (view.phase == meat::Phase::bailing && f.phase != meat::Phase::bailing) f.results_from = now;
    f.phase = view.phase;
    // The part of ImGui's display the picture shows. The overlay draws into the swapchain's
    // buffer at ImGui's display size, the window's client area; a game rendering below its
    // window's size (1920x1080 stretched over a 3440x1440 screen) shows only the buffer, the
    // top-left part of that, scaled up to fill the screen. Everything is placed inside it.
    ImVec2 display = ImGui::GetIO().DisplaySize;
    if (auto &o = detail::state(); o.swapchain) {
        DXGI_SWAP_CHAIN_DESC1 description{};
        if (SUCCEEDED(o.swapchain->GetDesc1(&description)) && description.Width && description.Height)
            display = ImVec2(std::min(display.x, static_cast<float>(description.Width)),
                             std::min(display.y, static_cast<float>(description.Height)));
    }
    if (display.x <= 0 || display.y <= 0) return;
    const float scale = std::clamp(display.y / 1080.0f, 0.75f, 2.5f);
    // The result shows all through the bail, and for a moment once the skater is up again.
    const bool bailing = view.phase == meat::Phase::bailing;
    const bool showing_result = bailing || (view.phase == meat::Phase::results && steady < view.results_until);
    // In over 0.25 s as the bail starts, out over the last 0.6 s after it.
    float alpha = std::clamp(static_cast<float>((now - f.results_from) / 0.25), 0.0f, 1.0f);
    if (!bailing && showing_result)
        alpha = std::min(alpha, std::clamp(std::chrono::duration<float>(view.results_until - steady).count() / 0.6f, 0.0f, 1.0f));
    auto *draw = ImGui::GetBackgroundDrawList();
    // The red screen comes and goes with the panel, under the skeleton.
    if (showing_result) draw_vignette(draw, display, alpha);
    // Only during the bail (or `meat xray always`): it is gone once the skater is up.
    const auto skeleton = [&](const meat::View &shown, float fade) {
        const auto &camera = shown.camera;
        if (fade <= 0 || !shown.pose || camera.vertical_fov <= 1 || camera.vertical_fov >= 175) return;
        const auto &m = camera.world;
        Projector project;
        project.right = {m[0], m[1], m[2]};
        project.up = {m[4], m[5], m[6]};
        project.back = {m[8], m[9], m[10]};
        project.origin = {m[12], m[13], m[14]};
        project.focal = display.y / (2.0f * std::tan(camera.vertical_fov * 3.14159265f / 360.0f));
        project.centre = ImVec2(display.x * 0.5f, display.y * 0.5f);
        draw_xray(draw, shown, project, now, fade);
    };
    // Held, the skeleton as it was in the kept picture; otherwise now, at the camera's latest.
    const bool held = h.frame.action == FrameHold::Action::show && h.view.serial == view.serial;
    if (!held) {
        h.view = view;
        (void)meat::live_camera(h.view.camera);
    }
    if (view.xray && view.pose) {
        // Replayed, the skeleton of the picture on screen, fading into the live one with it.
        if (replayed) {
            skeleton(replayed->view, h.frame.replay.opacity);
            skeleton(h.view, 1.0f - h.frame.replay.opacity);
        } else {
            skeleton(h.view, 1.0f);
        }
    }
    if (showing_result) draw_result(draw, view, display, scale, alpha);
}
} // namespace dingosdk::overlay
