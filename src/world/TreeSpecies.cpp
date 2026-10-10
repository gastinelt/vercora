#include "world/TreeSpecies.hpp"

#include <algorithm>
#include <cmath>

namespace vercora {
namespace {

// Hachage entier 32 bits (finalisateur de MurmurHash3). Meme fonction que celle
// employee ailleurs dans le projet pour transformer une clef en alea reproductible.
uint32_t mix(uint32_t h) {
    h ^= h >> 16;
    h *= 0x85EBCA6Bu;
    h ^= h >> 13;
    h *= 0xC2B2AE35u;
    h ^= h >> 16;
    return h;
}

// Quantification de la position avant hachage, en metres.
//
// POURQUOI QUANTIFIER. Deux systemes demandent l'essence du MEME arbre par des
// chemins de calcul differents (Forest garde une position posee sur le terrain,
// Vegetation la reconstruit depuis sa cellule de semis). Hacher les bits bruts
// d'un float ferait dependre la reponse du dernier bit de mantisse, donc du
// chemin - et deux arbres au meme endroit pourraient etre d'essences
// differentes. Un pas de 25 cm est bien plus fin que l'espacement minimal entre
// deux pieds (ForestParams::minSpacing = 5,5 m ; une cellule de decor fait
// 24 m), donc il ne fusionne jamais deux arbres distincts, et bien plus gros que
// le bruit de calcul.
constexpr float kPositionQuantum = 0.25f;

int32_t quantize(float value) {
    return static_cast<int32_t>(std::floor(value / kPositionQuantum));
}

float clamp01(float value) { return std::clamp(value, 0.0f, 1.0f); }

} // namespace

const char* treeSpeciesName(TreeSpecies species) {
    switch (species) {
        case TreeSpecies::Conifer:   return "resineux";
        case TreeSpecies::Broadleaf: return "feuillu";
    }
    return "inconnu";
}

float coniferShareFor(float aspectExposure, const TreeSpeciesParams& params) {
    // Exposition ecretee : une source qui renverrait n'importe quoi ne doit pas
    // pouvoir sortir la part de sa plage.
    const float exposure = std::clamp(aspectExposure, -1.0f, 1.0f);

    // Le SIGNE est le coeur de la regle : l'adret (exposure > 0) est plus chaud
    // et plus sec, donc MOINS de resineux ; l'ubac, plus frais et plus humide,
    // en porte davantage. C'est pour cela que le terme est soustrait.
    const float share = params.coniferShareNeutral - params.aspectSwing * exposure;

    // Bornes appliquees dans l'ordre min puis max, et non par un clamp qui
    // supposerait min <= max : un reglage incoherent doit donner une valeur
    // valide plutot qu'un comportement non defini.
    const float low = clamp01(params.coniferShareMin);
    const float high = clamp01(std::max(params.coniferShareMax, low));
    return std::clamp(clamp01(share), low, high);
}

TreeSpecies treeSpeciesAt(float worldX, float worldZ, float aspectExposure,
                          const TreeSpeciesParams& params) {
    const float share = coniferShareFor(aspectExposure, params);

    // Tirage uniforme dans [0, 1) issu du hachage de la position et de la
    // graine. Les deux coordonnees passent par des multiplicateurs impairs
    // distincts avant d'etre melangees : sans cela, les points de la diagonale
    // x = z se hacheraient tous pareil et une bande d'arbres identiques se
    // verrait sur le terrain.
    const uint32_t hx = static_cast<uint32_t>(quantize(worldX)) * 0x9E3779B1u;
    const uint32_t hz = static_cast<uint32_t>(quantize(worldZ)) * 0x85EBCA77u;
    const uint32_t h = mix(hx ^ mix(hz ^ params.seed));

    // Division par 2^32 : le quotient est dans [0, 1), donc un `share` de 0 ne
    // peut jamais rendre Conifer et un `share` de 1 le rend toujours.
    const float draw = static_cast<float>(h) * (1.0f / 4294967296.0f);
    return (draw < share) ? TreeSpecies::Conifer : TreeSpecies::Broadleaf;
}

TreeSpecies treeSpeciesAt(float worldX, float worldZ,
                          const AspectExposureSource* exposure,
                          const TreeSpeciesParams& params) {
    // Source absente = exposition nulle. Voir le "contrat total" de
    // AspectExposureSource : c'est ce qui rend la non-regression verifiable.
    const float value = (exposure != nullptr) ? exposure->aspectExposureAt(worldX, worldZ)
                                              : 0.0f;
    return treeSpeciesAt(worldX, worldZ, value, params);
}

} // namespace vercora
