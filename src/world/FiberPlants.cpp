#include "world/FiberPlants.hpp"

#include "core/Frustum.hpp"
#include "core/HeightSampler.hpp"

#include <algorithm>
#include <cmath>

namespace vercora {
namespace {

constexpr float kTwoPi = 6.28318530718f;

// Rampe lineaire ecretee : 0 sous `from`, 1 au-dessus de `to`. Meme fonction que
// celle de Vegetation.cpp, recopiee pour la meme raison qu'appendFacet dans
// HarvestMesh.cpp - quatre lignes sans etat ne justifient pas une dependance.
float ramp(float value, float from, float to) {
    if (to <= from) {
        return (value >= to) ? 1.0f : 0.0f;
    }
    return std::clamp((value - from) / (to - from), 0.0f, 1.0f);
}

// Melange 32 bits (finalisateur de MurmurHash3), le meme que world/Resin.cpp.
// Sans etat : le semis d'une cellule ne construit pas de generateur.
uint32_t mix32(uint32_t value) {
    value ^= value >> 16;
    value *= 0x85EBCA6Bu;
    value ^= value >> 13;
    value *= 0xC2B2AE35u;
    value ^= value >> 16;
    return value;
}

// Aleas d'un emplacement, dans [0, 1). `channel` separe des tirages qui doivent
// etre INDEPENDANTS : sans lui, la position et la taille d'une plante seraient
// correlees et toutes les grandes plantes se retrouveraient du meme cote de
// leur cellule.
float slotRandom(uint32_t seed, int32_t cellX, int32_t cellZ, uint32_t slot,
                 uint32_t channel) {
    uint32_t h = mix32(seed ^ 0x9E3779B9u);
    h = mix32(h ^ static_cast<uint32_t>(cellX));
    h = mix32(h ^ static_cast<uint32_t>(cellZ));
    h = mix32(h ^ (slot * 0x27D4EB2Fu));
    h = mix32(h ^ (channel * 0x165667B1u));
    return static_cast<float>(static_cast<double>(h) / 4294967296.0);
}

} // namespace

FiberPlants::FiberPlants(const HeightSampler& terrain, const HeightSampler* cover,
                         float terrainExtent, const FiberPlantParams& params)
    : terrain_(terrain), cover_(cover), terrainExtent_(terrainExtent), params_(params) {}

float FiberPlants::slopeAt(float worldX, float worldZ) const {
    // MEME definition que Vegetation::slopeAt : les deux filtres doivent parler
    // de la meme grandeur, sans quoi comparer une densite de ronces a une
    // densite d'arbres n'aurait aucun sens.
    const float h = terrain_.heightAt(worldX, worldZ);
    const float dx = terrain_.heightAt(worldX + 1.0f, worldZ) - h;
    const float dz = terrain_.heightAt(worldX, worldZ + 1.0f) - h;
    return std::sqrt(dx * dx + dz * dz);
}

float FiberPlants::coverAt(float worldX, float worldZ) const {
    if (cover_ == nullptr) {
        // Voir l'en-tete : sans carte de couvert, on ne sait pas ou est la
        // lisiere et on ne l'invente pas. -1 est la valeur sentinelle qui dit
        // "inconnu" et que edgeDensityAt traduit en densite nulle.
        return -1.0f;
    }
    return std::clamp(cover_->heightAt(worldX, worldZ), 0.0f, 1.0f);
}

float FiberPlants::edgeDensityAt(float worldX, float worldZ) const {
    const float cover = coverAt(worldX, worldZ);
    if (cover < 0.0f) {
        return 0.0f; // pas de carte : pas de lisiere connue
    }
    const float open = params_.coverOpen;
    const float closed = params_.coverClosed;
    if (!(closed > open)) {
        return 0.0f;
    }
    if (cover <= open || cover >= closed) {
        return 0.0f;
    }
    // TOIT TRIANGULAIRE sur la bande : maximum en son milieu, zero a ses deux
    // bords. C'est ce qui fait de ce critere une BORDURE et non un simple
    // intervalle - avec un creneau, une immense zone de couvert intermediaire
    // (un versant clairseme entier) porterait des ronces partout, ce qui n'est
    // pas une lisiere mais une lande.
    const float middle = 0.5f * (open + closed);
    const float half = 0.5f * (closed - open);
    return std::clamp(1.0f - std::fabs(cover - middle) / half, 0.0f, 1.0f);
}

float FiberPlants::clearingDensityAt(float worldX, float worldZ) const {
    if (clearing_ == nullptr) {
        return 0.0f;
    }
    const float opened = clearing_->clearing(worldX, worldZ);
    return ramp(opened, params_.clearingBegin, params_.clearingFull);
}

float FiberPlants::densityAt(float worldX, float worldZ) const {
    // MAXIMUM et non somme : une coupe faite en pleine lisiere ne doit pas
    // porter deux fois plus de ronces qu'ailleurs (voir l'en-tete).
    float density = std::max(edgeDensityAt(worldX, worldZ),
                             clearingDensityAt(worldX, worldZ));
    if (density <= 0.0f) {
        return 0.0f;
    }

    // Filtres de terrain, appliques APRES : ils ne choisissent pas ou pousser,
    // ils interdisent. Une paroi n'est pas une lisiere meme si son couvert le
    // laisse croire.
    if (slopeAt(worldX, worldZ) > params_.maxSlope) {
        return 0.0f;
    }
    if (terrain_.heightAt(worldX, worldZ) > params_.maxAltitude) {
        return 0.0f;
    }
    return density;
}

void FiberPlants::update(float deltaTime) {
    if (deltaTime <= 0.0f) {
        return;
    }
    for (auto it = pulled_.begin(); it != pulled_.end();) {
        it->second -= deltaTime;
        if (it->second <= 0.0f) {
            it = pulled_.erase(it); // repoussee : elle sort de la memoire
        } else {
            ++it;
        }
    }
}

bool FiberPlants::isStanding(const FiberPlantId& id) const {
    return pulled_.find(id) == pulled_.end();
}

void FiberPlants::buildCell(int32_t cellX, int32_t cellZ,
                            std::vector<FiberPlant>& out) const {
    const float cell = params_.cell;
    const float baseX = static_cast<float>(cellX) * cell;
    const float baseZ = static_cast<float>(cellZ) * cell;

    // La densite est lue UNE FOIS au centre de la cellule et non par
    // emplacement : c'est quatre fois moins d'appels a la carte de couvert et au
    // terrain, et la cellule fait 4 m - la lisiere ne bouge pas a l'interieur.
    const float density = densityAt(baseX + 0.5f * cell, baseZ + 0.5f * cell);
    if (density <= 0.0f) {
        return;
    }

    const float usable = std::max(0.0f, terrainExtent_ - 2.0f);
    const uint32_t slots = params_.slots;

    for (uint32_t slot = 0; slot < slots; ++slot) {
        // Chaque emplacement a son propre tirage d'existence, compare a la
        // densite : la cellule se remplit donc PROGRESSIVEMENT quand la densite
        // monte, au lieu d'apparaitre d'un bloc. C'est le meme principe que les
        // portees individuelles des emplacements d'arbres (Vegetation).
        if (slotRandom(params_.seed, cellX, cellZ, slot, 0) >= density) {
            continue;
        }

        const FiberPlantId id{cellX, cellZ, slot};
        if (!isStanding(id)) {
            continue; // arrachee, en cours de repousse
        }

        const float x = baseX + cell * slotRandom(params_.seed, cellX, cellZ, slot, 1);
        const float z = baseZ + cell * slotRandom(params_.seed, cellX, cellZ, slot, 2);
        if (usable > 0.0f && (std::fabs(x) > usable || std::fabs(z) > usable)) {
            continue; // hors des donnees de terrain
        }

        const float t = slotRandom(params_.seed, cellX, cellZ, slot, 3);

        FiberPlant plant;
        plant.id = id;
        plant.position = glm::vec3(x, terrain_.heightAt(x, z), z);
        plant.size = params_.sizeMin + t * (params_.sizeMax - params_.sizeMin);
        plant.yaw = kTwoPi * slotRandom(params_.seed, cellX, cellZ, slot, 4);
        plant.variation = slotRandom(params_.seed, cellX, cellZ, slot, 5);
        out.push_back(plant);
    }
}

std::vector<FiberPlant> FiberPlants::plantsNear(const glm::vec3& position,
                                                float reach) const {
    std::vector<FiberPlant> found;
    if (reach <= 0.0f) {
        return found;
    }

    const float cell = params_.cell;
    const int32_t cx0 = static_cast<int32_t>(std::floor((position.x - reach) / cell));
    const int32_t cx1 = static_cast<int32_t>(std::floor((position.x + reach) / cell));
    const int32_t cz0 = static_cast<int32_t>(std::floor((position.z - reach) / cell));
    const int32_t cz1 = static_cast<int32_t>(std::floor((position.z + reach) / cell));

    std::vector<FiberPlant> cellPlants;
    const float reach2 = reach * reach;
    for (int32_t cz = cz0; cz <= cz1; ++cz) {
        for (int32_t cx = cx0; cx <= cx1; ++cx) {
            cellPlants.clear();
            buildCell(cx, cz, cellPlants);
            for (const FiberPlant& plant : cellPlants) {
                // Portee HORIZONTALE seulement, comme Ground::findNearest : une
                // plante en contrebas d'un talus reste attrapable.
                const float dx = plant.position.x - position.x;
                const float dz = plant.position.z - position.z;
                if (dx * dx + dz * dz <= reach2) {
                    found.push_back(plant);
                }
            }
        }
    }
    return found;
}

bool FiberPlants::pullNearest(const glm::vec3& position, float reach,
                              FiberPlant* pulled) {
    const std::vector<FiberPlant> near = plantsNear(position, reach);

    const FiberPlant* best = nullptr;
    float bestDistance2 = 0.0f;
    for (const FiberPlant& plant : near) {
        const float dx = plant.position.x - position.x;
        const float dz = plant.position.z - position.z;
        const float d2 = dx * dx + dz * dz;
        if (best == nullptr || d2 < bestDistance2) {
            best = &plant;
            bestDistance2 = d2;
        }
    }
    if (best == nullptr) {
        return false;
    }

    if (pulled != nullptr) {
        *pulled = *best;
    }
    pulled_[best->id] = params_.regrowthDelay;
    return true;
}

void FiberPlants::gather(const glm::vec3& eye, const Frustum& frustum,
                         std::vector<InstanceData>& out) const {
    out.clear();

    const float cell = params_.cell;
    const float reach = params_.distanceFar;

    const int32_t cx0 = static_cast<int32_t>(std::floor((eye.x - reach) / cell));
    const int32_t cx1 = static_cast<int32_t>(std::floor((eye.x + reach) / cell));
    const int32_t cz0 = static_cast<int32_t>(std::floor((eye.z - reach) / cell));
    const int32_t cz1 = static_cast<int32_t>(std::floor((eye.z + reach) / cell));

    const float fadeSpan = std::max(1e-3f, params_.distanceFar - params_.distanceNear);

    std::vector<FiberPlant> cellPlants;
    for (int32_t cz = cz0; cz <= cz1; ++cz) {
        for (int32_t cx = cx0; cx <= cx1; ++cx) {
            const float baseX = static_cast<float>(cx) * cell;
            const float baseZ = static_cast<float>(cz) * cell;

            // Rejet par DISTANCE d'abord (un carre de distance, pas de frustum a
            // evaluer), puis par frustum. La cellule fait 4 m : un culling par
            // blocs comme celui de l'herbe n'apporterait presque rien sur une
            // portee de 46 m, qui ne couvre qu'une centaine de cellules.
            const float nearestX = std::clamp(eye.x, baseX, baseX + cell);
            const float nearestZ = std::clamp(eye.z, baseZ, baseZ + cell);
            const float ddx = eye.x - nearestX;
            const float ddz = eye.z - nearestZ;
            if (ddx * ddx + ddz * ddz > reach * reach) {
                continue;
            }

            const float cellY = terrain_.heightAt(baseX + 0.5f * cell, baseZ + 0.5f * cell);
            // Marge verticale large : la sonde centrale ne voit pas le relief de
            // toute la cellule, et ces plantes sont basses - mieux vaut ne jamais
            // les ecarter a tort.
            if (!frustum.intersectsAABB({ baseX, cellY - 20.0f, baseZ },
                                        { baseX + cell, cellY + 20.0f, baseZ + cell })) {
                continue;
            }

            cellPlants.clear();
            buildCell(cx, cz, cellPlants);
            for (const FiberPlant& plant : cellPlants) {
                const float dx = plant.position.x - eye.x;
                const float dz = plant.position.z - eye.z;
                const float distance = std::sqrt(dx * dx + dz * dz);
                if (distance > reach) {
                    continue;
                }

                InstanceData instance;
                instance.position = plant.position;
                instance.scale = plant.size;
                instance.yaw = plant.yaw;
                instance.variation = plant.variation;
                // Meme fondu que l'herbe : l'instance RETRECIT au lieu de
                // devenir transparente, ce qui evite tout melange alpha et donc
                // tout tri en profondeur.
                instance.fade =
                    std::clamp((params_.distanceFar - distance) / fadeSpan, 0.0f, 1.0f);
                out.push_back(instance);
            }
        }
    }

    lastPlantCount_ = static_cast<uint32_t>(out.size());
}

} // namespace vercora
