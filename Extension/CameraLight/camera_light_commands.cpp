#include "camera_light.h"
#include "Extension/Console/commands.h"

// The `light` console commands: the camera light's settings (also in the pause menu's PLAYER page).
namespace dingosdk::console {
void register_camera_light_commands(Commands &registry) {
    struct Verb {
        const char *name;
        const char *description;
        const char *usage;
    };
    const Verb verbs[]{
        {"on", "Light the skater at night as if a lamp sat on the camera", nullptr},
        {"off", "Turn the camera light off", nullptr},
        {"status", "The camera light's settings", nullptr},
        {"gain", "How bright the camera light is (1 by default)", "<0.25-4>"},
    };
    for (const auto &verb : verbs) {
        std::vector<Argument> arguments;
        if (verb.usage) {
            auto value = argument(verb.usage, Type::text, false);
            value.rest = true;
            arguments.push_back(std::move(value));
        }
        auto entry = action(std::string("light ") + verb.name, verb.description, Group::gameplay, std::move(arguments));
        entry.run = [name = std::string(verb.name)](const Model &, const Values &values, const Output &out) {
            std::vector<std::string> words;
            if (!values.empty())
                if (const auto *text = std::get_if<std::string>(&values[0]); text && !text->empty()) words.push_back(*text);
            out(camera_light::command(name, words));
        };
        registry.add(std::move(entry));
    }
}
} // namespace dingosdk::console
