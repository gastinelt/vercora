#include "world/RockMesh.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <random>
#include <utility>
#include <vector>

namespace vercora {
namespace {

// Calcaire clair : les pierriers de la zone sont les bandes les plus claires de
// l'ortho (luminance moyenne 161 dans le masque, NOTES-PHASE29-DESIGN.md), plus
// clairs que le galet ramassable (0,44), qui est une pierre de lit de ruisseau.
const glm::vec3 kLimestone{0.63f, 0.62f, 0.58f};

using Face = std::array<uint32_t, 3>;
using EdgeKey = std::pair<uint32_t, uint32_t>;

// PHASE 30 (Agent B) : `top` = sommet visible de la piece (y > 0, repere du
// mesh). Chaque SOMMET recoit l'ombre de contact a sa hauteur (voir
// rockContactShade) : la couleur est interpolee d'un sommet a l'autre, d'ou un
// degrade du pied assombri vers le haut clair, sans toucher au shader.
void appendFacet(MeshData& mesh, const glm::vec3& a, const glm::vec3& b,
                 const glm::vec3& c, const glm::vec3& color, float top) {
    const glm::vec3 raw = glm::cross(b - a, c - a);
    const float length = glm::length(raw);
    if (length < 1e-9f) {
        return; // triangle degenere (plan de coupe) : ni surface ni normale
    }
    const glm::vec3 normal = raw / length;
    const auto base = static_cast<uint32_t>(mesh.vertices.size());
    for (const glm::vec3& p : { a, b, c }) {
        Vertex v{};
        v.pos = p;
        v.color = color * rockContactShade(p.y, top);
        v.normal = normal;
        mesh.vertices.push_back(v);
    }
    mesh.indices.insert(mesh.indices.end(), { base, base + 1, base + 2 });
}

uint32_t midpoint(std::vector<glm::vec3>& points, std::map<EdgeKey, uint32_t>& cache,
                  uint32_t a, uint32_t b) {
    const EdgeKey key = (a < b) ? EdgeKey{ a, b } : EdgeKey{ b, a };
    const auto found = cache.find(key);
    if (found != cache.end()) {
        return found->second;
    }
    points.push_back(glm::normalize(points[a] + points[b]));
    const auto index = static_cast<uint32_t>(points.size() - 1);
    cache.emplace(key, index);
    return index;
}

// Octaedre subdivise `levels` fois, sur la sphere unite. Faces orientees vers
// l'exterieur (meme ordre que StoneMesh).
void unitSphere(int levels, std::vector<glm::vec3>& points, std::vector<Face>& faces) {
    points = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
    faces = { {0, 2, 4}, {2, 1, 4}, {1, 3, 4}, {3, 0, 4},
              {2, 0, 5}, {1, 2, 5}, {3, 1, 5}, {0, 3, 5} };
    for (int level = 0; level < levels; ++level) {
        std::map<EdgeKey, uint32_t> cache;
        std::vector<Face> next;
        next.reserve(faces.size() * 4);
        for (const Face& f : faces) {
            const uint32_t ab = midpoint(points, cache, f[0], f[1]);
            const uint32_t bc = midpoint(points, cache, f[1], f[2]);
            const uint32_t ca = midpoint(points, cache, f[2], f[0]);
            next.push_back({ f[0], ab, ca });
            next.push_back({ ab, f[1], bc });
            next.push_back({ ca, bc, f[2] });
            next.push_back({ ab, bc, ca });
        }
        faces = std::move(next);
    }
}

// PHASE 30 (Agent B) : icosaedre sur la sphere unite (20 faces), orientees vers
// l'exterieur. Sert aux cailloux de la pierraille : l'octaedre (8 faces) se lit
// comme une petite pyramide a 10-25 cm, l'octaedre subdivise (32 faces) coutait
// +0,5 ms par image a 361 plaques (mesure entrelacee, RAPPORT Phase 30).
void unitIcosahedron(std::vector<glm::vec3>& points, std::vector<Face>& faces) {
    const float t = 1.6180339887f;
    points = { { -1, t, 0 }, { 1, t, 0 }, { -1, -t, 0 }, { 1, -t, 0 },
               { 0, -1, t }, { 0, 1, t }, { 0, -1, -t }, { 0, 1, -t },
               { t, 0, -1 }, { t, 0, 1 }, { -t, 0, -1 }, { -t, 0, 1 } };
    for (glm::vec3& p : points) {
        p = glm::normalize(p);
    }
    faces = { {0, 11, 5}, {0, 5, 1}, {0, 1, 7}, {0, 7, 10}, {0, 10, 11},
              {1, 5, 9}, {5, 11, 4}, {11, 10, 2}, {10, 7, 6}, {7, 1, 8},
              {3, 9, 4}, {3, 4, 2}, {3, 2, 6}, {3, 6, 8}, {3, 8, 9},
              {4, 9, 5}, {2, 4, 11}, {6, 2, 10}, {8, 6, 7}, {9, 8, 1} };
    // Orientation verifiee plutot que supposee : normale sortante (convexe).
    for (Face& f : faces) {
        const glm::vec3 n = glm::cross(points[f[1]] - points[f[0]], points[f[2]] - points[f[0]]);
        if (glm::dot(n, points[f[0]] + points[f[1]] + points[f[2]]) < 0.0f) {
            std::swap(f[1], f[2]);
        }
    }
}

// Un caillou irregulier : sphere bosselee puis TRONQUEE par `cuts` plans
// aleatoires - chaque plan aplatit une calotte et donne une face de fracture
// franche, ce qui distingue un bloc d'eboulis d'un galet roule.
struct RockShape {
    int levels = 1;      // subdivisions de l'octaedre ; < 0 : icosaedre (20 faces)
    float bumpMin = 0.78f;
    int cuts = 5;
    float cutDepth = 0.72f; // distance du plan au centre (sphere unite)
    glm::vec3 halfSize{ 0.5f, 0.35f, 0.42f };
    float yOffset = 0.0f;   // translation verticale apres mise a l'echelle
    glm::vec2 offset{ 0.0f };
    glm::vec3 color = kLimestone;
    uint32_t seed = 1u;
};

void appendRock(MeshData& mesh, const RockShape& shape) {
    std::vector<glm::vec3> points;
    std::vector<Face> faces;
    if (shape.levels < 0) {
        unitIcosahedron(points, faces);
    } else {
        unitSphere(shape.levels, points, faces);
    }

    std::mt19937 rng(shape.seed);
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    std::uniform_real_distribution<float> signedUnit(-1.0f, 1.0f);

    for (glm::vec3& p : points) {
        p *= shape.bumpMin + (1.0f - shape.bumpMin) * unit(rng);
    }
    for (int cut = 0; cut < shape.cuts; ++cut) {
        glm::vec3 n(signedUnit(rng), signedUnit(rng), signedUnit(rng));
        if (glm::length(n) < 1e-3f) {
            n = { 0.0f, 1.0f, 0.0f };
        }
        n = glm::normalize(n);
        const float d = shape.cutDepth + 0.15f * unit(rng);
        for (glm::vec3& p : points) {
            const float excess = glm::dot(p, n) - d;
            if (excess > 0.0f) {
                p -= excess * n; // projection sur le plan de coupe
            }
        }
    }
    for (glm::vec3& p : points) {
        p = p * shape.halfSize;
        p.x += shape.offset.x;
        p.z += shape.offset.y;
        p.y += shape.yOffset;
    }
    float top = 0.0f;
    for (const glm::vec3& p : points) {
        top = std::max(top, p.y);
    }
    // Legere variation de teinte par face (patine, lichen) : sans elle un bloc a
    // facettes planes a l'air d'un solide de synthese uniforme.
    for (const Face& f : faces) {
        const float shade = 0.90f + 0.14f * unit(rng);
        appendFacet(mesh, points[f[0]], points[f[1]], points[f[2]], shape.color * shade, top);
    }
}

} // namespace

MeshData boulderMeshData() {
    MeshData mesh;
    mesh.name = "gros bloc d'eboulis (Phase 29)";
    RockShape shape;
    shape.levels = 2;
    shape.bumpMin = 0.82f;
    shape.cuts = 7;
    shape.cutDepth = 0.62f;
    // Hauteur totale 2 x 0,475 = 0,95 : 0,70 visibles, 0,25 enterres.
    shape.halfSize = { 0.52f, 0.475f, 0.44f };
    shape.yOffset = kRockVisibleHeight - 0.475f;
    // PHASE 30 (Agent B) : patine grise, albedo ~0,53 au lieu de 0,63. Le gros
    // bloc etait deja 36 % plus sombre que le sol rendu (Weber -0,36, mesure) ;
    // on CREUSE cet ecart, on ne l'inverse surtout pas : eclaircir "pour coller
    // au calcaire" fondrait le bloc dans un sol rendu tres clair (~224).
    shape.color = kLimestone * 0.85f;
    shape.seed = 0xB01DE5u;
    appendRock(mesh, shape);
    return mesh;
}

MeshData rockMeshData() {
    MeshData mesh;
    mesh.name = "rocher moyen d'eboulis (Phase 29)";
    RockShape shape;
    shape.levels = 1;
    shape.bumpMin = 0.72f;
    shape.cuts = 4;
    shape.cutDepth = 0.70f;
    shape.halfSize = { 0.50f, 0.475f, 0.38f };
    shape.yOffset = kRockVisibleHeight - 0.475f;
    shape.color = kLimestone * 0.95f;
    shape.seed = 0x70C4u;
    appendRock(mesh, shape);
    return mesh;
}

MeshData gravelMeshData() {
    MeshData mesh;
    mesh.name = "plaque de pierraille (Phase 29)";
    std::mt19937 rng(0x6FA7E1u);
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    constexpr int kPebbles = 11;
    for (int i = 0; i < kPebbles; ++i) {
        // Disque de rayon 0,5, repartition en racine pour ne pas tasser au centre.
        const float angle = 6.28318530718f * unit(rng);
        const float radius = 0.44f * std::sqrt(unit(rng));
        // PHASE 30 (Agent B) : des CAILLOUX et non du gravier. Demi-largeur 5-12 cm
        // -> 10-25 cm : a 5-12 cm, enterres a 60 %, il en depassait 2 a 5 cm, soit
        // 2 a 3 px a 10 m (mesure, build-phase30gentA\DIAGNOSTIC.md). Meme
        // nombre de tirages qu'avant : la disposition des cailloux est conservee.
        const float size = 0.10f + 0.15f * unit(rng);
        RockShape shape;
        // PHASE 30 : icosaedre, 20 faces (etait l'octaedre, 8). A 10-25 cm de
        // demi-largeur, l'octaedre se lisait comme une petite PYRAMIDE (vu sur
        // capture). L'octaedre subdivise (32 faces) etait plus rond mais coutait
        // +0,5 ms de drawFrame a 361 plaques (mesure entrelacee) ; l'icosaedre +
        // trois coupes donne un caillou anguleux et trapu pour 20 faces.
        shape.levels = -1;
        shape.bumpMin = 0.72f;
        shape.cuts = 3;
        shape.cutDepth = 0.55f;
        shape.halfSize = { size, 0.6f * size, 0.8f * size };
        // PHASE 30 : centre a +0,1 demi-hauteur (etait -0,2) : ~55 % de la
        // hauteur hors sol au lieu de 40 %, le pied reste enfoui de 0,9 demi-
        // hauteur (voir le controle de pose sur le DTM reel, RAPPORT Phase 30).
        shape.yOffset = 0.1f * shape.halfSize.y;
        shape.offset = { radius * std::cos(angle), radius * std::sin(angle) };
        shape.color = kLimestone * (0.85f + 0.2f * unit(rng));
        shape.seed = 0x9E11u + static_cast<uint32_t>(i) * 7919u;
        appendRock(mesh, shape);
    }
    return mesh;
}

} // namespace vercora
