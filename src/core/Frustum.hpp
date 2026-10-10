#pragma once

#include <glm/glm.hpp>

#include <array>

namespace vercora {

// Les six plans du tronc de vision de la camera, extraits d'une matrice
// view-projection, plus le test "cette boite est-elle potentiellement visible".
//
// PHASE 10. Header-only et ne dependant que de GLM, comme HeightSampler : le
// culling doit pouvoir etre teste SANS GPU ni Vulkan (tests/logic_tests.cpp), et
// il n'a besoin de rien d'autre qu'une matrice.
//
// Methode : Gribb-Hartmann. Un point p est a l'interieur du volume de clipping
// si -w <= x <= w (idem y) et 0 <= z <= w apres transformation. Chacune de ces
// six inegalites, reecrite en fonction de p en espace MONDE, est exactement un
// plan dont les coefficients sont une combinaison de LIGNES de la matrice.
// (0 <= z et non -w <= z : GLM_FORCE_DEPTH_ZERO_TO_ONE est actif - convention
// Vulkan - et prendre la convention OpenGL ici donnerait un plan proche FAUX,
// deux fois trop loin devant la camera.)
//
// Attention aux conventions GLM : m[c][r], donc la ligne i vaut
// (m[0][i], m[1][i], m[2][i], m[3][i]) et non m[i].
struct Frustum {
    // Chaque plan est (nx, ny, nz, d) avec la normale dirigee vers l'INTERIEUR :
    // dot(n, p) + d >= 0 <=> p est du bon cote.
    std::array<glm::vec4, 6> planes{};

    static Frustum fromViewProjection(const glm::mat4& viewProj) {
        const glm::vec4 row0(viewProj[0][0], viewProj[1][0], viewProj[2][0], viewProj[3][0]);
        const glm::vec4 row1(viewProj[0][1], viewProj[1][1], viewProj[2][1], viewProj[3][1]);
        const glm::vec4 row2(viewProj[0][2], viewProj[1][2], viewProj[2][2], viewProj[3][2]);
        const glm::vec4 row3(viewProj[0][3], viewProj[1][3], viewProj[2][3], viewProj[3][3]);

        Frustum frustum;
        frustum.planes[0] = row3 + row0; // gauche  :  x >= -w
        frustum.planes[1] = row3 - row0; // droite  :  x <=  w
        frustum.planes[2] = row3 + row1; // bas     :  y >= -w
        frustum.planes[3] = row3 - row1; // haut    :  y <=  w
        frustum.planes[4] = row2;        // proche  :  z >=  0  (convention Vulkan)
        frustum.planes[5] = row3 - row2; // lointain:  z <=  w

        for (glm::vec4& plane : frustum.planes) {
            const float length = glm::length(glm::vec3(plane));
            // Une matrice degeneree (projection non initialisee) donnerait une
            // normale nulle : on laisse le plan tel quel plutot que de diviser
            // par zero. Le test ci-dessous devient alors permissif, ce qui est
            // le bon repli - on prefere dessiner en trop que faire disparaitre
            // le terrain.
            if (length > 1e-8f) {
                plane /= length;
            }
        }
        return frustum;
    }

    // Test CONSERVATEUR boite-alignee / tronc de vision : renvoie false
    // seulement si la boite est entierement du mauvais cote d'un des plans.
    //
    // Conservateur dans le bon sens : une boite tout juste hors champ peut etre
    // declaree visible (on dessine pour rien, c'est benin), mais une boite
    // visible n'est JAMAIS declaree invisible - ce serait un trou dans le
    // terrain. Le cas classique "boite hors champ mais dans le mauvais cote
    // d'aucun plan a elle seule" (coin du tronc) est accepte volontairement :
    // le corriger demanderait un test plus couteux pour un gain nul a notre
    // nombre de noeuds.
    bool intersectsAABB(const glm::vec3& boxMin, const glm::vec3& boxMax) const {
        for (const glm::vec4& plane : planes) {
            // Sommet "positif" : celui de la boite le plus avance dans la
            // direction de la normale. Si LUI est derriere le plan, les huit
            // le sont.
            const glm::vec3 positive(plane.x >= 0.0f ? boxMax.x : boxMin.x,
                                     plane.y >= 0.0f ? boxMax.y : boxMin.y,
                                     plane.z >= 0.0f ? boxMax.z : boxMin.z);
            if (glm::dot(glm::vec3(plane), positive) + plane.w < 0.0f) {
                return false;
            }
        }
        return true;
    }
};

// Distance du point au pave aligne le plus proche (0 si le point est dedans).
// Sert a la selection de LOD : c'est le "a quelle distance suis-je de ce
// morceau de terrain", et non la distance a son centre - laquelle donnerait un
// niveau de detail trop grossier pour une grande tuile dont on rase le bord.
inline float distanceToAABB(const glm::vec3& point,
                            const glm::vec3& boxMin, const glm::vec3& boxMax) {
    const glm::vec3 clamped = glm::clamp(point, boxMin, boxMax);
    return glm::length(point - clamped);
}

} // namespace vercora
