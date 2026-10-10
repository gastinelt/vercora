#pragma once

#include "core/HeightSampler.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>

namespace vercora {

// ===========================================================================
// PHASE 24 - LES DEUX SEUILS DE PENTE DU JEU, EN UN SEUL ENDROIT.
// ===========================================================================
//
// Jusqu'ici la marche (core/Camera, mode Walk) n'avait AUCUNE limite de pente :
// un versant a 90 %, voire une paroi a 300 %, se montait debout a 14 m/s. Ce
// n'etait pas seulement irrealiste, ca rendait l'escalade FACULTATIVE - une
// activite entiere que rien n'obligeait jamais a employer, puisque la marche
// passait partout.
//
// Les valeurs sont en RISE PAR METRE HORIZONTAL, la meme unite que
// ClimberParams et BikeParams (1,00 = 45 degres = 100 %).
//
// --- kWalkComfortSlope : 0,60 (31 degres) ----------------------------------
//
// Pente au-dela de laquelle on ne marche plus a pleine vitesse. Ce n'est PAS
// une valeur neuve : c'est exactement ClimberParams::walkableSlope, deja
// justifiee en Phase 14 comme "la pente au-dela de laquelle un sentier de
// montagne ne se fait plus debout de facon soutenue", et deja employee par
// tools/find_climb_walls.py pour juger si une paroi a un pied abordable. Les
// trois DOIVENT rester d'accord, d'ou la constante partagee.
//
// --- kWalkMaxSlope : 0,84 (40 degres) --------------------------------------
//
// Pente au-dela de laquelle on ne progresse plus du tout debout. La consigne
// etait de choisir entre le seuil de marche libre (0,60) et le seuil de paroi
// de Climber (1,00) ; voici pourquoi 0,84 et pas autre chose :
//
//  1. C'est la BORNE HAUTE DE L'ANGLE DE REPOS des materiaux meubles (35 a 40
//     degres, soit 70 a 84 %), la meme grandeur physique que la Phase 14
//     invoquait deja pour placer wallSlope. Au-dessus, un versant de ce massif
//     ne retient ni terre ni eboulis : il n'y a plus de sol sous les pieds,
//     seulement du rocher. C'est litteralement la pente maximale a laquelle il
//     existe encore quelque chose sur quoi se tenir.
//  2. Ca laisse INTACTE la randonnee raide. Un sentier de montagne soutenu vit
//     entre 0,30 et 0,60 ; les raidillons ponctuels montent a 0,70 - 0,80.
//     Tout ca continue de passer, ralenti mais franchissable. Ce qui est
//     refuse commence la ou un randonneur reel met les mains.
//  3. Ca ne ferme pas le terrain. La distribution des pentes de la zone de jeu
//     (25 millions d'echantillons, tools/find_climb_walls.py) donne p75 =
//     65,7 % et p90 = 92,4 % : le seuil tombe entre les deux, et interdit donc
//     de l'ordre de 13 % du massif a la marche - assez pour que le relief
//     compte, pas assez pour qu'on soit constamment arrete.
//
// --- LE POINT LE PLUS IMPORTANT : PAS DE TROU ENTRE LES DEUX ACTIVITES -----
//
// ClimberParams::wallSlope vaut kWalkMaxSlope, la MEME constante, et ce n'est
// pas une coincidence de reglage : c'est le contrat. Si le seuil d'entree en
// escalade etait plus haut que la limite de marche - ce qu'il etait, 1,00
// contre 0,84 - il existerait une bande de pentes ou le joueur ne pourrait NI
// marcher NI grimper. Sur cette zone de jeu la bande [0,84 ; 1,00[ represente
// environ 5 % du massif : ce n'est pas un cas theorique, c'est un versant sur
// vingt. Faire des deux seuils une seule constante rend ce trou impossible a
// reintroduire par un reglage distrait.
//
// L'hysteresis de SORTIE d'escalade reste, elle, a kWalkComfortSlope (0,60),
// strictement en dessous : c'est elle qui empeche le mode escalade de battre a
// chaque frame sur une paroi reelle, qui oscille autour de son seuil.
inline constexpr float kWalkComfortSlope = 0.60f;
inline constexpr float kWalkMaxSlope = 0.84f;

struct WalkSlopeParams {
    float comfortSlope = kWalkComfortSlope;
    float maxSlope = kWalkMaxSlope;

