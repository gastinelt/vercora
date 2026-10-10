#include "world/Bushes.hpp"

#include "core/Frustum.hpp"
#include "core/HeightSampler.hpp"
#include "world/BushMesh.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace vercora {
namespace {

constexpr float kTwoPi = 6.28318530718f;
constexpr float kEdgeMargin = 10.0f;
constexpr float kTreeGridCell = 32.0f;

// Memes primitives que Rocks.cpp / Vegetation.cpp (hachage sans etat).
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
float ramp(float v, float a, float b) {
    if (b <= a) { return v >= b ? 1.0f : 0.0f; }
    return std::clamp((v - a) / (b - a), 0.0f, 1.0f);
}

} // namespace

Bushes::Bushes(const HeightSampler& terrain, const HeightSampler* lowVeg,
               const HeightSampler* lowVegHeight, const HeightSampler* cover, float terrainExtent,
               const BushParams& params)
    : terrain_(terrain), lowVeg_(lowVeg), lowVegHeight_(lowVegHeight), cover_(cover),
      terrainExtent_(terrainExtent), params_(params) {
    if (params_.cell <= 0.0f || params_.slots == 0) {
        throw std::runtime_error("Bushes : cellule ou emplacements nuls.");
    }
    if (params_.heightMin <= 0.0f || params_.heightMin > params_.heightMax) {
        throw std::runtime_error("Bushes : bornes de hauteur incoherentes.");
    }
    if (params_.lowVegMin >= params_.lowVegFull) {
        throw std::runtime_error("Bushes : lowVegMin doit rester sous lowVegFull.");
    }
    if (params_.shrubDistanceNear > params_.shrubDistanceFar ||
        params_.treeDistanceNear > params_.treeDistanceFar) {
        throw std::runtime_error("Bushes : portee proche au-dela de la portee lointaine.");
    }
}

void Bushes::setTreeFootprints(std::vector<glm::vec3> footprints) {
    treeGrid_.clear();
    treeGridMaxRadius_ = 0.0f;
    for (const glm::vec3& f : footprints) {
        const Key k{ static_cast<int32_t>(std::floor(f.x / kTreeGridCell)),
                     static_cast<int32_t>(std::floor(f.y / kTreeGridCell)) };
        treeGrid_[k].push_back(f);
        treeGridMaxRadius_ = std::max(treeGridMaxRadius_, f.z);
    }
    cache_.clear();
}

bool Bushes::inTreeFootprint(float x, float z) const {
    if (treeGrid_.empty()) { return false; }
    const float r = treeGridMaxRadius_;
    const auto x0 = static_cast<int32_t>(std::floor((x - r) / kTreeGridCell));
    const auto x1 = static_cast<int32_t>(std::floor((x + r) / kTreeGridCell));
    const auto z0 = static_cast<int32_t>(std::floor((z - r) / kTreeGridCell));
    const auto z1 = static_cast<int32_t>(std::floor((z + r) / kTreeGridCell));
    for (int32_t cz = z0; cz <= z1; ++cz) {
        for (int32_t cx = x0; cx <= x1; ++cx) {
            const auto it = treeGrid_.find(Key{ cx, cz });
            if (it == treeGrid_.end()) { continue; }
            for (const glm::vec3& f : it->second) {
                const float dx = x - f.x, dz = z - f.y;
                if (dx * dx + dz * dz < f.z * f.z) { return true; }
            }
        }
    }
    return false;
}

float Bushes::coverAt(float x, float z) const {
    return std::clamp(cover_->heightAt(x, z), 0.0f, 1.0f);
}
float Bushes::lowVegAt(float x, float z) const {
    return std::clamp(lowVeg_->heightAt(x, z), 0.0f, 1.0f);
}

bool Bushes::maskAt(float worldX, float worldZ) const {
    if (!enabled()) { return false; }
    const float usable = terrainExtent_ - kEdgeMargin;
    if (std::abs(worldX) > usable || std::abs(worldZ) > usable) { return false; }
    const float f3 = lowVegAt(worldX, worldZ);
    if (f3 < params_.lowVegMin) { return false; }
    const float cover = coverAt(worldX, worldZ);
    if (cover < params_.rockCoverMax) { return false; } // domaine des rochers
    return cover - f3 < params_.highVegMax;
}

