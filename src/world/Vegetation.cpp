#include "world/Vegetation.hpp"

#include "core/Frustum.hpp"
#include "core/HeightSampler.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace vercora {
namespace {

constexpr float kTwoPi = 6.28318530718f;

// --- Aleatoire DETERMINISTE et sans etat -----------------------------------
// Pas de std::mt19937 ici, volontairement. Un generateur a etat impose un ORDRE
// d'appel : la cellule (17, -42) ne donnerait le meme resultat que si toutes les
// cellules precedentes avaient ete tirees dans le meme ordre - ce qui est faux
// des que le joueur arrive par un autre chemin. Un HACHAGE, lui, ne depend que
// de ses entrees : la cellule (17, -42) donne toujours la meme foret, qu'on la
// decouvre en premier ou en dernier, et deux cellules voisines engendrees a des
// images differentes restent coherentes. C'est ce qui permet de ne rien stocker
// pour les 25 km2.
uint32_t hashCombine(uint32_t value) {
    // Melangeur entier de Murmur3 : bon melange des bits pour trois operations.
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    value ^= value >> 16;
    return value;
}

uint32_t cellHash(int32_t cx, int32_t cz, uint32_t seed, uint32_t stream) {
    uint32_t h = seed;
    h = hashCombine(h ^ static_cast<uint32_t>(cx) * 0x9E3779B1u);
    h = hashCombine(h ^ static_cast<uint32_t>(cz) * 0x85EBCA77u);
    h = hashCombine(h ^ (stream * 0xC2B2AE3Du));
    return h;
}

// Flottant dans [0,1) tire du hachage. Les 24 bits de poids fort suffisent
// largement et evitent les artefacts de periodicite des bits de poids faible.
float unitFloat(uint32_t hash) {
    return static_cast<float>(hash >> 8) * (1.0f / 16777216.0f);
}

// --- Bruit de valeur 2D -----------------------------------------------------
// Utilise pour GROUPER l'herbe en touffes. Un simple tirage par emplacement
// donnerait une repartition de Poisson : uniforme a grande echelle, donc
// exactement l'aspect "un brin tous les deux centimetres" qu'on cherche a
// eviter. Il faut une grandeur CORRELEE SPATIALEMENT, c'est-a-dire un bruit.
//
// Bruit de VALEUR et non Perlin : Perlin interpole des gradients, ce qui donne
// un champ plus isotrope mais coute quatre produits scalaires par point. Ici le
// champ ne sert qu'a moduler une probabilite, jamais a etre regarde
// directement ; ses defauts (legere preference pour les axes de la grille) sont
// invisibles une fois traduits en presence ou absence de touffes. Deux octaves
// suffisent a casser la regularite de la grille du bruit lui-meme.
float valueNoise(float x, float z, uint32_t seed) {
    const float fx = std::floor(x);
    const float fz = std::floor(z);
    const int32_t ix = static_cast<int32_t>(fx);
    const int32_t iz = static_cast<int32_t>(fz);
    const float tx = x - fx;
    const float tz = z - fz;

    // Courbe de lissage de Perlin (6t^5 - 15t^4 + 10t^3) : ses derivees
    // premiere ET seconde sont nulles aux noeuds, donc aucune arete visible aux
    // frontieres de la grille du bruit.
    const float sx = tx * tx * tx * (tx * (tx * 6.0f - 15.0f) + 10.0f);
    const float sz = tz * tz * tz * (tz * (tz * 6.0f - 15.0f) + 10.0f);

    const float v00 = unitFloat(cellHash(ix, iz, seed, 1u));
    const float v10 = unitFloat(cellHash(ix + 1, iz, seed, 1u));
    const float v01 = unitFloat(cellHash(ix, iz + 1, seed, 1u));
    const float v11 = unitFloat(cellHash(ix + 1, iz + 1, seed, 1u));

    return glm::mix(glm::mix(v00, v10, sx), glm::mix(v01, v11, sx), sz);
}

float clusterNoise(float x, float z, float frequency, uint32_t seed) {
    const float base = valueNoise(x * frequency, z * frequency, seed);
    const float detail = valueNoise(x * frequency * 2.7f, z * frequency * 2.7f, seed ^ 0x5bf03635u);
    return 0.68f * base + 0.32f * detail;
}

// Rampe lineaire ecretee : 0 en dessous de `from`, 1 au-dessus de `to`.
float ramp(float value, float from, float to) {
    if (to <= from) {
        return value >= to ? 1.0f : 0.0f;
    }
    return glm::clamp((value - from) / (to - from), 0.0f, 1.0f);
}

} // namespace

