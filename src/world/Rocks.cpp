#include "world/Rocks.hpp"

#include "core/Frustum.hpp"
#include "core/HeightSampler.hpp"
#include "world/RockMesh.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace vercora {
namespace {

constexpr float kTwoPi = 6.28318530718f;

// Memes primitives que Vegetation.cpp (hachage sans etat, bruit de valeur) :
// recopiees plutot que partagees pour ne pas toucher a Vegetation (non-regression
// litterale). Le semis ne depend que de (cellule, graine, flux).
uint32_t mix32(uint32_t v) {
    v ^= v >> 16; v *= 0x7feb352du;
    v ^= v >> 15; v *= 0x846ca68bu;
    v ^= v >> 16;
    return v;
}
uint32_t cellHash(int32_t cx, int32_t cz, uint32_t seed, uint32_t stream) {
    uint32_t h = seed;
    h = mix32(h ^ static_cast<uint32_t>(cx) * 0x9E3779B1u);
    h = mix32(h ^ static_cast<uint32_t>(cz) * 0x85EBCA77u);
    h = mix32(h ^ (stream * 0xC2B2AE3Du));
    return h;
}
float unitFloat(uint32_t h) { return static_cast<float>(h >> 8) * (1.0f / 16777216.0f); }

float valueNoise(float x, float z, uint32_t seed) {
    const float fx = std::floor(x), fz = std::floor(z);
    const auto ix = static_cast<int32_t>(fx);
    const auto iz = static_cast<int32_t>(fz);
    const float tx = x - fx, tz = z - fz;
    const float sx = tx * tx * tx * (tx * (tx * 6.0f - 15.0f) + 10.0f);
    const float sz = tz * tz * tz * (tz * (tz * 6.0f - 15.0f) + 10.0f);
    const float v00 = unitFloat(cellHash(ix, iz, seed, 1u));
    const float v10 = unitFloat(cellHash(ix + 1, iz, seed, 1u));
    const float v01 = unitFloat(cellHash(ix, iz + 1, seed, 1u));
    const float v11 = unitFloat(cellHash(ix + 1, iz + 1, seed, 1u));
    return glm::mix(glm::mix(v00, v10, sx), glm::mix(v01, v11, sx), sz);
}

float ramp(float value, float from, float to) {
    if (to <= from) { return value >= to ? 1.0f : 0.0f; }
    return glm::clamp((value - from) / (to - from), 0.0f, 1.0f);
}

// Marge au bord du terrain : la rugosite lit 4 m autour du point, la direction
// des trainees 8 m ; au-dela du bord heightAt ecrete et fausserait les deux.
constexpr float kEdgeMargin = 10.0f;

} // namespace

Rocks::Rocks(const HeightSampler& terrain, const HeightSampler* cover, float terrainExtent,
             const RockParams& params)
    : terrain_(terrain), cover_(cover), terrainExtent_(terrainExtent), params_(params) {
    if (params_.blockCell <= 0.0f || params_.gravelCell <= 0.0f) {
        throw std::runtime_error("Rocks : taille de cellule nulle ou negative.");
    }
    if (params_.slopeMin >= params_.slopeMax) {
        throw std::runtime_error("Rocks : slopeMin doit rester sous slopeMax.");
    }
    if (params_.boulderDistanceNear > params_.boulderDistanceFar ||
        params_.stoneDistanceNear > params_.stoneDistanceFar ||
        params_.gravelDistanceNear > params_.gravelDistanceFar) {
        throw std::runtime_error("Rocks : portee proche au-dela de la portee lointaine.");
    }
}

// ---------------------------------------------------------------------------
// Critere
// ---------------------------------------------------------------------------
float Rocks::coverAt(float worldX, float worldZ) const {
    return glm::clamp(cover_->heightAt(worldX, worldZ), 0.0f, 1.0f);
}

