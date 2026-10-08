#include "death_race.h"
#include "Extension/Console/commands.h"

// The `race` console commands: everything the pause menu's Death Race page does.
namespace dingosdk::console {
void register_death_race_commands(Commands &registry) {
    struct Verb {
        const char *name;
        const char *description;
        const char *usage;
    };
    const Verb verbs[]{
        {"status", "This map's Death Races and their best times", nullptr},
        {"new", "Set a new race's start flag where you stand", nullptr},
        {"checkpoint", "Add a checkpoint to the new race where you stand", nullptr},
        {"finish-here", "Set the new race's finish where you stand and save it", nullptr},
        {"cancel", "Drop the new race", nullptr},
        {"select", "Select a race on this map: its props stand in the world", "<number>"},
        {"hide", "Put the selected race's props away", nullptr},
        {"solo", "Race alone by stopping on the start line (off by default)", "[on|off]"},
        {"go", "Teleport to a race's start (the selected one by default)", "[number]"},
        {"delete", "Delete a race (the selected one by default)", "[number]"},
        {"abort", "Abort the race in progress", nullptr},
    };
    for (const auto &verb : verbs) {
        std::vector<Argument> arguments;
        if (verb.usage) {
            auto rest = argument(verb.usage, Type::text, verb.usage[0] == '[');
            rest.rest = true;
            arguments.push_back(std::move(rest));
        }
        auto entry = action(std::string("race ") + verb.name, verb.description, Group::gameplay, std::move(arguments));
        entry.run = [name = std::string(verb.name)](const Model &, const Values &values, const Output &out) {
            std::vector<std::string> words;
            if (!values.empty())
                if (const auto *text = std::get_if<std::string>(&values[0])) {
                    std::size_t start = 0;
                    while (start < text->size()) {
                        auto end = text->find(' ', start);
                        if (end == std::string::npos) end = text->size();
                        if (end > start) words.push_back(text->substr(start, end - start));
                        start = end + 1;
                    }
                }
            out(death_race::command(name, words));
        };
        registry.add(std::move(entry));
    }
}
} // namespace dingosdk::console
