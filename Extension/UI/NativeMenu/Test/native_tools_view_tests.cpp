#include "Extension/UI/NativeMenu/native_tools_view.h"
#include <cstdio>
#include <stdexcept>

using namespace dingosdk;
using namespace dingosdk::native_tools;
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
struct Capture {
    unsigned calls{};
    std::string command, root, destination, spawn;
    overlay::DebugRequest debug;
};
int main() {
    try {
        {
            // A BAM root with its seven time-of-day layers, as the level scan finds them.
            WorldLayerCatalog catalog{{{"Levels/Game/BAM_LevelRoot/Root", WorldMap::bam, -1, true}}, {0}, {}};
            for (const auto* time : time_of_day_keys) {
                const auto slot = static_cast<unsigned>(catalog.nodes.size());
                catalog.nodes.push_back({std::string("Levels/Game/BAM_LevelRoot/TOD_") + time, WorldMap::bam, 0, false});
                catalog.layers.push_back({std::string("bam_tod_") + time, time, "", WorldMap::bam, slot, slot, "Lighting"});
            }
            install_world_layer_catalog(std::move(catalog));
        }
        State state;
        overlay::Model model;
        Capture capture;
        overlay::CallbacksV3 cb;
        cb.user = &capture;
        cb.queue_load = [](void* user, const char* root, const char*, const char* destination, const char* spawn, char*, std::size_t) {
            auto& c = *static_cast<Capture*>(user); ++c.calls; c.root = root; c.destination = destination; c.spawn = spawn; return true;
        };
        cb.queue_debug = [](void* user, const overlay::DebugRequest& request, char*, std::size_t) {
            auto& c = *static_cast<Capture*>(user); ++c.calls; c.debug = request; return true;
        };
        cb.queue_console_command = [](void* user, const char* command, char*, std::size_t) {
            auto& c = *static_cast<Capture*>(user); ++c.calls; c.command = command; return true;
        };
        model.levels = {{std::string(root_asset), "Root"}, {"Studio/Example", "Studio", {"EntryA"}, {}, false}};
        model.can_queue_load = true;
        activate(state, model, cb, "select-level", "Studio/Example");
        check(capture.calls == 0 && state.destination == "Studio/Example", "Selecting a detached level must not load it");
        activate(state, model, cb, "load-level", "");
        check(capture.calls == 1 && capture.root == root_asset && capture.destination == "Studio/Example" && capture.spawn == "EntryA", "Detached level uses its sole manifest spawn automatically");
        model.levels.back().native_registered = true;
        activate(state, model, cb, "load-level", "");
        check(capture.calls == 2 && capture.spawn.empty(), "Registered level uses the engine's default spawn without retaining a previous selection");
        check(state.feedback.empty(), "Accepted changes do not display queue diagnostics");
        model.multiplayer.active = true;
        check(!can_load(state, model, cb), "Guest cannot load a level even with an otherwise ready loader");
        const auto guest_page = render(state, model, cb, 0);
        check(std::none_of(guest_page.main.begin(), guest_page.main.end(), [](const auto& row) {
            return row.command == "select-level";
        }), "Guest level choices are visibly locked");
        check(std::none_of(guest_page.side.begin(), guest_page.side.end(), [](const auto& row) {
            return row.command == "load-level";
        }), "Guest load button has no activation delegate");
        activate(state, model, cb, "load-level", "");
        check(capture.calls == 2 && state.feedback == "Only the lobby host can change levels.",
            "A stale level action cannot load after joining a host");
        model.multiplayer.hosting = true;
        check(can_load(state, model, cb), "Host retains level controls");
        model.multiplayer = {};
        check(can_load(state, model, cb), "Level controls return when the host ends the lobby");
        model.multiplayer.lobby_joining = true;
        check(!can_load(state, model, cb), "Joining cannot race a manual level change");
        model.multiplayer = {};
        model.can_queue_load = false;
        activate(state, model, cb, "load-level", "");
        check(capture.calls == 2, "Stale load button cannot queue during transition");
        model.can_queue_load = true;
        model.levels.pop_back();
        activate(state, model, cb, "load-level", "");
        check(capture.calls == 2 && state.destination.empty(), "Removed level selection must not load");
        model.world.map = WorldMap::bam; model.parks.available = true;
        activate(state, model, cb, "park-lot", "financial");
        activate(state, model, cb, "park-family", "");
        activate(state, model, cb, "load-park", "");
        check(capture.command == "park financial flumppark_01", "Selected park targets its own lot");
        const auto calls = capture.calls;
        model.parks.controlled_by_host = true;
        activate(state, model, cb, "load-park", "");
        check(capture.calls == calls, "Host ownership is rechecked on activation");
        activate(state, model, cb, "noclip", "");
        check(capture.calls == calls, "Unavailable debug control cannot queue");
        model.debug.noclip = true;
        activate(state, model, cb, "noclip", "");
        check(capture.debug.action == overlay::DebugAction::set_noclip && !capture.debug.enabled, "Active noclip can always be disabled");
        model.graphics.available = true; model.graphics.ready[0] = true; model.graphics.enabled[0] = false;
        activate(state, model, cb, "graphics", "film_grain");
        check(capture.command == "graphics film_grain 1", "Toggle reads effective authored graphic state");
        model.progression.challenges_enabled = true; model.progression.challenges_hidden = true;
        activate(state, model, cb, "challenges", "");
        check(capture.command == "challenges 1", "Hidden challenges use show command");
        model.world.ready = true; model.world_controls.population_available = true;
        model.world_controls.choices.traffic = 3;
        activate(state, model, cb, "traffic", "");
        check(capture.command == "environment traffic -1", "Population cycles back to authored default");
        const auto parks = render(state, model, cb, parks_section);
        const auto load = std::find_if(parks.side.begin(), parks.side.end(), [](const auto& row) { return row.id == "load-park"; });
        check(load != parks.side.end() && load->command.empty(), "Host-owned park action is visibly unavailable");
        cb.queue_debug = [](void*, const overlay::DebugRequest&, char* result, std::size_t size) {
            std::snprintf(result, size, "Queued for the game update thread."); return true;
        };
        activate(state, model, cb, "noclip", "");
        check(state.feedback.empty(), "Successful callback diagnostics stay out of the menu");
        model.debug.status = "Native callback diagnostic";
        const auto player_page = render(state, model, cb, player_section);
        check(std::none_of(player_page.side.begin(), player_page.side.end(), [](const auto& row) {
            return row.title.find("Native callback") != std::string::npos;
        }), "Debug model status stays out of the player menu");
        const auto find = [](const std::vector<Row>& rows, std::string_view id) {
            return std::find_if(rows.begin(), rows.end(), [&](const auto& row) { return row.id == id; });
        };
        model.multiplayer.local_name = "steam_name";
        model.player_card = {true, "Card Name", {}};
        const auto card_page = render(state, model, cb, player_section);
        const auto field = find(card_page.side, "card-name");
        check(field != card_page.side.end() && field->input && field->title == "steam_name" && field->argument == "Card Name",
            "Card name field shows the Steam name as its hint and the saved name as its text");
        check(find(card_page.side, "card-name-reset")->command == "card-name-reset", "A custom card name can be reset");
        model.player_card.custom_name.clear();
        const auto steam_page = render(state, model, cb, player_section);
        check(find(steam_page.side, "card-name-reset")->command.empty(), "Steam name reset is idle without a custom name");
        {
            // Time of day forces one of a map's seven time layers, others off.
            overlay::Model world;
            world.world.available = world.world.ready = true;
            world.world.map = WorldMap::bam;
            world.world.supported.assign(world_layers().size(), true);
            State tod;
            Capture log;
            overlay::CallbacksV3 record;
            record.user = &log;
            record.queue_console_command = [](void* user, const char* command, char*, std::size_t) {
                auto& c = *static_cast<Capture*>(user); ++c.calls; c.command = command; return true;
            };
            check(time_of_day_available(world, record) && time_of_day(world) == 0, "Time of day starts at the level's default");
            activate(tod, world, record, "time-of-day", {});
            check(log.calls == 7 && log.command == "world bam_tod_1_morning on", "Cycling selects Morning last, after the others are off");
            for (unsigned slot = 0; slot < time_of_day_keys.size(); ++slot)
                world.world.choices[time_of_day_layer(WorldMap::bam, slot)] = slot == 4 ? "on" : "off";
            check(time_of_day(world) == 5, "A single forced layer reads back as that time");
            const auto page = render(tod, world, record, 0);
            check(std::any_of(page.side.begin(), page.side.end(), [](const auto& row) { return row.title == "Time of day: Night"; }),
                "The World page shows the current time of day");
            for (unsigned slot = 0; slot < 6; ++slot) activate(tod, world, record, "time-of-day", {});
            world.world.choices[time_of_day_layer(WorldMap::bam, 6)] = "on";
            check(time_of_day(world) == 0, "Two forced times read as no single choice");
            for (auto& choice : world.world.choices) choice = "default";
            world.world.choices[time_of_day_layer(WorldMap::bam, 6)] = "on";
            for (unsigned slot = 0; slot < 6; ++slot) world.world.choices[time_of_day_layer(WorldMap::bam, slot)] = "off";
            log.calls = 0;
            activate(tod, world, record, "time-of-day", {});
            check(log.calls == 7 && log.command.ends_with(" default"), "Cycling past the last time returns all seven to default");
            world.world.controlled_by_host = true;
            log.calls = 0;
            activate(tod, world, record, "time-of-day", {});
            check(log.calls == 0 && !time_of_day_available(world, record), "A host-forced lobby cannot change time of day");
        }
        {
            overlay::Model maps;
            maps.levels = {{std::string(root_asset), "Root"}, {"levels/Game/BAM_LevelRoot/BAM_LevelRoot"},
                {"levels/Game/DingoLevel_Splash/DingoLevel_Splash"}, {"Levels/Custom/Bedroom", "Bedroom"}};
            maps.levels.back().custom = true;
            State split;
            const auto shows = [](const Page& page, std::string_view asset) {
                return std::any_of(page.main.begin(), page.main.end(), [&](const auto& row) { return row.argument == asset; });
            };
            const auto world_page = render(split, maps, cb, world_section), custom_page = render(split, maps, cb, custom_section);
            check(shows(world_page, maps.levels[1].asset) && !shows(world_page, "Levels/Custom/Bedroom"), "World lists only official levels");
            check(shows(custom_page, "Levels/Custom/Bedroom") && !shows(custom_page, maps.levels[1].asset), "Custom Maps lists only mod levels");
            check(!shows(world_page, maps.levels[2].asset) && !shows(custom_page, maps.levels[2].asset), "Splash is never listed");
            activate(split, maps, cb, "select-level", maps.levels[2].asset);
            check(split.destination.empty(), "Splash cannot be selected");
        }
        {
            // The Boneyard: locked until its settings are copied in, then each row flips one.
            State yard;
            overlay::Model none;
            const auto row = [](const Page& page, std::string_view id) -> const Row* {
                for (const auto& r : page.main) if (r.id == id) return &r;
                return nullptr;
            };
            auto page = render(yard, none, cb, player_section);
            check(row(page, "boneyard") && row(page, "boneyard")->command.empty(), "Boneyard waits for its settings");
            yard.boneyard = {true, true, true, false, true};
            page = render(yard, none, cb, player_section);
            check(row(page, "boneyard")->title == "Boneyard: On" && row(page, "boneyard")->command == "boneyard",
                "Boneyard shows it is on");
            check(row(page, "boneyard-slowmo")->title == "Bail slow motion: On" && row(page, "boneyard-stutter")->title == "Bail stutter: Off" &&
                  row(page, "boneyard-glow")->title == "X-ray glow: On",
                "Slow motion, stutter and glow show their own settings");
            activate(yard, none, cb, "boneyard", "");
            check(capture.command == "boneyard off", "Boneyard turns off");
            activate(yard, none, cb, "boneyard-slowmo", "");
            check(capture.command == "boneyard slowmo 0", "Slow motion turns off");
            activate(yard, none, cb, "boneyard-stutter", "");
            check(capture.command == "boneyard stutter 1", "Stutter turns on");
            activate(yard, none, cb, "boneyard-glow", "");
            check(capture.command == "boneyard glow 0", "The glow turns off");
            yard.boneyard.on = false;
            page = render(yard, none, cb, player_section);
            check(row(page, "boneyard")->command == "boneyard" && row(page, "boneyard-slowmo")->command.empty() &&
                  row(page, "boneyard-stutter")->command.empty() && row(page, "boneyard-glow")->command.empty(),
                  "With it off, only Boneyard itself can be pressed");
            activate(yard, none, cb, "boneyard", "");
            check(capture.command == "boneyard on", "Boneyard turns on");
        }
        {
            // The camera light: its brightness and colour a step a press, while it is on.
            State light;
            overlay::Model none;
            const auto row = [](const Page& page, std::string_view id) -> const Row* {
                for (const auto& r : page.main) if (r.id == id) return &r;
                return nullptr;
            };
            auto page = render(light, none, cb, player_section);
            check(row(page, "camera-light") && row(page, "camera-light")->command.empty() && row(page, "camera-light-gain")->command.empty(),
                "The camera light waits for its settings");
            light.camera_light = {true, false, 1.0f, {255, 255, 255}};
            page = render(light, none, cb, player_section);
            check(row(page, "camera-light")->command == "camera-light" && row(page, "camera-light-color")->command.empty(),
                "Off, only the light itself can be pressed");
            light.camera_light.on = true;
            page = render(light, none, cb, player_section);
            check(row(page, "camera-light-gain")->title == "Light intensity: 100%" && row(page, "camera-light-color")->title == "Light color: White" &&
                  row(page, "camera-light-red")->title == "Red: 255", "The rows show the light's settings");
            activate(light, none, cb, row(page, "camera-light-gain")->command, row(page, "camera-light-gain")->argument);
            check(capture.command == "light gain 1.50", "Intensity takes the next step");
            light.camera_light.gain = 4.0f;
            activate(light, none, cb, "camera-light-gain", "");
            check(capture.command == "light gain 0.25", "Past the brightest, intensity starts again from the dimmest");
            activate(light, none, cb, "camera-light-color", "");
            check(capture.command == "light color 255 190 120", "White is followed by Warm");
            activate(light, none, cb, row(page, "camera-light-green")->command, row(page, "camera-light-green")->argument);
            check(capture.command == "light color 255 0 255", "Green at its most starts again from none");
            light.camera_light.color = {255, 0, 255};
            page = render(light, none, cb, player_section);
            check(row(page, "camera-light-color")->title == "Light color: Custom", "A colour set channel by channel is Custom");
            activate(light, none, cb, "camera-light-color", "");
            check(capture.command == "light color 255 255 255", "From Custom the colours start again from White");
            activate(light, none, cb, "camera-light-channel", "Blue");
            check(capture.command == "light color 255 0 0", "Blue at its most starts again from none");
        }
        {
            // Death Races: this map's races to pick, and a new one set flag by flag.
            State races;
            overlay::Model none;
            const auto row_of = [](const std::vector<Row>& rows, std::string_view id) -> const Row* {
                for (const auto& r : rows) if (r.id == id) return &r;
                return nullptr;
            };
            races.death_race.available = races.death_race.ready = true;
            races.death_race.races = {{"Race 1", 83.4, 2}, {"Race 2", 0.0, 0}};
            races.death_race.selected = 0;
            auto page = render(races, none, cb, death_race_section);
            check(row_of(page.main, "race-0") && row_of(page.main, "race-0")->title == "Race 1  /  best 1:23.40  /  SELECTED",
                "A race shows its best time and that it is selected");
            check(row_of(page.main, "race-1")->title == "Race 2  /  no time yet" && row_of(page.main, "race-1")->argument == "2", "Races are numbered from 1");
            activate(races, none, cb, "race-select", "2");
            check(capture.command == "race select 2", "Selecting a race selects it");
            activate(races, none, cb, "race-go", "");
            check(capture.command == "race go", "Going to a race teleports to its start");
            check(row_of(page.side, "race-new") && !row_of(page.side, "race-finish"), "A new race starts with its start flag");
            activate(races, none, cb, "race-new", "");
            check(capture.command == "race new", "The start flag is set");
            races.death_race.building = true;
            races.death_race.draft_points = 1;
            page = render(races, none, cb, death_race_section);
            check(row_of(page.side, "race-checkpoint") && row_of(page.side, "race-finish") && !row_of(page.side, "race-new"),
                "While building: checkpoints and the finish");
            check(row_of(page.side, "race-go")->command.empty(), "No teleport while a new race is being set");
            activate(races, none, cb, "race-finish", "");
            check(capture.command == "race finish-here", "The finish saves the race");
            races.death_race.racing = true;
            page = render(races, none, cb, death_race_section);
            check(row_of(page.side, "race-abort") && row_of(page.side, "race-abort")->command == "race-abort", "A race in progress can be aborted");
            races.death_race.ready = false;
            races.death_race.racing = false;
            page = render(races, none, cb, death_race_section);
            check(row_of(page.side, "race-checkpoint")->command.empty(), "Without a skater nothing can be set");
        }
        std::puts("Native ReSkate control checks passed.");
        return 0;
    } catch (const std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }
}