bool Rocks::maskAt(float worldX, float worldZ) const {
    if (cover_ == nullptr) {
        return false; // pas de donnee reelle, pas de rocher
    }
    const float usable = terrainExtent_ - kEdgeMargin;
    if (std::abs(worldX) > usable || std::abs(worldZ) > usable) {
        return false;
    }
    // LA pente du critere : Climber::slopeAt, base de 4 m (et non
    // Vegetation::slopeAt, 1 m en differences avant : piege signale).
    const float slope = slopeProbe_.slopeAt(terrain_, worldX, worldZ);
    if (slope < params_.slopeMin || slope >= params_.slopeMax) {
        return false;
    }
    return coverAt(worldX, worldZ) < params_.coverMax;
}

float Rocks::roughnessAt(float worldX, float worldZ) const {
    // Definition EXACTE de data/phase29/probe_rocks.py au point considere :
    //   res = H - moyenne5x5(H) ; rug = sqrt(moyenne5x5(res^2)).
    // Le residu des 25 points de la fenetre demande leur propre moyenne 5x5,
    // donc une grille de 9 x 9 sondes au pas de 1 m (le pas du DTM).
    float h[9][9];
    for (int j = 0; j < 9; ++j) {
        for (int i = 0; i < 9; ++i) {
            h[j][i] = terrain_.heightAt(worldX + static_cast<float>(i - 4),
                                        worldZ + static_cast<float>(j - 4));
        }
    }
    double sumSq = 0.0;
    for (int j = 2; j <= 6; ++j) {
        for (int i = 2; i <= 6; ++i) {
            double mean = 0.0;
            for (int b = -2; b <= 2; ++b) {
                for (int a = -2; a <= 2; ++a) {
                    mean += h[j + b][i + a];
                }
            }
            mean /= 25.0;
            const double r = h[j][i] - mean;
            sumSq += r * r;
        }
    }
    return static_cast<float>(std::sqrt(sumSq / 25.0));
}

float Rocks::streakDensityAt(float worldX, float worldZ) const {
    // Repere local de la pente : u le long de la ligne de plus grande pente,
    // v en travers. Un bruit lent en u et rapide en v dessine des bandes
    // allongees dans le sens de la descente - la forme d'une trainee d'eboulis.
    //
    // PIEGE EVITE (mesure par le test d'anisotropie) : tourner le repere avec la
    // direction de pente AU POINT rend le bruit instable - loin de l'origine,
    // un ecart de direction de quelques degres deplace v de plusieurs periodes,
    // et le champ devient un bruit blanc. Le repere est donc fixe PAR TUILE
    // (streakTile), centre sur la tuile, avec la direction lue au centre sur une
    // base large ; les quatre tuiles voisines sont melangees en bilineaire lisse.
    const float tile = params_.streakTile;
    const float fx = worldX / tile - 0.5f;
    const float fz = worldZ / tile - 0.5f;
    const float ix = std::floor(fx), iz = std::floor(fz);
    const float tx = fx - ix, tz = fz - iz;
    const float sx = tx * tx * (3.0f - 2.0f * tx);
    const float sz = tz * tz * (3.0f - 2.0f * tz);
    const uint32_t seed = params_.seed ^ 0x51ED270Bu;
    float noise[2][2];
    for (int j = 0; j < 2; ++j) {
        for (int i = 0; i < 2; ++i) {
            const float cx = (ix + static_cast<float>(i) + 0.5f) * tile;
            const float cz = (iz + static_cast<float>(j) + 0.5f) * tile;
            const glm::vec2 g = walkTerrainGradient(terrain_, cx, cz, params_.streakDirectionStep);
            const float length = std::sqrt(g.x * g.x + g.y * g.y);
            const glm::vec2 down = (length > 1e-4f) ? g / length : glm::vec2(0.0f, 1.0f);
            const glm::vec2 across(-down.y, down.x);
            const glm::vec2 q(worldX - cx, worldZ - cz);
            // Decalage propre a la tuile : ses trainees ne sont pas celles de la voisine.
            const uint32_t h = cellHash(static_cast<int32_t>(ix) + i, static_cast<int32_t>(iz) + j, seed, 9u);
            const float u = glm::dot(q, down) * params_.streakAlong + 64.0f * unitFloat(h);
            const float v = glm::dot(q, across) * params_.streakAcross + 64.0f * unitFloat(h ^ 0xA5A5A5A5u);
            noise[j][i] = 0.7f * valueNoise(u, v, seed) +
                          0.3f * valueNoise(u * 2.3f, v * 2.3f, seed ^ 0x2545F491u);
        }
    }
    const float value = glm::mix(glm::mix(noise[0][0], noise[0][1], sx),
                                 glm::mix(noise[1][0], noise[1][1], sx), sz);
    return ramp(value, params_.streakThreshold,
                params_.streakThreshold + params_.streakSoftness);
}

