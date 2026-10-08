#include "death_race_format.h"
#include "Engine/Core/Json/json.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace dingosdk::death_race {
namespace {
constexpr std::size_t most_races = 100, most_checkpoints = 32;
// The relay's drop carries the race in this format.
constexpr std::uint8_t race_format = 1;

Json point_json(const Vec3 &p) { return Json::array({p[0], p[1], p[2]}); }
std::optional<Vec3> point_of(const Json &j) {
    if (!j.is_array() || j.size() != 3) return std::nullopt;
    Vec3 p{};
    for (std::size_t i = 0; i < 3; ++i) {
        if (!j.at(i).is_number()) return std::nullopt;
        p[i] = j.at(i).get<float>();
        if (!std::isfinite(p[i])) return std::nullopt;
    }
    return p;
}
} // namespace

RaceMaps races_from_json(std::string_view text) {
    const auto json = Json::parse(std::string(text));
    if (!json.is_object() || !json.contains("maps") || !json.at("maps").is_object()) throw std::runtime_error("not a races file");
    RaceMaps maps;
    for (const auto &[level, list] : json.at("maps").items()) {
        if (!list.is_array()) continue;
        auto &into = maps[level];
        for (std::size_t i = 0; i < list.size() && into.size() < most_races; ++i) {
            const auto &r = list.at(i);
            if (!r.is_object() || !r.contains("start") || !r.contains("finish")) continue;
            const auto start = point_of(r.at("start")), finish = point_of(r.at("finish"));
            if (!start || !finish) continue;
            Race race;
            race.name = r.value("name", std::string("Race"));
            if (race.name.empty() || race.name.size() > 64) race.name = "Race";
            race.start = *start;
            race.finish = *finish;
            if (r.contains("checkpoints") && r.at("checkpoints").is_array())
                for (std::size_t c = 0; c < r.at("checkpoints").size() && race.checkpoints.size() < most_checkpoints; ++c)
                    if (const auto p = point_of(r.at("checkpoints").at(c))) race.checkpoints.push_back(*p);
            race.best = std::max(0.0, r.value("best", 0.0));
            if (r.contains("heading") && r.at("heading").is_number()) {
                race.heading = r.at("heading").get<float>();
                race.has_heading = std::isfinite(race.heading);
            }
            into.push_back(std::move(race));
        }
    }
    return maps;
}

std::string races_to_json(const RaceMaps &maps) {
    auto out_maps = Json::object();
    for (const auto &[level, list] : maps) {
        if (list.empty()) continue;
        auto out = Json::array();
        for (const auto &race : list) {
            if (race.from) continue;
            auto r = Json::object();
            r["name"] = race.name;
            r["start"] = point_json(race.start);
            auto checkpoints = Json::array();
            for (const auto &c : race.checkpoints) checkpoints.push_back(point_json(c));
            r["checkpoints"] = std::move(checkpoints);
            r["finish"] = point_json(race.finish);
            r["best"] = race.best;
            if (race.has_heading) r["heading"] = race.heading;
            out.push_back(std::move(r));
        }
        out_maps[level] = std::move(out);
    }
    auto json = Json::object();
    json["schema"] = 1;
    json["maps"] = std::move(out_maps);
    return json.dump(2);
}

std::vector<std::uint8_t> encode_race(const std::string &level, const Race &race) {
    std::vector<std::uint8_t> out{race_format};
    const auto text = [&](const std::string &value, std::size_t most) {
        const auto n = std::min(value.size(), most);
        out.push_back(static_cast<std::uint8_t>(n));
        out.insert(out.end(), value.begin(), value.begin() + static_cast<std::ptrdiff_t>(n));
    };
    const auto number = [&](float value) {
        std::uint32_t bits{};
        std::memcpy(&bits, &value, sizeof(bits));
        for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(bits >> (8 * i)));
    };
    const auto point = [&](const Vec3 &p) {
        for (const auto v : p) number(v);
    };
    text(level, 255);
    text(race.name, 64);
    point(race.start);
    point(race.finish);
    number(race.heading);
    out.push_back(race.has_heading ? 1 : 0);
    const auto count = std::min(race.checkpoints.size(), most_checkpoints);
    out.push_back(static_cast<std::uint8_t>(count));
    for (std::size_t i = 0; i < count; ++i) point(race.checkpoints[i]);
    return out;
}

std::optional<std::pair<std::string, Race>> decode_race(const std::vector<std::uint8_t> &bytes) {
    std::size_t at{};
    bool ok = true;
    const auto byte = [&]() -> std::uint8_t {
        if (at >= bytes.size()) {
            ok = false;
            return 0;
        }
        return bytes[at++];
    };
    const auto text = [&](std::size_t most) {
        const std::size_t n = byte();
        if (!ok || n > most || n > bytes.size() - at) {
            ok = false;
            return std::string();
        }
        std::string value(reinterpret_cast<const char *>(bytes.data() + at), n);
        at += n;
        return value;
    };
    const auto number = [&]() {
        std::uint32_t bits{};
        for (int i = 0; i < 4; ++i) bits |= std::uint32_t{byte()} << (8 * i);
        float value{};
        std::memcpy(&value, &bits, sizeof(value));
        if (!std::isfinite(value) || std::abs(value) > 1e6f) ok = false;
        return value;
    };
    const auto point = [&]() {
        Vec3 p{};
        for (auto &v : p) v = number();
        return p;
    };
    if (byte() != race_format) return std::nullopt;
    std::pair<std::string, Race> out;
    out.first = text(255);
    auto &race = out.second;
    race.name = text(64);
    race.start = point();
    race.finish = point();
    race.heading = number();
    race.has_heading = byte() != 0;
    const std::size_t count = byte();
    if (count > most_checkpoints) return std::nullopt;
    for (std::size_t i = 0; i < count && ok; ++i) race.checkpoints.push_back(point());
    if (!ok || at != bytes.size() || race.name.empty()) return std::nullopt;
    return out;
}

float lane_offset(std::size_t lane) {
    if (!lane) return 0.0f;
    const float step = lane_spacing * static_cast<float>((lane + 1) / 2);
    return lane % 2 ? step : -step;
}
} // namespace dingosdk::death_race
