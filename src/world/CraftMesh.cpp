#include "world/CraftMesh.hpp"

#include <glm/glm.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <random>
#include <utility>
#include <vector>

namespace vercora {
namespace {

constexpr float kTwoPi = 6.28318530718f;

// Teintes de sommet (materiau Object, couleur au sommet, comme HarvestMesh).
const glm::vec3 kCordColor   {0.55f, 0.50f, 0.32f}; // fibre sechee, paille
const glm::vec3 kHandleColor {0.52f, 0.40f, 0.26f}; // bois ecorce
const glm::vec3 kBladeColor  {0.38f, 0.38f, 0.40f}; // silex, meme famille que l'eclat
const glm::vec3 kStoneColor  {0.50f, 0.49f, 0.46f}; // pierres du cercle
const glm::vec3 kStickColor  {0.36f, 0.26f, 0.17f}; // petit bois
const glm::vec3 kCharColor   {0.10f, 0.09f, 0.08f}; // fond noirci du foyer
const glm::vec3 kAshColor    {0.58f, 0.57f, 0.55f}; // cendre gris clair
const glm::vec3 kPitchColor  {0.12f, 0.08f, 0.05f}; // poix brun-noir

// Facette orientee VERS L'EXTERIEUR d'un point interieur donne : tous les
// volumes de ce fichier sont convexes par morceaux, ce qui suffit a garantir
// des normales sortantes sans se fier a l'ordre d'enumeration des sommets.
void facetOut(MeshData& mesh, glm::vec3 a, glm::vec3 b, glm::vec3 c,
              const glm::vec3& inside, const glm::vec3& color) {
    glm::vec3 rawNormal = glm::cross(b - a, c - a);
    const float length = glm::length(rawNormal);
    if (length < 1e-9f) {
        return; // triangle degenere : aucune normale, rien a dessiner
    }
    const glm::vec3 centroid = (a + b + c) / 3.0f;
    if (glm::dot(rawNormal, centroid - inside) < 0.0f) {
        std::swap(b, c);
        rawNormal = -rawNormal;
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

// Pave de demi-dimensions `half`, tourne de `yaw` autour de Y (meme convention
// que GroundItem : +X tourne vers (cos, 0, -sin)).
void appendBox(MeshData& mesh, const glm::vec3& center, const glm::vec3& half,
               float yaw, const glm::vec3& color) {
    const float c = std::cos(yaw);
    const float s = std::sin(yaw);
    std::array<glm::vec3, 8> p{};
    for (int i = 0; i < 8; ++i) {
        const glm::vec3 local{ (i & 1) ? half.x : -half.x,
                               (i & 2) ? half.y : -half.y,
                               (i & 4) ? half.z : -half.z };
        p[static_cast<std::size_t>(i)] =
            center + glm::vec3(c * local.x + s * local.z, local.y, -s * local.x + c * local.z);
    }
    // Six faces, deux triangles chacune (indices des coins : bit0 = x, bit1 = y,
    // bit2 = z).
    const int faces[6][4] = {
        {0, 2, 6, 4}, {1, 3, 7, 5}, // -X, +X
        {0, 1, 5, 4}, {2, 3, 7, 6}, // -Y, +Y
        {0, 1, 3, 2}, {4, 5, 7, 6}, // -Z, +Z
    };
    for (const auto& f : faces) {
        facetOut(mesh, p[f[0]], p[f[1]], p[f[2]], center, color);
        facetOut(mesh, p[f[0]], p[f[2]], p[f[3]], center, color);
    }
}

// Motte bruitee : octaedre subdivise une fois, sommets UNIQUES bruites (meme
// construction que le galet et la motte d'argile, pour la meme raison :
// bruiter les triangles independamment ouvrirait le volume).
void appendLump(MeshData& mesh, const glm::vec3& center, const glm::vec3& half,
                uint32_t seed, float minBump, const glm::vec3& color) {
    std::vector<glm::vec3> points = {
        { 1.0f, 0.0f, 0.0f }, { -1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f },
        { 0.0f, -1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, -1.0f },
    };
    const std::array<std::array<uint32_t, 3>, 8> faces = { {
        {0, 2, 4}, {2, 1, 4}, {1, 3, 4}, {3, 0, 4},
        {2, 0, 5}, {1, 2, 5}, {3, 1, 5}, {0, 3, 5},
    } };
    // Subdivision sans cache d'aretes : les milieux sont recalcules a
    // l'identique (meme formule, memes operandes), donc les sommets partages
    // coincident exactement et le volume reste ferme ; le bruit, lui, est tire
    // par POSITION (hash), pas par ordre de creation, pour la meme raison.
    std::vector<std::array<glm::vec3, 3>> tris;
    for (const auto& f : faces) {
        const glm::vec3 a = points[f[0]], b = points[f[1]], c = points[f[2]];
        const glm::vec3 ab = glm::normalize(a + b);
        const glm::vec3 bc = glm::normalize(b + c);
        const glm::vec3 ca = glm::normalize(c + a);
        tris.push_back({ a, ab, ca });
        tris.push_back({ ab, b, bc });
        tris.push_back({ ca, bc, c });
        tris.push_back({ ab, bc, ca });
    }
    const auto bumpAt = [&](const glm::vec3& v) {
        const auto q = [](float x) { return static_cast<uint32_t>(std::lround(x * 1000.0f) + 5000); };
        std::mt19937 rng(seed ^ (q(v.x) * 73856093u) ^ (q(v.y) * 19349663u) ^ (q(v.z) * 83492791u));
        return std::uniform_real_distribution<float>(minBump, 1.0f)(rng);
    };
    for (const auto& t : tris) {
        std::array<glm::vec3, 3> w{};
        for (int k = 0; k < 3; ++k) {
            const glm::vec3 v = t[static_cast<std::size_t>(k)] * bumpAt(t[static_cast<std::size_t>(k)]);
            w[static_cast<std::size_t>(k)] = center + v * half;
        }
        facetOut(mesh, w[0], w[1], w[2], center, color);
    }
}

// Cone tres surbaisse (meme forme que le tas de sable de la Phase 22).
void appendMound(MeshData& mesh, float halfHeight, float width, uint32_t seed,
                 const glm::vec3& color) {
    constexpr int kSides = 10;
    const glm::vec3 apex{ 0.0f, halfHeight, 0.0f };
    const glm::vec3 baseCenter{ 0.0f, -halfHeight, 0.0f };
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> wobble(0.80f, 1.0f);
    std::vector<glm::vec3> rim;
    for (int i = 0; i < kSides; ++i) {
        const float angle = kTwoPi * static_cast<float>(i) / static_cast<float>(kSides);
        const float r = width * wobble(rng);
        rim.push_back({ r * std::cos(angle), -halfHeight, r * std::sin(angle) });
    }
    const glm::vec3 inside{ 0.0f, -0.5f * halfHeight, 0.0f };
    for (int i = 0; i < kSides; ++i) {
        const glm::vec3& a = rim[static_cast<std::size_t>(i)];
        const glm::vec3& b = rim[static_cast<std::size_t>((i + 1) % kSides)];
        facetOut(mesh, apex, a, b, inside, color);
        // Base fermee (vue de dessous depuis un talus), orientee vers le bas.
        facetOut(mesh, baseCenter, b, a, 0.5f * apex, color);
    }
}

// PHASE 28 (Agent 3). Poutre de section carree (demi-cote t) de a a b, dans
// n'importe quelle direction (appendBox ne sait tourner qu'autour de Y).
void appendBeam(MeshData& mesh, const glm::vec3& a, const glm::vec3& b, float t,
                const glm::vec3& color) {
    const glm::vec3 axis = b - a;
    const float length = glm::length(axis);
    if (length < 1e-6f) {
        return;
    }
    const glm::vec3 d = axis / length;
    const glm::vec3 helper = (std::abs(d.y) < 0.9f) ? glm::vec3(0.0f, 1.0f, 0.0f)
                                                     : glm::vec3(1.0f, 0.0f, 0.0f);
    const glm::vec3 u = glm::normalize(glm::cross(d, helper)) * t;
    const glm::vec3 v = glm::normalize(glm::cross(d, u)) * t;
    std::array<glm::vec3, 8> p{};
    for (int i = 0; i < 8; ++i) {
        p[static_cast<std::size_t>(i)] = ((i & 1) ? b : a) + ((i & 2) ? u : -u) + ((i & 4) ? v : -v);
    }
    const glm::vec3 center = 0.5f * (a + b);
    const int faces[6][4] = {
        {0, 2, 6, 4}, {1, 3, 7, 5}, {0, 1, 5, 4}, {2, 3, 7, 6}, {0, 1, 3, 2}, {4, 5, 7, 6},
    };
    for (const auto& f : faces) {
        facetOut(mesh, p[f[0]], p[f[1]], p[f[2]], center, color);
        facetOut(mesh, p[f[0]], p[f[2]], p[f[3]], center, color);
    }
}

} // namespace

MeshData cordMeshData() {
    MeshData mesh;
    mesh.name = "ligature (Phase 28)";
    // Anneau de huit troncons : un echeveau enroule, pose a plat.
    constexpr int kSegments = 8;
    const float ringRadius = kCordWidth - kCordRadius;
    const float segmentHalf = ringRadius * std::sin(0.5f * kTwoPi / kSegments) + 0.004f;
    for (int i = 0; i < kSegments; ++i) {
        const float angle = kTwoPi * (static_cast<float>(i) + 0.5f) / static_cast<float>(kSegments);
        const glm::vec3 center{ ringRadius * std::cos(angle), 0.0f, -ringRadius * std::sin(angle) };
        // Tangente a l'anneau : yaw = angle + 90 degres.
        appendBox(mesh, center, { segmentHalf, kCordRadius, 0.8f * kCordRadius },
                  angle + 0.25f * kTwoPi, kCordColor);
    }
    return mesh;
}

MeshData axeMeshData() {
    MeshData mesh;
    mesh.name = "hache emmanchee (Phase 28)";
    const float handleHalf = 0.018f;
    const float bindExtra = 0.006f; // surepaisseur de la ligature
    // Manche le long de +X, sureleve de la surepaisseur de la ligature : c'est
    // elle, la plus epaisse, qui touche le sol.
    const float handleY = -kAxeRadius + handleHalf + bindExtra;
    appendBox(mesh, { 0.0f, handleY, 0.0f },
              { 0.5f * kAxeLength, handleHalf, handleHalf }, 0.0f, kHandleColor);
    // Lame : eclat aplati pres du bout +X, deborde d'un cote du manche.
    const float bladeX = 0.5f * kAxeLength - 0.05f;
    appendLump(mesh, { bladeX, 0.0f, 0.035f }, { 0.045f, kAxeRadius, 0.06f },
               0xA7E5u, 0.85f, kBladeColor);
    // Ligature : un manchon un peu plus epais que le manche, juste derriere.
    appendBox(mesh, { bladeX - 0.05f, handleY, 0.0f },
              { 0.02f, handleHalf + bindExtra, handleHalf + bindExtra }, 0.0f, kCordColor);
    return mesh;
}

MeshData hearthMeshData() {
    MeshData mesh;
    mesh.name = "foyer (Phase 28)";
    // Fond noirci : un disque tres fin au ras du sol.
    appendBox(mesh, { 0.0f, -kHearthRadius + 0.004f, 0.0f },
              { 0.22f, 0.004f, 0.22f }, 0.0f, kCharColor);
    // Les TROIS pierres consommees par la recette, en triangle.
    const float stoneHalf = 0.11f;
    const float ringRadius = kHearthWidth - stoneHalf;
    for (int i = 0; i < 3; ++i) {
        const float angle = kTwoPi * static_cast<float>(i) / 3.0f;
        appendLump(mesh,
                   { ringRadius * std::cos(angle), 0.0f, ringRadius * std::sin(angle) },
                   { stoneHalf, kHearthRadius, stoneHalf }, 0x4EA27u + static_cast<uint32_t>(i),
                   0.78f, kStoneColor);
    }
    // Les DEUX branches consommees, croisees au centre.
    const float stickHalf = 0.022f;
    appendBox(mesh, { 0.0f, -kHearthRadius + 0.008f + stickHalf, 0.0f },
              { 0.24f, stickHalf, stickHalf }, 0.5f, kStickColor);
    appendBox(mesh, { 0.0f, -kHearthRadius + 0.008f + 3.0f * stickHalf, 0.0f },
              { 0.24f, stickHalf, stickHalf }, -0.5f, kStickColor);
    return mesh;
}

MeshData ashMeshData() {
    MeshData mesh;
    mesh.name = "cendre (Phase 28)";
    appendMound(mesh, kAshRadius, kAshWidth, 0xA54u, kAshColor);
    return mesh;
}

MeshData pitchMeshData() {
    MeshData mesh;
    mesh.name = "poix (Phase 28)";
    appendLump(mesh, { 0.0f, 0.0f, 0.0f }, { kPitchWidth, kPitchRadius, kPitchWidth },
               0x917C4u, 0.90f, kPitchColor);
    return mesh;
}

MeshData pitchedAxeMeshData() {
    MeshData mesh = axeMeshData();
    mesh.name = "hache renforcee (Phase 28)";
    // Manchon de poix par-dessus la ligature : un peu plus long, meme
    // epaisseur maximale (la lame reste l'element le plus epais : kAxeRadius).
    const float handleHalf = 0.018f;
    const float bindExtra = 0.006f;
    const float handleY = -kAxeRadius + handleHalf + bindExtra;
    const float bladeX = 0.5f * kAxeLength - 0.05f;
    appendBox(mesh, { bladeX - 0.05f, handleY, 0.0f },
              { 0.032f, handleHalf + bindExtra, handleHalf + bindExtra + 0.002f },
              0.0f, kPitchColor);
    return mesh;
}

MeshData shelterMeshData() {
    MeshData mesh;
    mesh.name = "abri de branches (Phase 28)";
    const float bottom = -kShelterRadius;
    const float ridgeY = kShelterRadius - 0.06f;
    const float frontZ = -kShelterHalfDepth + 0.10f;
    const float backZ = kShelterHalfDepth - 0.10f;
    const float logHalf = 0.06f;
    // Deux montants (buches) a l'avant, du sol a la faitiere.
    for (const float x : { -0.85f, 0.85f }) {
        appendBeam(mesh, { x, bottom, frontZ }, { x, ridgeY, frontZ }, logHalf, kHandleColor);
    }
    // Faitiere (buche) posee en travers des montants.
    appendBeam(mesh, { -kShelterHalfWidth + logHalf, ridgeY, frontZ },
               { kShelterHalfWidth - logHalf, ridgeY, frontZ }, logHalf, kHandleColor);
    // Buche de base a l'arriere, au pied des branches.
    appendBeam(mesh, { -0.95f, bottom + logHalf, backZ }, { 0.95f, bottom + logHalf, backZ },
               logHalf, kHandleColor);
    // Six branches inclinees de la faitiere a la buche de base.
    for (int i = 0; i < 6; ++i) {
        const float x = -0.80f + 0.32f * static_cast<float>(i);
        appendBeam(mesh, { x, ridgeY - 0.02f, frontZ + 0.08f },
                   { x, bottom + 2.0f * logHalf + 0.03f, backZ - 0.02f }, 0.03f, kStickColor);
    }
    // Deux ligatures aux noeuds montant/faitiere.
    for (const float x : { -0.85f, 0.85f }) {
        appendBox(mesh, { x, ridgeY - 0.10f, frontZ }, { logHalf + 0.012f, 0.03f, logHalf + 0.012f },
                  0.0f, kCordColor);
    }
    return mesh;
}

} // namespace vercora
