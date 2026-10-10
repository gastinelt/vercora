#include "world/BushMesh.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace vercora {
namespace {

constexpr float kTwoPi = 6.28318530718f;

// Couleurs : feuillage vert moyen un peu jaune pour le buisson (feuillus bas,
// genevrier), vert sombre pour le jeune resineux ; bois brun gris.
const glm::vec3 kShrubLeaves{0.27f, 0.37f, 0.17f};
const glm::vec3 kConiferLeaves{0.15f, 0.29f, 0.14f};
const glm::vec3 kWood{0.33f, 0.26f, 0.19f};

void appendFacet(MeshData& mesh, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c,
                 const glm::vec3& color) {
    const glm::vec3 raw = glm::cross(b - a, c - a);
    const float length = glm::length(raw);
    if (length < 1e-9f) { return; }
    const glm::vec3 normal = raw / length;
    const auto base = static_cast<uint32_t>(mesh.vertices.size());
    for (const glm::vec3& p : { a, b, c }) {
        Vertex v{};
        v.pos = p;
        v.color = color;
        v.normal = normal;
        v.uv = { 0.5f, glm::clamp(p.y, 0.0f, 1.0f) };
        mesh.vertices.push_back(v);
    }
    mesh.indices.insert(mesh.indices.end(), { base, base + 1, base + 2 });
}

float hashUnit(uint32_t v) {
    v ^= v >> 16; v *= 0x7feb352du;
    v ^= v >> 15; v *= 0x846ca68bu;
    v ^= v >> 16;
    return static_cast<float>(v >> 8) * (1.0f / 16777216.0f);
}

// Octaedre subdivise une fois (32 faces), bossele de +/- 7 %, puis etire en
// ellipsoide de rayons `radii` autour de `center`. Faces vers l'exterieur.
void appendLobe(MeshData& mesh, const glm::vec3& center, const glm::vec3& radii, uint32_t seed,
                const glm::vec3& color) {
    std::vector<glm::vec3> p = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 },
                                 { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
    const std::array<std::array<uint32_t, 3>, 8> octa = { { {0, 2, 4}, {2, 1, 4}, {1, 3, 4}, {3, 0, 4},
                                                            {2, 0, 5}, {1, 2, 5}, {3, 1, 5}, {0, 3, 5} } };
    std::vector<std::array<uint32_t, 3>> faces;
    auto mid = [&](uint32_t a, uint32_t b) {
        const glm::vec3 m = glm::normalize(p[a] + p[b]);
        for (uint32_t i = 6; i < p.size(); ++i) {
            if (glm::length(p[i] - m) < 1e-5f) { return i; }
        }
        p.push_back(m);
        return static_cast<uint32_t>(p.size() - 1);
    };
    for (const auto& f : octa) {
        const uint32_t ab = mid(f[0], f[1]), bc = mid(f[1], f[2]), ca = mid(f[2], f[0]);
        faces.push_back({ f[0], ab, ca });
        faces.push_back({ ab, f[1], bc });
        faces.push_back({ ca, bc, f[2] });
        faces.push_back({ ab, bc, ca });
    }
    std::vector<glm::vec3> q(p.size());
    for (std::size_t i = 0; i < p.size(); ++i) {
        const float bump = 0.93f + 0.14f * hashUnit(seed * 131u + static_cast<uint32_t>(i) * 7919u);
        q[i] = center + p[i] * radii * bump;
    }
    for (const auto& f : faces) {
        appendFacet(mesh, q[f[0]], q[f[1]], q[f[2]], color);
    }
}

// Prisme (sides faces laterales) d'axe Y, de y0 a y1, rayons r0 -> r1.
void appendPrism(MeshData& mesh, float y0, float y1, float r0, float r1, int sides,
                 const glm::vec3& color) {
    for (int i = 0; i < sides; ++i) {
        const float a0 = kTwoPi * static_cast<float>(i) / static_cast<float>(sides);
        const float a1 = kTwoPi * static_cast<float>(i + 1) / static_cast<float>(sides);
        const glm::vec3 b0(r0 * std::cos(a0), y0, r0 * std::sin(a0));
        const glm::vec3 b1(r0 * std::cos(a1), y0, r0 * std::sin(a1));
        const glm::vec3 t0(r1 * std::cos(a0), y1, r1 * std::sin(a0));
        const glm::vec3 t1(r1 * std::cos(a1), y1, r1 * std::sin(a1));
        appendFacet(mesh, b0, t0, t1, color);
        appendFacet(mesh, b0, t1, b1, color);
    }
}

// Cone ouvert (base + pointe), `sides` faces, avec son disque de base.
void appendCone(MeshData& mesh, float yBase, float yApex, float radius, float phase, int sides,
                const glm::vec3& color) {
    const glm::vec3 apex(0.0f, yApex, 0.0f);
    const glm::vec3 centre(0.0f, yBase, 0.0f);
    for (int i = 0; i < sides; ++i) {
        const float a0 = phase + kTwoPi * static_cast<float>(i) / static_cast<float>(sides);
        const float a1 = phase + kTwoPi * static_cast<float>(i + 1) / static_cast<float>(sides);
        const glm::vec3 b0(radius * std::cos(a0), yBase, radius * std::sin(a0));
        const glm::vec3 b1(radius * std::cos(a1), yBase, radius * std::sin(a1));
        appendFacet(mesh, b0, apex, b1, color);   // flanc, vers l'exterieur
        appendFacet(mesh, b0, b1, centre, color); // dessous, vers le bas
    }
}

} // namespace

MeshData shrubMeshData() {
    MeshData mesh;
    mesh.name = "buisson (phase 29)";
    // Souche courte, enterree de kBushBuriedDepth.
    appendPrism(mesh, -kBushBuriedDepth, 0.30f, 0.06f, 0.04f, 5, kWood);
    // Trois lobes : un central et deux lateraux plus bas.
    appendLobe(mesh, { 0.00f, 0.52f, 0.00f }, { 0.46f, 0.44f, 0.46f }, 1u, kShrubLeaves);
    appendLobe(mesh, { 0.24f, 0.36f, 0.12f }, { 0.34f, 0.31f, 0.34f }, 2u, kShrubLeaves * 0.92f);
    appendLobe(mesh, { -0.20f, 0.35f, -0.18f }, { 0.32f, 0.30f, 0.32f }, 3u, kShrubLeaves * 1.06f);
    // Normalisation : sommet exactement a 1 (l'echelle de l'instance = hauteur).
    float top = 0.0f;
    for (const Vertex& v : mesh.vertices) { top = std::max(top, v.pos.y); }
    for (Vertex& v : mesh.vertices) {
        if (v.pos.y > 0.0f) { v.pos.y /= top; }
    }
    return mesh;
}

MeshData smallTreeMeshData() {
    MeshData mesh;
    mesh.name = "petit arbre (phase 29)";
    appendPrism(mesh, -kBushBuriedDepth, 0.30f, 0.055f, 0.04f, 6, kWood);
    appendCone(mesh, 0.16f, 0.74f, 0.40f, 0.0f, 8, kConiferLeaves);
    appendCone(mesh, 0.50f, 1.00f, 0.27f, 0.39f, 8, kConiferLeaves * 1.08f);
    return mesh;
}

} // namespace vercora
