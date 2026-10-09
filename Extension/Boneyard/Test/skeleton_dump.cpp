// Finds and prints the game's skeleton assets, to build boneyard_skeleton.h from the
// Animation/Dingo/AnimBase_Default_Skeleton the skater's pose uses.
//   dingosdk_boneyard_skeleton_dump <Skate folder> [<word>]          ebx names containing the word ("skeleton")
//   dingosdk_boneyard_skeleton_dump <Skate folder> --dump <asset name>  every field of the asset (with its offset
//                                                                           in its instance), arrays in full
#include "Engine/Resource/ebx_document.h"
#include "Engine/Vfs/game_bundles.h"
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <variant>

namespace ebx = dingosdk::frostbite::ebx;
namespace fs = std::filesystem;
namespace {
std::string lower(std::string_view text) {
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}
// The document being printed, for its fields' offsets.
const ebx::Document *printed{};
void print(const ebx::Value &value, int depth, const std::string &indent);
void print(const ebx::Object &object, int depth, const std::string &indent) {
    for (const auto &field : object.fields) {
        std::cout << indent << (field.name.empty() ? "?" : field.name);
        if (printed && field.descriptor < printed->fields.size()) std::cout << " (+0x" << std::hex << printed->fields[field.descriptor].dataOffset << std::dec << ")";
        std::cout << " = ";
        print(field.value, depth, indent);
    }
}
void print(const ebx::Value &value, int depth, const std::string &indent) {
    if (depth > 16) { std::cout << "...\n"; return; }
    if (const auto *b = std::get_if<bool>(&value.data)) std::cout << (*b ? "true" : "false") << '\n';
    else if (const auto *i = std::get_if<std::int64_t>(&value.data)) std::cout << *i << '\n';
    else if (const auto *u = std::get_if<std::uint64_t>(&value.data)) std::cout << *u << '\n';
    else if (const auto *d = std::get_if<double>(&value.data)) std::cout << *d << '\n';
    else if (const auto *s = std::get_if<std::string>(&value.data)) std::cout << '"' << *s << "\"\n";
    else if (const auto *r = std::get_if<ebx::PointerReference>(&value.data))
        std::cout << (r->kind == ebx::PointerKind::null ? "null" : r->kind == ebx::PointerKind::internal ? "-> instance " : "-> import ") << r->index << '\n';
    else if (const auto *o = std::get_if<std::shared_ptr<ebx::Object>>(&value.data)) {
        std::cout << "{\n";
        if (*o) print(**o, depth + 1, indent + "  ");
        std::cout << indent << "}\n";
    } else if (const auto *a = std::get_if<ebx::Value::Array>(&value.data)) {
        std::cout << "[" << a->size() << "]\n";
        std::size_t index{};
        for (const auto &element : *a) {
            std::cout << indent << "  [" << index++ << "] ";
            print(element, depth + 1, indent + "    ");
        }
    } else std::cout << "(other)\n";
}
} // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        std::cerr << "usage: dingosdk_boneyard_skeleton_dump <Skate folder> [<word> | --dump <asset name>]\n";
        return 2;
    }
    try {
        const fs::path game_root(argv[1]);
        const dingosdk::vfs::GameData data(game_root);
        const bool dump = argc >= 4 && std::string(argv[2]) == "--dump";
        const auto wanted = lower(dump ? argv[3] : argc >= 3 ? argv[2] : "skeleton");
        const auto data_root = game_root / "Data";
        for (const auto &entry : fs::recursive_directory_iterator(data_root)) {
            if (!entry.is_regular_file() || lower(entry.path().extension().string()) != ".toc") continue;
            const auto relative = fs::relative(entry.path(), data_root).generic_string();
            dingosdk::frostbite::TocDocument toc;
            try { toc = data.read_toc(relative); } catch (const std::exception &error) {
                std::cerr << relative << ": " << error.what() << "\n";
                continue;
            }
            for (const auto &listed : toc.bundles) {
                std::optional<dingosdk::vfs::GameBundle> bundle;
                try { bundle = data.read_bundle(toc, listed.name); } catch (...) { continue; }
                if (!bundle) continue;
                for (std::size_t i = 0; i < bundle->manifest.ebx.size(); ++i) {
                    const auto name = lower(bundle->manifest.ebx[i].name);
                    if (!dump) {
                        if (name.find(wanted) != std::string::npos)
                            std::cout << bundle->manifest.ebx[i].name << "\t" << relative << "\t" << listed.name << "\n";
                        continue;
                    }
                    if (name != wanted) continue;
                    const auto *payload = bundle->payload(dingosdk::frostbite::AssetKind::ebx, i);
                    if (!payload) continue;
                    const auto document = ebx::read_document(data.read(*payload));
                    std::cerr << bundle->manifest.ebx[i].name << " (" << relative << ", " << listed.name << "): root type "
                              << document.rootType << ", " << document.instances.size() << " instances\n";
                    std::size_t number{};
                    printed = &document;
                    for (const auto &instance : document.instances) {
                        const auto type = instance.descriptor >= 0 && static_cast<std::size_t>(instance.descriptor) < document.types.size()
                            ? document.types[static_cast<std::size_t>(instance.descriptor)].name : std::string("?");
                        std::cout << "# instance " << number++ << " " << type << "\n";
                        if (instance.object) print(*instance.object, 0, "  ");
                    }
                    return 0;
                }
            }
        }
        if (dump) { std::cerr << "no such asset\n"; return 1; }
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "error: " << error.what() << "\n";
        return 1;
    }
}
