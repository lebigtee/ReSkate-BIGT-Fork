#include "camera_light_native.h"
#include "camera_light.h"
#include "camera_light_state.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Abi/native_data.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/supported_build.h"
#include "Engine/Game/Build/20260929/ai_skaters.h"
#include "Engine/Game/Build/20260929/camera_light.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/native_skater.h"
#include "Engine/Game/Build/20260929/skater_entities.h"
#include "Engine/Game/UI/game_view.h"
#include "Extension/Multiplayer/Remote/native_creation_list.h"
#include "Extension/Multiplayer/Remote/native_skater.h"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <intrin.h>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

// Skate has no light that follows the camera, but its character light rig (the lights of its
// character screens) is a prefab of spot lights the game creates and moves. While the camera light
// is on, the rig is created as ReSkate creates its AI skaters (ai_skaters.cpp): from its
// blueprint, found by name in the loaded assets, on an entity bus of its own. Its first light is
// made a filmer's lamp for the creation (the rig's data is set back afterwards) and placed on the
// camera every client tick; the others are made dark and put away under the map. Off, the rig is
// destroyed.
namespace dingosdk::camera_light {
namespace {
namespace ai = addr::ai_skaters;
namespace entities = addr::skater_entities;
namespace rig_light = addr::camera_light;
namespace native = addr::native_skater;
using Clock = std::chrono::steady_clock;
using Matrix = std::array<float, 16>;
constexpr std::uintptr_t highest = memory::highest_user_address;
constexpr std::string_view player_blueprint = "Gameplay/Characters/CharacterBlueprint_Physics_Skater_RSP_CAS";
constexpr std::string_view rig_blueprint = "Lighting/Prefabs/Character/PF_LightRig_Ambient";

// The filmer's lamp: its cone (degrees: full light inside the inner, none past the outer), its
// reach (metres) and its brightness at gain 1 (the rig's studio lights are 1.2e6 to 2.5e7).
constexpr float lamp_inner = 1.0f, lamp_outer = 80.0f, lamp_range = 30.0f, lamp_intensity = 1e4f;
// Where the lamp sits on the camera (metres up and to the right): off the lens, so the shadows it
// casts show beside what casts them instead of hiding behind it. And how far it is tipped down
// from where the camera looks (degrees), so the skater's legs and board get as much as its head.
constexpr float lamp_above = 0.5f, lamp_aside = 0.3f, lamp_tilt = 10.0f;
// Ticks the rig's data stays a lamp's after the creation, for the lights to take it up.
constexpr int restore_after_ticks = 60;
// Where the rig's other lights are put away.
constexpr std::array<float, 3> away{0.0f, -10000.0f, 0.0f};
// Failed creations wait this long before the next try.
constexpr auto retry_after = std::chrono::seconds(5);

struct Unavailable : std::runtime_error {
    using std::runtime_error::runtime_error;
};
void require(bool value, const std::string &reason) {
    if (!value) throw Unavailable(reason);
}
bool readable(std::uintptr_t address, void *out, std::size_t size) noexcept {
    if (address < 0x10000 || size > highest || address > highest - size) return false;
    __try {
        std::memcpy(out, reinterpret_cast<const void *>(address), size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool writable(std::uintptr_t address, const void *data, std::size_t size) noexcept {
    if (address < 0x10000 || size > highest || address > highest - size) return false;
    __try {
        std::memcpy(reinterpret_cast<void *>(address), data, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
template <class T> T read(std::uintptr_t object, std::uintptr_t offset = 0) {
    T value{};
    if (!(object >= 0x10000 && object <= highest && offset <= highest - object && readable(object + offset, &value, sizeof(value))))
        throw Unavailable(std::format("Camera light read failed at {:#x}+{:#x}.", object, offset));
    return value;
}
std::uintptr_t ptr(std::uintptr_t object, std::uintptr_t offset = 0) { return read<std::uintptr_t>(object, offset); }
bool code(std::uintptr_t base, std::uintptr_t address) { return address >= base && address < base + supported_build::game_image_size; }
template <std::size_t N> void prefix(std::uintptr_t base, std::uintptr_t rva, const std::array<std::uint8_t, N> &expected) {
    std::array<std::uint8_t, 32> bytes{};
    require(expected.size() <= bytes.size() && readable(base + rva, bytes.data(), expected.size()) &&
                std::equal(expected.begin(), expected.end(), bytes.begin()),
            std::format("native function {:#x} changed", rva));
}
std::string name_at(std::uintptr_t address) {
    std::string result;
    for (std::size_t i = 0; i < 256; ++i) {
        char c{};
        if (!readable(address + i, &c, 1)) return {};
        if (!c) return result;
        result += c;
    }
    return result;
}
bool same_name(std::string_view a, std::string_view b) {
    return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](char x, char y) {
        return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
    });
}
void say(logging::Level level, const std::string &text) { logging::write(level, logging::Channel::graphics, text); }

void require_job_context(std::uintptr_t base, std::uintptr_t context) {
    require(read<std::uint8_t>(base, addr::engine::entity_creation_ready) != 0, "native entity creation is not ready");
    const auto tls_array = static_cast<std::uintptr_t>(__readgsqword(0x58));
    const auto tls_index = read<std::uint32_t>(base, addr::engine::tls_index);
    require(tls_index <= 4095, "native TLS index changed");
    const auto tls = ptr(tls_array, std::uintptr_t{tls_index} * 8);
    require(read<std::uint8_t>(tls, 0xb19) != 0 && ptr(tls, 0x550) == context, "waiting for the native client job context");
}
// The player's skater source (ai_skaters.cpp reads them the same way): its blueprint anchors the
// asset lookup, its parent bus parents the rig's.
struct Source {
    std::uintptr_t parent{}, blueprint{};
};
Source player_source(std::uintptr_t base, std::uintptr_t context) {
    const auto partition_offset = read<std::uint32_t>(base, addr::engine::context_partition_offset);
    require(partition_offset <= 0x1000000, "source partition offset changed");
    const auto partition = read<std::uint32_t>(context, partition_offset);
    require(partition <= 4, "source partition unavailable");
    for (unsigned kind = 0; kind < 7; ++kind) {
        const auto manager = ptr(base, addr::engine::source_managers + (std::uintptr_t{partition} * 7 + kind) * 8);
        if (!manager) continue;
        require(ptr(manager) == base + addr::engine::source_manager_vtable && ptr(manager, 0x18) == context, "source manager differs");
        const auto begin = ptr(manager, 0xa0), end = ptr(manager, 0xa8);
        require(begin <= end && (end - begin) % 0x18 == 0 && (end - begin) / 0x18 <= 64, "source collection exceeds bounds");
        for (auto at = begin; at < end; at += 0x18) {
            const auto address = ptr(at, 8);
            if (!address || ptr(address) != base + addr::engine::client_skater_source_vtable) continue;
            const auto data = ptr(address, 0x38), parent = ptr(address, 0x30);
            if (!data || !parent || ptr(address, 0x20) != context || ptr(parent, 0x20) != context) continue;
            const auto blueprint = ptr(data, 0xd0) & ~std::uintptr_t{4};
            if (blueprint && same_name(name_at(ptr(blueprint, 0x18)), player_blueprint)) return {parent, blueprint};
        }
    }
    throw Unavailable("no player skater source is loaded");
}
// A loaded asset by name, from the anchor's asset domain up (ai_skaters.cpp's lookup). The rig is
// a prefab blueprint, not one of the object blueprints the skaters are: its own name says what it is.
std::uintptr_t find_asset(std::uintptr_t base, std::uintptr_t anchor, std::string_view name) {
    const auto find = game::native_data().find_asset;
    require(find != nullptr, "the native asset lookup is unavailable");
    const std::string text(name);
    auto domain = read<std::uint16_t>(anchor, 0x16);
    std::set<std::uint16_t> visited;
    while (domain < 0xbbf && visited.size() < 32 && visited.insert(domain).second) {
        const auto owner = ptr(base, addr::engine::domain_owners + domain * std::uintptr_t{8});
        if (!owner) return 0;
        if (const auto asset = find(domain, text.c_str())) return same_name(name_at(ptr(asset, 0x18)), name) ? asset : 0;
        domain = read<std::uint16_t>(owner, 0x42);
    }
    return 0;
}

// A spot light's data and its values as found.
using Color = std::array<float, 3>;
struct LightData {
    std::uintptr_t address{};
    float inner{}, range{}, outer{}, intensity{};
    Color color{};
};
std::optional<LightData> light_data(std::uintptr_t object) {
    LightData data{object};
    if (!readable(object + rig_light::spot_inner_angle, &data.inner, 4) || !readable(object + rig_light::spot_attenuation_radius, &data.range, 4) ||
        !readable(object + rig_light::spot_outer_angle, &data.outer, 4) || !readable(object + rig_light::spot_intensity, &data.intensity, 4) ||
        !readable(object + rig_light::spot_color, data.color.data(), sizeof(Color)))
        return std::nullopt;
    if (!(data.outer > 1.0f && data.outer < 180.0f && data.inner >= 0.0f && data.inner <= data.outer && data.range > 0.1f && data.range < 1000.0f &&
          data.intensity > 1.0f && data.intensity < 1e9f &&
          std::all_of(data.color.begin(), data.color.end(), [](float c) { return c >= 0.0f && c <= 100.0f; })))
        return std::nullopt;
    return data;
}
void write_light(const LightData &data) {
    writable(data.address + rig_light::spot_inner_angle, &data.inner, 4);
    writable(data.address + rig_light::spot_attenuation_radius, &data.range, 4);
    writable(data.address + rig_light::spot_outer_angle, &data.outer, 4);
    writable(data.address + rig_light::spot_intensity, &data.intensity, 4);
    writable(data.address + rig_light::spot_color, data.color.data(), sizeof(Color));
}
// The setting (0xRRGGBB) as the light's colour, 1 1 1 white.
Color lamp_color(std::uint32_t rgb) {
    return {static_cast<float>(rgb >> 16 & 0xff) / 255.0f, static_cast<float>(rgb >> 8 & 0xff) / 255.0f, static_cast<float>(rgb & 0xff) / 255.0f};
}
// The rig's spot lights' data: its blueprint's objects (an array: element pointers, the count just
// before them) that hold spot light values.
std::vector<LightData> rig_lights(std::uintptr_t blueprint) {
    for (std::uintptr_t field = 0x20; field < 0x100; field += 8) {
        std::uintptr_t array{};
        std::uint32_t count{};
        if (!readable(blueprint + field, &array, 8) || array < 0x10000 || !readable(array - 4, &count, 4) || !count || count > 64) continue;
        std::vector<LightData> lights;
        bool objects = true;
        for (std::uint32_t i = 0; i < count && objects; ++i) {
            std::uintptr_t object{};
            objects = readable(array + i * 8, &object, 8) && object > blueprint - 0x200000 && object < blueprint + 0x200000;
            if (objects)
                if (const auto data = light_data(object)) lights.push_back(*data);
        }
        if (objects && !lights.empty()) return lights;
    }
    return {};
}

// A light of the rig: the entity, its class, its spatial interface and that interface's setter.
struct Light {
    std::uintptr_t entity{}, vtable{}, spatial{}, setter{}, data{};
};
// The entity as one of the rig's spot lights (addr::camera_light), or nothing.
std::optional<Light> light_of(std::uintptr_t base, std::uintptr_t entity) {
    Light light{entity};
    const auto spatial = entity + rig_light::light_spatial_interface;
    std::uintptr_t interface_vtable{}, getter{};
    std::array<unsigned char, rig_light::light_transform_setter_prefix.size()> code_bytes{};
    if (!readable(entity, &light.vtable, 8) || !readable(entity + rig_light::light_data, &light.data, 8) || !readable(spatial, &interface_vtable, 8) ||
        !code(base, interface_vtable) || !readable(interface_vtable + 0x10, &light.setter, 8) || !readable(interface_vtable + 0x18, &getter, 8) ||
        getter != base + rig_light::spatial_transform_getter || !code(base, light.setter) ||
        !readable(light.setter, code_bytes.data(), code_bytes.size()) || code_bytes != rig_light::light_transform_setter_prefix)
        return std::nullopt;
    light.spatial = spatial;
    return light;
}
void place(const Light &light, const Matrix &at) {
    alignas(16) const Matrix m = at;
    reinterpret_cast<void (*)(std::uintptr_t, const void *)>(light.setter)(light.spatial, m.data());
}
Matrix put_away() { return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, away[0], away[1], away[2], 1}; }

// The rig in the game: every entity it made (with its class, to tell it is still the same), the
// lamp, and its entity bus (held until the rig is destroyed).
struct Rig {
    std::uintptr_t context{}, bus{};
    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> entities;
    Light lamp;
    float gain{};
    std::uint32_t color{};
    std::vector<LightData> changed; // the rig's data as it was, set back after restore_after_ticks
    int ticks{};
};
// The rig while there is one, and when to try again after a failed creation.
struct Keeper {
    std::optional<Rig> rig;
    Clock::time_point next_try{};
    std::string last_error;
};
Keeper &keeper() {
    static auto *k = new Keeper;
    return *k;
}

// The rig's data set back as it was.
void restore(Rig &rig) {
    for (const auto &data : rig.changed) write_light(data);
    rig.changed.clear();
}
bool alive(const Rig &rig, std::uintptr_t client) {
    std::uintptr_t context{};
    if (!readable(client + 8, &context, 8) || context != rig.context) return false;
    for (const auto &[entity, vtable] : rig.entities) {
        std::uintptr_t now{};
        if (!readable(entity, &now, 8) || now != vtable) return false;
    }
    return true;
}

Rig create(std::uintptr_t base, std::uintptr_t client) {
    std::string detail;
    require(multiplayer::install_entity_hooks(base, detail), "native entity hooks unavailable: " + detail);
    prefix(base, ai::create_bus, ai::create_bus_prefix);
    prefix(base, ai::acquire_bus, ai::acquire_bus_prefix);
    const auto local = multiplayer::capture_local(base, client, false);
    require(local.ready, local.detail);
    require_job_context(base, local.context);
    const auto source = player_source(base, local.context);
    const auto blueprint = find_asset(base, source.blueprint, rig_blueprint);
    require(blueprint != 0, "the game's character light rig is not loaded on this map");
    auto lights = rig_lights(blueprint);
    require(!lights.empty(), "the character light rig holds no spot light");
    Rig rig;
    rig.context = local.context;
    rig.gain = state::gain.load();
    rig.color = state::color.load();
    rig.changed = lights;
    // For the creation: the first light a filmer's lamp, the others dark.
    for (std::size_t i = 0; i < lights.size(); ++i) {
        auto light = lights[i];
        if (i == 0) light = {light.address, lamp_inner, lamp_range, lamp_outer, lamp_intensity * rig.gain, lamp_color(rig.color)};
        else light.intensity = 0.0f;
        write_light(light);
    }
    // Set back at once if the creation fails.
    struct Undo {
        Rig *rig;
        bool done{};
        ~Undo() {
            if (!done) restore(*rig);
        }
    } undo{&rig};
    // An entity bus of its own under the player's source's, as for an AI skater; held until the rig
    // is destroyed.
    using CreateBus = std::uintptr_t (*)(std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uint8_t, std::uintptr_t);
    rig.bus = reinterpret_cast<CreateBus>(base + ai::create_bus)(ptr(ptr(source.parent, 8), 0x48), source.parent, 0, 0, 0, 0);
    require(rig.bus != 0, "the engine did not create an entity bus for the light");
    reinterpret_cast<int (*)(std::uintptr_t)>(base + ai::acquire_bus)(rig.bus);
    alignas(16) const Matrix at = put_away();
    alignas(16) std::array<std::uint8_t, 0x190> descriptor{};
    using Init = void *(*)(void *, std::uintptr_t, std::uintptr_t, const void *);
    reinterpret_cast<Init>(base + entities::descriptor_init)(descriptor.data(), 0, rig.bus, at.data());
    struct DescriptorScope {
        std::uintptr_t base;
        void *data;
        ~DescriptorScope() { reinterpret_cast<void (*)(void *)>(base + entities::descriptor_destroy)(static_cast<std::uint8_t *>(data) + 0x10); }
    } descriptor_scope{base, descriptor.data()};
    // Local id 255 (no player), and a creation list the rig's entities are collected in.
    multiplayer::NativeCreationList list{};
    auto *list_address = &list;
    const std::uint32_t id = 255;
    std::memcpy(descriptor.data() + 0x38, &id, sizeof(id));
    descriptor[0x151] = 0;
    std::memcpy(descriptor.data() + 0x158, &list_address, sizeof(list_address));
    std::array<std::uintptr_t, 3> result{};
    using Create = void *(*)(void *, void *, std::uintptr_t, std::uintptr_t, std::uintptr_t);
    reinterpret_cast<Create>(base + entities::create_entity)(result.data(), descriptor.data(), blueprint, 0, 0);
    if (result[1]) reinterpret_cast<void (*)(std::uintptr_t)>(base + entities::release_reference)(result[1]);
    const auto readable_list = [](std::uintptr_t address, void *out, std::size_t size) { return readable(address, out, size); };
    const auto created = multiplayer::read_native_creation_list(readable_list, list);
    // Initialized together, as a skateboard's are (native_skater_spawn.cpp); then the list's pages
    // go back (they hold borrowed pointers).
    if (!created.entities.empty()) reinterpret_cast<void (*)(void *)>(base + native::initialize_creation_list)(&list);
    for (const auto page : created.pages) {
        const std::uint32_t empty = 0;
        writable(page + 8, &empty, sizeof(empty));
        reinterpret_cast<void (*)(void *, std::uintptr_t, std::size_t)>(base + native::free_creation_page)(&list, page, 0x1f0);
    }
    for (const auto entity : created.entities) rig.entities.emplace_back(entity, ptr(entity));
    // The lamp is the light made from the first light's data; the others go away.
    for (const auto entity : created.entities) {
        const auto light = light_of(base, entity);
        if (!light) continue;
        if (light->data == lights.front().address && !rig.lamp.entity) rig.lamp = *light;
        else place(*light, put_away());
    }
    undo.done = true; // the data stays the lamp's for a few ticks
    if (!rig.lamp.entity) {
        say(logging::Level::warning, std::format("Camera light: the rig made {} entities but not its lamp.", created.entities.size()));
        restore(rig);
    }
    say(logging::Level::info, std::format("Camera light: on ({} entities, the lamp {:#x}).", created.entities.size(), rig.lamp.entity));
    return rig;
}

// Destroys what the rig made, then lets its bus go (ai_skaters.cpp destroys its skaters the same way).
void destroy(std::uintptr_t base, std::uintptr_t client, Rig &rig) {
    restore(rig);
    if (alive(rig, client)) {
        for (auto at = rig.entities.rbegin(); at != rig.entities.rend(); ++at)
            reinterpret_cast<void (*)(std::uintptr_t, std::uintptr_t)>(base + entities::destroy_entity)(at->first, rig.bus);
        reinterpret_cast<void (*)(std::uintptr_t)>(base + entities::release_reference)(rig.bus);
    }
    say(logging::Level::info, "Camera light: off.");
}

// The lamp on the camera, facing the way it looks. A light shines along the back of its transform,
// as a view looks (the camera's rows: right, up, back, position), so the camera's own transform
// does; the character rig turns its look-at transforms round (ReverseDirection) for its lights.
void follow(const Rig &rig) {
    const auto view = latest_game_view();
    if (!view || !rig.lamp.entity) return;
    const auto &m = view->world;
    const auto at = [&](int axis) { return m[12 + axis] + m[4 + axis] * lamp_above + m[axis] * lamp_aside; };
    // Tipped down about the camera's right: the back leans up, the up leans forward.
    const float c = std::cos(lamp_tilt * 3.14159265f / 180.0f), s = std::sin(lamp_tilt * 3.14159265f / 180.0f);
    const auto up = [&](int axis) { return m[4 + axis] * c - m[8 + axis] * s; };
    const auto back = [&](int axis) { return m[8 + axis] * c + m[4 + axis] * s; };
    place(rig.lamp, {m[0], m[1], m[2], 0, up(0), up(1), up(2), 0, back(0), back(1), back(2), 0, at(0), at(1), at(2), 1});
}
} // namespace

void tick(std::uintptr_t base, std::uintptr_t client, bool ready) noexcept {
    auto &s = keeper();
    try {
        // A map change takes the rig with it.
        if (s.rig && !alive(*s.rig, client)) {
            restore(*s.rig);
            s.rig.reset();
        }
        const bool wanted = ready && enabled();
        // Off, or another brightness or colour: the rig goes (and comes back with it).
        if (s.rig && (!wanted || s.rig->gain != state::gain.load(std::memory_order_relaxed) ||
                      s.rig->color != state::color.load(std::memory_order_relaxed))) {
            destroy(base, client, *s.rig);
            s.rig.reset();
        }
        if (wanted && !s.rig && Clock::now() >= s.next_try) {
            try {
                s.rig = create(base, client);
                s.last_error.clear();
            } catch (const std::exception &e) {
                s.next_try = Clock::now() + retry_after;
                // Often only not yet (the skater is still being made as a map comes in): tried again.
                if (s.last_error != e.what())
                    say(logging::Level::info, std::format("Camera light: not there yet ({}); trying again in {} s.", e.what(), retry_after.count()));
                s.last_error = e.what();
            }
        }
        if (!s.rig) return;
        if (++s.rig->ticks >= restore_after_ticks) restore(*s.rig);
        follow(*s.rig);
    } catch (const std::exception &e) {
        say(logging::Level::error, std::string("Camera light: ") + e.what());
    } catch (...) {
    }
}
} // namespace dingosdk::camera_light