float Bushes::densityAt(float worldX, float worldZ) const {
    if (!maskAt(worldX, worldZ)) { return 0.0f; }
    const float t = ramp(lowVegAt(worldX, worldZ), params_.lowVegMin, params_.lowVegFull);
    return params_.minDensity + (1.0f - params_.minDensity) * t;
}

std::vector<BushInstance> Bushes::cellBushes(int32_t cx, int32_t cz) const {
    std::vector<BushInstance> out;
    if (!enabled()) { return out; }
    const float size = params_.cell;
    const float baseX = static_cast<float>(cx) * size;
    const float baseZ = static_cast<float>(cz) * size;
    for (uint32_t slot = 0; slot < params_.slots; ++slot) {
        const uint32_t stream = slot * 6u + 40u;
        const float x = baseX + unitFloat(cellHash(cx, cz, params_.seed, stream)) * size;
        const float z = baseZ + unitFloat(cellHash(cx, cz, params_.seed, stream + 1u)) * size;
        // 1. Filtre dur + densite : l'emplacement `slot` n'est retenu que si la
        //    densite locale lui laisse la place (meme regle que Rocks/Vegetation).
        const float density = densityAt(x, z);
        if (density <= 0.0f || static_cast<float>(slot) >= density * static_cast<float>(params_.slots)) {
            continue;
        }
        // 2. Hauteur tiree SOUS la hauteur max de classe 3 de la cellule 5 m.
        const float hMap = lowVegHeight_->heightAt(x, z);
        if (!(hMap >= params_.heightMapMin)) { continue; } // pas de hauteur mesuree
        const float draw = glm::mix(params_.heightDrawMin, params_.heightDrawMax,
                                    unitFloat(cellHash(cx, cz, params_.seed, stream + 2u)));
        const float height = std::clamp(hMap * draw, params_.heightMin, params_.heightMax);
        // 3. Pas dans l'emprise d'un arbre existant.
        if (inTreeFootprint(x, z)) { continue; }
        BushInstance b;
        b.kind = height >= params_.smallTreeFrom ? BushKind::SmallTree : BushKind::Shrub;
        b.position = { x, terrain_.heightAt(x, z), z };
        b.height = height;
        b.yaw = unitFloat(cellHash(cx, cz, params_.seed, stream + 3u)) * kTwoPi;
        b.tint = unitFloat(cellHash(cx, cz, params_.seed, stream + 4u));
        const float t = params_.slots > 1
                            ? static_cast<float>(slot) / static_cast<float>(params_.slots - 1)
                            : 0.0f;
        b.maxDistance = b.kind == BushKind::SmallTree
                            ? glm::mix(params_.treeDistanceFar, params_.treeDistanceNear, t)
                            : glm::mix(params_.shrubDistanceFar, params_.shrubDistanceNear, t);
        out.push_back(b);
    }
    return out;
}

Bushes::Block Bushes::buildBlock(int32_t bx, int32_t bz) const {
    Block block;
    bool first = true;
    for (int32_t j = 0; j < kBlockCells; ++j) {
        for (int32_t i = 0; i < kBlockCells; ++i) {
            for (const BushInstance& b : cellBushes(bx * kBlockCells + i, bz * kBlockCells + j)) {
                const float r = kShrubFootprintRadius * b.height;
                const glm::vec3 lo(b.position.x - r, b.position.y - kBushBuriedDepth * b.height, b.position.z - r);
                const glm::vec3 hi(b.position.x + r, b.position.y + b.height, b.position.z + r);
                block.boxMin = first ? lo : glm::min(block.boxMin, lo);
                block.boxMax = first ? hi : glm::max(block.boxMax, hi);
                first = false;
                block.bushes.push_back(b);
            }
        }
    }
    return block;
}

