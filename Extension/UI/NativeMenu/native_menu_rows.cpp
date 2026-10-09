#include "native_menu_internal.h"
#include <stdexcept>

namespace dingosdk::multiplayer {
using namespace menu_data;
using namespace native_menu_detail;
namespace {
Value primary_action(const Context& context, Value row) {
    // CoreButton.Interactable -> native interact handler -> primary action.
    return context.path(row, {button_field, 0xa704272a, 0xc52416ef, 0xa704272a, 0xa2b71c93, 0x9c76b86c});
}
Value string_field(const Context& context, Value row) { return context.path(row, {label, text_field}); }
Row& row(const Context& context, const std::string& id, RowKind kind = RowKind::button) {
    auto& s = state();
    if (const auto existing = s.rows.find(id); existing != s.rows.end()) return existing->second;
    Row next{make(context, kind == RowKind::input ? input : kind == RowKind::text ? Schema{0xbfe23948, 104} : button),
        make(context, {0x0e3be640, 192}), kind, {}};
    next.width = s.row_width;
    next.height = kind == RowKind::input ? 216.f : kind == RowKind::text ? 60.f : 168.f;
    const Widget widget{blueprint(kind == RowKind::input ? "UI/Foundations/Components/Text/InputField/InputField_Widget" :
        kind == RowKind::text ? "UI/Foundations/Components/Text/Label/Widget/Label_Widget" :
        "UI/Foundations/Components/Buttons/LabelButton/LabelButton_Widget"), {0, next.model.handle}};
    context.set(context.field(next.presenter, 0x214d4984), widget);
    // Reserve room for padding and focus borders. ButtonVerticalLayout stretches
    // the native button across this explicit height instead of its text height.
    context.set(context.field(next.presenter, 0x1cff7243),
                std::array<float, 2>{next.width, next.height});
    context.set(context.field(next.presenter, 0x6efc1a61), false); // Fit width.
    context.set(context.field(next.presenter, 0xbe5683d9), false);
    context.set(context.field(next.presenter, 0x144aee01), kind != RowKind::text); // IsFocusable.
    if (kind != RowKind::input) {
        const auto text_model = kind == RowKind::text ? next.model : context.field(next.model, label);
        context.set(context.field(text_model, 0x42924a4), true); // Treat user names as plain text.
        context.set(context.field(text_model, 0xfc53d427), false);
    }
    if (kind == RowKind::button) {
        // Focus is independent of action availability. Even an unavailable
        // button stays navigable, with an explicitly empty activation delegate.
        context.set(primary_action(context, next.model), Address{});
        const auto text_model = context.field(next.model, label);
        const auto black = next.dark_text_style = read<Ref>(context.address(context.field(text_model, 0xf94d8cc6)));
        context.set(context.field(text_model, 0xf94d8cc6), s.white_text_style);
        context.set(context.field(text_model, 0x25189231), s.white_text_style);
        context.set(context.field(text_model, 0xa1e84eb1), black);
        context.set(context.field(next.model, 0x659db23e), false); // Keep the authored label styles.
        context.set(context.field(text_model, 0x3d8639d0), 0); // Left aligned.
        context.set(context.field(next.model, 0xf9515817),
                    std::array<float, 6>{0.f, 0.f, 0.f, 0.5f, 1.f, 0.f});
        context.set(context.path(next.model, {button_field, 0x8cc042ef}), Ref{0, s.menu_style.handle});
    }
    if (kind == RowKind::text) {
        // A standalone Label defaults to the black button font. Reuse its
        // native white style without changing the shared font resource.
        const auto white = context.address(context.field(next.model, 0xa1e84eb1));
        s.white_text_style = read<Ref>(white);
        for (const auto hash : {0xf94d8cc6U, 0xfc23a999U, 0x25189231U})
            context.copy(context.field(next.model, hash), white);
        context.set(context.field(next.model, 0xcd71a279), false); // isFocusReactive.
        context.set(context.field(next.model, 0x3fa0b887), false); // isDisabledReactive.
        context.set(context.field(next.model, 0xfc53d427), false); // FitWidthToContent.
    }
    if (kind == RowKind::input) {
        const auto style = make(context, {0x97b23691, 96});
        const auto inherited = read<Ref>(context.address(context.field(next.model, 0x8cc042ef)));
        if (inherited.record) {
            require(read<Address>(inherited.record + 0x18) == style.type, "Native input style differs.");
            context.copy(style, read<Address>(inherited.record + 0x20));
        }
        const bool password = id == "host-password" || id == "join-password";
        context.set(context.field(style, 0x919e3190), id == "card-name" ? 32 : password ? 64 : 128); // MaxChars.
        context.set(context.field(style, 0x6cb56b65), false); // AllowReturn.
        context.set(context.field(style, 0x33fe05ad), false); // AllowTabs.
        context.set(context.field(style, 0xafe37e0), 1); // MinVisibleLines.
        context.set(context.field(style, 0x33d47c0), 1); // MaxVisibleLines.
        context.set(context.field(style, 0x514c3d0d), false); // WordWrap.
        context.set(context.field(style, 0x1f7c7845), 44.f); // Single-line input height.
        context.set(context.field(style, 0xa45380e6), password ? 1 : 0); // SecurityMode.
        context.set(context.field(style, 0x868002f7), 0); // CharacterRestriction: unrestricted.
        context.set(context.field(next.model, 0x8cc042ef), Ref{0, style.handle});
        context.set(context.field(next.model, 0xc80d0b44), false); // ShouldModerate: local data, no service request.
        context.set(context.field(next.model, 0xce3f32e1), false);
    }
    return s.rows.emplace(id, std::move(next)).first->second;
}
void retain_style(const Context& context, Value target, Ref desired) {
    const auto current = read<Ref>(context.address(target));
    if (current.record != desired.record || current.handle != desired.handle) context.set(target, desired);
}
void style_row(const Context& context, Row& value) {
    if (value.kind == RowKind::input) return;
    const auto text_model = value.kind == RowKind::text ? value.model : context.field(value.model, label);
    const auto style = value.primary ? value.dark_text_style : state().white_text_style;
    // Native widgets initialize these fields after mounting. Reconcile only
    // changed references so constructor defaults cannot restore black labels.
    for (const auto hash : {0xf94d8cc6U, 0xa1e84eb1U, 0xfc23a999U, 0x25189231U})
        retain_style(context, context.field(text_model, hash), style);
    if (value.kind == RowKind::button)
        retain_style(context, context.path(value.model, {button_field, 0x8cc042ef}),
            Ref{0, value.primary ? state().primary_style.handle : state().menu_style.handle});
}
void publish_fixed(const Context& context, Value panel, unsigned slot, const std::vector<std::string>& visible) {
    auto& s = state();
    auto& nodes = s.fixed_rows[slot];
    if (nodes.empty()) nodes.push_back(panel);
    if (s.displayed[slot] == visible) return;
    while (nodes.size() < visible.size()) nodes.push_back(make(context, vertical_panel));
    for (std::size_t i = 0; i < visible.size(); ++i) {
        const auto& value = s.rows.at(visible[i]);
        const auto partition = context.field(nodes[i], 0x2f1e0746);
        context.set(context.field(partition, 0x51fa3f1c), 0.f); // Fixed top edge.
        context.set(context.field(partition, 0x307f2934), 32.f);
        context.set(context.field(partition, 0x613f2c2), value.height + 16.f);
        context.set(context.field(partition, 0x4fdf869b), 0);
        context.set(context.field(nodes[i], 0xd7b53526), Widget{blueprint(anchored_widget), {0, value.presenter.handle}});
        context.set(context.field(nodes[i], 0xcb57674f), i + 1 < visible.size() ?
            Widget{blueprint(vertical_widget), {0, nodes[i + 1].handle}} : Widget{});
    }
    if (visible.empty()) {
        context.set(context.field(panel, 0xd7b53526), Widget{});
        context.set(context.field(panel, 0xcb57674f), Widget{});
    }
    s.displayed[slot] = visible;
}
} // namespace
namespace native_menu_detail {
void add_text(const Context& context, std::vector<std::string>& visible, const std::string& id, const std::string& title, float height) {
    if (title.empty()) return;
    auto& value = row(context, id, RowKind::text);
    if (value.height != height) {
        value.height = height;
        context.set(context.field(value.presenter, 0x1cff7243), std::array<float, 2>{value.width, height});
    }
    if (value.last_text != title) {
        context.text(context.field(value.model, text_field), title);
        value.last_text = title;
    }
    style_row(context, value);
    visible.push_back(id);
}
void add_button(const Context& context, std::vector<std::string>& visible, const std::string& id,
                std::string title, std::string command, std::string argument, bool primary, float row_height) {
    auto& value = row(context, id);
    if (value.last_text != title) {
        context.text(string_field(context, value.model), title);
        value.last_text = std::move(title);
    }
    const auto callback = command.empty() ? Address{} : action(context, std::move(command), std::move(argument));
    if (value.callback != callback) {
        context.set(primary_action(context, value.model), callback);
        value.callback = callback;
    }
    value.primary = primary;
    const auto height = row_height > 0.f ? row_height :
        primary || value.last_text.find('\n') != std::string::npos ? 192.f : 168.f;
    if (value.height != height) {
        value.height = height;
        context.set(context.field(value.presenter, 0x1cff7243), std::array<float, 2>{value.width, height});
    }
    style_row(context, value);
    visible.push_back(id);
}
void add_input(const Context& context, std::vector<std::string>& visible, const std::string& id,
               const std::string& title, const std::string& initial) {
    const bool fresh = !state().rows.contains(id);
    auto& value = row(context, id, RowKind::input);
    if (fresh) {
        context.text(context.field(value.model, value_field), initial);
        context.text(context.field(value.model, 0xbb27f3ea), title);
        context.text(context.field(value.model, 0x84f79340), title);
        context.text(context.field(value.model, 0xbe79fc4a), std::string{});
    }
    visible.push_back(id);
}
std::string input_text(const Context& context, const char* id) {
    const auto found = state().rows.find(id);
    return found == state().rows.end() ? std::string{} : context.text(context.field(found->second.model, value_field), 2048);
}
void clear_input(const Context& context, const char* id) {
    const auto found = state().rows.find(id);
    if (found != state().rows.end()) context.text(context.field(found->second.model, value_field), std::string{});
}
// Lists that scroll: every page's first main column, both columns of the
// Current Session and Voice Chat sections, whose settings outgrow the screen,
// the Custom Maps list, which grows with every map in the Mods folder, and the
// Player list, which the Boneyard rows outgrow, and the Death Race lists, which grow
// with every race on the map.
bool scrolling_list(unsigned page_slot, unsigned slot) {
    constexpr auto session = static_cast<unsigned>(Section::session) * 2, voice = static_cast<unsigned>(Section::voice) * 2;
    constexpr auto custom_maps = static_cast<unsigned>(native_tools::custom_section) * 2;
    constexpr auto player = static_cast<unsigned>(native_tools::player_section) * 2;
    constexpr auto races = static_cast<unsigned>(native_tools::death_race_section) * 2;
    return slot == 0 || (page_slot == 0 && (slot == session || slot == session + 1 || slot == voice || slot == voice + 1)) ||
        (page_slot == menu_view::tools_page && (slot == custom_maps || slot == player || slot == races || slot == races + 1));
}
void publish_rows(const Context& context, Value list, unsigned slot, const std::vector<std::string>& visible) {
    if (!scrolling_list(state().slot, slot)) { publish_fixed(context, list, slot, visible); return; }
    auto& s = state();
    auto& displayed = s.displayed[slot];
    const auto current = read<Address>(context.address(context.field(list, 0xf2c90867)));
    const auto count = current ? read<std::uint32_t>(current - 4) & 0x7fffffff : 0;
    if (visible == displayed && count == visible.size()) return;
    const auto focused = read<int>(context.address(context.field(list, 0x55511280)));
    const std::string focused_id = focused >= 0 && static_cast<std::size_t>(focused) < displayed.size() ? displayed[focused] : "";
    std::vector<Handle> handles;
    int first = -1, preserved = -1;
    for (const auto& id : visible) {
        const auto& value = s.rows.at(id);
        if (value.kind != RowKind::text) {
            if (first < 0) first = static_cast<int>(handles.size());
            if (id == focused_id) preserved = static_cast<int>(handles.size());
        }
        handles.push_back(value.presenter.handle);
    }
    context.array(context.field(list, 0xf2c90867), handles);
    context.set(context.field(list, 0x55511280), preserved >= 0 ? preserved : first);
    displayed = visible;
}
} // namespace native_menu_detail
} // namespace dingosdk::multiplayer