// ---------------------------------------------------------------------------
// Semis
// ---------------------------------------------------------------------------
void Rocks::fitBox(Cell& cell, float baseX, float baseZ, float size) const {
    float minY = 0.0f, maxY = 0.0f;
    bool first = true;
    for (const RockInstance& r : cell.rocks) {
        const float lo = r.position.y - kRockBuriedDepth * r.scale;
        const float hi = r.position.y + (kRockVisibleHeight + 0.3f) * r.scale;
        minY = first ? lo : std::min(minY, lo);
        maxY = first ? hi : std::max(maxY, hi);
        first = false;
    }
    if (first) {
        minY = maxY = terrain_.heightAt(baseX + 0.5f * size, baseZ + 0.5f * size);
    }
    // Marge horizontale : un bloc incline deborde un peu de sa cellule.
    cell.boxMin = { baseX - 1.5f, minY, baseZ - 1.5f };
    cell.boxMax = { baseX + size + 1.5f, maxY, baseZ + size + 1.5f };
}

bool Rocks::makeCandidate(float x, float z, RockKind kind, uint32_t slot, uint32_t slots,
                          uint32_t hSize, uint32_t hYaw, uint32_t hTint,
                          RockInstance& out) const {
    float sizeMin = 0.0f, sizeMax = 0.0f, far = 0.0f, near = 0.0f;
    switch (kind) {
    case RockKind::Boulder:
        sizeMin = params_.boulderSizeMin; sizeMax = params_.boulderSizeMax;
        far = params_.boulderDistanceFar; near = params_.boulderDistanceNear;
        break;
    case RockKind::Stone:
        sizeMin = params_.stoneSizeMin; sizeMax = params_.stoneSizeMax;
        far = params_.stoneDistanceFar; near = params_.stoneDistanceNear;
        break;
    case RockKind::Gravel:
        sizeMin = params_.gravelSizeMin; sizeMax = params_.gravelSizeMax;
        far = params_.gravelDistanceFar; near = params_.gravelDistanceNear;
        break;
    }
    const glm::vec2 gradient = walkTerrainGradient(terrain_, x, z, slopeProbe_.params().slopeSampleStep);
    out.kind = kind;
    out.position = { x, terrain_.heightAt(x, z), z };
    // Taille : loi en carre, les petits sujets dominent comme dans un vrai eboulis.
    const float s = unitFloat(hSize);
    out.scale = sizeMin + (sizeMax - sizeMin) * s * s;
    out.yaw = unitFloat(hYaw) * kTwoPi;
    out.variation = packRockVariation(unitFloat(hTint), gradient * params_.tiltFollow);
    const float t = slots > 1 ? static_cast<float>(slot) / static_cast<float>(slots - 1) : 0.0f;
    out.maxDistance = glm::mix(far, near, t);
    return true;
}

