#include "world/Ground.hpp"

#include "core/HeightSampler.hpp"
#include "world/CraftMesh.hpp"
#include "world/HarvestMesh.hpp"
#include "world/StoneMesh.hpp"
#include "world/TreeMesh.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace vercora {
namespace {

constexpr float kPi = 3.14159265359f;

} // namespace

// ---------------------------------------------------------------------------
// GroundItem
// ---------------------------------------------------------------------------
float groundItemRestRadius(GroundItemKind kind) {
    switch (kind) {
        case GroundItemKind::Log:    return kLogRadius;
        case GroundItemKind::Branch: return kBranchRadius;
        case GroundItemKind::Stone:  return kStoneRadius;
        case GroundItemKind::Flake:  return kFlakeRadius;
        // PHASE 22. Les quatre nouvelles matieres sont de PETITS objets poses a
        // plat : leur demi-hauteur vient de world/HarvestMesh.hpp, exactement
        // comme celle du galet vient de world/StoneMesh.hpp.
        case GroundItemKind::Clay:   return kClayRadius;
        case GroundItemKind::Sand:   return kSandRadius;
        case GroundItemKind::Resin:  return kResinRadius;
        case GroundItemKind::Fiber:  return kFiberBundleRadius;
        // PHASE 28 : objets faconnes, demi-hauteurs dans world/CraftMesh.hpp.
        case GroundItemKind::Cord:   return kCordRadius;
        case GroundItemKind::Axe:    return kAxeRadius;
        case GroundItemKind::Hearth: return kHearthRadius;
        case GroundItemKind::Ash:    return kAshRadius;
        case GroundItemKind::Pitch:  return kPitchRadius;
        case GroundItemKind::PitchedAxe: return kAxeRadius;
        case GroundItemKind::Shelter: return kShelterRadius;
    }
    return 0.0f;
}

const char* groundItemLabel(GroundItemKind kind) {
    switch (kind) {
        case GroundItemKind::Log:    return "buche";
        case GroundItemKind::Branch: return "branche";
        case GroundItemKind::Stone:  return "pierre";
        case GroundItemKind::Flake:  return "eclat";
        case GroundItemKind::Clay:   return "motte d'argile";
        case GroundItemKind::Sand:   return "poignee de sable";
        case GroundItemKind::Resin:  return "resine";
        case GroundItemKind::Fiber:  return "tige a fibres";
        case GroundItemKind::Cord:   return "ligature";
        case GroundItemKind::Axe:    return "hache";
        case GroundItemKind::Hearth: return "foyer";
        case GroundItemKind::Ash:    return "cendre";
        case GroundItemKind::Pitch:  return "poix";
        case GroundItemKind::PitchedAxe: return "hache renforcee";
        case GroundItemKind::Shelter: return "abri";
    }
    return "objet";
}

const char* groundItemLabelIndefinite(GroundItemKind kind) {
    switch (kind) {
        case GroundItemKind::Log:    return "une buche";
        case GroundItemKind::Branch: return "une branche";
        case GroundItemKind::Stone:  return "une pierre";
        case GroundItemKind::Flake:  return "un eclat";
        // Le genre fait partie du libelle, pas de la phrase appelante (voir
        // l'en-tete de GroundItem.hpp) : "une motte", "un peu de resine".
        case GroundItemKind::Clay:   return "une motte d'argile";
        case GroundItemKind::Sand:   return "une poignee de sable";
        case GroundItemKind::Resin:  return "un peu de resine";
        case GroundItemKind::Fiber:  return "une tige a fibres";
        case GroundItemKind::Cord:   return "une ligature";
        case GroundItemKind::Axe:    return "une hache";
        case GroundItemKind::Hearth: return "un foyer";
        case GroundItemKind::Ash:    return "un peu de cendre";
        case GroundItemKind::Pitch:  return "un peu de poix";
        case GroundItemKind::PitchedAxe: return "une hache renforcee";
        case GroundItemKind::Shelter: return "un abri";
    }
    return "un objet";
}

const char* groundItemLabelPlural(GroundItemKind kind) {
    switch (kind) {
        case GroundItemKind::Log:    return "buches";
        case GroundItemKind::Branch: return "branches";
        case GroundItemKind::Stone:  return "pierres";
        case GroundItemKind::Flake:  return "eclats";
        case GroundItemKind::Clay:   return "argile";
        case GroundItemKind::Sand:   return "sable";
        case GroundItemKind::Resin:  return "resine";
        case GroundItemKind::Fiber:  return "fibres";
        case GroundItemKind::Cord:   return "ligatures";
        case GroundItemKind::Axe:    return "haches";
        case GroundItemKind::Hearth: return "foyers";
        case GroundItemKind::Ash:    return "cendre";
        case GroundItemKind::Pitch:  return "poix";
        case GroundItemKind::PitchedAxe: return "haches renforcees";
        case GroundItemKind::Shelter: return "abris";
    }
    return "objets";
}

glm::mat4 GroundItem::modelMatrix() const {
    // Les meshes couches (buche, branche, eclat) sont generes le long de +X. Une
    // rotation d'angle a autour de Y envoie +X sur (cos a, 0, -sin a) : pour
    // aligner l'objet sur une direction donnee, l'appelant fournit donc
    // yaw = atan2(-dz, dx).
    // Le roll ne fait que tourner l'objet sur lui-meme (l'orientation de sa face
    // de coupe), il est purement esthetique.
    glm::mat4 model = glm::translate(glm::mat4(1.0f), position);
    model = glm::rotate(model, yaw, glm::vec3(0.0f, 1.0f, 0.0f));
    model = glm::rotate(model, roll, glm::vec3(1.0f, 0.0f, 0.0f));
    return model;
}

