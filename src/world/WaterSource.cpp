#include "world/WaterSource.hpp"

#include "core/HeightSampler.hpp"

#include <algorithm>
#include <cmath>

namespace vercora {

namespace {

// Les HUIT directions de sonde, normalisees : les quatre axes et les quatre
// diagonales. Le pourquoi de huit et non quatre est dans WaterSurveyParams
// (risingProbesRequired) - c'est ce qui separe un talweg d'un col.
constexpr float kDiagonal = 0.70710678f; // 1/sqrt(2)
constexpr std::size_t kProbeCount = 8;
const glm::vec2 kProbeDirections[kProbeCount] = {
    { 1.0f, 0.0f}, {-1.0f, 0.0f}, {0.0f,  1.0f}, {0.0f, -1.0f},
    { kDiagonal,  kDiagonal}, { kDiagonal, -kDiagonal},
    {-kDiagonal,  kDiagonal}, {-kDiagonal, -kDiagonal},
};

// Pente locale (magnitude du gradient), differences finies centrees. Meme
// construction que Bike::terrainGradient et Needs::terrainExposure ; elle n'est
// pas partagee avec eux parce que ce fichier ne doit dependre que du
// HeightSampler, exactement comme les trois activites.
float localSlope(const HeightSampler& terrain, float x, float z, float step) {
    const float s = step > 0.0f ? step : 2.0f;
    const float dhdx = (terrain.heightAt(x + s, z) - terrain.heightAt(x - s, z)) / (2.0f * s);
    const float dhdz = (terrain.heightAt(x, z + s) - terrain.heightAt(x, z - s)) / (2.0f * s);
    return std::sqrt(dhdx * dhdx + dhdz * dhdz);
}

} // namespace

std::vector<WaterSource> findWaterSources(const HeightSampler& terrain,
                                          float minX, float maxX,
                                          float minZ, float maxZ,
                                          const WaterSurveyParams& params) {
    std::vector<WaterSource> found;

    const float step = params.scanStep > 0.0f ? params.scanStep : 50.0f;
    const float probe = params.probeDistance > 0.0f ? params.probeDistance : 25.0f;
    if (!(maxX > minX) || !(maxZ > minZ)) {
        return found;
    }

    // Les sondes sortiraient du rectangle sur les bords ; heightAt est TOTALE
    // (voir son contrat) et prolongerait la hauteur du bord, ce qui fabriquerait
    // de fausses cuvettes tout le long du perimetre. On rentre donc d'une
    // portee de sonde.
    const float x0 = minX + probe;
    const float x1 = maxX - probe;
    const float z0 = minZ + probe;
    const float z1 = maxZ - probe;
    if (!(x1 > x0) || !(z1 > z0)) {
        return found;
    }

    std::vector<WaterSource> candidates;

    for (float z = z0; z <= z1; z += step) {
        for (float x = x0; x <= x1; x += step) {
            if (localSlope(terrain, x, z, params.slopeSampleStep) > params.maxSlope) {
                continue;
            }

            const float h = terrain.heightAt(x, z);

            int rising = 0;
            float sum = 0.0f;
            for (const glm::vec2& direction : kProbeDirections) {
                const float rise =
                    terrain.heightAt(x + direction.x * probe, z + direction.y * probe) - h;
                sum += rise;
                if (rise >= params.minDepth) {
                    ++rising;
                }
            }
            if (rising < params.risingProbesRequired) {
                continue;
            }

            const float depth = sum / static_cast<float>(kProbeCount);
            if (depth < params.minDepth) {
                continue;
            }

            candidates.push_back(WaterSource{glm::vec3(x, h, z), depth});
        }
    }

    // Du plus creux au moins creux : c'est dans cet ordre qu'on veut les garder
    // quand il y en a trop, et c'est aussi celui qui rend l'ecartement stable -
    // le meilleur point d'un groupe survit, pas celui que le balayage a croise
    // en premier.
    std::sort(candidates.begin(), candidates.end(),
              [](const WaterSource& a, const WaterSource& b) {
                  if (a.depth != b.depth) {
                      return a.depth > b.depth;
                  }
                  // Depart au coordonnees a egalite de profondeur : sans lui,
                  // deux points aussi creux l'un que l'autre pourraient sortir
                  // dans un ordre dependant de l'implementation du tri, et le
                  // resultat cesserait d'etre reproductible d'un run a l'autre.
                  if (a.position.x != b.position.x) {
                      return a.position.x < b.position.x;
                  }
                  return a.position.z < b.position.z;
              });

    const float separation2 = params.minSeparation * params.minSeparation;
    for (const WaterSource& candidate : candidates) {
        if (found.size() >= params.maxSources) {
            break;
        }
        bool tooClose = false;
        for (const WaterSource& kept : found) {
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

const WaterSource* nearestWaterSource(const std::vector<WaterSource>& sources,
                                      const glm::vec3& position,
                                      float* outDistance) {
    const WaterSource* best = nullptr;
    float bestDistance2 = 0.0f;

    for (const WaterSource& source : sources) {
        const float dx = source.position.x - position.x;
        const float dz = source.position.z - position.z;
        const float d2 = dx * dx + dz * dz;
        if (best == nullptr || d2 < bestDistance2) {
            best = &source;
            bestDistance2 = d2;
        }
    }

    if (outDistance) {
        *outDistance = best ? std::sqrt(bestDistance2) : 0.0f;
    }
    return best;
}

} // namespace vercora
