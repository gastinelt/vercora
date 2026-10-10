#pragma once

#include "world/WaterSource.hpp"

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace vercora {

class HeightSampler;

// ===========================================================================
// PHASE 22 : les gisements d'ARGILE et de SABLE.
// ===========================================================================
//
//   AVERTISSEMENT, ET IL EST LE COEUR DE CE FICHIER : CE PLACEMENT N'EST PAS
//   GEOLOGIQUE. Le LiDAR ne porte AUCUNE information de nature de terrain - ni
//   lithologie, ni granulometrie, ni carte des formations superficielles. Il
//   donne une SURFACE, et rien d'autre. Il est donc strictement impossible de
//   distinguer, a partir de ces donnees, un vrai gisement d'argile d'un vrai
//   gisement de sable, ou de l'un ou l'autre d'un banc de calcaire.
//
//   Ce que fait ce fichier est un PROXY MORPHOLOGIQUE : il place les gisements
//   la ou, dans un massif de ce type, ils seraient PLAUSIBLES. C'est une
//   vraisemblance, pas une mesure, et rien ici ne doit etre lu comme une carte.
//   Le jour ou les formations superficielles du BRGM seront chargees, c'est
//   cette fonction qui disparaitra, pas ses appelants - exactement le sort
//   annonce pour findWaterSources quand l'IGN fournira le reseau
//   hydrographique.
//
// LA REGLE DU PROXY, ET POURQUOI ELLE. Les deux matieres sont des SEDIMENTS
// FINS : elles ne se forment pas la ou elles sont arrachees, elles se deposent
// la ou l'eau ralentit et les abandonne. Deux conditions morphologiques
// suivent, et ce sont exactement celles que ce fichier teste :
//
//   1. UN CREUX. Meme critere de concavite que world/WaterSource - huit sondes
//      de pourtour, dont un nombre minimal doit etre plus haut - et pour la
//      meme raison : c'est la seule forme, sur un relief mesure, ou quelque
//      chose peut s'accumuler au lieu d'etre emporte. Le code de detection est
//      volontairement le MEME que celui des points d'eau, aux seuils pres.
//
//   2. UN VERSANT AU-DESSUS. La matiere doit VENIR de quelque part : un creux
//      au milieu d'un plateau ne recoit rien. On mesure donc le denivele amont
//      (`upslope`), c'est-a-dire de combien le pourtour domine le point.
//
// CE QUI SEPARE L'ARGILE DU SABLE. Une seule variable, et elle est assumee : la
// PROXIMITE D'UN POINT D'EAU deja detecte par le jeu (Phase 16).
//
//   argile - a moins de clayWaterRadius d'un point d'eau. L'argile est la
//            fraction la PLUS FINE : elle ne se depose que la ou l'eau stagne
//            vraiment, c'est-a-dire au fond des cuvettes et des talwegs que le
//            detecteur de la Phase 16 retient deja.
//   sable  - au-dela. Plus grossier, il se depose plus haut et plus tot, au
//            debouche des pentes, sans avoir besoin d'eau stagnante.
//
// Ce partage est GROSSIER et il est faux par endroits, comme l'heuristique des
// points d'eau dont il herite : une argile de versant existe, un sable de fond
// de mare aussi. Il est retenu parce qu'il est VERIFIABLE (les tests headless le
// confrontent a un relief construit expres), bon marche (un balayage unique au
// chargement), et parce qu'il donne au joueur une regle qu'il peut APPRENDRE en
// jouant - "l'argile est pres de l'eau, le sable en amont" - au lieu d'un semis
// aleatoire qu'aucune observation ne recompense.
//
// Ne connait ni Vulkan, ni le rendu : il produit des positions et des natures.
// Entierement testable sans GPU.
// ===========================================================================

enum class DepositKind : uint8_t {
    Clay, // argile : creux PRES d'un point d'eau
    Sand, // sable  : creux LOIN d'un point d'eau
};

// Libelle francais au singulier, pour les journaux.
const char* depositLabel(DepositKind kind);

// Un gisement affleurant, ou le joueur peut creuser.
struct MineralDeposit {
    glm::vec3 position{0.0f}; // sur la surface : y = heightAt(x, z)
    DepositKind kind = DepositKind::Sand;

    // Concavite apparente en m (elevation moyenne du pourtour au-dessus du
    // point). Sert a classer les candidats du plus creux au moins creux, donc a
    // garder les meilleurs quand il y en a trop.
    float depth = 0.0f;

    // Denivele AMONT en m : de combien la plus haute des huit sondes domine le
    // point. C'est la mesure de "il y a un versant au-dessus qui alimente ce
    // creux". Un creux sans amont ne recoit rien et n'est pas retenu.
    float upslope = 0.0f;

    // Distance horizontale au point d'eau le plus proche, en m, au moment du
    // balayage. Vaut l'infini quand aucun point d'eau n'a ete fourni - et dans
    // ce cas TOUT est classe sable, ce qui est le comportement voulu : sans
    // eau connue, on ne peut rien affirmer sur l'argile.
    float waterDistance = 0.0f;
};

struct DepositSurveyParams {
    // Pas du balayage, en m. 20 m, MEME VALEUR ET MEME RAISON que
    // WaterSurveyParams::scanStep : le rayon utile d'un creux sur ce relief vaut
    // une vingtaine de metres, et un pas plus large passe simplement a cote.
    float scanStep = 20.0f;

    // Rayon des sondes de pourtour, en m.
    float probeDistance = 25.0f;

    // Demi-base de la lecture de pente, en m. 2,0, donc une base de 4 m : meme
    // valeur et MEME RAISON que BikeParams::slopeSampleStep et que
    // WaterSurveyParams::slopeSampleStep - au pas de 1 m, le bruit
    // decimetrique du LiDAR fait lire des pentes qui n'existent pas.
    float slopeSampleStep = 2.0f;

    // Pente locale maximale admise. 0,22 - donc PLUS PERMISSIF que les 0,15 des
    // points d'eau, et c'est voulu : l'eau doit stagner pour qu'on y boive, un
    // sediment n'a qu'a s'etre depose. Un cone de dejection au debouche d'un
    // ravin est nettement plus pentu qu'une mare, et l'exclure viderait la
    // moitie des emplacements credibles de ce massif (qui, rappel de la Phase
    // 16, est raide : 562 m de denivele sur 1 km).
    float maxSlope = 0.22f;

    // Concavite moyenne minimale, en m, pour que le point compte comme un creux.
    // 0,6 m sur 25 m de rayon, soit 2,4 % - plus bas que le 1,0 m des points
    // d'eau, meme raison que ci-dessus : on cherche un endroit ou la matiere se
    // pose, pas un endroit ou l'eau reste.
    float minDepth = 0.6f;

    // Nombre de sondes du pourtour (sur HUIT) qui doivent etre plus hautes d'au
    // moins minDepth. 4 sur 8 et non 5 : le seuil de 5 des points d'eau existe
    // pour ECARTER LES COLS, ou il n'y a rien a boire. Ici un col n'est pas
    // absurde - un replat de col recoit bel et bien des depots - et le critere
    // qui compte a la place est le denivele amont (minUpslope), qu'un col
    // franchit sans peine mais qu'un sommet ne franchit pas.
    int risingProbesRequired = 4;

    // Denivele amont minimal, en m : la plus haute sonde doit dominer le point
    // d'au moins ca. 3,0 m sur 25 m de rayon. C'est ce seuil, et non la
    // concavite, qui elimine les replats de crete - un creux de sommet est
    // concave mais n'a rien au-dessus de lui.
    float minUpslope = 3.0f;

    // Distance a un point d'eau en dessous de laquelle un creux est classe
    // ARGILE, en m. 120 m : de l'ordre de la separation des points d'eau
    // (150 m), donc un creux appartient au plus a un point d'eau, et les deux
    // matieres ne se superposent jamais au meme endroit.
    float clayWaterRadius = 120.0f;

    // Distance minimale entre deux gisements retenus, en m. 90 m : plus serre
    // que les 150 m des points d'eau parce qu'il y a DEUX matieres a placer et
    // qu'un joueur doit pouvoir en trouver sans traverser la carte.
    float minSeparation = 90.0f;

    // Garde-fou : au-dela, on ne garde que les plus creux.
    std::size_t maxDeposits = 64;
};

// Balaie le rectangle [minX, maxX] x [minZ, maxZ] (coordonnees monde, en m) et
// rend les gisements retenus, du plus creux au moins creux.
//
// `waterSources` est la liste deja produite par findWaterSources : elle sert
// UNIQUEMENT a classer argile/sable. Une liste vide est licite et rend alors
// tout en sable (voir MineralDeposit::waterDistance).
std::vector<MineralDeposit> findMineralDeposits(const HeightSampler& terrain,
                                                float minX, float maxX,
                                                float minZ, float maxZ,
                                                const std::vector<WaterSource>& waterSources,
                                                const DepositSurveyParams& params = {});

// Portee pour creuser, en m, mesuree a l'HORIZONTALE. 3,5 m : entre le
// ramassage d'objet (2,5 m) et la portee de boisson (4,0 m). Un gisement est une
// ZONE, comme un point d'eau, mais on doit s'en approcher plus pres pour y
// mettre les mains que pour y tremper une gourde.
inline constexpr float kDigReach = 3.5f;

// Gisement le plus proche d'une position, a l'horizontale. Renvoie nullptr si la
// liste est vide. outDistance, s'il est fourni, recoit la distance horizontale
// au gisement rendu.
const MineralDeposit* nearestDeposit(const std::vector<MineralDeposit>& deposits,
                                     const glm::vec3& position,
                                     float* outDistance = nullptr);

} // namespace vercora
