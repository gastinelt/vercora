#pragma once

#include "core/HeightSampler.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>

namespace vercora {

// ---------------------------------------------------------------------------
// PHASE 15 : intersection d'un rayon avec la SURFACE du terrain.
// ---------------------------------------------------------------------------
// Ce que la Phase 5 avait deja, et pourquoi ca ne suffisait pas ici :
// `Forest::pickTree` intersecte un rayon avec des CYLINDRES (les troncs), ce qui
// est un probleme analytique a solution fermee. La surface du terrain, elle,
// n'est pas une primitive : c'est une grille bilineaire de plusieurs millions de
// cellules. Il n'y a pas de formule, on MARCHE le long du rayon.
//
// L'editeur en a besoin pour la raison la plus directe qui soit : "poser l'objet
// a la position exacte visee" veut dire savoir OU le regard rencontre le sol.
// C'est aussi ce qui manquait a toute idee de curseur monde (marqueur de voie,
// mesure de distance, previsualisation d'abattage) - d'ou le placement dans
// core/ a cote de HeightSampler, et non dans editor/ : la fonction ne connait
// que le contrat d'echantillonnage, elle est reutilisable telle quelle.
//
// ALGORITHME. Marche a pas CROISSANT puis dichotomie :
//
//  1. on avance le long du rayon en surveillant le signe de
//     `hauteurDuRayon - hauteurDuTerrain` ;
//  2. des que le signe s'inverse, la surface a ete traversee entre le pas
//     precedent et le pas courant : 24 iterations de dichotomie ramenent
//     l'incertitude a maxDistance / 2^24, soit moins d'un dixieme de millimetre
//     sur 1 km.
//
// Le pas CROIT avec la distance (geometriquement, jusqu'a un plafond) pour une
// raison de cout : pres de l'oeil il faut de la finesse - un pas de 8 m
// traverserait un rocher entier sans le voir - alors qu'a 400 m un pas de 8 m ne
// represente que quelques pixels a l'ecran. Un pas constant assez fin pour le
// proche couterait des milliers d'echantillons par frame ; celui-ci en demande
// ~150 pour 400 m, soit une poignee de microsecondes.
//
// LIMITE ASSUMEE, a connaitre avant de s'en servir ailleurs : une marche a pas
// fini peut ENJAMBER une arete plus fine que le pas courant (une crete
// rocheuse vue de loin et par-dessus). Le rayon ressort alors "sous" le relief
// et l'intersection trouvee est plus loin qu'elle ne devrait. Pour l'usage
// editeur ce n'est pas genant - on vise ce qu'on voit, donc de pres - et le
// remede exact (parcours cellule par cellule facon DDA sur la grille) exigerait
// de connaitre la grille elle-meme, donc de renoncer a l'interface
// HeightSampler. C'est le compromis retenu.
//
// SECOND POINT IMPORTANT : `HeightSampler::heightAt` est TOTALE par contrat -
// hors des donnees elle renvoie la hauteur du bord le plus proche. Un rayon qui
// part vers l'exterieur "toucherait" donc une etagere plate infinie. D'ou
// `halfExtent` : au-dela de cette demi-emprise horizontale, le rayon est
// considere comme sorti du monde et ne touche RIEN. Passer 0 desactive le
// controle (terrain sans bord connu).

struct TerrainRayHit {
    // true si le rayon rencontre la surface dans les limites demandees.
    bool hit = false;

    // Point d'intersection en coordonnees monde.
    glm::vec3 position{0.0f};

