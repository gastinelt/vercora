#pragma once

#include <glm/glm.hpp>

#include <cstddef>
#include <vector>

namespace vercora {

class HeightSampler;

// ===========================================================================
// PHASE 16 : les points d'eau.
// ===========================================================================
// Le jeu n'a PAS d'eau : ni surface liquide, ni rendu, ni physique de fluide.
// Ce fichier ne pretend pas en ajouter. Il repond a une question beaucoup plus
// etroite, celle que pose le cahier des charges de la phase : ou, sur un relief
// LiDAR reel, y a-t-il un endroit ou il serait credible de boire ?
//
// La reponse est cherchee dans la FORME du terrain, sans donnee
// supplementaire : l'eau s'accumule dans les creux et les talwegs, donc dans
// les points a la fois PEU PENTUS et CONCAVES - ceux que le relief entoure de
// terrain plus haut. C'est exactement ce que teste findWaterSources.
//
// C'est une heuristique, et elle est fausse par endroits : une source de
// versant ne le passe pas, et un replat de col assez ferme peut le passer. Elle est retenue parce
// qu'elle est verifiable (les tests headless la confrontent a une cuvette
// construite expres, a un plan incline et a une crete), qu'elle est bon marche (un
// balayage unique au chargement, voir WaterSurveyParams::scanStep), et
// surtout parce que la lecon de la Phase 14 vaut ici aussi : savoir OU il y a
// de la pente ne dit pas par ou l'aborder, et savoir ou l'eau POURRAIT etre ne
// dit pas qu'elle y est. Le jour ou le reseau hydrographique de l'IGN sera
// charge, c'est cette fonction qui disparaitra, pas ses appelants.
// ===========================================================================

// Un point ou le joueur peut boire.
struct WaterSource {
    glm::vec3 position{0.0f}; // sur la surface : y = heightAt(x, z)

    // Profondeur apparente de la cuvette en m (elevation moyenne du pourtour
    // au-dessus du point). Sert a classer les candidats du plus creux au moins
    // creux, donc a garder les meilleurs quand il y en a trop.
    float depth = 0.0f;
};

struct WaterSurveyParams {
    // Pas du balayage, en m. 20 m, et ce n'est PAS un reglage de confort : le
    // rayon utile d'un creux (celui ou la pente reste sous maxSlope tout en
    // gardant une concavite d'au moins minDepth sur 25 m) vaut une vingtaine de
    // metres. Un pas plus large passe simplement a cote. Mesure sur la dalle
    // reelle du Gua : au pas de 50 m, ZERO point d'eau sur 1 km2 ; au pas de
    // 20 m, quatre. La zone de 25 km2 demande 62 500 candidats, soit environ
    // 800 000 appels a heightAt, une seule fois au chargement.
    float scanStep = 20.0f;

    // Rayon des sondes de pourtour, en m. 25 m : assez large pour voir un
    // talweg, assez etroit pour ne pas confondre un vallon avec la vallee
    // entiere.
    float probeDistance = 25.0f;

    // Demi-base de la lecture de pente, en m. 2,0, donc une base de 4 m :
    // meme valeur et MEME RAISON que BikeParams::slopeSampleStep - au pas de
    // 1 m, le bruit decimetrique du LiDAR fait lire des pentes qui n'existent
    // pas, et le filtre de platitude ne selectionnerait plus rien.
    float slopeSampleStep = 2.0f;

    // Pente locale maximale admise. 0,15 : au-dela de 15 %, l'eau s'ecoule au
    // lieu de stagner. Le seuil etait a 0,10 dans une premiere version ; sur la
    // dalle reelle, SEULS 10 points sur 361 passent sous 10 % de pente - ce
    // massif est raide, 562 m de denivele sur 1 km - et le detecteur ne rendait
    // rien. 15 % reste tres en dessous des 60 % typiques du versant.
    float maxSlope = 0.15f;

    // Elevation moyenne du pourtour au-dessus du point, en m, pour que le point
    // compte comme un creux. 1,0 m sur 25 m de rayon, soit une concavite de
    // 4 % - modeste, mais c'est ce qu'on trouve reellement en montagne hors des
    // lacs, qui eux ne sont pas dans cette dalle.
    float minDepth = 1.0f;

    // Nombre de sondes du pourtour (sur HUIT) qui doivent etre plus hautes d'au
    // moins minDepth.
    //
    // POURQUOI HUIT SONDES ET NON QUATRE. Avec quatre sondes sur les axes, un
    // talweg - la forme qui compte vraiment ici - en a deux plus hautes (les
    // deux versants) et deux plus basses (l'amont et l'aval). Exiger trois
    // sondes montantes ne retenait donc que les cuvettes FERMEES, qui
    // n'existent pratiquement pas sur ce relief : mesure, zero point d'eau sur
    // la dalle reelle. Descendre a deux sondes sur quatre, en revanche, laisse
    // passer les COLS, qui ont exactement la meme signature et ou il n'y a rien
    // a boire.
    //
    // Huit directions separent les deux cas : un talweg a ses deux versants qui
    // couvrent chacun plus d'une direction (5 a 6 sondes montantes), un col n'en
    // a que quatre. Le seuil de 5 sur 8 est donc au-dessus du col et en dessous
    // du talweg. Verifie sur la dalle reelle : 4 points retenus la ou le critere
    // en quatre sondes en rendait 0 ou 12 selon le seuil.
    int risingProbesRequired = 5;

    // Distance minimale entre deux points retenus, en m. 150 m : sans elle, une
    // cuvette de 300 m de large rendrait quarante points collants.
    float minSeparation = 150.0f;

    // Garde-fou : au-dela, on ne garde que les plus creux. Un joueur n'a pas
    // besoin de plus, et une liste bornee garantit que la recherche du plus
    // proche reste triviale a chaque image.
    std::size_t maxSources = 64;
};

// Balaie le rectangle [minX, maxX] x [minZ, maxZ] (coordonnees monde, en m) et
// rend les points d'eau retenus, du plus creux au moins creux.
//
// Le rectangle est celui du terrain charge ; l'appelant le connait
// (Heightmap::halfSize), la fonction ne le devine pas - c'est ce qui lui permet
// de rester dependante du seul HeightSampler, comme Bike et Climber.
std::vector<WaterSource> findWaterSources(const HeightSampler& terrain,
                                          float minX, float maxX,
                                          float minZ, float maxZ,
                                          const WaterSurveyParams& params = {});

// Portee pour boire, en m, mesuree a l'HORIZONTALE. 4 m : un peu plus que le
// ramassage d'objet (2,5 m), parce qu'un point d'eau est une zone et non un
// objet pose au centimetre pres.
inline constexpr float kDrinkReach = 4.0f;

// Point d'eau le plus proche d'une position, a l'horizontale. Renvoie nullptr
// si la liste est vide. outDistance, s'il est fourni, recoit la distance
// horizontale au point rendu.
const WaterSource* nearestWaterSource(const std::vector<WaterSource>& sources,
                                      const glm::vec3& position,
                                      float* outDistance = nullptr);

} // namespace vercora