Rocks::Cell Rocks::buildBlockCell(int32_t cx, int32_t cz) const {
    Cell cell;
    const float size = params_.blockCell;
    const float baseX = static_cast<float>(cx) * size;
    const float baseZ = static_cast<float>(cz) * size;
    if (cover_ != nullptr) {
        const uint32_t total = params_.boulderSlots + params_.stoneSlots;
        for (uint32_t slot = 0; slot < total; ++slot) {
            const bool boulder = slot < params_.boulderSlots;
            const uint32_t kindSlot = boulder ? slot : slot - params_.boulderSlots;
            const uint32_t kindSlots = boulder ? params_.boulderSlots : params_.stoneSlots;
            const uint32_t stream = slot * 5u + 300u;
            const float x = baseX + unitFloat(cellHash(cx, cz, params_.seed, stream)) * size;
            const float z = baseZ + unitFloat(cellHash(cx, cz, params_.seed, stream + 1u)) * size;
            // 1. Filtre DUR (masque de la conception).
            if (!maskAt(x, z)) { continue; }
            // 2. Densite modulee par les trainees : l'emplacement n'est retenu que
            //    si la densite lui laisse la place (meme regle d'ordre que les arbres).
            const float allowed = streakDensityAt(x, z) * static_cast<float>(kindSlots);
            if (static_cast<float>(kindSlot) >= allowed) { continue; }
            // 3. Choix du mesh par la rugosite : les blocs ne vont que sur le rugueux.
            if (roughnessAt(x, z) < params_.roughBlocks) { continue; }
            RockInstance r;
            makeCandidate(x, z, boulder ? RockKind::Boulder : RockKind::Stone, kindSlot,
                          kindSlots, cellHash(cx, cz, params_.seed, stream + 2u),
                          cellHash(cx, cz, params_.seed, stream + 3u),
                          cellHash(cx, cz, params_.seed, stream + 4u), r);
            cell.rocks.push_back(r);
        }
    }
    fitBox(cell, baseX, baseZ, size);
    return cell;
}

Rocks::Cell Rocks::buildGravelCell(int32_t cx, int32_t cz) const {
    Cell cell;
    const float size = params_.gravelCell;
    const float baseX = static_cast<float>(cx) * size;
    const float baseZ = static_cast<float>(cz) * size;
    if (cover_ != nullptr) {
        // Rugosite UNE fois par cellule, au centre : la cellule de 4 m est de la
        // taille de la fenetre 5x5 de la definition, et 81 sondes par caillou
        // seraient le premier poste du semis.
        bool roughKnown = false;
        bool rough = false;
        const uint32_t seed = params_.seed ^ 0x6A09E667u;
        for (uint32_t slot = 0; slot < params_.gravelSlots; ++slot) {
            const uint32_t stream = slot * 5u + 700u;
            const float x = baseX + unitFloat(cellHash(cx, cz, seed, stream)) * size;
            const float z = baseZ + unitFloat(cellHash(cx, cz, seed, stream + 1u)) * size;
            if (!maskAt(x, z)) { continue; }
            const float allowed = streakDensityAt(x, z) * static_cast<float>(params_.gravelSlots);
            if (static_cast<float>(slot) >= allowed) { continue; }
            if (!roughKnown) {
                rough = roughnessAt(baseX + 0.5f * size, baseZ + 0.5f * size) >= params_.roughBlocks;
                roughKnown = true;
            }
            if (rough) { break; } // zone rugueuse : ce sont les blocs qui l'occupent
            RockInstance r;
            makeCandidate(x, z, RockKind::Gravel, slot, params_.gravelSlots,
                          cellHash(cx, cz, seed, stream + 2u), cellHash(cx, cz, seed, stream + 3u),
                          cellHash(cx, cz, seed, stream + 4u), r);
            cell.rocks.push_back(r);
        }
    }
    fitBox(cell, baseX, baseZ, size);
    return cell;
}

std::vector<RockInstance> Rocks::blockCellRocks(int32_t cx, int32_t cz) const {
    return buildBlockCell(cx, cz).rocks;
}
std::vector<RockInstance> Rocks::gravelCellRocks(int32_t cx, int32_t cz) const {
    return buildGravelCell(cx, cz).rocks;
}

Rocks::Cell Rocks::buildBlock(int32_t bx, int32_t bz, bool gravelGrid) const {
    // Un BLOC = kBlockCells x kBlockCells cellules de semis, cache d'un seul
    // tenant. MESURE : avec un cache par cellule, le semis coutait ~400 us par
    // image en Debug, surtout en recherches dans la table de hachage - alors que
    // plus de 90 % des cellules sont hors masque, donc vides.
    Cell block;
    bool first = true;
    for (int32_t j = 0; j < kBlockCells; ++j) {
        for (int32_t i = 0; i < kBlockCells; ++i) {
            const int32_t cx = bx * kBlockCells + i, cz = bz * kBlockCells + j;
            const Cell cell = gravelGrid ? buildGravelCell(cx, cz) : buildBlockCell(cx, cz);
            if (cell.rocks.empty()) { continue; }
            block.boxMin = first ? cell.boxMin : glm::min(block.boxMin, cell.boxMin);
            block.boxMax = first ? cell.boxMax : glm::max(block.boxMax, cell.boxMax);
            first = false;
            block.rocks.insert(block.rocks.end(), cell.rocks.begin(), cell.rocks.end());
        }
    }
    return block;
}

