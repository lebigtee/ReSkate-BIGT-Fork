#include "native_throwdown_tile.h"
#include "native_menu_data.h"
#include "Extension/DeathRace/death_race.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/native_menu.h"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <format>
#include <map>
#include <optional>
#include <string_view>
#include <vector>

// The throwdown mode select is a LinearList (Throwdown_ModeSelect_LinearList) whose ListItems are
// references to runtime rows the game makes when the page opens, one per mode, each with a
// CategoryTile (title, icon, a description presenter and its label). The DEATH RACE tile is a
// fourth set of those four models, copied from Spot Battle's, appended to the list; its button
// calls back here. A tile of our own starts no throwdown, though (the page knows its own rows):
// when the tile opens a race, carried by a Skate Jam, ours leaves the list and the game's own
// Skate Jam row shows our tile instead, keeping its own action, so the game's whole Jam flow runs
// from it (flag, Customize, countdown, the call to the start). The race knows the Jam is its own
// from the cursor having been on that tile. Ours are released a moment after the page's list is
// gone, and before level transitions; the Jam row gets its own tile back first.
//
// While such a race is being set up, its setup page is the race's: titled Death Race, with one
// setting, the players slider turned into how many AI riders race (0 to 4), and no Customize
// button. The slider's value is the race's (death_race::set_riders); the players limit the
// game sends is put right on its way out (throwdown_lab.cpp). While the race runs, the
// throwdown's timer is off the screen: the race ends when everyone has crossed the line.
namespace dingosdk::multiplayer {
using namespace menu_data;
namespace {
namespace engine = addr::engine;
constexpr Schema row_schema{0xdeb20e0f, 1200}, tile_schema{0x628aa99c, 320}, anchored_schema{0x0e3be640, 192},
    label_schema{0xbfe23948, 104};
constexpr std::uint32_t list_items = 0x67223da7;
// Row -> its presenter -> {button, content widget (the CategoryTile and its data)}.
constexpr std::uint32_t row_presenter = 0x214d4984, row_button = 0x0fb0d794, row_content = 0x214d4984;
// The tile data: title label, icons (idle, focused) and the description's presenter.
constexpr std::uint32_t tile_title = 0x77b8ed0b, tile_icon = 0x189084da, tile_icon_focused = 0xc2917efc,
    tile_description = 0x214d4984;
// An anchored presenter's content widget; a label's text.
constexpr std::uint32_t anchored_content = 0x214d4984, label_text = 0x4d8e01b9;
constexpr std::uint32_t action_navigation = 0xb1e14cd6, action_delegate = 0x9c76b86c;
// The list's index fields: the first follows the cursor (-1 when the page has no focus).
constexpr std::uint32_t list_focus = 0x55511280, list_entry = 0x1600d48e;
// A row's tile size (a Vector2, 1124 x 1374 as shipped): the page has room for three across,
// and no scrolling, so with ours every tile shrinks to fit the same width.
constexpr std::uint32_t row_size = 0x868043e1;
using Size = std::array<float, 2>;
// Inside a tile, laid out for the shipped size: the icon's (two floats, 900 each), three margins
// (rects of four ints), the description's box (its width, 970, wraps the text), and the text
// styles of the title and the description (a label's four: idle, focused and two others). Shrunk
// tiles get them all scaled, and smaller styles, else their text runs off the tile.
using Margin = std::array<std::int32_t, 4>;
constexpr std::uint32_t tile_icon_width = 0x495ffd43, tile_icon_height = 0x24fb5ca8, anchored_box = 0x1cff7243;
constexpr std::array<std::uint32_t, 3> tile_margins{0x6257a113, 0x39c25d63, 0xb1e77a9e};
constexpr std::array<std::uint32_t, 4> label_styles{0xf94d8cc6, 0xa1e84eb1, 0xfc23a999, 0x25189231};
// Each style the tiles use, and the smaller ones for shrunk tiles, the first that is loaded.
constexpr std::array<std::pair<std::string_view, std::array<std::string_view, 3>>, 5> smaller_styles{{
    {"TextStylesList/H96-XBold-Caps_White", {"TextStylesList/H72-XBold-Caps_White", "TextStylesList/H64-XBold-Caps_White", ""}},
    {"TextStylesList/H96-XBold-Caps_Black",
     {"TextStylesList/H72-XBold-Caps_Black", "TextStylesList/H72-XBold-Caps_DarkestDark", "TextStylesList/H64-XBold-Caps_Black"}},
    {"TextStylesList/B56-SemiB_White", {"TextStylesList/SP48-SemiB_White", "", ""}},
    {"TextStylesList/B56-SemiB_Black", {"TextStylesList/SP48-SemiB_Black", "", ""}},
    {"TextStylesList/B52-SemiB_Black", {"TextStylesList/SP48-SemiB_Black", "", ""}},
}};
struct Layout {
    Size icon{}, box{};
    std::array<Margin, 3> margins{};
    std::array<Ref, 4> title{}, description{};
};
constexpr std::string_view mode_select = "Throwdown_ModeSelect_ContentResources/Throwdown_ModeSelect_";
constexpr std::string_view template_name = "Throwdown_ModeSelect_ContentResources/Throwdown_ModeSelect_SpotBattle_CategoryTile";
constexpr std::string_view jam_suffix = "_Jam_CategoryTile";
// The tile's icon, the first one loaded (else Spot Battle's stays).
constexpr std::array<const char*, 2> icons{
    "UI/Textures/Icons/Activities/Challenges/LocationIndication/img_LocationIndication_FlameMarker_Color_Full_256",
    "UI/Textures/Icons/Activities/Challenges/Icon/img_Icon_SpeedlineChallenge_256"};
constexpr std::uint64_t model_id = 0x5244524354494c45ULL; // "RDRCTILE"

// A tile's data, its description presenter and label; ours and the one lent to the Jam row.
struct TileModels { Value tile, anchored, label; };
struct Shown { std::string title, description; }; // as last published
struct Models { Value row; TileModels own, lent; };
struct Entry { Ref ref; Value row; Widget content; std::string name; };
struct State {
    std::atomic<ULONGLONG> scan_until{};
    Address manager{};
    Value list;   // the mode select's list while it is open
    Models ours;  // the tile's models
    std::uint64_t next_id{};
    ULONGLONG gone_since{}, next_scan{}, next_error{};
    Shown own_shown, lent_shown;
    ULONGLONG next_dress{}, next_timer{};
    std::atomic<int> shown_action{-1}, shown_race{-1};
    // The Skate Jam row while it shows our tile, and its own content to give back.
    Value jam;
    Widget jam_content{};
    bool lent{};
    std::int32_t focus = -2, entry = -2;
    std::optional<Size> original; // a native tile's size before ours made them shrink
    Size fitted{};
    // Each tile's layout as shipped (by its data's handle), and the scale it was last given.
    std::map<Handle, Layout> layouts;
    std::map<Handle, float> scaled;
    std::map<Address, Address> smaller; // a style's record -> its smaller one (itself when none)
    bool styles_reported{};
    unsigned focus_logs{};
    // The button's native void-action descriptor: the game borrows it for the process lifetime.
    std::array<Address, 0x70 / sizeof(Address)> descriptor{};
    Address info{};
};
State& state() {
    static auto* value = new State;
    return *value;
}
void say(const std::string& text) { logging::write(logging::Level::info, logging::Channel::ui, text); }

// The game's UI runs this when our own tile is pressed.
void pressed() noexcept {
    auto& s = state();
    const auto action = s.shown_action.load(std::memory_order_acquire);
    if (action < 0) return;
    death_race::tile_pressed(static_cast<death_race::TileAction>(action), s.shown_race.load(std::memory_order_acquire));
}
Address delegate(const Context& c) {
    auto& s = state();
    if (!s.info) {
        const auto prototype = c.base + addr::native_menu::action_prototype;
        require(read<Address>(prototype + 0x40) == c.base + addr::native_menu::action_invoker && read<std::uint8_t>(prototype + 0x68) == 0,
                "Native action ABI differs.");
        require(memory::read_bytes(prototype, s.descriptor.data(), sizeof(s.descriptor)), "Native action template is unavailable.");
        const auto function = reinterpret_cast<Address>(&pressed);
        for (const auto offset : {0x30, 0x40, 0x48}) s.descriptor[offset / sizeof(Address)] = function;
        s.info = reinterpret_cast<Address>(s.descriptor.data());
    }
    return reinterpret_cast<Address>(&s.info);
}

std::string record_name(Address record) { return record ? string(read<Address>(record + 0x28), 160) : std::string{}; }
bool alive(const Context& c, Value value) { return value.handle && c.type_of(value.handle) == value.type; }
Value action_of(const Context& c, Value row) {
    return c.path(row, {row_presenter, row_button, 0xa704272a, 0xc52416ef, 0xa704272a, 0xa2b71c93});
}
std::vector<Entry> entries(const Context& c, Value items) {
    unsigned count{}, stride{};
    const auto bytes = c.array(items, 16, count, stride);
    require(stride == sizeof(Ref), "Throwdown mode list differs.");
    const auto row_type = c.type(row_schema);
    std::vector<Entry> result;
    for (unsigned i = 0; i < count; ++i) {
        Entry e;
        std::memcpy(&e.ref, bytes.data() + std::size_t{i} * stride, sizeof(Ref));
        if (e.ref.handle && c.type_of(e.ref.handle) == row_type) {
            e.row = {e.ref.handle, row_type};
            e.content = read<Widget>(c.address(c.path(e.row, {row_presenter, row_content})));
            e.name = record_name(e.content.data.record);
        }
        result.push_back(std::move(e));
    }
    return result;
}
bool is_mode_select(const std::vector<Entry>& list) {
    return std::count_if(list.begin(), list.end(), [](const Entry& e) { return e.name.starts_with(mode_select); }) >= 2;
}

// The Skate Jam row gets its own tile back.
void give_back(const Context& c) {
    auto& s = state();
    if (s.lent && alive(c, s.jam)) {
        const auto field = c.path(s.jam, {row_presenter, row_content});
        if (read<Widget>(c.address(field)).data.handle == s.ours.lent.tile.handle) c.set(field, s.jam_content);
    }
    s.lent = false;
}
void destroy(const Context& c, Models& m) {
    give_back(c);
    for (auto* value : {&m.row, &m.own.tile, &m.own.anchored, &m.own.label, &m.lent.tile, &m.lent.anchored, &m.lent.label}) {
        try { c.destroy(*value); } catch (...) {}
        *value = {};
    }
}
// ---- a tile's layout ------------------------------------------------------------------------
// Its description presenter and label.
Value description_of(const Context& c, Value tile) {
    return {read<Widget>(c.address(c.field(tile, tile_description))).data.handle, c.type(anchored_schema)};
}
Value label_of(const Context& c, Value anchored) {
    return {read<Widget>(c.address(c.field(anchored, anchored_content))).data.handle, c.type(label_schema)};
}
Layout layout_of(const Context& c, Value tile) {
    Layout l;
    l.icon = {read<float>(c.address(c.field(tile, tile_icon_width))), read<float>(c.address(c.field(tile, tile_icon_height)))};
    for (std::size_t i = 0; i < tile_margins.size(); ++i) l.margins[i] = read<Margin>(c.address(c.field(tile, tile_margins[i])));
    const auto anchored = description_of(c, tile);
    require(alive(c, anchored), "Throwdown tile description differs.");
    l.box = read<Size>(c.address(c.field(anchored, anchored_box)));
    const auto label = label_of(c, anchored);
    require(alive(c, label), "Throwdown tile label differs.");
    const auto title = c.field(tile, tile_title);
    for (std::size_t i = 0; i < label_styles.size(); ++i) {
        l.title[i] = read<Ref>(c.address(c.field(title, label_styles[i])));
        l.description[i] = read<Ref>(c.address(c.field(label, label_styles[i])));
    }
    return l;
}
// The smaller style for `record` (itself when there is none loaded), looked up once.
Address smaller_style(const Context& c, Address record, Address anchor) {
    auto& s = state();
    if (!record) return record;
    if (const auto known = s.smaller.find(record); known != s.smaller.end()) return known->second;
    Address found = record;
    const auto name = record_name(record);
    for (const auto& [from, to] : smaller_styles) {
        if (name != from) continue;
        for (const auto candidate : to) {
            if (candidate.empty()) continue;
            const auto asset = named_asset(c, anchor, std::string(candidate).c_str());
            if (asset && read<Address>(asset + 8) == c.base + addr::native_menu::asset_record_type && record_name(asset) == candidate) {
                found = asset;
                break;
            }
        }
        if (found == record && !s.styles_reported) {
            s.styles_reported = true;
            say(std::format("Death Race tile: no smaller text style loaded for {}; the shrunk tiles keep it.", name));
        }
    }
    s.smaller.emplace(record, found);
    return found;
}
// `tile` laid out at `k` times its shipped size (`l`), smaller styles below the shipped size.
template <class T> void set_if(const Context& c, Value field, const T& value) {
    if (read<T>(c.address(field)) != value) c.set(field, value);
}
void lay_out(const Context& c, Value tile, const Layout& l, float k) {
    set_if(c, c.field(tile, tile_icon_width), l.icon[0] * k);
    set_if(c, c.field(tile, tile_icon_height), l.icon[1] * k);
    for (std::size_t i = 0; i < tile_margins.size(); ++i) {
        Margin m{};
        for (std::size_t j = 0; j < m.size(); ++j) m[j] = static_cast<std::int32_t>(std::lround(l.margins[i][j] * k));
        set_if(c, c.field(tile, tile_margins[i]), m);
    }
    const auto anchored = description_of(c, tile);
    const auto label = label_of(c, anchored);
    if (!alive(c, anchored) || !alive(c, label)) return;
    set_if(c, c.field(anchored, anchored_box), Size{l.box[0] * k, l.box[1]});
    const auto anchor = read<Address>(c.address(c.field(tile, tile_icon)));
    const auto title = c.field(tile, tile_title);
    for (std::size_t i = 0; i < label_styles.size(); ++i) {
        for (const auto& [field, style] : {std::pair{c.field(title, label_styles[i]), l.title[i]},
                                           std::pair{c.field(label, label_styles[i]), l.description[i]}}) {
            Ref want = style;
            if (k < 0.999f) want.record = smaller_style(c, style.record, anchor);
            const auto now = read<Ref>(c.address(field));
            if (now.record != want.record || now.handle != want.handle) c.set(field, want);
        }
    }
}
// Every tile known back as shipped (the page closing, or no longer four tiles).
void restore_layouts(const Context& c) {
    auto& s = state();
    const auto type = c.type(tile_schema);
    for (const auto& [handle, layout] : s.layouts) {
        const auto scaled = s.scaled.find(handle);
        if (scaled == s.scaled.end() || scaled->second == 1.0f) continue;
        try {
            const Value tile{handle, type};
            if (alive(c, tile)) lay_out(c, tile, layout, 1.0f);
        } catch (...) {}
    }
    s.layouts.clear();
    s.scaled.clear();
}
// A copy of a tile that is shrunk takes the shipped layout, and is known by it.
void restore_copy(const Context& c, Value source, Value copy) {
    auto& s = state();
    const auto known = s.layouts.find(source.handle);
    if (known == s.layouts.end()) return;
    lay_out(c, copy, known->second, 1.0f);
    s.layouts[copy.handle] = known->second;
    s.scaled[copy.handle] = 1.0f;
}

// A copy of a native tile's data, description presenter and label.
void make_tile(const Context& c, const Entry& from, TileModels& m) {
    auto& s = state();
    const auto make = [&](Schema schema) { return c.create(schema, model_id + s.next_id++); };
    const Value source_tile{from.content.data.handle, c.type(tile_schema)};
    require(alive(c, source_tile), "Spot Battle tile data differs.");
    const auto description = read<Widget>(c.address(c.field(source_tile, tile_description)));
    const Value source_anchored{description.data.handle, c.type(anchored_schema)};
    require(alive(c, source_anchored), "Spot Battle tile description differs.");
    const auto label = read<Widget>(c.address(c.field(source_anchored, anchored_content)));
    const Value source_label{label.data.handle, c.type(label_schema)};
    require(alive(c, source_label), "Spot Battle tile label differs.");
    m.label = make(label_schema);
    c.copy(m.label, c.address(source_label));
    m.anchored = make(anchored_schema);
    c.copy(m.anchored, c.address(source_anchored));
    c.set(c.field(m.anchored, anchored_content), Widget{label.blueprint, {label.data.record, m.label.handle}});
    m.tile = make(tile_schema);
    c.copy(m.tile, c.address(source_tile));
    c.set(c.field(m.tile, tile_description), Widget{description.blueprint, {description.data.record, m.anchored.handle}});
    restore_copy(c, source_tile, m.tile);
    const auto icon = c.field(m.tile, tile_icon);
    const auto anchor = read<Address>(c.address(icon));
    for (const auto* name : icons) {
        if (const auto asset = named_asset(c, anchor, name)) {
            c.set(icon, asset);
            c.set(c.field(m.tile, tile_icon_focused), asset);
            break;
        }
    }
}
Models build(const Context& c, const Entry& from) {
    auto& s = state();
    Models m;
    try {
        make_tile(c, from, m.own);
        make_tile(c, from, m.lent);
        m.row = c.create(row_schema, model_id + s.next_id++);
        c.copy(m.row, c.address(from.row));
        c.set(c.path(m.row, {row_presenter, row_content}), Widget{from.content.blueprint, {from.content.data.record, m.own.tile.handle}});
        const auto action = action_of(c, m.row);
        c.text(c.field(action, action_navigation), "");
        c.set(c.field(action, action_delegate), delegate(c));
    } catch (...) {
        destroy(c, m);
        throw;
    }
    s.own_shown = s.lent_shown = {"\x01", "\x01"}; // everything published on the first show
    return m;
}
void show(const Context& c, const TileModels& m, Shown& shown, const death_race::Tile& tile) {
    if (tile.title != shown.title) {
        c.text(c.path(m.tile, {tile_title, label_text}), tile.title);
        shown.title = tile.title;
    }
    if (tile.description != shown.description) {
        c.text(c.field(m.label, label_text), tile.description);
        shown.description = tile.description;
    }
}
// Every tile at the size that puts them all where the native three were.
void fit(const Context& c, Value items) {
    auto& s = state();
    const auto list = entries(c, items);
    std::vector<Value> rows;
    std::optional<Size> native;
    for (const auto& e : list) {
        if (!e.row.handle) continue;
        rows.push_back(e.row);
        if (!native && e.name.starts_with(mode_select) && e.ref.handle != s.ours.row.handle)
            native = read<Size>(c.address(c.field(e.row, row_size)));
    }
    if (!native || (rows.size() <= 3 && !s.original)) return;
    // A fresh native row (the page reopened) still has the shipped size.
    if (!s.original || *native != s.fitted) s.original = *native;
    const float k = rows.size() > 3 ? 3.0f / static_cast<float>(rows.size()) : 1.0f;
    s.fitted = {(*s.original)[0] * k, (*s.original)[1] * k};
    for (const auto row : rows) {
        const auto field = c.field(row, row_size);
        if (read<Size>(c.address(field)) != s.fitted) c.set(field, s.fitted);
    }
    // What is inside each tile too.
    const auto tile_type = c.type(tile_schema);
    for (const auto& e : list) {
        const Value tile{e.content.data.handle, tile_type};
        if (!e.row.handle || !alive(c, tile)) continue;
        auto known = s.layouts.find(tile.handle);
        if (known == s.layouts.end()) {
            if (k == 1.0f) continue; // as shipped
            known = s.layouts.emplace(tile.handle, layout_of(c, tile)).first;
        }
        if (const auto at = s.scaled.find(tile.handle); at != s.scaled.end() && at->second == k) continue;
        lay_out(c, tile, known->second, k);
        s.scaled[tile.handle] = k;
    }
}
void ensure(const Context& c) {
    auto& s = state();
    const auto items = c.field(s.list, list_items);
    const auto list = entries(c, items);
    const Entry* from{};
    const Entry* jam{};
    for (const auto& e : list) {
        if (e.name == template_name) from = &e;
        if (e.name.ends_with(jam_suffix)) jam = &e;
    }
    if (!from)
        for (const auto& e : list)
            if (e.name.starts_with(mode_select) && e.content.data.handle != s.ours.own.tile.handle &&
                e.content.data.handle != s.ours.lent.tile.handle) { from = &e; break; }
    if (!from) return; // the game is filling it again
    const auto& o = s.ours;
    if (!alive(c, o.row) || !alive(c, o.own.tile) || !alive(c, o.own.anchored) || !alive(c, o.own.label) || !alive(c, o.lent.tile) ||
        !alive(c, o.lent.anchored) || !alive(c, o.lent.label)) {
        destroy(c, s.ours);
        s.ours = build(c, *from);
    }
    const auto tile = death_race::tile();
    // Opening a race: the Skate Jam row shows the race's tile, and ours stays out.
    const bool lend = tile.action == death_race::TileAction::open && jam;
    show(c, lend ? s.ours.lent : s.ours.own, lend ? s.lent_shown : s.own_shown, tile);
    s.shown_race.store(tile.race, std::memory_order_release);
    s.shown_action.store(lend ? -1 : static_cast<int>(tile.action), std::memory_order_release);
    if (jam && (!s.jam.handle || s.jam.handle != jam->row.handle)) {
        s.jam = jam->row;
        s.lent = false;
    }
    int jam_index = -1;
    std::vector<Ref> wanted;
    for (std::size_t i = 0; i < list.size(); ++i) {
        if (list[i].ref.handle == s.ours.row.handle) continue;
        if (jam && list[i].ref.handle == jam->ref.handle) jam_index = static_cast<int>(wanted.size());
        wanted.push_back(list[i].ref);
    }
    if (!lend) wanted.push_back(Ref{0, s.ours.row.handle});
    if (jam) {
        const auto field = c.path(jam->row, {row_presenter, row_content});
        const auto current = read<Widget>(c.address(field));
        if (lend && current.data.handle != s.ours.lent.tile.handle) {
            s.jam_content = current;
            c.set(field, Widget{current.blueprint, {current.data.record, s.ours.lent.tile.handle}});
            s.lent = true;
            say("Death Race tile: the Skate Jam tile opens the race here.");
        } else if (!lend && s.lent) {
            give_back(c);
        }
    }
    bool same = wanted.size() == list.size();
    for (std::size_t i = 0; same && i < wanted.size(); ++i)
        same = wanted[i].handle == list[i].ref.handle && wanted[i].record == list[i].ref.record;
    if (!same) {
        c.array(items, wanted);
        std::string names;
        for (const auto& e : list)
            if (e.ref.handle != s.ours.row.handle) names += (names.empty() ? "" : ", ") + (e.name.empty() ? std::string("?") : e.name.substr(e.name.rfind('/') + 1));
        say(std::format("Death Race tile after the throwdown modes ({}).", names));
    }
    fit(c, items);
    const auto focus = read<std::int32_t>(c.address(c.field(s.list, list_focus)));
    const auto entry = read<std::int32_t>(c.address(c.field(s.list, list_entry)));
    if ((focus != s.focus || entry != s.entry) && s.focus_logs < 64) {
        ++s.focus_logs;
        say(std::format("Throwdown modes: list index fields {} / {}.", focus, entry));
    }
    s.focus = focus;
    s.entry = entry;
    // On the Skate Jam tile while it opens a race: a Skate Jam started from here is that race's.
    if (lend && focus >= 0 && focus == jam_index) death_race::tile_focused(tile.race);
}
// The throwdown setup page: its header (ActivityDisplay: subtitle, title) and its settings, inline
// items of LinearLists, each a SettingWrapper whose data is the setting (its label names it, its
// presenter's data is the control: a BasicSelector, or a Slider for the players).
constexpr Schema setting_schema{0x94803e85, 176}, slider_schema{0xcdc749b2, 840}, header_schema{0x4efe5e19, 312};
constexpr std::uint32_t inline_items = 0x345b75b1, setting_label = 0x58c9d354, setting_control = 0x716496c8, item_content = 0x214d4984;
constexpr std::uint32_t header_title = 0x00f3b15e;
// A slider: its value (0..1 across its range), range (min, max) and steps (count, and one past the end).
constexpr std::uint32_t slider_value = 0x5a547e87, slider_range = 0x170bebf7, slider_steps = 0x1c1fc8d9, steps_count = 0xb107fa2b,
    steps_end = 0x42c5397e;
constexpr std::string_view players_label = "ID_SETTINGS_PLAYERS", riders_label = "AI opponents", race_title = "Death Race";
constexpr int most_riders = 4;
using Range = std::array<float, 2>;
// The players slider, as the race's AI riders: 0 to 4. Its value is the race's.
void dress_slider(const Context& c, Value setting) {
    const auto label = c.path(setting, {setting_label, label_text});
    if (c.text(label, 64) != riders_label) c.text(label, std::string(riders_label));
    const auto control = read<Widget>(c.address(c.field(setting, setting_control)));
    const Value slider{control.data.handle, c.type(slider_schema)};
    if (!alive(c, slider)) return;
    const auto range = c.field(slider, slider_range);
    const auto value = c.field(slider, slider_value);
    if (read<Range>(c.address(range)) != Range{0.0f, static_cast<float>(most_riders)}) {
        c.set(range, Range{0.0f, static_cast<float>(most_riders)});
        c.set(c.path(slider, {slider_steps, steps_count}), std::int32_t{most_riders});
        c.set(c.path(slider, {slider_steps, steps_end}), static_cast<float>(most_riders + 1));
        c.set(value, static_cast<float>(death_race::riders()) / most_riders);
        say(std::format("Death Race setup: the players slider is the AI riders ({}).", death_race::riders()));
        return;
    }
    const auto riders = std::clamp(static_cast<int>(std::lround(read<float>(c.address(value)) * most_riders)), 0, most_riders);
    if (riders != death_race::riders()) {
        death_race::set_riders(riders);
        say(std::format("Death Race setup: {} AI rider(s).", riders));
    }
}
// The setup page's buttons (Activities_Details_Page): a LinearList of button models, Customize
// (its action navigates to AdvancedParameters, every other setting) and Confirm
// (ThrowdownPlacement). Customize goes: the race has nothing more to set.
constexpr std::string_view customize_navigation = "AdvancedParameters";
void drop_customize(const Context& c, Value list) {
    const auto items = c.field(list, list_items);
    unsigned count{}, stride{};
    const auto bytes = c.array(items, 8, count, stride);
    if (!count || stride != sizeof(Ref)) return;
    const auto button_type = c.type(button);
    std::vector<Ref> kept;
    bool dropped{};
    for (unsigned i = 0; i < count; ++i) {
        Ref ref{};
        std::memcpy(&ref, bytes.data() + std::size_t{i} * stride, sizeof(ref));
        if (ref.handle && c.type_of(ref.handle) == button_type) {
            const Value model{ref.handle, button_type};
            const auto navigation = c.text(c.field(c.path(model, {0x0fb0d794, 0xa704272a, 0xc52416ef, 0xa704272a, 0xa2b71c93}),
                                                   action_navigation), 64);
            if (navigation == customize_navigation) {
                dropped = true;
                continue;
            }
        }
        kept.push_back(ref);
    }
    if (!dropped) return;
    c.array(items, kept);
    say("Death Race setup: took out the Customize button.");
}
void dress_setup(const Context& c) {
    const auto setting_type = c.type(setting_schema), header_type = c.type(header_schema), list_type = c.type(linear_list);
    for (const auto& root : c.roots({header_schema.hash, linear_list.hash})) {
        try {
            if (root.model.type == header_type) {
                // The Skate Jam's title (the page writes the mode's name in).
                const auto title = c.path(root.model, {header_title, label_text});
                auto text = c.text(title, 64);
                std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                if (text.find("jam") != std::string::npos) c.text(title, std::string(race_title));
                continue;
            }
            if (root.model.type != list_type) continue;
            drop_customize(c, root.model);
            const auto items = c.field(root.model, inline_items);
            unsigned count{}, stride{};
            const auto bytes = c.array(items, 32, count, stride);
            if (!count) continue;
            std::vector<std::byte> kept;
            std::vector<Value> riders;
            bool settings{};
            std::string names;
            for (unsigned i = 0; i < count; ++i) {
                const auto content = read<Widget>(c.address(c.field(c.element(items, i), item_content)));
                std::string label;
                if (content.data.handle && c.type_of(content.data.handle) == setting_type)
                    label = c.text(c.path(Value{content.data.handle, setting_type}, {setting_label, label_text}), 64);
                settings = settings || label.starts_with("ID_THROWDOWN_") || label == riders_label;
                if (label == players_label || label == riders_label) {
                    riders.push_back({content.data.handle, setting_type});
                    kept.insert(kept.end(), bytes.begin() + std::size_t{i} * stride, bytes.begin() + std::size_t{i + 1} * stride);
                } else {
                    names += (names.empty() ? "" : ", ") + label;
                }
            }
            // Only the setup page's lists: one of its settings is a throwdown's.
            if (!settings) continue;
            if (kept.size() != bytes.size()) {
                c.array(items, kept, static_cast<unsigned>(kept.size() / stride));
                say("Death Race setup: took out " + names + ".");
            }
            for (const auto setting : riders) dress_slider(c, setting);
        } catch (...) {} // another list
    }
}
// The throwdown's timer on the HUD: a stack (0xd71ee0a1) whose one item presents a model
// (0x1955583c) whose own stack's one item presents the countdown (0x6a468d43, its text at
// 0x168f5425, "4m 54s"). The outer stack's item goes while the race runs.
constexpr Schema hud_stack{0xd71ee0a1, 80}, timer_group{0x1955583c, 80}, countdown{0x6a468d43, 528};
constexpr std::uint32_t stack_items = 0x61742cb4, stack_presenter = 0x716496c8, group_stack = 0x17d7a1a7, countdown_text = 0x168f5425;
// The stacks emptied of the race's timer (to look for each once only).
struct HiddenTimer {
    Value stack;
};
std::vector<HiddenTimer>& hidden_timers() {
    static auto* value = new std::vector<HiddenTimer>;
    return *value;
}
// The model a stack's first item presents, when it is of `schema`.
Value first_presented(const Context& c, Value stack, Schema schema) {
    unsigned count{}, stride{};
    (void)c.array(c.field(stack, stack_items), 4, count, stride);
    if (count < 1) return {};
    const auto widget = read<Widget>(c.address(c.field(c.element(c.field(stack, stack_items), 0), stack_presenter)));
    const auto type = c.type(schema);
    if (!widget.data.handle || c.type_of(widget.data.handle) != type) return {};
    return {widget.data.handle, type};
}
void hide_race_timer(const Context& c) {
    auto& hidden = hidden_timers();
    const auto stack_type = c.type(hud_stack);
    for (const auto& root : c.roots({hud_stack.hash})) {
        try {
            if (root.model.type != stack_type) continue;
            const auto group = first_presented(c, root.model, timer_group);
            if (!group.handle) continue;
            const auto clock = first_presented(c, c.field(group, group_stack), countdown);
            if (!clock.handle) continue;
            const auto text = c.text(c.field(clock, countdown_text), 32);
            if (text.find('m') == std::string::npos && text.find('s') == std::string::npos) continue;
            const auto items = c.field(root.model, stack_items);
            unsigned count{}, stride{};
            (void)c.array(items, 4, count, stride);
            if (!count) continue;
            hidden.push_back({root.model});
            c.array(items, std::span<const std::byte>{}, 0);
            say(std::format("Death Race: the throwdown's timer ({}) is off the screen.", text));
        } catch (...) {} // another stack
    }
}
// The race over: what was taken out stays out. The bytes kept are no items any more (taking them
// out released what they held), so they are never published again; each throwdown's HUD is made
// anew with its own timer.
void show_race_timer(const Context&) { hidden_timers().clear(); }
} // namespace

void throwdown_mode_select_opened() noexcept {
    death_race::throwdown_modes_opened();
    state().scan_until.store(GetTickCount64() + 3000, std::memory_order_relaxed);
}

void tick_throwdown_tile(std::uintptr_t base) noexcept {
    auto& s = state();
    const auto now = GetTickCount64();
    const bool scanning = now < s.scan_until.load(std::memory_order_relaxed);
    const bool setting_up = death_race::setting_up();
    const bool racing = death_race::race_on();
    if (!scanning && !s.list.handle && !s.ours.row.handle && !setting_up && !racing && hidden_timers().empty()) return;
    try {
        const auto ui = read<Address>(base + engine::ui_manager);
        const auto manager = ui ? read<Address>(ui + 0x140) : 0;
        if (!manager || !game::native_data().models.create) return;
        if (s.manager && s.manager != manager) {
            // Another manager: the old one's handles went with it.
            s.list = {};
            s.ours = {};
            s.jam = {};
            s.lent = false;
            s.layouts.clear();
            s.scaled.clear();
            s.smaller.clear();
        }
        s.manager = manager;
        const Context c(base, manager);
        game::ModelWriteLock lock(manager);
        if (s.list.handle && !alive(c, s.list)) {
            s.list = {};
            s.gone_since = now;
            give_back(c); // should the game keep its rows for the next time
            restore_layouts(c);
        }
        if (setting_up && now >= s.next_dress) {
            s.next_dress = now + 150;
            dress_setup(c);
        }
        // The race's timer off the screen while it runs (looked for again: the HUD fills in late).
        if (racing && now >= s.next_timer) {
            s.next_timer = now + 250;
            hide_race_timer(c);
        } else if (!racing && !hidden_timers().empty()) {
            show_race_timer(c);
        }
        if (!s.list.handle && scanning && now >= s.next_scan) {
            s.next_scan = now + 100;
            for (const auto& root : c.roots({linear_list.hash})) {
                try {
                    if (!is_mode_select(entries(c, c.field(root.model, list_items)))) continue;
                } catch (...) {
                    continue; // another list: longer, or of other things
                }
                s.list = root.model;
                s.focus = s.entry = -2;
                break;
            }
        }
        if (!s.list.handle) {
            // The page closed and its widgets went with it: ours go a moment later.
            if (s.ours.row.handle && now - s.gone_since > 1500) destroy(c, s.ours);
            return;
        }
        ensure(c);
    } catch (const std::exception& e) {
        s.list = {};
        s.gone_since = now;
        s.next_scan = now + 1000;
        if (now >= s.next_error) {
            s.next_error = now + 10000;
            logging::write(logging::Level::warning, logging::Channel::ui, std::string("Death Race tile: ") + e.what());
        }
    } catch (...) {
        s.list = {};
        s.gone_since = now;
        s.next_scan = now + 1000;
    }
}

void release_throwdown_tile(std::uintptr_t base) noexcept {
    auto& s = state();
    s.shown_action.store(-1, std::memory_order_release);
    if (!s.ours.row.handle && !s.list.handle) return;
    try {
        const auto ui = read<Address>(base + engine::ui_manager);
        const auto manager = ui ? read<Address>(ui + 0x140) : 0;
        if (!manager || manager != s.manager) {
            s.list = {};
            s.ours = {};
            s.jam = {};
            s.lent = false;
            s.layouts.clear();
            s.scaled.clear();
            s.smaller.clear();
            return;
        }
        const Context c(base, manager);
        game::ModelWriteLock lock(manager);
        give_back(c);
        restore_layouts(c);
        if (alive(c, s.list) && s.ours.row.handle) {
            const auto items = c.field(s.list, list_items);
            unsigned count{}, stride{};
            const auto bytes = c.array(items, 16, count, stride);
            std::vector<Ref> kept;
            for (unsigned i = 0; i < count; ++i) {
                Ref ref{};
                std::memcpy(&ref, bytes.data() + std::size_t{i} * stride, sizeof(ref));
                if (ref.handle != s.ours.row.handle) kept.push_back(ref);
            }
            if (kept.size() != count) c.array(items, kept);
        }
        destroy(c, s.ours);
    } catch (...) {}
    s.list = {};
    s.ours = {};
    s.jam = {};
    s.lent = false;
}
} // namespace dingosdk::multiplayer
