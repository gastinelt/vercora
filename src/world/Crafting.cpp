#include "world/Crafting.hpp"

#include "world/Ground.hpp"
#include "world/Inventory.hpp"
#include "world/Knapping.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>

namespace vercora {
namespace {

constexpr glm::vec3 kFallbackFacing{ 1.0f, 0.0f, 0.0f };

// Meme projection que knapStones : longueur testee AVANT de normaliser.
glm::vec3 flatFacing(const glm::vec3& facing) {
    glm::vec3 flat(facing.x, 0.0f, facing.z);
    const float flatLength = std::sqrt(flat.x * flat.x + flat.z * flat.z);
    return (flatLength > 1e-4f) ? flat / flatLength : kFallbackFacing;
}

float horizontalDistanceSq(const glm::vec3& a, const glm::vec3& b) {
    const float dx = a.x - b.x;
    const float dz = a.z - b.z;
    return dx * dx + dz * dz;
}

// Besoin agrege d'une recette : total par nature et par source (deux lignes de
// la meme nature, comme le nucleus et le percuteur, s'additionnent).
struct Need {
    std::array<int, kGroundItemKindCount> carried{};
    std::array<int, kGroundItemKindCount> placed{};
};

Need needOf(const Recipe& recipe) {
    Need need;
    for (uint8_t i = 0; i < recipe.inCount; ++i) {
        const CraftIngredient& ing = recipe.in[i];
        auto& table = (ing.source == CraftSource::Carried) ? need.carried : need.placed;
        table[static_cast<std::size_t>(ing.kind)] += ing.count;
    }
    return need;
}

std::string plural(GroundItemKind kind, int n) {
    return (n > 1) ? std::to_string(n) + " " + groundItemLabelPlural(kind)
                   : std::string(groundItemLabelIndefinite(kind));
}

} // namespace

int axeBonusLogs(const Inventory& inventory) {
    if (inventory.count(GroundItemKind::PitchedAxe) > 0) {
        return kPitchedAxeBonusLogs;
    }
    return inventory.count(GroundItemKind::Axe) > 0 ? kAxeBonusLogs : 0;
}

bool shelterNear(const Ground& ground, const glm::vec3& position) {
    // Boucle directe, nature testee EN PREMIER : appelee a chaque image, elle ne
    // doit rien couter quand aucun abri n'existe (cas de tous les scenarios
    // anterieurs).
    const float reachSq = kShelterReach * kShelterReach;
    for (const GroundItem& item : ground.items()) {
        if (item.kind != GroundItemKind::Shelter) {
            continue;
        }
        if (std::abs(item.position.y - position.y) > kShelterVerticalTolerance) {
            continue;
        }
        if (horizontalDistanceSq(item.position, position) <= reachSq) {
            return true;
        }
    }
    return false;
}

int findRecipe(const char* id) {
    for (std::size_t i = 0; i < kRecipes.size(); ++i) {
        if (std::strcmp(kRecipes[i].id, id) == 0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// ---------------------------------------------------------------------------
// HearthSet
// ---------------------------------------------------------------------------
void HearthSet::addLit(const glm::vec3& position, float nowHours) {
    HearthState state;
    state.position = position;
    state.lit = true;
    state.burnsUntilHours = nowHours + params_.burnHours;
    hearths_.push_back(state);
}

int HearthSet::nearest(const glm::vec3& position, float reach, bool wantLit) const {
    int best = -1;
    float bestSq = reach * reach;
    for (std::size_t i = 0; i < hearths_.size(); ++i) {
        if (hearths_[i].lit != wantLit) {
            continue;
        }
        const float d = horizontalDistanceSq(hearths_[i].position, position);
        if (d <= bestSq) {
            bestSq = d;
            best = static_cast<int>(i);
        }
    }
    return best;
}

void HearthSet::relight(int index, float nowHours) {
    if (index < 0 || static_cast<std::size_t>(index) >= hearths_.size()) {
        return;
    }
    HearthState& h = hearths_[static_cast<std::size_t>(index)];
    h.lit = true;
    h.burnsUntilHours = nowHours + params_.burnHours;
}

void HearthSet::feed(int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= hearths_.size()) {
        return;
    }
    HearthState& h = hearths_[static_cast<std::size_t>(index)];
    if (h.lit) {
        h.burnsUntilHours += params_.feedHours;
    }
}

int HearthSet::update(float nowHours, Ground& ground) {
    int extinguished = 0;
    for (HearthState& h : hearths_) {
        if (h.lit && nowHours >= h.burnsUntilHours) {
            h.lit = false;
            // La cendre se pose au CENTRE du foyer. Le foyer n'etant pas
            // ramassable (findNearest l'ignore), c'est bien elle que R attrape.
            ground.drop(GroundItemKind::Ash, h.position.x, h.position.z, 0.0f, 0.0f);
            ++extinguished;
        }
    }
    return extinguished;
}

float HearthSet::remainingHours(int index, float nowHours) const {
    if (index < 0 || static_cast<std::size_t>(index) >= hearths_.size()) {
        return 0.0f;
    }
    const HearthState& h = hearths_[static_cast<std::size_t>(index)];
    return h.lit ? std::max(0.0f, h.burnsUntilHours - nowHours) : 0.0f;
}

float HearthSet::warmthAt(const glm::vec3& position) const {
    if (params_.warmthRadius <= 0.0f) {
        return 0.0f;
    }
    float total = 0.0f;
    for (const HearthState& h : hearths_) {
        if (!h.lit) {
            continue;
        }
        const float d = std::sqrt(horizontalDistanceSq(h.position, position));
        total += params_.warmthC * std::max(0.0f, 1.0f - d / params_.warmthRadius);
    }
    return total;
}

// ---------------------------------------------------------------------------
// craftAt
// ---------------------------------------------------------------------------
CraftOutcome craftAt(Inventory& inventory, Ground& ground, HearthSet& hearths,
                     const glm::vec3& playerPosition, const glm::vec3& facing,
                     float nowHours) {
    // 1. Ce qui est POSE a portee, par nature (les objets fixes n'en sont pas).
    std::array<std::vector<int>, kGroundItemKindCount> placedByKind;
    for (const int index : ground.indicesWithin(playerPosition, kCraftReach)) {
        const GroundItem& item = ground.items()[static_cast<std::size_t>(index)];
        if (groundItemIsPortable(item.kind)) {
            placedByKind[static_cast<std::size_t>(item.kind)].push_back(index);
        }
    }
    // Les plus proches d'abord : c'est eux qui seront consommes.
    for (auto& list : placedByKind) {
        std::sort(list.begin(), list.end(), [&](int a, int b) {
            const float da = horizontalDistanceSq(ground.items()[static_cast<std::size_t>(a)].position, playerPosition);
            const float db = horizontalDistanceSq(ground.items()[static_cast<std::size_t>(b)].position, playerPosition);
            return (da != db) ? da < db : a < b;
        });
    }

    const int litHearth = hearths.nearest(playerPosition, kStationReach, true);
    const int coldHearth = hearths.nearest(playerPosition, kStationReach, false);

    // 2. Premiere recette satisfaite ; sinon, la plus proche (moins d'unites
    //    manquantes ; a egalite, celle dont le joueur a DEJA reuni le plus
    //    d'unites ; a egalite encore, la plus SIMPLE, donc la plus loin dans la
    //    table - c'est ce qui rend, les mains vides, le refus historique de la
    //    percussion).
    int chosen = -1;
    int closest = -1;
    int closestMissing = 1 << 30;
    int closestSatisfied = -1;
    for (std::size_t r = 0; r < kRecipes.size(); ++r) {
        const Recipe& recipe = kRecipes[r];
        const Need need = needOf(recipe);
        int missing = 0;
        int satisfied = 0;
        for (uint32_t k = 0; k < kGroundItemKindCount; ++k) {
            const auto kind = static_cast<GroundItemKind>(k);
            const int carried = inventory.count(kind);
            const int placed = static_cast<int>(placedByKind[k].size());
            missing += std::max(0, need.carried[k] - carried);
            missing += std::max(0, need.placed[k] - placed);
            satisfied += std::min(need.carried[k], carried) + std::min(need.placed[k], placed);
        }
        if (recipe.station == CraftStation::LitHearth && litHearth < 0) ++missing;
        if (recipe.station == CraftStation::ColdHearth && coldHearth < 0) ++missing;
        if (missing == 0) {
            chosen = static_cast<int>(r);
            break;
        }
        if (missing < closestMissing ||
            (missing == closestMissing && satisfied >= closestSatisfied)) {
            closestMissing = missing;
            closestSatisfied = satisfied;
            closest = static_cast<int>(r);
        }
    }

    CraftOutcome outcome;
    if (chosen < 0) {
        outcome.result = CraftResult::MissingIngredients;
        outcome.recipe = closest;
        const Recipe& recipe = kRecipes[static_cast<std::size_t>(closest)];
        // La percussion garde MOT POUR MOT son message historique (Phase 6).
        if (std::strcmp(recipe.id, "knap") == 0) {
            outcome.message = knapResultMessage(KnapResult::NotEnoughStones);
            return outcome;
        }
        const Need need = needOf(recipe);
        std::string list;
        const auto append = [&](const std::string& part) {
            list += list.empty() ? part : ", " + part;
        };
        for (uint32_t k = 0; k < kGroundItemKindCount; ++k) {
            const auto kind = static_cast<GroundItemKind>(k);
            const int c = need.carried[k] - inventory.count(kind);
            if (c > 0) append(plural(kind, c) + " (a porter)");
            const int p = need.placed[k] - static_cast<int>(placedByKind[k].size());
            if (p > 0) append(plural(kind, p) + " (a poser a moins de 2,5 m)");
        }
        if (recipe.station == CraftStation::LitHearth && litHearth < 0) {
            append("un foyer allume a moins de 2 m (il faut chauffer)");
        }
        if (recipe.station == CraftStation::ColdHearth && coldHearth < 0) {
            append("un foyer eteint a moins de 2 m");
        }
        outcome.message = std::string("Faconner (") + recipe.label + ") : il manque " +
                          list + ".";
        return outcome;
    }

    // 3. Execution. Tout a ete verifie : a partir d'ici plus aucun refus.
    const Recipe& recipe = kRecipes[static_cast<std::size_t>(chosen)];
    std::vector<int> toRemove;
    glm::vec3 barycenter(0.0f);
    for (uint8_t i = 0; i < recipe.inCount; ++i) {
        const CraftIngredient& ing = recipe.in[i];
        if (!ing.consumed) {
            continue;
        }
        if (ing.source == CraftSource::Carried) {
            inventory.remove(ing.kind, ing.count);
        } else {
            auto& list = placedByKind[static_cast<std::size_t>(ing.kind)];
            for (uint8_t n = 0; n < ing.count; ++n) {
                const int index = list.front();
                list.erase(list.begin());
                toRemove.push_back(index);
                barycenter += ground.items()[static_cast<std::size_t>(index)].position;
            }
        }
    }
    if (!toRemove.empty()) {
        barycenter /= static_cast<float>(toRemove.size());
    }
    // Indices DECROISSANTS : pickUp echange avec le dernier, un ordre croissant
    // deplacerait un objet a consommer sous un indice deja traite.
    std::sort(toRemove.begin(), toRemove.end(), std::greater<int>());
    for (const int index : toRemove) {
        ground.pickUp(index);
    }

    const glm::vec3 flat = flatFacing(facing);
    if (recipe.hasOutput) {
        float x = playerPosition.x + flat.x * kCraftDropDistance;
        float z = playerPosition.z + flat.z * kCraftDropDistance;
        if (!toRemove.empty()) {
            x = barycenter.x;
            z = barycenter.z;
        }
        ground.drop(recipe.out, x, z, std::atan2(-flat.z, flat.x), 0.0f);
        if (recipe.effect == CraftEffect::LightNewHearth) {
            hearths.addLit(ground.items().back().position, nowHours);
        }
    }
    if (recipe.effect == CraftEffect::RelightHearth) {
        hearths.relight(coldHearth, nowHours);
    } else if (recipe.effect == CraftEffect::FeedHearth) {
        hearths.feed(litHearth);
    }

    outcome.result = CraftResult::Success;
    outcome.recipe = chosen;
    outcome.message = recipe.successMessage;
    return outcome;
}

// ---------------------------------------------------------------------------
// Reposer
// ---------------------------------------------------------------------------
bool chooseKindToPutDown(const Inventory& inventory, GroundItemKind preferred,
                         GroundItemKind* kindOut) {
    if (inventory.count(preferred) > 0 && groundItemIsPortable(preferred)) {
        if (kindOut != nullptr) *kindOut = preferred;
        return true;
    }
    for (uint32_t k = 0; k < kGroundItemKindCount; ++k) {
        const auto kind = static_cast<GroundItemKind>(k);
        if (inventory.count(kind) > 0 && groundItemIsPortable(kind)) {
            if (kindOut != nullptr) *kindOut = kind;
            return true;
        }
    }
    return false;
}

bool putDown(Inventory& inventory, Ground& ground, GroundItemKind kind,
             const glm::vec3& playerPosition, const glm::vec3& facing) {
    if (!groundItemIsPortable(kind) || inventory.count(kind) <= 0) {
        return false;
    }
    const glm::vec3 flat = flatFacing(facing);
    const glm::vec3 spot{ playerPosition.x + flat.x * kPutDownDistance, playerPosition.y,
                          playerPosition.z + flat.z * kPutDownDistance };
    // Decalage en spirale de Vogel (angle d'or) selon le nombre d'objets deja
    // poses pres du point : n = 0 -> au point exact, puis 0,25 m x sqrt(n).
    constexpr float kGoldenAngle = 2.39996323f;
    constexpr float kSpacing = 0.25f;
    const int n = static_cast<int>(ground.indicesWithin(spot, 0.6f).size());
    const float radius = kSpacing * std::sqrt(static_cast<float>(n));
    const float angle = kGoldenAngle * static_cast<float>(n);
    const float x = spot.x + radius * std::cos(angle);
    const float z = spot.z + radius * std::sin(angle);

    inventory.remove(kind, 1);
    ground.drop(kind, x, z, std::atan2(-flat.z, flat.x), 0.0f);
    return true;
}

} // namespace vercora
