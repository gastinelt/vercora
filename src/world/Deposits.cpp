#include "world/Deposits.hpp"

#include "core/HeightSampler.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace vercora {

namespace {

// Les HUIT memes directions de sonde que world/WaterSource.cpp. Recopiees et non
// partagees : les deux fichiers ne doivent dependre que du HeightSampler, et
// sortir huit constantes dans un en-tete commun creerait une dependance entre
// deux detecteurs qui n'ont aucune raison d'evoluer ensemble.
constexpr float kDiagonal = 0.70710678f; // 1/sqrt(2)
constexpr std::size_t kProbeCount = 8;
const glm::vec2 kProbeDirections[kProbeCount] = {
    { 1.0f, 0.0f}, {-1.0f, 0.0f}, {0.0f,  1.0f}, {0.0f, -1.0f},
    { kDiagonal,  kDiagonal}, { kDiagonal, -kDiagonal},
    {-kDiagonal,  kDiagonal}, {-kDiagonal, -kDiagonal},
};

float localSlope(const HeightSampler& terrain, float x, float z, float step) {
    const float s = step > 0.0f ? step : 2.0f;
    const float dhdx = (terrain.heightAt(x + s, z) - terrain.heightAt(x - s, z)) / (2.0f * s);
    const float dhdz = (terrain.heightAt(x, z + s) - terrain.heightAt(x, z - s)) / (2.0f * s);
    return std::sqrt(dhdx * dhdx + dhdz * dhdz);
}

// Distance horizontale au point d'eau le plus proche. Rend l'infini quand la
// liste est vide : tout est alors classe sable, ce qui est le comportement
// annonce dans l'en-tete - sans eau connue, on n'affirme rien sur l'argile.
float distanceToWater(const std::vector<WaterSource>& sources, float x, float z) {
    float best = std::numeric_limits<float>::infinity();
    for (const WaterSource& source : sources) {
        const float dx = source.position.x - x;
        const float dz = source.position.z - z;
        const float d2 = dx * dx + dz * dz;
        if (d2 < best) {
            best = d2;
        }
    }
    return std::isinf(best) ? best : std::sqrt(best);
}

} // namespace

const char* depositLabel(DepositKind kind) {
    switch (kind) {
        case DepositKind::Clay: return "argile";
        case DepositKind::Sand: return "sable";
    }
    return "gisement";
}

std::vector<MineralDeposit> findMineralDeposits(const HeightSampler& terrain,
                                                float minX, float maxX,
                                                float minZ, float maxZ,
                                                const std::vector<WaterSource>& waterSources,
                                                const DepositSurveyParams& params) {
    std::vector<MineralDeposit> found;

    const float step = params.scanStep > 0.0f ? params.scanStep : 20.0f;
    const float probe = params.probeDistance > 0.0f ? params.probeDistance : 25.0f;
    if (!(maxX > minX) || !(maxZ > minZ)) {
        return found;
    }

    // On rentre d'une portee de sonde, MEME RAISON qu'au detecteur de points
    // d'eau : heightAt est TOTALE et prolonge la hauteur du bord au-dela des
    // donnees, ce qui fabriquerait de fausses cuvettes tout le long du
    // perimetre.
    const float x0 = minX + probe;
    const float x1 = maxX - probe;
    const float z0 = minZ + probe;
    const float z1 = maxZ - probe;
    if (!(x1 > x0) || !(z1 > z0)) {
        return found;
    }

    std::vector<MineralDeposit> candidates;

    for (float z = z0; z <= z1; z += step) {
        for (float x = x0; x <= x1; x += step) {
            if (localSlope(terrain, x, z, params.slopeSampleStep) > params.maxSlope) {
                continue;
            }

            const float h = terrain.heightAt(x, z);

            int rising = 0;
            float sum = 0.0f;
            float highest = 0.0f; // denivele de la sonde la PLUS haute
            for (const glm::vec2& direction : kProbeDirections) {
                const float rise =
                    terrain.heightAt(x + direction.x * probe, z + direction.y * probe) - h;
                sum += rise;
                if (rise >= params.minDepth) {
                    ++rising;
                }
                if (rise > highest) {
                    highest = rise;
                }
            }
            if (rising < params.risingProbesRequired) {
                continue;
            }

            const float depth = sum / static_cast<float>(kProbeCount);
            if (depth < params.minDepth) {
                continue;
            }

            // LE critere qui distingue ce detecteur de celui des points d'eau :
            // il faut un versant AU-DESSUS. Un creux sans amont ne recoit aucun
            // sediment, et c'est ce test - pas la concavite - qui elimine les
            // replats de crete.
            if (highest < params.minUpslope) {
                continue;
            }

            const float water = distanceToWater(waterSources, x, z);

            MineralDeposit deposit;
            deposit.position = glm::vec3(x, h, z);
            deposit.kind = (water <= params.clayWaterRadius) ? DepositKind::Clay
                                                             : DepositKind::Sand;
            deposit.depth = depth;
            deposit.upslope = highest;
            deposit.waterDistance = water;
            candidates.push_back(deposit);
        }
    }

    // Du plus creux au moins creux, avec depart aux coordonnees a egalite :
    // MEME TRI que findWaterSources, et pour la meme raison - sans le second
    // critere, deux points aussi creux l'un que l'autre pourraient sortir dans
    // un ordre dependant de l'implementation du tri, et le resultat cesserait
    // d'etre reproductible d'un run a l'autre.
    std::sort(candidates.begin(), candidates.end(),
              [](const MineralDeposit& a, const MineralDeposit& b) {
                  if (a.depth != b.depth) {
                      return a.depth > b.depth;
                  }
                  if (a.position.x != b.position.x) {
                      return a.position.x < b.position.x;
                  }
                  return a.position.z < b.position.z;
              });

    const float separation2 = params.minSeparation * params.minSeparation;
    for (const MineralDeposit& candidate : candidates) {
        if (found.size() >= params.maxDeposits) {
            break;
        }
        bool tooClose = false;
        for (const MineralDeposit& kept : found) {
            const float dx = kept.position.x - candidate.position.x;
            const float dz = kept.position.z - candidate.position.z;
            if (dx * dx + dz * dz < separation2) {
                tooClose = true;
                break;
            }
        }
        if (!tooClose) {
            found.push_back(candidate);
        }
    }

    return found;
}

const MineralDeposit* nearestDeposit(const std::vector<MineralDeposit>& deposits,
                                     const glm::vec3& position,
                                     float* outDistance) {
    const MineralDeposit* best = nullptr;
    float bestDistance2 = 0.0f;

    for (const MineralDeposit& deposit : deposits) {
        const float dx = deposit.position.x - position.x;
        const float dz = deposit.position.z - position.z;
        const float d2 = dx * dx + dz * dz;
        if (best == nullptr || d2 < bestDistance2) {
            best = &deposit;
            bestDistance2 = d2;
        }
    }

    if (outDistance) {
        *outDistance = best ? std::sqrt(bestDistance2) : 0.0f;
    }
    return best;
}

} // namespace vercora