const Rocks::Cell& Rocks::cachedBlock(int32_t bx, int32_t bz, bool gravelGrid) const {
    Cache& cache = gravelGrid ? gravelCache_ : blockCache_;
    const CellKey key{ bx, bz };
    auto found = cache.find(key);
    if (found != cache.end()) { return found->second; }
    return cache.emplace(key, buildBlock(bx, bz, gravelGrid)).first->second;
}

void Rocks::gather(const glm::vec3& eye, const Frustum& frustum,
                   std::vector<InstanceData>& boulders, std::vector<InstanceData>& stones,
                   std::vector<InstanceData>& gravel) {
    boulders.clear();
    stones.clear();
    gravel.clear();
    if (cover_ != nullptr) {
        std::vector<InstanceData>* outs[3] = { &boulders, &stones, &gravel };
        gatherGrid(eye, frustum, false, outs);
        gatherGrid(eye, frustum, true, outs);
    }
    lastBoulders_ = static_cast<uint32_t>(boulders.size());
    lastStones_ = static_cast<uint32_t>(stones.size());
    lastGravel_ = static_cast<uint32_t>(gravel.size());
}

void Rocks::gatherGrid(const glm::vec3& eye, const Frustum& frustum, bool gravelGrid,
                       std::vector<InstanceData>* outs[3]) {
    const float cellSize = gravelGrid ? params_.gravelCell : params_.blockCell;
    const float reach = gravelGrid ? params_.gravelDistanceFar
                                   : std::max(params_.boulderDistanceFar, params_.stoneDistanceFar);
    const uint32_t budget = gravelGrid ? params_.gravelCellBudget : params_.blockCellBudget;
    Cache& cache = gravelGrid ? gravelCache_ : blockCache_;
    const float blockSize = static_cast<float>(kBlockCells) * cellSize;

    const auto bx0 = static_cast<int32_t>(std::floor((eye.x - reach) / blockSize));
    const auto bx1 = static_cast<int32_t>(std::floor((eye.x + reach) / blockSize));
    const auto bz0 = static_cast<int32_t>(std::floor((eye.z - reach) / blockSize));
    const auto bz1 = static_cast<int32_t>(std::floor((eye.z + reach) / blockSize));
    uint32_t built = 0;

    for (int32_t bz = bz0; bz <= bz1; ++bz) {
        for (int32_t bx = bx0; bx <= bx1; ++bx) {
            const float bMinX = static_cast<float>(bx) * blockSize;
            const float bMinZ = static_cast<float>(bz) * blockSize;
            const float bdx = eye.x - glm::clamp(eye.x, bMinX, bMinX + blockSize);
            const float bdz = eye.z - glm::clamp(eye.z, bMinZ, bMinZ + blockSize);
            if (bdx * bdx + bdz * bdz > reach * reach) { continue; }
            auto found = cache.find(CellKey{ bx, bz });
            if (found == cache.end()) {
                // Bloc inconnu : culle sur une boite grossiere AVANT de payer le
                // semis (cinq sondes + marge large, comme Vegetation), puis budget.
                float lo = terrain_.heightAt(bMinX + 0.5f * blockSize, bMinZ + 0.5f * blockSize);
                float hi = lo;
                for (int corner = 0; corner < 4; ++corner) {
                    const float h = terrain_.heightAt(bMinX + ((corner & 1) ? blockSize : 0.0f),
                                                      bMinZ + ((corner & 2) ? blockSize : 0.0f));
                    lo = std::min(lo, h);
                    hi = std::max(hi, h);
                }
                const float margin = gravelGrid ? 40.0f : 130.0f;
                if (!frustum.intersectsAABB({ bMinX, lo - margin, bMinZ },
                                            { bMinX + blockSize, hi + margin, bMinZ + blockSize })) {
                    continue;
                }
                if (built >= budget) { continue; } // servi a une image suivante
                ++built;
                found = cache.emplace(CellKey{ bx, bz }, buildBlock(bx, bz, gravelGrid)).first;
            }
            const Cell& block = found->second;
            if (block.rocks.empty() || !frustum.intersectsAABB(block.boxMin, block.boxMax)) {
                continue;
            }
            for (const RockInstance& r : block.rocks) {
                const glm::vec3 d = eye - r.position;
                const float d2 = glm::dot(d, d);
                if (d2 >= r.maxDistance * r.maxDistance) { continue; }
                const float distance = std::sqrt(d2);
                InstanceData instance;
                instance.position = r.position;
                instance.scale = r.scale;
                instance.yaw = r.yaw;
                instance.variation = r.variation;
                instance.fade = 1.0f - ramp(distance, 0.82f * r.maxDistance, r.maxDistance);
                outs[static_cast<int>(r.kind)]->push_back(instance);
            }
        }
    }

    // Purge : au-dela de 1,5 x la portee (plus un bloc), comme Vegetation.
    const float keep = 1.5f * reach + blockSize;
    for (auto it = cache.begin(); it != cache.end();) {
        const float ccx = (static_cast<float>(it->first.x) + 0.5f) * blockSize;
        const float ccz = (static_cast<float>(it->first.z) + 0.5f) * blockSize;
        const float ddx = eye.x - ccx, ddz = eye.z - ccz;
        it = (ddx * ddx + ddz * ddz > keep * keep) ? cache.erase(it) : std::next(it);
    }
}

