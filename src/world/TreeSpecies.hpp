#pragma once

#include <cstdint>

namespace vercora {

// ===========================================================================
// PHASE 26 : L'ESSENCE D'UN ARBRE, ET LE SEUL ENDROIT QUI LA DECIDE.
// ===========================================================================
//
// CE QUE CE FICHIER REFERME. world/Resin.hpp ecrivait, en Phase 22 : « Le jeu
// n'a AUCUNE notion d'essence d'arbre [...] un joueur ne peut pas VOIR quel
// arbre est resineux avant d'essayer ». La recolte de resine reposait donc sur
// un MARQUAGE ARBITRAIRE - un hachage de l'indice de l'arbre - sans aucun
// rapport avec ce qui etait dessine a l'ecran. C'etait la dette principale que
// cette phase-la assumait, et c'est elle qu'on referme ici.
//
// LE PRINCIPE : UNE SEULE SOURCE. L'essence est une FONCTION PURE de la
// position monde et d'une graine. Elle n'est stockee nulle part comme verite -
// Forest la memorise dans chaque arbre par commodite d'acces, Vegetation la
// met en cache avec sa cellule - mais les deux la tirent du MEME appel. Ce qui
// est resineux a l'oeil est donc resineux a la saignee, par construction et non
// par accord entre deux tables qu'il faudrait tenir a jour.
//
// LE FACTEUR : L'EXPOSITION ADRET / UBAC, DEJA CALCULEE AILLEURS. La Phase 23
// a fait de WeatherSystem la seule autorite sur l'adret/ubac, et la Phase 25 l'a
// rendue visible sans la recalculer. Cette phase ne la recalcule pas davantage :
// elle la CONSOMME au travers de AspectExposureSource, dont l'unique
// implementation du jeu delegue a WeatherSystem::aspectExposureAt.
//
// POURQUOI UNE EXPOSITION "DE MIDI" ET NON CELLE DE L'INSTANT. Le signal
// d'adret de WeatherSample::aspectC depend du soleil COURANT : il tourne avec
// l'heure et s'eteint la nuit - c'est justement ce qui fait sa justesse pour la
// temperature. Une essence, elle, ne peut pas changer entre midi et minuit. Ce
// qu'il faut ici est la grandeur CLIMATIQUE : l'ensoleillement que ce versant
// recoit habituellement. WeatherSystem l'expose en evaluant EXACTEMENT le meme
// signal au soleil de midi. Une seule formule, deux instants d'evaluation.
//
// UNE TENDANCE, PAS UNE REGLE. Un versant nord n'est pas fait que d'epiceas et
// un versant sud que de hetres. Les bornes de TreeSpeciesParams interdisent le
// 0 % et le 100 % : sur l'adret le plus franc il reste des resineux, et
// reciproquement. Un joueur doit lire une DOMINANTE en levant les yeux, pas une
// frontiere.
//
// CE FICHIER NE CONNAIT NI VULKAN, NI LE TERRAIN, NI LA METEO : une position,
// une exposition, une graine. Entierement testable sans GPU.
// ===========================================================================

// Les essences representees. Deux suffisent a porter l'opposition adret/ubac,
// et chacune a sa silhouette dans world/TreeMesh.hpp.
enum class TreeSpecies : uint8_t {
    Conifer = 0,   // sapin / epicea : silhouette conique, dominante en ubac
    Broadleaf = 1, // hetre / erable : houppier arrondi, dominante en adret
};

inline constexpr int kTreeSpeciesCount = 2;

// Nom francais, pour les journaux et les messages de jeu.
const char* treeSpeciesName(TreeSpecies species);

// ---------------------------------------------------------------------------
// Source d'exposition adret / ubac.
// ---------------------------------------------------------------------------
// Meme patron que GroundCoverSource (Phase 17) : une interface minuscule qui
// permet a Forest et Vegetation d'etre semees SANS connaitre WeatherSystem, donc
// d'etre testables sans monter tout le systeme meteo.
//
// CONTRAT TOTAL, et il compte : une source ABSENTE (pointeur nul) vaut une
// exposition NULLE partout, donc la part de resineux neutre partout, donc
// exactement le comportement d'avant cette phase cote proportions. C'est ce qui
// rend la non-regression verifiable au lieu d'etre esperee.
class AspectExposureSource {
public:
    virtual ~AspectExposureSource() = default;