// ---------------------------------------------------------------------------
// Ground
// ---------------------------------------------------------------------------
Ground::Ground(const HeightSampler& terrain, uint32_t seed)
    : terrain_(terrain), rng_(seed) {}

void Ground::drop(GroundItemKind kind, float worldX, float worldZ, float yaw, float roll) {
    GroundItem item;
    item.kind = kind;
    item.position = { worldX,
                      terrain_.heightAt(worldX, worldZ) + groundItemRestRadius(kind),
                      worldZ };
    item.yaw = yaw;
    item.roll = roll;
    items_.push_back(item);
}

uint32_t Ground::scatterStones(uint32_t count, float areaRadius,
                               float minDistanceFromStart, float terrainExtent) {
    if (count == 0 || areaRadius <= minDistanceFromStart) {
        return 0;
    }

    std::uniform_real_distribution<float> angleDist(0.0f, 2.0f * kPi);
    std::uniform_real_distribution<float> unitDist(0.0f, 1.0f);

    // Marge par rapport au bord du terrain, meme raison que pour le semis
    // d'arbres : au-dela, heightAt ne renvoie plus que la valeur ecretee.
    const float usableExtent = std::max(0.0f, terrainExtent - 3.0f);

    // Deux pierres collees seraient ramassees d'un seul geste depuis le meme
    // endroit, ce qui viderait la mecanique de percussion de son deplacement.
    constexpr float kMinSpacing = 3.0f;

    uint32_t placed = 0;
    const uint32_t maxAttempts = count * 60u;

    for (uint32_t attempt = 0; attempt < maxAttempts && placed < count; ++attempt) {
        // Racine carree sur le rayon : sans elle, le tirage se concentre au
        // centre du disque (meme argument que Forest).
        const float r0 = minDistanceFromStart;
        const float r1 = areaRadius;
        const float u = unitDist(rng_);
        const float radius = std::sqrt(r0 * r0 + u * (r1 * r1 - r0 * r0));
        const float angle = angleDist(rng_);

        const float x = radius * std::cos(angle);
        const float z = radius * std::sin(angle);

        if (std::abs(x) > usableExtent || std::abs(z) > usableExtent) {
            continue;
        }

        bool tooClose = false;
        for (const GroundItem& other : items_) {
            const float dx = other.position.x - x;
            const float dz = other.position.z - z;
            if (dx * dx + dz * dz < kMinSpacing * kMinSpacing) {
                tooClose = true;
                break;
            }
        }
        if (tooClose) {
            continue;
        }

        // Les deux tirages sont nommes AVANT l'appel : l'ordre d'evaluation des
        // arguments n'est pas specifie en C++, les ecrire dans l'appel laisserait
        // le compilateur decider laquelle des deux valeurs devient le yaw. Le
        // semis cesserait d'etre reproductible d'un compilateur a l'autre (meme
        // precaution que dans Forest).
        const float yaw = angleDist(rng_);
        const float roll = angleDist(rng_);
        drop(GroundItemKind::Stone, x, z, yaw, roll);
        ++placed;
    }

    return placed;
}

int Ground::findNearest(const glm::vec3& playerPosition, float reach,
                        float verticalTolerance) const {
    int bestIndex = kNoGroundItem;
    float bestDistanceSq = reach * reach;

    for (std::size_t i = 0; i < items_.size(); ++i) {
        const GroundItem& item = items_[i];

        // PHASE 28 : un objet FIXE (le foyer) ne se ramasse pas.
        if (!groundItemIsPortable(item.kind)) {
            continue;
        }

        if (std::abs(item.position.y - playerPosition.y) > verticalTolerance) {
            continue;
        }

        const float dx = item.position.x - playerPosition.x;
        const float dz = item.position.z - playerPosition.z;
        const float distanceSq = dx * dx + dz * dz;

        // Comparaison stricte : a egalite parfaite on garde le premier trouve,
        // ce qui rend le choix deterministe.
        if (distanceSq < bestDistanceSq) {
            bestDistanceSq = distanceSq;
            bestIndex = static_cast<int>(i);
        }
    }

    return bestIndex;
}

std::vector<int> Ground::indicesWithin(const glm::vec3& position, float reach,
                                      float verticalTolerance) const {
    std::vector<int> result;
    const float reachSq = reach * reach;
    for (std::size_t i = 0; i < items_.size(); ++i) {
        const GroundItem& item = items_[i];
        if (std::abs(item.position.y - position.y) > verticalTolerance) {
            continue;
        }
        const float dx = item.position.x - position.x;
        const float dz = item.position.z - position.z;
        if (dx * dx + dz * dz <= reachSq) {
            result.push_back(static_cast<int>(i));
        }
    }
    return result;
}

bool Ground::pickUp(int index, GroundItemKind* kindOut) {
    if (index < 0 || static_cast<std::size_t>(index) >= items_.size()) {
        return false;
    }

    const auto slot = static_cast<std::size_t>(index);
    if (kindOut != nullptr) {
        *kindOut = items_[slot].kind;
    }

    // Suppression par echange avec le dernier : O(1), et l'ordre de la liste n'a
    // aucun sens de jeu (rien ne depend de l'ordre d'apparition des objets).
    // ATTENTION : les indices renvoyes par findNearest sont donc invalides des
    // qu'un ramassage a eu lieu - ils ne doivent jamais etre conserves d'une
    // frame a l'autre.
    items_[slot] = items_.back();
    items_.pop_back();
    return true;
}

} // namespace vercora