Rocks::Stats Rocks::estimate(uint32_t step) const {
    Stats stats;
    if (cover_ == nullptr) { return stats; }
    step = std::max(step, 1u);
    const float cell = params_.blockCell;
    const auto half = static_cast<int32_t>((terrainExtent_ - kEdgeMargin) / cell) - 1;
    uint64_t cells = 0, masked = 0, probes = 0, boulders = 0, stones = 0, gravel = 0;
    const auto ratio = static_cast<int32_t>(std::lround(params_.blockCell / params_.gravelCell));
    for (int32_t cz = -half; cz <= half; cz += static_cast<int32_t>(step)) {
        for (int32_t cx = -half; cx <= half; cx += static_cast<int32_t>(step)) {
            ++cells;
            for (const RockInstance& r : buildBlockCell(cx, cz).rocks) {
                (r.kind == RockKind::Boulder ? boulders : stones) += 1;
            }
            // Une cellule de pierraille sur quatre de celles qui recouvrent la
            // cellule de blocs (coin), remise a l'echelle ensuite.
            gravel += buildGravelCell(cx * ratio, cz * ratio).rocks.size();
            for (int j = 0; j < 4; ++j) {
                for (int i = 0; i < 4; ++i) {
                    ++probes;
                    masked += maskAt((static_cast<float>(cx) + 0.125f + 0.25f * i) * cell,
                                     (static_cast<float>(cz) + 0.125f + 0.25f * j) * cell) ? 1 : 0;
                }
            }
        }
    }
    if (cells == 0) { return stats; }
    const double ha = static_cast<double>(cells) * cell * cell / 10000.0;
    const double gravelHa = static_cast<double>(cells) * params_.gravelCell * params_.gravelCell / 10000.0;
    stats.maskFraction = static_cast<double>(masked) / static_cast<double>(probes);
    stats.bouldersPerHa = static_cast<double>(boulders) / ha;
    stats.stonesPerHa = static_cast<double>(stones) / ha;
    stats.gravelPerHa = static_cast<double>(gravel) / gravelHa;
    return stats;
}

