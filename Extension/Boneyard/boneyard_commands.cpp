#include "boneyard.h"
#include "Extension/Console/commands.h"

// The `boneyard` console commands: every Boneyard setting, and the tools to try it out.
namespace dingosdk::console {
void register_boneyard_commands(Commands &registry) {
    struct Verb {
        const char *name;
        const char *description;
        const char *usage;
    };
    const Verb verbs[]{
        {"status", "What the Boneyard is doing", nullptr},
        {"on", "Show the bones you break when you bail", nullptr},
        {"off", "Turn the Boneyard off", nullptr},
        {"xray", "Draw the skeleton over the skater during a bail", "0|1|always"},
        {"slowmo", "Slow motion as a bail starts: the game alone, a replay on your screen in a multiplayer session", "0|1"},
        {"stutter", "Make the picture jerky as a bail starts (alone; the replay always is)", "0|1"},
        {"glow", "A glow round the X-ray's bones, as on a lit film", "0|1"},
        {"sensitivity", "How easily bones break: 1 is normal, higher breaks more", "<0.25-4>"},
        {"duration", "Seconds the result stays once the skater is up again", "<0.5-30>"},
        {"drop", "Teleport the skater up to fall and bail (default 12 m)", "[metres]"},
        {"last", "Every bone's impact in the last bail", nullptr},
        {"probe", "Record each bail with the game's own contact records (reverse engineering)", "on|off"},
        {"demo", "Show a made-up result on the current pose", nullptr},
    };
    for (const auto &verb : verbs) {
        std::vector<Argument> arguments;
        if (verb.usage) {
            auto rest = argument(verb.usage, Type::text, true);
            rest.rest = true;
            arguments.push_back(std::move(rest));
        }
        auto entry = action(std::string("boneyard ") + verb.name, verb.description, Group::gameplay, std::move(arguments));
        // Its first name's commands still work.
        entry.aliases = {std::string("meat ") + verb.name};
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
            out(boneyard::command(name, words));
        };
        registry.add(std::move(entry));
    }
}
} // namespace dingosdk::console