    // Demi-base des differences finies centrees, en m : la pente est lue sur
    // 2 x sampleStep. MEME VALEUR ET MEME RAISON que BikeParams (Phase 13) et
    // ClimberParams (Phase 14) : au pas de 1 m de la heightmap, le bruit
    // decimetrique d'un DTM LiDAR fabrique 8 % de micro-sections en
    // contre-pente et, ici, fabriquerait des murs invisibles en pleine prairie.
    float sampleStep = 2.0f;

    // Vitesse de glissade, en m/s PAR UNITE DE PENTE EN EXCES au-dela de
    // maxSlope. A 100 % de pente l'exces vaut 0,16, donc 0,48 m/s : on decolle
    // doucement. A 200 % il vaut 1,16, donc 3,5 m/s : on devale.
    float slideSpeed = 3.0f;

    // Plafond de la glissade, en m/s. Sans lui, une paroi a 3000 % - il y en a
    // sur cette dalle, max mesure 5018 % - ejecterait le joueur a 150 m/s.
    float slideSpeedMax = 6.0f;
};

// Gradient du terrain en (worldX, worldZ), par differences finies CENTREES.
// Pointe vers la MONTEE. Symetrique, donc le meme versant a la meme pente
// qu'on l'aborde par la gauche ou par la droite.
inline glm::vec2 walkTerrainGradient(const HeightSampler& terrain, float worldX,
                                     float worldZ, float sampleStep) {
    const float s = sampleStep;
    const float hxPlus = terrain.heightAt(worldX + s, worldZ);
    const float hxMinus = terrain.heightAt(worldX - s, worldZ);
    const float hzPlus = terrain.heightAt(worldX, worldZ + s);
    const float hzMinus = terrain.heightAt(worldX, worldZ - s);
    return glm::vec2((hxPlus - hxMinus) / (2.0f * s), (hzPlus - hzMinus) / (2.0f * s));
}

// Norme de la pente en (worldX, worldZ), en rise par metre horizontal.
inline float walkTerrainSlope(const HeightSampler& terrain, float worldX,
                              float worldZ, float sampleStep) {
    const glm::vec2 g = walkTerrainGradient(terrain, worldX, worldZ, sampleStep);
    return std::sqrt(g.x * g.x + g.y * g.y);
}

// Fraction de la vitesse de marche encore disponible sur une pente donnee :
// 1 jusqu'a comfortSlope, puis DECROISSANCE LINEAIRE jusqu'a 0 a maxSlope.
//
// Pourquoi une rampe et non un mur : un blocage sec a 0,84 serait exactement le
// "mur invisible incomprehensible" qu'il fallait eviter. Avec la rampe, le
// joueur SENT le terrain se durcir bien avant d'etre arrete - a 0,72, soit a
// mi-chemin, il n'avance deja plus qu'a moitie - et l'arret, quand il vient,
// est la fin d'un mouvement continu au lieu d'un evenement.
inline float walkSpeedFactor(float slope, const WalkSlopeParams& params) {
    if (slope <= params.comfortSlope) {
        return 1.0f;
    }
    if (slope >= params.maxSlope) {
        return 0.0f;
    }
    const float span = params.maxSlope - params.comfortSlope;
    if (span <= 0.0f) {
        return 0.0f; // reglage degenere : on prefere refuser que diviser par zero
    }
    return 1.0f - (slope - params.comfortSlope) / span;
}

// Resultat d'un pas de marche filtre par la pente. Tout est HORIZONTAL : la
// verticale appartient au terrain (Camera::snapToTerrain), et la reprendre ici
// ferait lutter deux regles l'une contre l'autre.
struct WalkStepResult {
    glm::vec3 position{0.0f}; // position horizontale retenue (y inchange)
    float slopeUnderFoot = 0.0f; // pente au point de DEPART
    float slopeAhead = 0.0f;     // pente au point VISE
    float speedFactor = 1.0f;    // fraction de vitesse accordee au pas
    bool sliding = false;        // le joueur ne tient plus debout et devale
};

// Filtre un pas de marche par la pente du terrain.
//
//   from        : position horizontale de depart (y ignore et rendu tel quel)
//   desiredStep : le pas que le clavier demandait, deja multiplie par dt
//   deltaTime   : necessaire a la glissade, qui est une VITESSE et non un pas
//
// TROIS REGLES, dans cet ordre :
//
//  1. La vitesse accordee est celle de la pente AU POINT VISE, pas au point de
//     depart. C'est ce qui fait qu'on s'arrete AU PIED d'une paroi au lieu d'y
//     entrer d'abord puis d'y etre coince : le pas qui mettrait le joueur sur
//     du trop raide est refuse tant qu'il est encore sur du marchable.
//  2. Seule la composante VERS L'AMONT est facturee. Redescendre est toujours
//     accorde, a pleine vitesse. Sans cette dissymetrie, un joueur pose sur du
//     trop raide serait fige - le "blocage brutal frustrant" - au lieu de
//     pouvoir simplement revenir sur ses pas.
//  3. Au-dela de maxSlope sous les pieds, le joueur GLISSE vers l'aval, a une
//     vitesse proportionnelle a l'exces de pente. C'est la regle qui garantit
//     qu'aucune position du monde n'est un piege : ou qu'on le depose, le
//     joueur finit par redescendre tout seul jusqu'a du terrain marchable.
inline WalkStepResult resolveWalkStep(const HeightSampler& terrain,
                                      const glm::vec3& from,
                                      const glm::vec3& desiredStep, float deltaTime,
                                      const WalkSlopeParams& params) {
    constexpr float kEpsilon = 1e-6f;

    WalkStepResult result;
    result.position = from;

    const glm::vec2 gradient =
        walkTerrainGradient(terrain, from.x, from.z, params.sampleStep);
    result.slopeUnderFoot = std::sqrt(gradient.x * gradient.x + gradient.y * gradient.y);

    // Direction de la plus grande pente, vers le HAUT, dans le plan horizontal.
    glm::vec3 uphill(0.0f);
    if (result.slopeUnderFoot > kEpsilon) {
        uphill = glm::vec3(gradient.x / result.slopeUnderFoot, 0.0f,
                           gradient.y / result.slopeUnderFoot);
    }

    glm::vec3 step(desiredStep.x, 0.0f, desiredStep.z);
    const float stepLength = std::sqrt(step.x * step.x + step.z * step.z);

    if (stepLength > kEpsilon) {
        const glm::vec3 target = from + step;
        result.slopeAhead =
            walkTerrainSlope(terrain, target.x, target.z, params.sampleStep);
        result.speedFactor = walkSpeedFactor(result.slopeAhead, params);

        // Decomposition amont / lateral. Note : quand le terrain est
        // strictement plat, uphill est nul, donc `along` l'est aussi et tout le
        // pas part dans `lateral` - qui est alors accorde en entier puisque
        // speedFactor vaut 1. Le cas degenere se traite donc tout seul.
        const float along = step.x * uphill.x + step.z * uphill.z;
        const glm::vec3 lateral = step - uphill * along;

        // La composante amont est facturee ; la composante aval passe entiere.
        const float allowedAlong = (along > 0.0f) ? along * result.speedFactor : along;

        // Le lateral est facture lui aussi : traverser une pente a 80 % en
        // devers est aussi difficile que la monter, et laisser le lateral libre
        // permettrait de gagner de la hauteur en zigzag a pleine vitesse, ce qui
        // annulerait la limite qu'on vient de poser.
        result.position = from + uphill * allowedAlong + lateral * result.speedFactor;
    } else {
        result.slopeAhead = result.slopeUnderFoot;
        result.speedFactor = walkSpeedFactor(result.slopeUnderFoot, params);
    }

    // --- Glissade ------------------------------------------------------------
    if (result.slopeUnderFoot > params.maxSlope && result.slopeUnderFoot > kEpsilon &&
        deltaTime > 0.0f) {
        result.sliding = true;
        const float excess = result.slopeUnderFoot - params.maxSlope;
        const float speed = std::min(params.slideSpeed * excess, params.slideSpeedMax);
        result.position -= uphill * (speed * deltaTime);
    }

    result.position.y = from.y;
    return result;
}

} // namespace vercora