// ---------------------------------------------------------------------------
// Collision : gros blocs seulement
// ---------------------------------------------------------------------------
// Un gros bloc ne bloque que le corps qui est a sa hauteur : un parapente qui le
// survole, ou un joueur dont les pieds sont deja au-dessus de son sommet moins
// une marche (stepOver), passe. Meme tolerance de contact que Forest.
glm::vec3 Rocks::resolve(const BodyCylinder& body) const {
    glm::vec3 corrected = body.position;
    if (!params_.collide || cover_ == nullptr) { return corrected; }
    const float maxRadius = kRockFootprintRadius * params_.colliderRadiusFactor * params_.boulderSizeMax;
    const float reach = maxRadius + body.radius;
    const float cell = params_.blockCell * static_cast<float>(kBlockCells); // blocs
    for (int pass = 0; pass < 3; ++pass) {
        bool touched = false;
        const auto cx0 = static_cast<int32_t>(std::floor((corrected.x - reach) / cell));
        const auto cx1 = static_cast<int32_t>(std::floor((corrected.x + reach) / cell));
        const auto cz0 = static_cast<int32_t>(std::floor((corrected.z - reach) / cell));
        const auto cz1 = static_cast<int32_t>(std::floor((corrected.z + reach) / cell));
        for (int32_t cz = cz0; cz <= cz1; ++cz) {
            for (int32_t cx = cx0; cx <= cx1; ++cx) {
                for (const RockInstance& r : cachedBlock(cx, cz, false).rocks) {
                    if (r.kind != RockKind::Boulder || r.scale < params_.collideMinScale) { continue; }
                    const float top = r.position.y + kRockVisibleHeight * r.scale;
                    if (body.feetY + params_.stepOver >= top || body.headY <= r.position.y) { continue; }
                    const float radius = kRockFootprintRadius * params_.colliderRadiusFactor * r.scale;
                    const float contact = radius + body.radius;
                    const float dx = corrected.x - r.position.x;
                    const float dz = corrected.z - r.position.z;
                    const float d2 = dx * dx + dz * dz;
                    if (d2 >= contact * contact) { continue; }
                    const float d = std::sqrt(d2);
                    const glm::vec2 outward = (d > 1e-4f) ? glm::vec2(dx / d, dz / d) : glm::vec2(1.0f, 0.0f);
                    corrected.x = r.position.x + outward.x * contact;
                    corrected.z = r.position.z + outward.y * contact;
                    touched = true;
                }
            }
        }
        if (!touched) { break; }
    }
    return corrected;
}

bool Rocks::overlaps(const BodyCylinder& body) const {
    if (!params_.collide || cover_ == nullptr) { return false; }
    const float maxRadius = kRockFootprintRadius * params_.colliderRadiusFactor * params_.boulderSizeMax;
    const float reach = maxRadius + body.radius;
    const float cell = params_.blockCell * static_cast<float>(kBlockCells); // blocs
    const auto cx0 = static_cast<int32_t>(std::floor((body.position.x - reach) / cell));
    const auto cx1 = static_cast<int32_t>(std::floor((body.position.x + reach) / cell));
    const auto cz0 = static_cast<int32_t>(std::floor((body.position.z - reach) / cell));
    const auto cz1 = static_cast<int32_t>(std::floor((body.position.z + reach) / cell));
    constexpr float kContactEpsilon = 1e-3f; // meme tolerance que Forest::overlaps
    for (int32_t cz = cz0; cz <= cz1; ++cz) {
        for (int32_t cx = cx0; cx <= cx1; ++cx) {
            for (const RockInstance& r : cachedBlock(cx, cz, false).rocks) {
                if (r.kind != RockKind::Boulder || r.scale < params_.collideMinScale) { continue; }
                const float top = r.position.y + kRockVisibleHeight * r.scale;
                if (body.feetY + params_.stepOver >= top || body.headY <= r.position.y) { continue; }
                const float contact = kRockFootprintRadius * params_.colliderRadiusFactor * r.scale + body.radius;
                const float dx = body.position.x - r.position.x;
                const float dz = body.position.z - r.position.z;
                if (dx * dx + dz * dz < (contact - kContactEpsilon) * (contact - kContactEpsilon)) {
                    return true;
                }
            }
        }
    }
    return false;
}

} // namespace vercora
