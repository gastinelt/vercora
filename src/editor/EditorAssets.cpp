#include "editor/EditorAssets.hpp"

#include <glm/glm.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

namespace vercora {
namespace {

// Teintes de sommet. Le materiau Object (cube.vert/frag) prend la couleur du
// sommet telle quelle : pas de texture, pas de materiau, une seule voie de
// rendu partagee avec le cube, les arbres et les pierres.
const glm::vec3 kRockGrey     {0.46f, 0.45f, 0.43f}; // calcaire, comme les galets
const glm::vec3 kRockDark     {0.33f, 0.33f, 0.32f}; // faces d'ombre / socle
const glm::vec3 kRockLight    {0.60f, 0.59f, 0.55f}; // faces fraiches
const glm::vec3 kWoodColor    {0.42f, 0.30f, 0.18f};
const glm::vec3 kSignColor    {0.78f, 0.68f, 0.30f}; // plaque de balisage

// --- Petit atelier de geometrie facettee ------------------------------------
// Les meshes de cette session sont tous facettes (une normale par triangle) :
// une silhouette bas-poly en normales lissees ressemble a un galet de plage,
// alors que du rocher casse a des aretes franches. Meme raisonnement qu'en
// Phase 6 (world/StoneMesh.cpp) - le code est repris ici plutot que partage,
// parce que le rendre commun demanderait de sortir un "MeshBuilder" que rien
// d'autre ne reclame encore.

void appendFacet(MeshData& mesh, const glm::vec3& a, const glm::vec3& b,
                 const glm::vec3& c, const glm::vec3& color) {
    const glm::vec3 rawNormal = glm::cross(b - a, c - a);
    const float length = glm::length(rawNormal);
    if (length < 1e-9f) {
        return; // triangle degenere : pas de normale, rien a dessiner
    }
    const glm::vec3 normal = rawNormal / length;

    const auto base = static_cast<uint32_t>(mesh.vertices.size());
    for (const glm::vec3& position : { a, b, c }) {
        Vertex vertex{};
        vertex.pos = position;
        vertex.color = color;
        vertex.normal = normal;
        mesh.vertices.push_back(vertex);
    }
    mesh.indices.insert(mesh.indices.end(), { base, base + 1, base + 2 });
}

// Quad facette, decoupe en deux triangles partageant la meme normale.
void appendQuad(MeshData& mesh, const glm::vec3& a, const glm::vec3& b,
                const glm::vec3& c, const glm::vec3& d, const glm::vec3& color) {
    appendFacet(mesh, a, b, c, color);
    appendFacet(mesh, a, c, d, color);
}

// Boite alignee sur les axes, puis tournee de `yawRadians` autour de Y et
// translatee. Les six faces sont facettees, donc la boite reste lisible meme
// posee de travers.
//
// `center` est le CENTRE de la boite (y compris verticalement) : c'est ce qui
// rend l'empilement du cairn lisible a ecrire (chaque pierre connait la hauteur
// de celle d'en dessous).
void appendBox(MeshData& mesh, const glm::vec3& center, const glm::vec3& halfSize,
               float yawRadians, const glm::vec3& color,
               const glm::vec3& topColor) {
    const float c = std::cos(yawRadians);
    const float s = std::sin(yawRadians);
    const auto place = [&](float x, float y, float z) {
        return center + glm::vec3(c * x - s * z, y, s * x + c * z);
    };

    const float hx = halfSize.x;
    const float hy = halfSize.y;
    const float hz = halfSize.z;

    const glm::vec3 p000 = place(-hx, -hy, -hz);
    const glm::vec3 p100 = place(+hx, -hy, -hz);
    const glm::vec3 p110 = place(+hx, +hy, -hz);
    const glm::vec3 p010 = place(-hx, +hy, -hz);
    const glm::vec3 p001 = place(-hx, -hy, +hz);
    const glm::vec3 p101 = place(+hx, -hy, +hz);
    const glm::vec3 p111 = place(+hx, +hy, +hz);
    const glm::vec3 p011 = place(-hx, +hy, +hz);

    appendQuad(mesh, p001, p101, p111, p011, color);      // +Z
    appendQuad(mesh, p100, p000, p010, p110, color);      // -Z
    appendQuad(mesh, p101, p100, p110, p111, color);      // +X
    appendQuad(mesh, p000, p001, p011, p010, color);      // -X
    appendQuad(mesh, p011, p111, p110, p010, topColor);   // +Y
    appendQuad(mesh, p000, p100, p101, p001, kRockDark);  // -Y
}

// --- Bruit deterministe -----------------------------------------------------
// Les deformations des rochers sont tirees d'un HACHAGE de l'indice de sommet,
// et non d'un generateur a etat.
//
// Ce n'est pas de la coquetterie : c'est la lecon de la Phase 11 (semis de la
// vegetation). Un `mt19937` rend une suite qui depend de l'ORDRE des appels ;
// deux constructions du meme mesh dans un ordre different donneraient deux
// geometries differentes. Un hachage rend chaque sommet independant de tous les
// autres, donc `editorBoulderMeshData()` produit exactement la meme geometrie a
// chaque appel, dans n'importe quel contexte. Ca compte ici parce que la
// COLLISION est deduite de la boite englobante du mesh : un mesh qui varierait
// donnerait une empreinte de collision qui varie, et une scene sauvegardee ne
// se rechargerait pas a l'identique.
float hashUnit(uint32_t value, uint32_t salt) {
    uint32_t h = value * 0x9E3779B9u + salt * 0x85EBCA6Bu;
    h ^= h >> 15;
    h *= 0x2545F491u;
    h ^= h >> 13;
    h *= 0xC2B2AE35u;
    h ^= h >> 16;
    // [0, 1) en divisant par 2^32.
    return static_cast<float>(h) * (1.0f / 4294967296.0f);
}

// --- Sphere subdivisee (base des rochers) -----------------------------------
using EdgeKey = std::pair<uint32_t, uint32_t>;

uint32_t midpointIndex(std::vector<glm::vec3>& points, std::map<EdgeKey, uint32_t>& cache,
                       uint32_t a, uint32_t b) {
    const EdgeKey key = (a < b) ? EdgeKey{ a, b } : EdgeKey{ b, a };
    const auto found = cache.find(key);
    if (found != cache.end()) {
        return found->second;
    }
    // Milieu REPROJETE sur la sphere : sans ca les subdivisions successives
    // rabattraient le volume vers l'octaedre de depart.
    points.push_back(glm::normalize(points[a] + points[b]));
    const auto index = static_cast<uint32_t>(points.size() - 1);
    cache.emplace(key, index);
    return index;
}

// Octaedre subdivise `subdivisions` fois, sommets deformes par hachage, aplati
// et pose sur y = 0.
//
// Le bruit porte sur les sommets UNIQUES, AVANT le decoupage en triangles
// facettes : bruiter chaque triangle separement dechirerait le volume (deux
// triangles voisins ne partageraient plus leur arete) et on verrait a travers
// le rocher. Meme piege qu'en Phase 6.
MeshData buildBoulder(uint32_t subdivisions, uint32_t salt, float radius,
                      float flatten, float roughness, const glm::vec3& color,
                      const char* name) {
    std::vector<glm::vec3> points = {
        { 1.0f, 0.0f, 0.0f }, { -1.0f, 0.0f, 0.0f },
        { 0.0f, 1.0f, 0.0f }, { 0.0f, -1.0f, 0.0f },
        { 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, -1.0f },
    };
    std::vector<std::array<uint32_t, 3>> faces = {
        {0, 2, 4}, {2, 1, 4}, {1, 3, 4}, {3, 0, 4},
        {2, 0, 5}, {1, 2, 5}, {3, 1, 5}, {0, 3, 5},
    };

    for (uint32_t pass = 0; pass < subdivisions; ++pass) {
        std::map<EdgeKey, uint32_t> cache;
        std::vector<std::array<uint32_t, 3>> next;
        next.reserve(faces.size() * 4);
        for (const auto& face : faces) {
            const uint32_t ab = midpointIndex(points, cache, face[0], face[1]);
            const uint32_t bc = midpointIndex(points, cache, face[1], face[2]);
            const uint32_t ca = midpointIndex(points, cache, face[2], face[0]);
            next.push_back({ face[0], ab, ca });
            next.push_back({ face[1], bc, ab });
            next.push_back({ face[2], ca, bc });
            next.push_back({ ab, bc, ca });
        }
        faces.swap(next);
    }

    // Deformation + aplatissement.
    for (uint32_t i = 0; i < points.size(); ++i) {
        const float jitter = 1.0f + roughness * (hashUnit(i, salt) - 0.5f) * 2.0f;
        points[i] *= radius * jitter;
        points[i].y *= flatten;
    }

    // On pose le volume sur y = 0 : le point le plus bas devient exactement 0.
    float minY = points.front().y;
    for (const glm::vec3& p : points) {
        minY = std::min(minY, p.y);
    }
    for (glm::vec3& p : points) {
        p.y -= minY;
    }

    MeshData mesh;
    mesh.name = name;
    mesh.vertices.reserve(faces.size() * 3);
    mesh.indices.reserve(faces.size() * 3);
    for (const auto& face : faces) {
        // Une teinte par face, legerement variee : sans ca un rocher facette de
        // couleur uniforme se lit comme une seule masse, et les aretes que le
        // facettage vient de creer ne se voient qu'a contre-jour.
        const float shade = 0.85f + 0.30f * hashUnit(face[0] * 7919u + face[1], salt + 17u);
        appendFacet(mesh, points[face[0]], points[face[1]], points[face[2]],
                    color * shade);
    }
    return mesh;
}

const std::vector<BuiltinAsset> kBuiltins = {
    { "rocher", "rocher arrondi, 1,8 m", &editorBoulderMeshData },
    { "bloc", "dalle basculee anguleuse, 3,2 m", &editorSlabMeshData },
    { "cairn", "cairn de sentier, 1,3 m", &editorCairnMeshData },
    { "abri", "abri de pierre a deux pentes, 3,4 m", &editorShelterMeshData },
    { "poteau", "poteau de balisage avec plaque, 2,1 m", &editorMarkerPostMeshData },
};

} // namespace

const std::vector<BuiltinAsset>& builtinEditorAssets() { return kBuiltins; }

MeshData editorBoulderMeshData() {
    // 2 subdivisions = 128 faces : assez pour que la silhouette soit ronde,
    // assez peu pour rester dans l'esprit bas-poly du reste du projet.
    return buildBoulder(2, 20260822u, 0.95f, 0.78f, 0.26f, kRockGrey,
                        "rocher (asset d'editeur genere, Phase 15)");
}

MeshData editorSlabMeshData() {
    // Une dalle basculee : trois boites imbriquees de tailles decroissantes,
    // inclinees differemment. Volontairement construite de primitives et non
    // bruitee, pour que sa silhouette soit ANGULEUSE la ou celle du rocher est
    // arrondie - les deux assets doivent se distinguer d'un coup d'oeil dans une
    // scene de test, sinon on ne sait plus lequel on vient de poser.
    MeshData mesh;
    mesh.name = "bloc (asset d'editeur genere, Phase 15)";

    appendBox(mesh, { 0.0f, 0.35f, 0.0f }, { 1.10f, 0.35f, 0.85f },
              0.0f, kRockDark, kRockGrey);
    appendBox(mesh, { 0.10f, 1.20f, -0.05f }, { 0.85f, 0.60f, 0.62f },
              0.42f, kRockGrey, kRockLight);
    appendBox(mesh, { -0.15f, 2.35f, 0.10f }, { 0.55f, 0.62f, 0.42f },
              -0.65f, kRockGrey, kRockLight);
    return mesh;
}

MeshData editorCairnMeshData() {
    MeshData mesh;
    mesh.name = "cairn (asset d'editeur genere, Phase 15)";

    // Quatre pierres empilees, chacune un peu plus petite et tournee autrement.
    // Les hauteurs sont cumulees a la main : chaque centre est pose sur le
    // sommet du bloc precedent, sans recouvrement, sinon la pile "fond" les unes
    // dans les autres et ne se lit plus comme un empilement.
    struct Stone { float halfX, halfY, halfZ, yaw; };
    constexpr std::array<Stone, 4> stones = { {
        { 0.42f, 0.16f, 0.36f,  0.00f },
        { 0.34f, 0.14f, 0.30f,  0.70f },
        { 0.26f, 0.13f, 0.23f, -0.55f },
        { 0.17f, 0.11f, 0.15f,  1.15f },
    } };

    float y = 0.0f;
    uint32_t index = 0;
    for (const Stone& stone : stones) {
        const float shade = 0.88f + 0.24f * hashUnit(index, 4242u);
        appendBox(mesh, { 0.0f, y + stone.halfY, 0.0f },
                  { stone.halfX, stone.halfY, stone.halfZ }, stone.yaw,
                  kRockGrey * shade, kRockLight * shade);
        y += 2.0f * stone.halfY;
        ++index;
    }
    return mesh;
}

MeshData editorShelterMeshData() {
    MeshData mesh;
    mesh.name = "abri (asset d'editeur genere, Phase 15)";

    constexpr float halfWidth = 1.70f;  // le long de X
    constexpr float halfDepth = 1.30f;  // le long de Z
    constexpr float wallHeight = 1.55f;
    constexpr float ridgeHeight = 2.60f;
    constexpr float thickness = 0.22f;
    constexpr float doorHalfWidth = 0.42f;
    constexpr float doorHeight = 1.30f;

    // Murs pignons (le long de Z, aux deux extremites en X), pleins.
    for (const float sx : { -1.0f, 1.0f }) {
        appendBox(mesh, { sx * (halfWidth - 0.5f * thickness), 0.5f * wallHeight, 0.0f },
                  { 0.5f * thickness, 0.5f * wallHeight, halfDepth },
                  0.0f, kRockGrey, kRockLight);
    }
    // Mur arriere plein (-Z).
    appendBox(mesh, { 0.0f, 0.5f * wallHeight, -(halfDepth - 0.5f * thickness) },
              { halfWidth - thickness, 0.5f * wallHeight, 0.5f * thickness },
              0.0f, kRockGrey, kRockLight);

    // Mur avant (+Z) PERCE d'une porte : deux jambages plus un linteau.
    const float jambWidth = 0.5f * ((halfWidth - thickness) * 2.0f - 2.0f * doorHalfWidth);
    for (const float sx : { -1.0f, 1.0f }) {
        const float centerX = sx * (doorHalfWidth + 0.5f * jambWidth);
        appendBox(mesh, { centerX, 0.5f * wallHeight, halfDepth - 0.5f * thickness },
                  { 0.5f * jambWidth, 0.5f * wallHeight, 0.5f * thickness },
                  0.0f, kRockGrey, kRockLight);
    }
    appendBox(mesh,
              { 0.0f, 0.5f * (doorHeight + wallHeight), halfDepth - 0.5f * thickness },
              { doorHalfWidth, 0.5f * (wallHeight - doorHeight), 0.5f * thickness },
              0.0f, kRockDark, kRockGrey);

    // Toit a deux pentes : deux quads inclines plus les deux tympans
    // triangulaires qui bouchent les pignons. Le faitage court le long de X.
    const float eaveZ = halfDepth + 0.18f; // debord de toit
    const float roofX = halfWidth + 0.12f;
    const glm::vec3 roofColor = kRockDark * 1.15f;

    appendQuad(mesh,
               { -roofX, wallHeight, eaveZ }, { roofX, wallHeight, eaveZ },
               { roofX, ridgeHeight, 0.0f }, { -roofX, ridgeHeight, 0.0f }, roofColor);
    appendQuad(mesh,
               { roofX, wallHeight, -eaveZ }, { -roofX, wallHeight, -eaveZ },
               { -roofX, ridgeHeight, 0.0f }, { roofX, ridgeHeight, 0.0f }, roofColor);
    for (const float sx : { -1.0f, 1.0f }) {
        const float x = sx * (halfWidth - 0.5f * thickness);
        appendFacet(mesh, { x, wallHeight, -halfDepth }, { x, wallHeight, halfDepth },
                    { x, ridgeHeight, 0.0f },
                    sx > 0.0f ? kRockLight : kRockGrey);
    }
    return mesh;
}

MeshData editorMarkerPostMeshData() {
    MeshData mesh;
    mesh.name = "poteau (asset d'editeur genere, Phase 15)";

    constexpr float postHeight = 1.90f;
    constexpr float postHalf = 0.045f; // 9 cm de cote : volontairement mince

    // Socle de pierre, pour que le poteau ne semble pas planter dans le vide.
    appendBox(mesh, { 0.0f, 0.06f, 0.0f }, { 0.20f, 0.06f, 0.20f },
              0.0f, kRockDark, kRockGrey);
    // Le mat.
    appendBox(mesh, { 0.0f, 0.12f + 0.5f * postHeight, 0.0f },
              { postHalf, 0.5f * postHeight, postHalf }, 0.0f, kWoodColor,
              kWoodColor * 1.2f);
    // La plaque, en haut, decalee sur +X comme un panneau directionnel.
    appendBox(mesh, { 0.24f, 0.12f + postHeight - 0.16f, 0.0f },
              { 0.24f, 0.11f, 0.012f }, 0.0f, kSignColor, kSignColor);
    return mesh;
}

} // namespace vercora