    // Exposition CLIMATIQUE au point (worldX, worldZ), dans [-1, +1] :
    //   +1 = adret franc (versant au soleil), 0 = plat ou indifferent,
    //   -1 = ubac franc (versant a l'ombre).
    // Doit etre STABLE dans le temps : c'est une propriete du relief, pas de
    // l'heure qu'il est.
    virtual float aspectExposureAt(float worldX, float worldZ) const = 0;
};

struct TreeSpeciesParams {
    // Part de resineux sur un terrain SANS exposition marquee (plat, ou source
    // absente). 0,55 reprend TEL QUEL l'ancien ResinParams::coniferShare : le
    // reglage de la Phase 22 n'est pas perdu, il devient le point neutre autour
    // duquel l'exposition fait pencher la balance. Sur ce massif, l'epicea et le
    // sapin dominent l'etage montagnard sans etre seuls.
    float coniferShareNeutral = 0.55f;

    // Amplitude de la tendance : de combien la part de resineux se deplace entre
    // l'ubac franc et l'adret franc. 0,35 donne 0,90 en ubac et 0,20 en adret -
    // une dominante nette des deux cotes, jamais une exclusivite.
    float aspectSwing = 0.35f;

    // Bornes dures de la part de resineux. Elles ne mordent pas avec les valeurs
    // ci-dessus ; elles sont la pour que RE-REGLER aspectSwing ne puisse pas,
    // par inadvertance, transformer la tendance en regle stricte. Un versant
    // monospecifique se verrait immediatement, et se verrait comme un bug.
    float coniferShareMin = 0.10f;
    float coniferShareMax = 0.92f;

    // Graine du tirage. Reprend celle de l'ancien ResinParams pour que le
    // marquage ne soit pas rebattu sans raison, et reste DISTINCTE de celle du
    // semis (ForestParams::seed, VegetationParams::seed) : changer ou poussent
    // les arbres ne doit pas changer leur essence, et reciproquement.
    uint32_t seed = 20260827u;
};

// L'essence de l'arbre qui pousse en (worldX, worldZ), sous l'exposition donnee.
//
// FONCTION PURE et DETERMINISTE : meme position, meme exposition, meme graine =>
// meme essence, d'une execution a l'autre et d'un systeme a l'autre. C'est ce
// qui permet a Forest et a Vegetation d'y repondre pareil sans se parler, et ce
// qui rend les captures comparables.
//
// Le tirage porte sur la POSITION et non sur un indice : un indice n'existe que
// dans le systeme qui l'attribue, et les deux systemes n'en ont pas de commun.
TreeSpecies treeSpeciesAt(float worldX, float worldZ, float aspectExposure,
                          const TreeSpeciesParams& params = {});

// Meme chose, en allant chercher l'exposition dans la source (nulle = 0).
// Sucre destine aux appelants qui tiennent deja un AspectExposureSource*.
TreeSpecies treeSpeciesAt(float worldX, float worldZ,
                          const AspectExposureSource* exposure,
                          const TreeSpeciesParams& params = {});

// Part de resineux ATTENDUE pour une exposition donnee, dans [0, 1]. Exposee
// parce que c'est la grandeur que les tests headless verifient : on peut
// affirmer "plus de resineux en ubac" sur la LOI avant de le compter sur un
// echantillon, et l'ecart entre les deux est alors du bruit d'echantillonnage
// et non un desaccord.
float coniferShareFor(float aspectExposure, const TreeSpeciesParams& params = {});

} // namespace vercora
