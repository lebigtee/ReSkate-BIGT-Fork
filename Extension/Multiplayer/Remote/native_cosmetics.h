#pragma once
#include "native_skater.h"
namespace dingosdk::multiplayer {
std::optional<Appearance> capture_cosmetics(std::uintptr_t base, const NativeFrame &, std::string &detail);
// Only called for the owned remote actor on the verified client update thread.
void apply_cosmetic_recipe(std::uintptr_t base, std::uintptr_t entity, std::uintptr_t local_entity,
                           const CosmeticRecipe &);
// Puts `asset` in the first slot of `recipe` (the local skater's template) that takes its
// category. False when it is not installed or no slot takes it. Client update thread.
bool wear_cosmetic(std::uintptr_t base, std::uintptr_t local_entity, CosmeticRecipe &recipe, const std::string &asset) noexcept;
} // namespace dingosdk::multiplayer