Vegetation::Vegetation(const HeightSampler& terrain, const HeightSampler* cover,
                       const HeightSampler* canopyHeight, float terrainExtent,
                       const VegetationParams& params)
    : terrain_(terrain), cover_(cover), canopyHeight_(canopyHeight),
      terrainExtent_(terrainExtent), params_(params) {
    if (params_.treeCell <= 0.0f || params_.grassCell <= 0.0f) {
        throw std::runtime_error("Vegetation : taille de cellule nulle ou negative.");
    }
    if (params_.treeSlots == 0) {
        throw std::runtime_error("Vegetation : treeSlots nul, aucun arbre possible.");
    }
    if (params_.treeDistanceNear > params_.treeDistanceFar) {
        throw std::runtime_error(
            "Vegetation : treeDistanceNear depasse treeDistanceFar. Le niveau de "
            "detail de densite suppose que l'emplacement 0 porte le plus loin.");
    }
    if (params_.treeMinHeight <= 0.0f || params_.treeMaxHeight < params_.treeMinHeight) {
        throw std::runtime_error("Vegetation : bornes de hauteur d'arbre incoherentes.");
    }
}

// ---------------------------------------------------------------------------
// Interrogation des cartes
// ---------------------------------------------------------------------------
float Vegetation::slopeAt(float worldX, float worldZ) const {
    // Pas de sonde d'un metre : c'est la resolution de la heightmap la plus fine,
    // donc la plus petite difference finie qui ait un sens.
    const float h = terrain_.heightAt(worldX, worldZ);
    const float dx = terrain_.heightAt(worldX + 1.0f, worldZ) - h;
    const float dz = terrain_.heightAt(worldX, worldZ + 1.0f) - h;
    return std::sqrt(dx * dx + dz * dz);
}

float Vegetation::coverAt(float worldX, float worldZ) const {
    if (cover_ != nullptr) {
        return glm::clamp(cover_->heightAt(worldX, worldZ), 0.0f, 1.0f);
    }

    // SUBSTITUT quand la carte LiDAR n'est pas disponible (terrain synthetique,
    // ou dalle sans carte produite). On se rabat sur ce que le relief seul
    // permet de dire : la foret de montagne s'arrete a une limite d'altitude et
    // ne tient pas sur les parois. C'est GROSSIER et ca ne connait ni les
    // pistes, ni les clairieres, ni les eboulis - toutes choses que la vraie
    // carte, elle, restitue. D'ou le fait que ce soit un repli et non le
    // comportement normal.
    const float altitude = terrain_.heightAt(worldX, worldZ);
    const float slope = slopeAt(worldX, worldZ);
    const float byAltitude = 1.0f - ramp(altitude, 1650.0f, 1950.0f);
    const float bySlope = 1.0f - ramp(slope, 0.7f, 1.2f);
    return glm::clamp(byAltitude * bySlope, 0.0f, 1.0f);
}

float Vegetation::canopyHeightAt(float worldX, float worldZ) const {
    if (canopyHeight_ != nullptr) {
        return std::max(0.0f, canopyHeight_->heightAt(worldX, worldZ));
    }
    // Sans carte de canopee, une futaie moyenne de montagne.
    return 18.0f;
}