    // Distance parcourue depuis l'origine du rayon, en unites monde (= metres).
    float distance = 0.0f;
};

// Normale approchee de la surface en (worldX, worldZ), par differences CENTREES
// sur une base de `sampleStep` metres de part et d'autre.
//
// La base par defaut est de 2 m (donc 4 m d'un bord a l'autre) et ce n'est pas
// un chiffre en l'air : c'est celle que la Phase 13 a du adopter pour le velo,
// apres avoir mesure qu'au pas de 1 m - le pas de la heightmap - le bruit
// decimetrique d'un DTM LiDAR fait lire 8 % d'une VRAIE descente en contre-pente.
// Une normale calculee sur 1 m serait tout aussi bruitee, et un objet pose
// dessus tressauterait d'un metre a l'autre sans raison visible.
inline glm::vec3 terrainNormalAt(const HeightSampler& terrain, float worldX, float worldZ,
                                 float sampleStep = 2.0f) {
    const float dydx = (terrain.heightAt(worldX + sampleStep, worldZ) -
                        terrain.heightAt(worldX - sampleStep, worldZ)) /
                       (2.0f * sampleStep);
    const float dydz = (terrain.heightAt(worldX, worldZ + sampleStep) -
                        terrain.heightAt(worldX, worldZ - sampleStep)) /
                       (2.0f * sampleStep);
    // La surface est le graphe y = h(x, z) ; sa normale sortante est
    // (-dh/dx, 1, -dh/dz), normalisee.
    return glm::normalize(glm::vec3(-dydx, 1.0f, -dydz));
}

// Intersection rayon / surface. `direction` doit etre normalisee.
inline TerrainRayHit raycastTerrain(const HeightSampler& terrain,
                                    const glm::vec3& origin,
                                    const glm::vec3& direction,
                                    float maxDistance,
                                    float halfExtent = 0.0f,
                                    float firstStep = 0.5f,
                                    float maxStep = 8.0f,
                                    float stepGrowth = 1.02f) {
    TerrainRayHit result;
    if (!(maxDistance > 0.0f)) {
        return result;
    }

    const auto outside = [&](const glm::vec3& p) {
        return halfExtent > 0.0f &&
               (std::abs(p.x) > halfExtent || std::abs(p.z) > halfExtent);
    };

    // Signe de depart. Si l'oeil est DEJA sous la surface (on est dans le
    // relief - ca arrive en mode vol libre ou apres un repli de collision), on
    // ne peut pas raisonner par changement de signe : le premier point est deja
    // "dedans". On rend alors le point de depart projete sur la surface, ce qui
    // est le comportement utile - l'objet se pose la ou on est.
    const float startClearance = origin.y - terrain.heightAt(origin.x, origin.z);
    if (startClearance <= 0.0f && !outside(origin)) {
        result.hit = true;
        result.position = { origin.x, terrain.heightAt(origin.x, origin.z), origin.z };
        result.distance = 0.0f;
        return result;
    }

    float previousT = 0.0f;
    float step = std::max(firstStep, 1e-3f);

    for (float t = 0.0f; t < maxDistance;) {
        t = std::min(t + step, maxDistance);
        step = std::min(step * stepGrowth, maxStep);

        const glm::vec3 point = origin + direction * t;
        if (outside(point)) {
            // Sorti de l'emprise : tout ce qui suit est hors donnees. On
            // n'accroche PAS l'etagere plate que renverrait heightAt.
            return result;
        }

        const float clearance = point.y - terrain.heightAt(point.x, point.z);
        if (clearance <= 0.0f) {
            // Traversee entre previousT (au-dessus) et t (au-dessous).
            float low = previousT;
            float high = t;
            for (int i = 0; i < 24; ++i) {
                const float mid = 0.5f * (low + high);
                const glm::vec3 probe = origin + direction * mid;
                if (probe.y - terrain.heightAt(probe.x, probe.z) > 0.0f) {
                    low = mid;
                } else {
                    high = mid;
                }
            }
            result.hit = true;
            result.distance = high;
            const glm::vec3 point3 = origin + direction * high;
            // On REPOSE le point sur la surface plutot que de garder le point
            // du rayon : la dichotomie s'arrete a un cheveu au-dessous, et un
            // objet pose la s'enfoncerait visiblement d'autant.
            result.position = { point3.x, terrain.heightAt(point3.x, point3.z), point3.z };
            return result;
        }

        previousT = t;
    }

    return result;
}

} // namespace vercora
