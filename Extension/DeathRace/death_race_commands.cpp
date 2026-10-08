#include "death_race_internal.h"
#include "Extension/Console/commands.h"
#include "Extension/Trainer/trainer.h"
#include <charconv>
#include <format>

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

namespace dingosdk::death_race {
namespace {
using namespace detail;
std::optional<int> index_of(const std::vector<std::string> &arguments) {
    if (arguments.empty()) return std::nullopt;
    int value{};
    const auto &text = arguments[0];
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value < 1) return std::nullopt;
    return value - 1;
}
} // namespace

std::string command(std::string_view verb, const std::vector<std::string> &arguments) {
    auto &s = state();
    load(s);
    auto &list = races();
    const auto need_skater = [&]() -> std::string {
        if (s.level.empty() || !s.skater) return "error: no skater in play.";
        return {};
    };
    if (verb == "status" || verb == "list") {
        std::string text = std::format("Death Race: {} race(s) on this map{}.", list.size(),
                                       s.building ? std::format("; building one ({} point(s) set)", s.draft.size()) : "");
        for (std::size_t i = 0; i < list.size(); ++i)
            text += std::format("\n  {}. {} - {} checkpoint(s), best {}{}", i + 1, list[i].name, list[i].checkpoints.size(),
                                list[i].best > 0 ? clock_text(list[i].best) : std::string("none"), list[i].from ? " (another player's)" : "");
        return text;
    }
    if (verb == "new" || verb == "start-here") {
        if (auto e = need_skater(); !e.empty()) return e;
        if (s.phase == Phase::countdown || s.phase == Phase::racing) return "error: finish or abort the race first.";
        s.building = true;
        s.draft = {s.position};
        s.draft_heading = s.heading;
        // The ride to the finish is the race's first AI line.
        line_start(seconds());
        s.recording = State::Recording::route;
        return "Start flag set. Add checkpoints if you like, then ride to the finish and set it.";
    }
    if (verb == "checkpoint") {
        if (auto e = need_skater(); !e.empty()) return e;
        if (!s.building || s.draft.empty()) return "error: set a start first.";
        if (s.draft.size() > 32) return "error: 32 checkpoints at most.";
        if (across(s.position, s.draft.back()) < flag_radius * 2) return "error: too close to the last flag.";
        s.draft.push_back(s.position);
        return std::format("Checkpoint {} set.", s.draft.size() - 1);
    }
    if (verb == "finish-here") {
        if (auto e = need_skater(); !e.empty()) return e;
        if (!s.building || s.draft.empty()) return "error: set a start first.";
        if (across(s.position, s.draft.back()) < flag_radius * 2) return "error: too close to the last flag.";
        Race race;
        race.name = next_name(list);
        race.start = s.draft.front();
        race.checkpoints.assign(s.draft.begin() + 1, s.draft.end());
        race.finish = s.position;
        race.heading = s.draft_heading;
        race.has_heading = true;
        s.selected = add_race(s, std::move(race));
        s.building = false;
        s.draft.clear();
        s.armed_away = false; // not straight away, if the finish is near a start
        if (s.recording == State::Recording::route) {
            s.recording = State::Recording::none;
            if (auto line = line_stop()) s.lines[s.level + "/" + list[s.selected].name] = std::move(line);
        }
        save(s);
        return std::format("{} saved. Stop on its start flag to race it.", list[s.selected].name);
    }
    if (verb == "cancel") {
        s.building = false;
        s.draft.clear();
        if (s.recording == State::Recording::route) {
            line_stop();
            s.recording = State::Recording::none;
        }
        return "New race cancelled.";
    }
    if (verb == "solo") {
        const auto choice = arguments.empty() ? std::string("status") : arguments[0];
        if (choice == "on" || choice == "off") s.solo_on_stop = choice == "on";
        return std::string("Racing alone by stopping on the start line: ") + (s.solo_on_stop ? "on." : "off.");
    }
    if (verb == "hide") {
        if (s.phase == Phase::countdown || s.phase == Phase::racing) return "error: finish or abort the race first.";
        s.selected = -1;
        return "Race props put away.";
    }
    if (verb == "select") {
        const auto i = index_of(arguments);
        if (!i || *i >= static_cast<int>(list.size())) return "error: no such race.";
        s.selected = *i;
        return std::format("{} selected.", list[*i].name);
    }
    if (verb == "go") {
        if (auto e = need_skater(); !e.empty()) return e;
        const auto i = arguments.empty() ? std::optional<int>(s.selected) : index_of(arguments);
        if (!i || *i < 0 || *i >= static_cast<int>(list.size())) return "error: no such race.";
        if (s.phase == Phase::countdown || s.phase == Phase::racing) stop(s, "restarted");
        s.selected = *i;
        s.armed_away = true;
        const auto &start = list[*i].start;
        // The trainer's teleport keeps a session's teleport rules.
        return trainer::command("tp", {std::format("{:.2f}", start[0]), std::format("{:.2f}", start[1] + 0.5f), std::format("{:.2f}", start[2])});
    }
    if (verb == "delete") {
        const auto i = arguments.empty() ? std::optional<int>(s.selected) : index_of(arguments);
        if (!i || *i < 0 || *i >= static_cast<int>(list.size())) return "error: no such race.";
        if (list[*i].from) return "error: that race is another player's.";
        if (s.active == *i) stop(s, "deleted");
        const auto name = list[*i].name;
        list.erase(list.begin() + *i);
        s.selected = -1;
        if (s.active > *i) --s.active;
        save(s);
        return name + " deleted.";
    }
    if (verb == "abort") {
        if (s.phase != Phase::countdown && s.phase != Phase::racing) return "No race in progress.";
        stop(s, "aborted");
        return "Race aborted.";
    }
    return "error: unknown command.";
}
} // namespace dingosdk::death_race