// ---------------------------------------------------------------------------
// Semis des arbres
// ---------------------------------------------------------------------------
Vegetation::Cell Vegetation::buildTreeCell(int32_t cx, int32_t cz) const {
    Cell cell;

    const float cellSize = params_.treeCell;
    const float baseX = static_cast<float>(cx) * cellSize;
    const float baseZ = static_cast<float>(cz) * cellSize;

    // Marge sur le bord du terrain : au-dela, heightAt ne renvoie plus que la
    // valeur ecretee du bord et un arbre y flotterait a plat.
    const float usable = std::max(0.0f, terrainExtent_ - 2.0f);

    float minY = 0.0f;
    float maxY = 0.0f;
    bool first = true;

    cell.candidates.reserve(params_.treeSlots);
    for (uint32_t slot = 0; slot < params_.treeSlots; ++slot) {
        const uint32_t h0 = cellHash(cx, cz, params_.seed, slot * 4u + 100u);
        const uint32_t h1 = cellHash(cx, cz, params_.seed, slot * 4u + 101u);
        const uint32_t h2 = cellHash(cx, cz, params_.seed, slot * 4u + 102u);
        const uint32_t h3 = cellHash(cx, cz, params_.seed, slot * 4u + 103u);

        const float x = baseX + unitFloat(h0) * cellSize;
        const float z = baseZ + unitFloat(h1) * cellSize;

        if (std::abs(x) > usable || std::abs(z) > usable) {
            continue;
        }

        // --- Filtre par le COUVERT REEL ------------------------------------
        // C'est le coeur de la tache 2. La densite locale vaut 0 sous coverMin
        // et 1 au-dessus de coverFull ; l'emplacement `slot` n'est retenu que si
        // cette densite lui laisse la place. Comparer `slot` a `densite * slots`
        // plutot que de tirer un des a chaque emplacement garantit que les
        // emplacements sont retenus TOUJOURS DANS LE MEME ORDRE : c'est ce qui
        // rend la portee par emplacement (plus bas) coherente, et ce qui evite
        // qu'un arbre change d'identite quand la densite bouge d'un cheveu.
        const float density = ramp(coverAt(x, z), params_.coverMin, params_.coverFull);
        const float allowed = density * static_cast<float>(params_.treeSlots);
        if (static_cast<float>(slot) >= allowed) {
            continue;
        }

        if (slopeAt(x, z) > params_.treeMaxSlope) {
            continue;
        }

        // --- Hauteur ---------------------------------------------------------
        // La carte de canopee donne la hauteur du PLUS GRAND arbre de sa cellule
        // de 5 m. Les arbres d'un meme bosquet se repartissent entre 55 % et
        // 100 % de cette valeur : une futaie ou tous les sujets ont exactement la
        // meme taille se voit immediatement (effet "brosse a dents").
        const float local = glm::clamp(canopyHeightAt(x, z),
                                       params_.treeMinHeight, params_.treeMaxHeight);
        const float height = local * (0.55f + 0.45f * unitFloat(h2));
        if (height < params_.treeMinHeight) {
            continue;
        }

        Candidate candidate;
        candidate.position = { x, terrain_.heightAt(x, z), z };
        candidate.height = height;
        candidate.yaw = unitFloat(h3) * kTwoPi;
        candidate.variation = unitFloat(h2 ^ 0x9E3779B9u);

        // --- ESSENCE (PHASE 26) ------------------------------------------
        // NOTER L'ENDROIT : ici, et pas plus haut. Tous les filtres de PRESENCE
        // - couvert LiDAR, pente, altitude, hauteur de canopee - ont deja
        // statue ; l'emplacement EST retenu. L'exposition n'a donc aucun moyen
        // d'ajouter ou de retirer un arbre, elle ne fait que dire lequel c'est.
        // C'est la contrainte du chantier 4, et elle est tenue par la STRUCTURE
        // du code plutot que par une intention : deplacer cet appel de vingt
        // lignes vers le haut serait le seul moyen de la rompre.
        candidate.species = treeSpeciesAt(x, z, aspectExposure_, params_.species);

        // --- Portee PROPRE a l'emplacement (niveau de detail de densite) -----
        // L'emplacement 0 porte a treeDistanceFar, le dernier a
        // treeDistanceNear, les autres entre les deux. Consequence : de loin
        // seuls les premiers emplacements de chaque cellule subsistent, donc la
        // foret s'eclaircit progressivement au lieu de disparaitre d'un bloc, et
        // chaque arbre a sa propre distance de fondu - il n'y a jamais de saut
        // collectif.
        const float t = params_.treeSlots > 1
            ? static_cast<float>(slot) / static_cast<float>(params_.treeSlots - 1)
            : 0.0f;
        candidate.maxDistance = glm::mix(params_.treeDistanceFar,
                                         params_.treeDistanceNear, t);

        const float top = candidate.position.y + height;
        if (first) {
            minY = candidate.position.y;
            maxY = top;
            first = false;
        } else {
            minY = std::min(minY, candidate.position.y);
            maxY = std::max(maxY, top);
        }
        cell.candidates.push_back(candidate);
    }

    // Boite englobante de la cellule, pour le frustum culling. Une cellule vide
    // recoit une boite plate a l'altitude du terrain : elle sera cullee ou non,
    // sans consequence puisqu'elle ne produit rien.
    if (first) {
        const float y = terrain_.heightAt(baseX + 0.5f * cellSize, baseZ + 0.5f * cellSize);
        minY = y;
        maxY = y;
    }
    cell.boxMin = { baseX, minY, baseZ };
    cell.boxMax = { baseX + cellSize, maxY, baseZ + cellSize };
    return cell;
}

