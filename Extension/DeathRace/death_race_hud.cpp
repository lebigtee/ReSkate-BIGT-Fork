#include "death_race_hud.h"
#include "death_race.h"
#include "Extension/HallOfMeat/hall_of_meat.h"
#include "Extension/UI/Overlay/overlay_internal.h"
#include "Engine/Core/Profiling/profiler.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <string>

// Presentation thread: draws the Death Race from the snapshot death_race.cpp publishes. In the
// world the race is the game's own props (start line, slalom flags, finish arch); only when they
// are not standing (yet) does the overlay draw its flags, columns of light. While racing, an
// arrow at the screen's edge points to the next flag; the countdown, the clock and the result sit
// over the picture.
namespace dingosdk::overlay {
namespace {
namespace race = death_race;
using Clock = std::chrono::steady_clock;
using Vec3 = race::Vec3;

constexpr float flag_tall = 7.0f;       // metres of light above the flag's foot
constexpr float shown_within = 300.0f;  // start flags further than this are not drawn
constexpr double go_shown = 0.9;        // seconds "GO!" stays after the countdown

float dot(const Vec3 &a, const Vec3 &b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
ImU32 rgba(int r, int g, int b, float alpha) { return IM_COL32(r, g, b, static_cast<int>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f)); }
// The camera's pinhole, as the Hall of Meat's X-ray places bones.
struct Projector {
    Vec3 right{}, up{}, back{}, origin{};
    float focal{};
    ImVec2 centre;
    // Camera space: x right, y up, depth ahead.
    void space(const Vec3 &point, float &x, float &y, float &depth) const {
        const Vec3 d{point[0] - origin[0], point[1] - origin[1], point[2] - origin[2]};
        x = dot(d, right);
        y = dot(d, up);
        depth = -dot(d, back);
    }
    bool operator()(const Vec3 &point, ImVec2 &out, float &depth) const {
        float x{}, y{};
        space(point, x, y, depth);
        if (depth < 0.1f) return false;
        out = ImVec2(centre.x + x * focal / depth, centre.y - y * focal / depth);
        return true;
    }
};
void outlined(ImDrawList *draw, ImFont *font, float size, ImVec2 at, ImU32 fill, float alpha, const std::string &value) {
    const ImU32 edge = rgba(4, 6, 12, 0.9f * alpha);
    const float o = std::max(1.0f, size * 0.07f);
    const std::array<ImVec2, 8> around{{{-o, 0}, {o, 0}, {0, -o}, {0, o}, {-o, -o}, {o, -o}, {-o, o}, {o, o}}};
    for (const auto &d : around) draw->AddText(font, size, ImVec2(at.x + d.x, at.y + d.y), edge, value.c_str());
    draw->AddText(font, size, at, fill, value.c_str());
}
float width(ImFont *font, float size, const std::string &value) { return font->CalcTextSizeA(size, FLT_MAX, 0, value.c_str()).x; }
void centred(ImDrawList *draw, ImFont *font, float size, float x, float y, ImU32 fill, float alpha, const std::string &value) {
    outlined(draw, font, size, ImVec2(x - width(font, size, value) * 0.5f, y), fill, alpha, value);
}
std::string clock_text(double s) {
    const int whole = static_cast<int>(s);
    return std::format("{}:{:02}.{:02}", whole / 60, whole % 60, static_cast<int>((s - whole) * 100));
}

struct Fonts {
    ImFont *title, *bold, *body;
};
Fonts fonts() {
    auto &s = detail::state();
    return {s.menu.title ? s.menu.title : ImGui::GetFont(), s.menu.bold ? s.menu.bold : ImGui::GetFont(),
            s.menu.body ? s.menu.body : ImGui::GetFont()};
}

// One flag: a column of light from its foot, a pennant at its top, its name above, fading with
// distance. `label` is skipped when empty.
void draw_flag(ImDrawList *draw, const Projector &project, const Vec3 &foot, const std::array<int, 3> &colour, const std::string &label,
               float metres, float scale, float alpha) {
    ImVec2 bottom, top;
    float depth_bottom{}, depth_top{};
    const Vec3 head{foot[0], foot[1] + flag_tall, foot[2]};
    if (!project(foot, bottom, depth_bottom) || !project(head, top, depth_top)) return;
    const float fade = alpha * std::clamp(1.0f - (metres - 200.0f) / 100.0f, 0.15f, 1.0f);
    const float half = std::max(1.5f, project.focal / depth_bottom * 0.18f);
    const auto [r, g, b] = colour;
    // The column: brightest at its foot, gone at its top, and a glow around it.
    draw->AddRectFilledMultiColor(ImVec2(bottom.x - half * 3, top.y), ImVec2(bottom.x + half * 3, bottom.y), rgba(r, g, b, 0.0f),
                                  rgba(r, g, b, 0.0f), rgba(r, g, b, 0.18f * fade), rgba(r, g, b, 0.18f * fade));
    draw->AddRectFilledMultiColor(ImVec2(bottom.x - half, top.y), ImVec2(bottom.x + half, bottom.y), rgba(r, g, b, 0.05f * fade),
                                  rgba(r, g, b, 0.05f * fade), rgba(r, g, b, 0.85f * fade), rgba(r, g, b, 0.85f * fade));
    // A ring on the ground where it counts: the flag's reach.
    const float ring = project.focal / depth_bottom * race::flag_radius;
    if (ring > 4.0f && ring < 4000.0f) draw->AddEllipse(bottom, ImVec2(ring, ring * 0.3f), rgba(r, g, b, 0.55f * fade), 0, 48, std::max(1.5f, scale * 2.0f));
    // The pennant.
    const float p = std::max(6.0f, project.focal / depth_top * 0.9f);
    draw->AddTriangleFilled(top, ImVec2(top.x + p * 1.4f, top.y + p * 0.5f), ImVec2(top.x, top.y + p), rgba(r, g, b, 0.95f * fade));
    if (label.empty()) return;
    const auto f = fonts();
    const float size = std::clamp(project.focal / depth_top * 0.55f, 13.0f * scale, 26.0f * scale);
    const std::string text = metres > 25.0f ? std::format("{}  {:.0f} m", label, metres) : label;
    centred(draw, f.bold, size, top.x, top.y - size * 1.3f, rgba(255, 255, 255, fade), fade, text);
}

// The next flag's marker: a chevron floating over it, bobbing, with its name and distance. A
// throwdown hides the race's props, so this is what shows the way while racing.
void draw_marker(ImDrawList *draw, const Projector &project, const Vec3 &target, const std::array<int, 3> &colour,
                 const std::string &label, float metres, float scale, double t) {
    const float bob = static_cast<float>(std::sin(t * 3.0)) * 0.25f;
    const Vec3 top{target[0], target[1] + 4.0f + bob, target[2]};
    ImVec2 at, foot;
    float depth{}, foot_depth{};
    if (!project(top, at, depth)) return;
    const float size = std::clamp(project.focal / depth * 1.1f, 20.0f * scale, 56.0f * scale);
    const auto [r, g, b] = colour;
    // A faint beam down to the ground under it.
    if (project(target, foot, foot_depth))
        draw->AddLine(ImVec2(at.x, at.y + size * 0.5f), foot, rgba(r, g, b, 0.35f), std::max(1.5f, 2.0f * scale));
    const ImVec2 left(at.x - size * 0.6f, at.y - size * 0.45f), right(at.x + size * 0.6f, at.y - size * 0.45f),
        tip(at.x, at.y + size * 0.5f), notch(at.x, at.y + size * 0.05f);
    const float o = std::max(2.0f, size * 0.08f);
    draw->AddTriangleFilled(ImVec2(left.x, left.y + o), ImVec2(right.x, right.y + o), ImVec2(tip.x, tip.y + o), rgba(0, 0, 0, 0.45f));
    draw->AddTriangleFilled(left, notch, tip, rgba(r, g, b, 0.95f));
    draw->AddTriangleFilled(notch, right, tip, rgba(std::min(255, r + 30), std::min(255, g + 30), std::min(255, b + 30), 0.95f));
    draw->AddTriangle(left, right, tip, rgba(4, 6, 12, 0.9f), std::max(1.5f, size * 0.05f));
    const auto f = fonts();
    const float text = std::clamp(15.0f * scale, 13.0f, 30.0f);
    centred(draw, f.bold, text, at.x, at.y - size * 0.45f - text * 1.35f, rgba(255, 255, 255, 1.0f), 1.0f,
            std::format("{}  {:.0f} m", label, metres));
}

// Where the next flag is when it is off screen: an arrow at the edge pointing to it.
void draw_edge_arrow(ImDrawList *draw, const Projector &project, const ImVec2 &display, const Vec3 &target, const std::array<int, 3> &colour,
                     float metres, float scale) {
    float x{}, y{}, depth{};
    project.space(target, x, y, depth);
    ImVec2 at;
    float d{};
    const float margin = 60.0f * scale;
    if (depth > 0.1f && project(target, at, d) && at.x > margin && at.x < display.x - margin && at.y > margin && at.y < display.y - margin) return;
    // Direction on screen (behind the camera, the way it would turn).
    float dx = x, dy = -y;
    if (depth <= 0.1f) {
        dx = x >= 0 ? 1.0f : -1.0f;
        dy = 0.0f;
    }
    const float n = std::max(std::hypot(dx, dy), 1e-4f);
    dx /= n;
    dy /= n;
    const ImVec2 centre(display.x * 0.5f, display.y * 0.5f);
    const float reach = std::min((display.x * 0.5f - margin) / std::max(std::abs(dx), 1e-4f), (display.y * 0.5f - margin) / std::max(std::abs(dy), 1e-4f));
    const ImVec2 tip(centre.x + dx * reach, centre.y + dy * reach);
    const float size = 22.0f * scale;
    const ImVec2 side(-dy, dx);
    const auto [r, g, b] = colour;
    const ImVec2 a(tip.x - dx * size * 1.6f + side.x * size, tip.y - dy * size * 1.6f + side.y * size);
    const ImVec2 c(tip.x - dx * size * 1.6f - side.x * size, tip.y - dy * size * 1.6f - side.y * size);
    draw->AddTriangleFilled(ImVec2(tip.x + dx * 2, tip.y + dy * 2), a, c, rgba(0, 0, 0, 0.6f));
    draw->AddTriangleFilled(tip, a, c, rgba(r, g, b, 0.95f));
    const auto f = fonts();
    centred(draw, f.bold, 15.0f * scale, tip.x - dx * size * 3.0f, tip.y - dy * size * 3.0f - 8.0f * scale, rgba(255, 255, 255, 1.0f), 1.0f,
            std::format("{:.0f} m", metres));
}

constexpr std::array<int, 3> start_colour{70, 230, 120}, checkpoint_colour{255, 210, 60}, finish_colour{245, 70, 60};
} // namespace

bool death_race_hud_pending() {
    const auto v = race::view();
    if (v.hidden || Clock::now() - v.published > std::chrono::milliseconds(300)) return false;
    if (v.phase != race::Phase::idle || v.building || Clock::now() < v.notice_until) return true;
    if (!v.skater_known || v.props_live || v.shown < 0 || v.shown >= static_cast<int>(v.races.size())) return false;
    const auto &r = v.races[v.shown];
    return std::hypot(r.start[0] - v.skater[0], r.start[2] - v.skater[2]) < shown_within;
}

void draw_death_race_hud() {
    DINGO_PROFILE_ZONE("overlay/death race");
    const auto v = race::view();
    const auto steady = Clock::now();
    if (v.hidden || steady - v.published > std::chrono::milliseconds(300)) return;
    ImVec2 display = ImGui::GetIO().DisplaySize;
    if (auto &o = detail::state(); o.swapchain) {
        DXGI_SWAP_CHAIN_DESC1 description{};
        if (SUCCEEDED(o.swapchain->GetDesc1(&description)) && description.Width && description.Height)
            display = ImVec2(std::min(display.x, static_cast<float>(description.Width)), std::min(display.y, static_cast<float>(description.Height)));
    }
    if (display.x <= 0 || display.y <= 0) return;
    const float scale = std::clamp(display.y / 1080.0f, 0.75f, 2.5f);
    auto *draw = ImGui::GetBackgroundDrawList();
    const auto f = fonts();

    // ---- the flags in the world ----
    auto camera = v.camera;
    (void)hall_of_meat::live_camera(camera);
    Projector project;
    const bool seeing = camera.vertical_fov > 1 && camera.vertical_fov < 175;
    if (seeing) {
        const auto &m = camera.world;
        project.right = {m[0], m[1], m[2]};
        project.up = {m[4], m[5], m[6]};
        project.back = {m[8], m[9], m[10]};
        project.origin = {m[12], m[13], m[14]};
        project.focal = display.y / (2.0f * std::tan(camera.vertical_fov * 3.14159265f / 360.0f));
        project.centre = ImVec2(display.x * 0.5f, display.y * 0.5f);
    }
    const auto metres = [&](const Vec3 &p) {
        return v.skater_known ? std::sqrt((p[0] - v.skater[0]) * (p[0] - v.skater[0]) + (p[1] - v.skater[1]) * (p[1] - v.skater[1]) +
                                          (p[2] - v.skater[2]) * (p[2] - v.skater[2]))
                              : 0.0f;
    };
    const bool active_race = v.active >= 0 && v.active < static_cast<int>(v.races.size());
    // The overlay's flags stand in only while the game's props do not.
    const bool flags = !v.props_live;
    if (seeing) {
        if ((v.phase == race::Phase::racing || (v.phase == race::Phase::countdown && v.carried)) && active_race) {
            // Racing (and on the start, before the go): the next flag's marker, and an arrow at
            // the screen's edge while it is out of sight.
            const auto &r = v.races[v.active];
            const std::size_t next = v.phase == race::Phase::racing ? v.next : 0;
            const bool last = next >= r.checkpoints.size();
            const auto &target = last ? r.finish : r.checkpoints[next];
            const auto label = last ? std::string("FINISH") : std::format("CHECKPOINT {}/{}", next + 1, r.checkpoints.size());
            const double t = std::chrono::duration<double>(steady.time_since_epoch()).count();
            draw_marker(draw, project, target, last ? finish_colour : checkpoint_colour, label, metres(target), scale, t);
            if (!last) draw_marker(draw, project, r.finish, finish_colour, "FINISH", metres(r.finish), scale * 0.7f, t + 1.0);
            draw_edge_arrow(draw, project, display, target, last ? finish_colour : checkpoint_colour, metres(target), scale);
        } else if (flags) {
            // Riding around: the race shown (its start, and its finish just after a run); a new race's points.
            if (v.shown >= 0 && v.shown < static_cast<int>(v.races.size()) && !v.building) {
                const auto &r = v.races[v.shown];
                const float d = metres(r.start);
                if (d <= shown_within)
                    draw_flag(draw, project, r.start, start_colour,
                              r.best > 0 ? std::format("{}  (best {})", r.name, clock_text(r.best)) : r.name, d, scale, 1.0f);
                if (v.phase == race::Phase::finished)
                    draw_flag(draw, project, r.finish, finish_colour, "FINISH", metres(r.finish), scale, 1.0f);
            }
            for (std::size_t i = 0; i < v.draft.size(); ++i)
                draw_flag(draw, project, v.draft[i], i == 0 ? start_colour : checkpoint_colour,
                          i == 0 ? std::string("NEW RACE: START") : std::format("CHECKPOINT {}", i), metres(v.draft[i]), scale, 1.0f);
        }
    }

    // ---- over the picture ----
    const float cx = display.x * 0.5f;
    if (v.building) {
        const std::string line = v.draft.size() <= 1 ? "NEW RACE: ride to the finish, then Throwdowns > SET FINISH HERE"
                                                      : std::format("NEW RACE: {} checkpoint(s). Ride to the finish, then Throwdowns > SET FINISH HERE.", v.draft.size() - 1);
        centred(draw, f.bold, 20.0f * scale, cx, display.y * 0.06f, rgba(255, 255, 255, 1.0f), 1.0f, line);
    }
    // Opened as a throwdown, the countdown is the game's own.
    if (v.phase == race::Phase::countdown && active_race && !v.carried) {
        const int n = std::max(1, static_cast<int>(std::ceil(v.countdown)));
        const float t = static_cast<float>(std::ceil(v.countdown) - v.countdown); // 0 -> 1 within each second
        const float size = (150.0f - 40.0f * t) * scale;
        centred(draw, f.title, 26.0f * scale, cx, display.y * 0.16f, rgba(255, 255, 255, 1.0f), 1.0f, v.races[v.active].name);
        centred(draw, f.title, size, cx, display.y * 0.30f - size * 0.5f, rgba(255, 255, 255, 1.0f - t * 0.4f), 1.0f, std::to_string(n));
        centred(draw, f.body, 17.0f * scale, cx, display.y * 0.30f + 70.0f * scale, rgba(220, 230, 240, 1.0f), 1.0f,
                v.carried ? "Get ready" : "Stay on the start line");
    }
    if (v.phase == race::Phase::racing && active_race) {
        const auto &r = v.races[v.active];
        if (v.elapsed < go_shown) {
            const float t = static_cast<float>(v.elapsed / go_shown);
            const float size = (160.0f + 60.0f * t) * scale;
            centred(draw, f.title, size, cx, display.y * 0.30f - size * 0.5f, rgba(90, 240, 130, 1.0f - t), 1.0f - t, "GO!");
        }
        // The clock, top centre, on a plate.
        const auto clock = clock_text(v.elapsed);
        const float clock_size = 48.0f * scale, w = std::max(width(f.title, clock_size, clock), 220.0f * scale) + 40.0f * scale;
        const float top = display.y * 0.035f;
        draw->AddRectFilled(ImVec2(cx - w * 0.5f, top), ImVec2(cx + w * 0.5f, top + clock_size + 46.0f * scale), rgba(4, 8, 16, 0.55f), 8.0f * scale);
        centred(draw, f.bold, 14.0f * scale, cx, top + 5.0f * scale, rgba(200, 210, 225, 1.0f), 1.0f, r.name);
        centred(draw, f.title, clock_size, cx, top + 20.0f * scale, rgba(255, 255, 255, 1.0f), 1.0f, clock);
        const std::string next = v.next < r.checkpoints.size() ? std::format("CHECKPOINT {}/{}  -  {:.0f} m", v.next + 1, r.checkpoints.size(), v.distance)
                                                               : std::format("FINISH  -  {:.0f} m", v.distance);
        centred(draw, f.bold, 15.0f * scale, cx, top + clock_size + 24.0f * scale, rgba(255, 220, 120, 1.0f), 1.0f, next);
    }
    if (v.phase == race::Phase::finished && active_race) {
        const float left = std::chrono::duration<float>(v.result_until - steady).count();
        const float alpha = std::clamp(left / 0.6f, 0.0f, 1.0f);
        const float w = 420.0f * scale, h = 240.0f * scale, top = display.y * 0.18f;
        draw->AddRectFilled(ImVec2(cx - w * 0.5f, top), ImVec2(cx + w * 0.5f, top + h), rgba(4, 8, 16, 0.7f * alpha), 10.0f * scale);
        draw->AddRectFilled(ImVec2(cx - w * 0.5f, top), ImVec2(cx + w * 0.5f, top + 5.0f * scale), rgba(245, 70, 60, alpha), 10.0f * scale,
                            ImDrawFlags_RoundCornersTop);
        const auto ordinal = [](int n) { return std::to_string(n) + (n == 1 ? "st" : n == 2 ? "nd" : n == 3 ? "rd" : "th"); };
        // Won or lost against the riders, then the place.
        const bool won = v.racers > 1 && v.place == 1;
        centred(draw, f.title, 46.0f * scale, cx, top + 14.0f * scale,
                v.racers < 2 ? rgba(255, 255, 255, alpha) : won ? rgba(90, 240, 130, alpha) : rgba(245, 90, 80, alpha), alpha,
                v.racers < 2 ? std::string("FINISH") : won ? std::string("YOU WIN!") : std::string("YOU LOSE"));
        if (v.racers > 1)
            centred(draw, f.title, 30.0f * scale, cx, top + 64.0f * scale, rgba(255, 255, 255, alpha), alpha,
                    std::format("{} OF {}", ordinal(v.place), v.racers));
        centred(draw, f.title, 50.0f * scale, cx, top + 100.0f * scale, rgba(255, 255, 255, alpha), alpha, clock_text(v.time));
        const std::string verdict = v.record ? (v.previous_best > 0 ? std::format("NEW RECORD!  ({} before)", clock_text(v.previous_best)) : "FIRST TIME SET!")
                                             : std::format("Best {}  (+{})", clock_text(v.previous_best), clock_text(v.time - v.previous_best));
        centred(draw, f.bold, 20.0f * scale, cx, top + 176.0f * scale, v.record ? rgba(90, 240, 130, alpha) : rgba(255, 210, 120, alpha), alpha, verdict);
        centred(draw, f.body, 14.0f * scale, cx, top + 206.0f * scale, rgba(200, 210, 225, alpha), alpha, v.races[v.active].name);
    }
    if (steady < v.notice_until && !v.notice.empty()) {
        const float alpha = std::clamp(std::chrono::duration<float>(v.notice_until - steady).count() / 0.6f, 0.0f, 1.0f);
        centred(draw, f.bold, 20.0f * scale, cx, display.y * 0.80f, rgba(255, 230, 160, alpha), alpha, v.notice);
    }
}
} // namespace dingosdk::overlay