void Bushes::gather(const glm::vec3& eye, const Frustum& frustum, std::vector<InstanceData>& shrubs,
                    std::vector<InstanceData>& smallTrees) {
    shrubs.clear();
    smallTrees.clear();
    if (enabled()) {
        const float reach = std::max(params_.shrubDistanceFar, params_.treeDistanceFar);
        const float blockSize = static_cast<float>(kBlockCells) * params_.cell;
        const auto bx0 = static_cast<int32_t>(std::floor((eye.x - reach) / blockSize));
        const auto bx1 = static_cast<int32_t>(std::floor((eye.x + reach) / blockSize));
        const auto bz0 = static_cast<int32_t>(std::floor((eye.z - reach) / blockSize));
        const auto bz1 = static_cast<int32_t>(std::floor((eye.z + reach) / blockSize));
        uint32_t built = 0;
        for (int32_t bz = bz0; bz <= bz1; ++bz) {
            for (int32_t bx = bx0; bx <= bx1; ++bx) {
                const float mx = static_cast<float>(bx) * blockSize;
                const float mz = static_cast<float>(bz) * blockSize;
                const float dx = eye.x - std::clamp(eye.x, mx, mx + blockSize);
                const float dz = eye.z - std::clamp(eye.z, mz, mz + blockSize);
                if (dx * dx + dz * dz > reach * reach) { continue; }
                auto found = cache_.find(Key{ bx, bz });
                if (found == cache_.end()) {
                    float lo = terrain_.heightAt(mx + 0.5f * blockSize, mz + 0.5f * blockSize);
                    float hi = lo;
                    for (int c = 0; c < 4; ++c) {
                        const float h = terrain_.heightAt(mx + ((c & 1) ? blockSize : 0.0f),
                                                          mz + ((c & 2) ? blockSize : 0.0f));
                        lo = std::min(lo, h);
                        hi = std::max(hi, h);
                    }
                    if (!frustum.intersectsAABB({ mx, lo - 60.0f, mz },
                                                { mx + blockSize, hi + 60.0f, mz + blockSize })) {
                        continue;
                    }
                    if (built >= params_.blockBudget) { continue; }
                    ++built;
                    found = cache_.emplace(Key{ bx, bz }, buildBlock(bx, bz)).first;
                }
                const Block& block = found->second;
                if (block.bushes.empty() || !frustum.intersectsAABB(block.boxMin, block.boxMax)) {
                    continue;
                }
                for (const BushInstance& b : block.bushes) {
                    const glm::vec3 d = eye - b.position;
                    const float d2 = glm::dot(d, d);
                    if (d2 >= b.maxDistance * b.maxDistance) { continue; }
                    InstanceData inst;
                    inst.position = b.position;
                    inst.scale = b.height;
                    inst.yaw = b.yaw;
                    inst.variation = b.tint;
                    inst.fade = 1.0f - ramp(std::sqrt(d2), 0.82f * b.maxDistance, b.maxDistance);
                    (b.kind == BushKind::SmallTree ? smallTrees : shrubs).push_back(inst);
                }
            }
        }
        const float keep = 1.5f * reach + blockSize;
        for (auto it = cache_.begin(); it != cache_.end();) {
            const float cx = (static_cast<float>(it->first.x) + 0.5f) * blockSize;
            const float cz = (static_cast<float>(it->first.z) + 0.5f) * blockSize;
            const float ex = eye.x - cx, ez = eye.z - cz;
            it = (ex * ex + ez * ez > keep * keep) ? cache_.erase(it) : std::next(it);
        }
    }
    lastShrubs_ = static_cast<uint32_t>(shrubs.size());
    lastTrees_ = static_cast<uint32_t>(smallTrees.size());
}

Bushes::Stats Bushes::estimate(uint32_t step) const {
    Stats stats;
    if (!enabled()) { return stats; }
    step = std::max(step, 1u);
    const float cell = params_.cell;
    const auto half = static_cast<int32_t>((terrainExtent_ - kEdgeMargin) / cell) - 1;
    uint64_t cells = 0, masked = 0, shrubs = 0, trees = 0;
    for (int32_t cz = -half; cz <= half; cz += static_cast<int32_t>(step)) {
        for (int32_t cx = -half; cx <= half; cx += static_cast<int32_t>(step)) {
            ++cells;
            masked += maskAt((static_cast<float>(cx) + 0.5f) * cell, (static_cast<float>(cz) + 0.5f) * cell) ? 1 : 0;
            for (const BushInstance& b : cellBushes(cx, cz)) {
                (b.kind == BushKind::SmallTree ? trees : shrubs) += 1;
            }
        }
    }
    if (cells == 0) { return stats; }
    const double ha = static_cast<double>(cells) * cell * cell / 10000.0;
    stats.maskFraction = static_cast<double>(masked) / static_cast<double>(cells);
    stats.shrubsPerHa = static_cast<double>(shrubs) / ha;
    stats.smallTreesPerHa = static_cast<double>(trees) / ha;
    return stats;
}

} // namespace vercora