const Vegetation::Cell& Vegetation::treeCell(int32_t cx, int32_t cz) {
    const CellKey key{ cx, cz };
    auto found = cache_.find(key);
    if (found != cache_.end()) {
        return found->second;
    }
    return cache_.emplace(key, buildTreeCell(cx, cz)).first->second;
}

void Vegetation::setAspectExposure(const AspectExposureSource* source) {
    if (source == aspectExposure_) { return; }
    aspectExposure_ = source;
    // L'essence est mise en cache AVEC la cellule : garder les cellules deja
    // engendrees laisserait cohabiter deux lois dans le meme monde, avec une
    // frontiere invisible la ou le joueur se trouvait au moment du changement.
    cache_.clear();
}

void Vegetation::gatherTrees(const glm::vec3& eye, const Frustum& frustum,
                             std::vector<InstanceData>& out) {
    // Les DEUX sorties du coeur pointent sur le MEME vecteur : l'union est donc
    // rendue dans exactement l'ordre d'avant la Phase 26, sans copie ni
    // concatenation, et sans qu'il existe une seconde version du parcours.
    gatherTrees(eye, frustum, out, out);
}

void Vegetation::gatherTrees(const glm::vec3& eye, const Frustum& frustum,
                             std::vector<InstanceData>& conifers,
                             std::vector<InstanceData>& broadleaves) {
    conifers.clear();
    broadleaves.clear();
    lastConiferCount_ = 0;
    lastBroadleafCount_ = 0;

    const float cellSize = params_.treeCell;
    const float reach = params_.treeDistanceFar;

    const int32_t cx0 = static_cast<int32_t>(std::floor((eye.x - reach) / cellSize));
    const int32_t cx1 = static_cast<int32_t>(std::floor((eye.x + reach) / cellSize));
    const int32_t cz0 = static_cast<int32_t>(std::floor((eye.z - reach) / cellSize));
    const int32_t cz1 = static_cast<int32_t>(std::floor((eye.z + reach) / cellSize));

    uint32_t built = 0;

    // --- Culling par BLOCS de cellules --------------------------------------
    // Sans lui, chaque image visite les ~8 400 cellules du carre de portee et
    // fait une recherche dans le cache pour chacune, y compris pour celles
    // situees derriere le joueur. Mesure directe : camera tournee vers le CIEL,
    // donc pratiquement aucun arbre dessine, la vegetation faisait quand meme
    // tomber la scene de 1946 a 351 FPS - soit 2,3 ms par image de balayage PUR,
    // sans le moindre triangle en plus. Le cout des arbres n'etait pas
    // geometrique, il etait dans cette boucle.
    //
    // Un bloc de 8x8 cellules est teste d'un coup contre le tronc de vision ;
    // s'il echoue, 64 cellules sont ecartees pour un seul test. Comme le champ
    // de vision ne couvre qu'une fraction du disque de portee, l'essentiel des
    // blocs tombe ici.
    //
    // Les bornes verticales du bloc sont ESTIMEES (cinq sondes d'altitude, puis
    // une marge large) et non exactes. C'est sans danger : un bloc situe derriere
    // le joueur est rejete par les plans lateraux et le plan proche, qui ne
    // dependent pas de cette estimation ; la marge ne sert qu'a ne jamais ecarter
    // par erreur un bloc reellement visible.
    constexpr int32_t kBlock = 8;
    const auto blockFloor = [](int32_t value) {
        // Division entiere qui arrondit vers le BAS, y compris en negatif : en
        // C++ la division tronque vers zero, ce qui decalerait les blocs d'un
        // cran du cote des coordonnees negatives.
        return (value >= 0) ? value / kBlock : -((-value + kBlock - 1) / kBlock);
    };

    for (int32_t bz = blockFloor(cz0); bz <= blockFloor(cz1); ++bz) {
        for (int32_t bx = blockFloor(cx0); bx <= blockFloor(cx1); ++bx) {
            const float blockMinX = static_cast<float>(bx * kBlock) * cellSize;
            const float blockMinZ = static_cast<float>(bz * kBlock) * cellSize;
            const float blockSize = static_cast<float>(kBlock) * cellSize;

            const float bnx = glm::clamp(eye.x, blockMinX, blockMinX + blockSize);
            const float bnz = glm::clamp(eye.z, blockMinZ, blockMinZ + blockSize);
            const float bdx = eye.x - bnx;
            const float bdz = eye.z - bnz;
            if (bdx * bdx + bdz * bdz > reach * reach) {
                continue;
            }

            float lowest = terrain_.heightAt(blockMinX + 0.5f * blockSize,
                                             blockMinZ + 0.5f * blockSize);
            float highest = lowest;
            for (int corner = 0; corner < 4; ++corner) {
                const float sx = blockMinX + ((corner & 1) ? blockSize : 0.0f);
                const float sz = blockMinZ + ((corner & 2) ? blockSize : 0.0f);
                const float h = terrain_.heightAt(sx, sz);
                lowest = std::min(lowest, h);
                highest = std::max(highest, h);
            }
            // Marge large : les cinq sondes ne voient pas l'interieur du bloc,
            // et un ressaut de terrain de 100 m sur 192 m est courant ici.
            if (!frustum.intersectsAABB({ blockMinX, lowest - 130.0f, blockMinZ },
                                        { blockMinX + blockSize, highest + 130.0f,
                                          blockMinZ + blockSize })) {
                continue;
            }

            const int32_t inCx0 = std::max(cx0, bx * kBlock);
            const int32_t inCx1 = std::min(cx1, bx * kBlock + kBlock - 1);
            const int32_t inCz0 = std::max(cz0, bz * kBlock);
            const int32_t inCz1 = std::min(cz1, bz * kBlock + kBlock - 1);

    for (int32_t cz = inCz0; cz <= inCz1; ++cz) {
        for (int32_t cx = inCx0; cx <= inCx1; ++cx) {
            // Rejet par la DISTANCE avant tout le reste : c'est le test le moins
            // cher, et il elimine les coins du carre de cellules balaye, qui ne
            // sont pas dans le disque de portee.
            const float boxMinX = static_cast<float>(cx) * cellSize;
            const float boxMinZ = static_cast<float>(cz) * cellSize;
            const float nearestX = glm::clamp(eye.x, boxMinX, boxMinX + cellSize);
            const float nearestZ = glm::clamp(eye.z, boxMinZ, boxMinZ + cellSize);
            const float dx = eye.x - nearestX;
            const float dz = eye.z - nearestZ;
            if (dx * dx + dz * dz > reach * reach) {
                continue;
            }

            // Une cellule DEJA en cache est cullee sur sa vraie boite ; une
            // cellule inconnue ne peut pas l'etre sans etre engendree, donc on
            // la teste sur une boite verticale genereuse avant de payer le
            // semis. Sans cette precaution, le budget par image serait consomme
            // par des cellules situees derriere le joueur.
            auto found = cache_.find(CellKey{ cx, cz });
            if (found == cache_.end()) {
                const float groundY = terrain_.heightAt(boxMinX + 0.5f * cellSize,
                                                        boxMinZ + 0.5f * cellSize);
                const glm::vec3 coarseMin{ boxMinX, groundY - 40.0f, boxMinZ };
                const glm::vec3 coarseMax{ boxMinX + cellSize, groundY + 40.0f,
                                           boxMinZ + cellSize };
                if (!frustum.intersectsAABB(coarseMin, coarseMax)) {
                    continue;
                }
                if (built >= params_.treeCellBudget) {
                    continue; // servie a une image suivante
                }
                ++built;
            }

            const Cell& cell = treeCell(cx, cz);
            if (cell.candidates.empty()) {
                continue;
            }
            if (!frustum.intersectsAABB(cell.boxMin, cell.boxMax)) {
                continue;
            }

            for (const Candidate& candidate : cell.candidates) {
                const float distance = glm::length(eye - candidate.position);
                if (distance >= candidate.maxDistance) {
                    continue;
                }

                InstanceData instance;
                instance.position = candidate.position;
                instance.scale = candidate.height;
                instance.yaw = candidate.yaw;
                instance.variation = candidate.variation;
                // Fondu sur les 18 derniers pour cent de la portee de CET
                // emplacement : l'arbre retrecit jusqu'a disparaitre au lieu de
                // s'evanouir d'un coup. Voir shaders/foliage.vert.
                const float fadeStart = candidate.maxDistance * 0.82f;
                instance.fade = 1.0f - ramp(distance, fadeStart, candidate.maxDistance);
                if (candidate.species == TreeSpecies::Conifer) {
                    conifers.push_back(instance);
                    ++lastConiferCount_;
                } else {
                    broadleaves.push_back(instance);
                    ++lastBroadleafCount_;
                }
            }
        }
    }
        }
    }

    // --- Purge du cache -----------------------------------------------------
    // Toute cellule au-dela de 1,5 fois la portee est oubliee. Sans purge,
    // traverser les 25 km2 finirait par tout garder en memoire, ce qui annulerait
    // exactement l'interet du semis a la demande.
    const float keep = reach * 1.5f;
    for (auto it = cache_.begin(); it != cache_.end();) {
        const float centreX = (static_cast<float>(it->first.x) + 0.5f) * cellSize;
        const float centreZ = (static_cast<float>(it->first.z) + 0.5f) * cellSize;
        const float ddx = eye.x - centreX;
        const float ddz = eye.z - centreZ;
        if (ddx * ddx + ddz * ddz > keep * keep) {
            it = cache_.erase(it);
        } else {
            ++it;
        }
    }

    // Somme des deux compteurs, et non `out.size()` : quand les deux sorties
    // aliasent le meme vecteur (surcharge d'union), sa taille est deja le total ;
    // quand elles sont distinctes, elle ne serait que la moitie du travail.
    // Les compteurs, eux, disent la meme chose dans les deux cas.
    lastTreeCount_ = lastConiferCount_ + lastBroadleafCount_;
}

// ---------------------------------------------------------------------------
// Semis de l'herbe
// ---------------------------------------------------------------------------
void Vegetation::gatherGrass(const glm::vec3& eye, const Frustum& frustum,
                             std::vector<InstanceData>& out) const {
    out.clear();

    const float cellSize = params_.grassCell;
    const float reach = params_.grassDistanceFar;

    const int32_t cx0 = static_cast<int32_t>(std::floor((eye.x - reach) / cellSize));
    const int32_t cx1 = static_cast<int32_t>(std::floor((eye.x + reach) / cellSize));
    const int32_t cz0 = static_cast<int32_t>(std::floor((eye.z - reach) / cellSize));
    const int32_t cz1 = static_cast<int32_t>(std::floor((eye.z + reach) / cellSize));

    const float usable = std::max(0.0f, terrainExtent_ - 2.0f);

    // Culling par BLOCS, exactement comme pour les arbres (voir gatherTrees).
    // La cellule d'herbe fait 1 m : un bloc de 16x16 couvre 16 m et epargne 256
    // tests de cellule d'un seul coup.
    constexpr int32_t kBlock = 16;
    const auto blockFloor = [](int32_t value) {
        return (value >= 0) ? value / kBlock : -((-value + kBlock - 1) / kBlock);
    };

    for (int32_t bz = blockFloor(cz0); bz <= blockFloor(cz1); ++bz) {
        for (int32_t bx = blockFloor(cx0); bx <= blockFloor(cx1); ++bx) {
            const float blockMinX = static_cast<float>(bx * kBlock) * cellSize;
            const float blockMinZ = static_cast<float>(bz * kBlock) * cellSize;
            const float blockSize = static_cast<float>(kBlock) * cellSize;

            const float bnx = glm::clamp(eye.x, blockMinX, blockMinX + blockSize);
            const float bnz = glm::clamp(eye.z, blockMinZ, blockMinZ + blockSize);
            const float bdx = eye.x - bnx;
            const float bdz = eye.z - bnz;
            if (bdx * bdx + bdz * bdz > reach * reach) {
                continue;
            }

            const float blockY = terrain_.heightAt(blockMinX + 0.5f * blockSize,
                                                   blockMinZ + 0.5f * blockSize);
            // Marge verticale large : cinq sondes ne verraient pas l'interieur du
            // bloc, et l'herbe est basse - mieux vaut ne jamais l'ecarter a tort.
            if (!frustum.intersectsAABB({ blockMinX, blockY - 45.0f, blockMinZ },
                                        { blockMinX + blockSize, blockY + 45.0f,
                                          blockMinZ + blockSize })) {
                continue;
            }

            const int32_t inCx0 = std::max(cx0, bx * kBlock);
            const int32_t inCx1 = std::min(cx1, bx * kBlock + kBlock - 1);
            const int32_t inCz0 = std::max(cz0, bz * kBlock);
            const int32_t inCz1 = std::min(cz1, bz * kBlock + kBlock - 1);

    for (int32_t cz = inCz0; cz <= inCz1; ++cz) {
        for (int32_t cx = inCx0; cx <= inCx1; ++cx) {
            const float baseX = static_cast<float>(cx) * cellSize;
            const float baseZ = static_cast<float>(cz) * cellSize;

            const float nearestX = glm::clamp(eye.x, baseX, baseX + cellSize);
            const float nearestZ = glm::clamp(eye.z, baseZ, baseZ + cellSize);
            const float ddx = eye.x - nearestX;
            const float ddz = eye.z - nearestZ;
            if (ddx * ddx + ddz * ddz > reach * reach) {
                continue;
            }

            const float groundY = terrain_.heightAt(baseX + 0.5f * cellSize,
                                                    baseZ + 0.5f * cellSize);
            // Boite basse : une touffe ne depasse guere le demi-metre.
            if (!frustum.intersectsAABB({ baseX, groundY - 3.0f, baseZ },
                                        { baseX + cellSize, groundY + 3.0f,
                                          baseZ + cellSize })) {
                continue;
            }

            // --- Densite locale --------------------------------------------
            // Trois facteurs INDEPENDANTS, evalues au centre de la cellule (les
            // evaluer par touffe couterait trois fois plus pour une difference
            // invisible a 3 m de resolution) :
            //
            //  1. le BRUIT groupe les touffes. C'est lui qui repond a l'exigence
            //     "pas une grille reguliere" : sans lui, une touffe par
            //     emplacement partout donnerait un tapis d'une regularite
            //     immediatement lisible.
            //  2. l'ALTITUDE rarefie l'herbe vers le haut (pelouse alpine rase
            //     puis rocher nu).
            //  3. la donnee LiDAR distingue le PRE du SOUS-BOIS et du LAPIAZ :
            //     il faut des retours de vegetation (sinon c'est du rocher) mais
            //     pas de canopee haute (sinon c'est une futaie, ou il pousse peu
            //     d'herbe).
            const float noise = clusterNoise(baseX, baseZ,
                                             params_.grassClusterFrequency,
                                             params_.seed ^ 0x2545F491u);
            const float clustered = ramp(noise, params_.grassClusterThreshold,
                                         params_.grassClusterThreshold +
                                             params_.grassClusterSoftness);
            if (clustered <= 0.0f) {
                continue;
            }

            const float altitude = groundY;
            const float byAltitude = 1.0f - ramp(altitude, params_.grassAltitudeLush,
                                                 params_.grassAltitudeBarren);
            if (byAltitude <= 0.0f) {
                continue;
            }

            const float centreX = baseX + 0.5f * cellSize;
            const float centreZ = baseZ + 0.5f * cellSize;
            // Il faut des retours de vegetation (sinon c'est du rocher nu), et
            // la densite baisse - sans s'annuler - sous une canopee haute : une
            // futaie de montagne est claire, son sol porte de l'herbe.
            const float openness = 1.0f - ramp(canopyHeightAt(centreX, centreZ), 3.0f, 14.0f);
            const float meadow = ramp(coverAt(centreX, centreZ), 0.05f, 0.30f) *
                                 glm::mix(params_.grassUnderCanopy, 1.0f, openness);
            if (meadow <= 0.0f) {
                continue;
            }

            if (slopeAt(centreX, centreZ) > params_.grassMaxSlope) {
                continue;
            }

            // PHASE 17 : etat du sol. Quatrieme facteur, et le seul qui ne
            // soit pas une propriete du terrain : il vient de ce que le JOUEUR a
            // fait ici (voir world/Habitat.hpp). Une cellule jamais touchee rend
            // 1 et le produit est inchange - la non-regression est litterale, pas
            // approximative.
            const float degraded = (groundCover_ != nullptr)
                ? std::clamp(groundCover_->groundCover(centreX, centreZ), 0.0f, 1.0f)
                : 1.0f;

            const float density = clustered * byAltitude * meadow * degraded;
            const float allowed = density * static_cast<float>(params_.grassSlots);

            // Taille decroissant avec l'altitude : c'est la demande explicite de
            // la tache 3. La meme rampe sert a la densite et a la taille, ce qui
            // fait que la transition vers la pelouse rase est continue.
            const float sizeFactor = ramp(altitude, params_.grassAltitudeLush,
                                          params_.grassAltitudeBarren);
            const float tuftSize = glm::mix(params_.grassSizeLow,
                                            params_.grassSizeHigh, sizeFactor);

            // --- PHASE 18 : hauteur du sol, une fois pour la CELLULE ------
            // Le semis appelait terrain_.heightAt() UNE FOIS PAR TOUFFE. A 56
            // emplacements par cellule et un millier de cellules par image,
            // c'est le poste le plus lourd de tout le semis - et il grandit
            // exactement au rythme de la densite, donc c'est lui qui plafonnait
            // le tapis.
            //
            // On echantillonne desormais les QUATRE COINS de la cellule, puis on
            // interpole a l'interieur. Ce n'est pas une approximation quand la
            // cellule d'herbe coincide avec une cellule du terrain (1 m et 1 m
            // sur cette zone) : on reproduit ici le MEME decoupage en deux
            // triangles le long de la diagonale h00-h11 que Heightmap::heightAt,
            // donc la touffe est posee au pixel pres sur la surface affichee.
            // Sur un terrain au pas de 2 m, l'ecart residuel est celui de la
            // courbure du sol sur un metre - de l'ordre du centimetre, sous la
            // hauteur d'un brin.
            const float h00 = terrain_.heightAt(baseX, baseZ);
            const float h10 = terrain_.heightAt(baseX + cellSize, baseZ);
            const float h01 = terrain_.heightAt(baseX, baseZ + cellSize);
            const float h11 = terrain_.heightAt(baseX + cellSize, baseZ + cellSize);
            const auto heightInCell = [&](float tx, float tz) {
                return (tz > tx) ? h00 + (h01 - h00) * tz + (h11 - h01) * tx
                                 : h00 + (h10 - h00) * tx + (h11 - h10) * tz;
            };

            for (uint32_t slot = 0; slot < params_.grassSlots; ++slot) {
                if (static_cast<float>(slot) >= allowed) {
                    break;
                }
                const uint32_t h0 = cellHash(cx, cz, params_.seed ^ 0x68bc21ebu, slot * 3u + 7u);
                const uint32_t h1 = cellHash(cx, cz, params_.seed ^ 0x68bc21ebu, slot * 3u + 8u);
                const uint32_t h2 = cellHash(cx, cz, params_.seed ^ 0x68bc21ebu, slot * 3u + 9u);

                const float tx = unitFloat(h0);
                const float tz = unitFloat(h1);
                const float x = baseX + tx * cellSize;
                const float z = baseZ + tz * cellSize;
                if (std::abs(x) > usable || std::abs(z) > usable) {
                    continue;
                }

                // AUCUN appel a heightAt par touffe (PHASE 18) : quatre par
                // cellule, interpoles ici. La Phase 11 en faisait deja passer le
                // compte de deux a un par touffe - un pour le test de distance,
                // un pour la position ; c'est la meme lecon poussee un cran plus
                // loin, au niveau de la cellule.
                const glm::vec3 foot(x, heightInCell(tx, tz), z);
                const float distance = glm::length(eye - foot);
                if (distance >= reach) {
                    continue;
                }

                InstanceData instance;
                instance.position = foot;
                // Taille variee de +/- 25 % autour de la taille locale.
                instance.scale = tuftSize * (0.75f + 0.5f * unitFloat(h2));
                instance.yaw = unitFloat(h2 ^ 0x1b873593u) * kTwoPi;
                instance.variation = unitFloat(h0 ^ 0xcc9e2d51u);
                // Fondu sur toute la bande entre grassDistanceNear et
                // grassDistanceFar - bien plus large que pour les arbres, parce
                // que l'herbe est proche : un fondu court se lirait comme une
                // muraille de touffes qui poussent devant le joueur.
                instance.fade = 1.0f - ramp(distance, params_.grassDistanceNear,
                                            params_.grassDistanceFar);
                out.push_back(instance);
            }
        }
    }
        }
    }

    lastGrassCount_ = static_cast<uint32_t>(out.size());
}

// ---------------------------------------------------------------------------
double Vegetation::estimateTreeDensityPerHectare(uint32_t step) const {
    if (step == 0) {
        step = 1;
    }
    const float cellSize = params_.treeCell;
    const int32_t half = static_cast<int32_t>(terrainExtent_ / cellSize);

    uint64_t trees = 0;
    uint64_t cells = 0;
    for (int32_t cz = -half; cz <= half; cz += static_cast<int32_t>(step)) {
        for (int32_t cx = -half; cx <= half; cx += static_cast<int32_t>(step)) {
            trees += buildTreeCell(cx, cz).candidates.size();
            ++cells;
        }
    }
    if (cells == 0) {
        return 0.0;
    }
    const double areaHectares = static_cast<double>(cells) * cellSize * cellSize / 10000.0;
    return static_cast<double>(trees) / areaHectares;
}

} // namespace vercora
