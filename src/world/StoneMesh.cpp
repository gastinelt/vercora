#include "world/StoneMesh.hpp"

#include <glm/glm.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <random>
#include <utility>
#include <vector>

namespace vercora {
namespace {

constexpr float kTwoPi = 6.28318530718f;

// Teintes de sommet. Comme pour l'arbre, la couleur vient du SOMMET et non d'une
// texture : le materiau Object (cube.vert/frag) l'utilise telle quelle.
const glm::vec3 kStoneColor     {0.44f, 0.43f, 0.41f}; // calcaire gris
const glm::vec3 kFlakeDorsal    {0.38f, 0.37f, 0.36f}; // face dorsale, mate
const glm::vec3 kFlakeVentral   {0.62f, 0.61f, 0.57f}; // face fraiche, plus claire

// Ajoute un triangle FACETTE : trois sommets qui lui appartiennent, une seule
// normale. C'est ce qui donne aux cassures leur arete franche - un galet ou un
// eclat en normales lissees ressemblerait a une pomme de terre.
void appendFacet(MeshData& mesh, const glm::vec3& a, const glm::vec3& b,
                 const glm::vec3& c, const glm::vec3& color) {
    const glm::vec3 edge1 = b - a;
    const glm::vec3 edge2 = c - a;
    const glm::vec3 rawNormal = glm::cross(edge1, edge2);

    const float length = glm::length(rawNormal);
    if (length < 1e-9f) {
        return; // triangle degenere : rien a dessiner, et sa normale n'existe pas
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

// --- Construction du galet -------------------------------------------------
// Un octaedre subdivise une fois (8 -> 32 faces), dont chaque sommet UNIQUE est
// ensuite pousse d'un facteur aleatoire le long de son rayon.
//
// Le point delicat est la ou porte le bruit : il est applique aux sommets
// UNIQUES du polyedre, avant de decouper les triangles facettes. Bruiter chaque
// triangle independamment ouvrirait le volume (deux triangles voisins ne
// partageraient plus leur arete) et on verrait a travers le galet.

using EdgeKey = std::pair<uint32_t, uint32_t>;

uint32_t midpointIndex(std::vector<glm::vec3>& points,
                       std::map<EdgeKey, uint32_t>& cache,
                       uint32_t a, uint32_t b) {
    // Cle ordonnee : l'arete (a,b) et l'arete (b,a) sont la meme, sans quoi les
    // deux triangles qui la partagent creeraient chacun leur milieu et le
    // maillage se dechirerait au moment du bruitage.
    const EdgeKey key = (a < b) ? EdgeKey{ a, b } : EdgeKey{ b, a };

    const auto found = cache.find(key);
    if (found != cache.end()) {
        return found->second;
    }

    // Milieu REPROJETE sur la sphere : sans normalisation, les subdivisions
    // successives aplatiraient progressivement le volume vers l'octaedre initial.
    const glm::vec3 midpoint = glm::normalize(points[a] + points[b]);
    points.push_back(midpoint);

    const auto index = static_cast<uint32_t>(points.size() - 1);
    cache.emplace(key, index);
    return index;
}

} // namespace

MeshData stoneMeshData() {
    MeshData mesh;
    mesh.name = "pierre synthetique (Phase 6)";

    // Octaedre de depart : 6 sommets, 8 faces.
    std::vector<glm::vec3> points = {
        { 1.0f,  0.0f,  0.0f}, {-1.0f,  0.0f,  0.0f},
        { 0.0f,  1.0f,  0.0f}, { 0.0f, -1.0f,  0.0f},
        { 0.0f,  0.0f,  1.0f}, { 0.0f,  0.0f, -1.0f},
    };
    std::vector<std::array<uint32_t, 3>> faces = {
        {0, 2, 4}, {2, 1, 4}, {1, 3, 4}, {3, 0, 4},
        {2, 0, 5}, {1, 2, 5}, {3, 1, 5}, {0, 3, 5},
    };

    // Une subdivision : 32 faces. Deux subdivisions donneraient 128 faces, ce qui
    // est beaucoup pour un galet vu de loin ; 32 suffit a ce que la silhouette ne
    // se lise plus comme un octaedre une fois bruitee.
    std::map<EdgeKey, uint32_t> midpoints;
    std::vector<std::array<uint32_t, 3>> subdivided;
    subdivided.reserve(faces.size() * 4);

    for (const auto& face : faces) {
        const uint32_t ab = midpointIndex(points, midpoints, face[0], face[1]);
        const uint32_t bc = midpointIndex(points, midpoints, face[1], face[2]);
        const uint32_t ca = midpointIndex(points, midpoints, face[2], face[0]);

        subdivided.push_back({ face[0], ab, ca });
        subdivided.push_back({ ab, face[1], bc });
        subdivided.push_back({ ca, bc, face[2] });
        subdivided.push_back({ ab, bc, ca });
    }
    faces = std::move(subdivided);

    // Bruitage. Graine FIXE et locale : toutes les pierres partagent ce mesh
    // unique (une seule geometrie sur le GPU, N matrices), donc il n'y a rien a
    // varier d'une instance a l'autre - seules leur orientation et leur position
    // different. Une graine fixe garde en prime le mesh reproductible d'une
    // execution a l'autre.
    std::mt19937 rng(0x5701Eu);
    std::uniform_real_distribution<float> bump(0.74f, 1.0f);

    for (glm::vec3& point : points) {
        point *= bump(rng);
        // Aplatissement vertical : un galet pose au sol est plus large que haut.
        // Applique ICI, dans la geometrie, et non par une echelle d'instance :
        // une echelle non uniforme casserait la normal matrix mat3(model) sur
        // laquelle repose le contrat de DrawItem.
        point.x *= kStoneWidth;
        point.z *= kStoneWidth;
        point.y *= kStoneRadius;
    }

    for (const auto& face : faces) {
        appendFacet(mesh, points[face[0]], points[face[1]], points[face[2]], kStoneColor);
    }

    return mesh;
}

MeshData flakeMeshData() {
    MeshData mesh;
    mesh.name = "eclat de pierre taillee (Phase 6)";

    // Contour irregulier dans le plan XZ, allonge le long de +X (meme convention
    // que la buche et la branche : les objets couches pointent vers +X, ce qui
    // rend GroundItem::yaw interchangeable entre tous les objets au sol).
    constexpr int kSides = 7;
    const float halfLength = 0.5f * kFlakeLength;
    const float halfWidth = 0.5f * kFlakeWidth;

    std::mt19937 rng(0xEC1A7u);
    std::uniform_real_distribution<float> wobble(0.78f, 1.0f);

    std::vector<glm::vec3> rim;
    rim.reserve(kSides);
    for (int i = 0; i < kSides; ++i) {
        const float angle = kTwoPi * static_cast<float>(i) / static_cast<float>(kSides);
        const float r = wobble(rng);
        rim.push_back({ halfLength * r * std::cos(angle), 0.0f,
                        halfWidth * r * std::sin(angle) });
    }

    // Face VENTRALE : plate, tournee vers le bas. C'est la surface laissee par
    // l'onde de choc de la percussion, et c'est elle qui repose sur le sol.
    const glm::vec3 ventralCenter{ 0.0f, -kFlakeRadius, 0.0f };
    for (int i = 0; i < kSides; ++i) {
        const glm::vec3& a = rim[static_cast<std::size_t>(i)];
        const glm::vec3& b = rim[static_cast<std::size_t>((i + 1) % kSides)];
        // Les points du contour sont ramenes au plan ventral : le perimetre est
        // donc l'arete vive entre les deux faces, exactement comme sur un eclat.
        appendFacet(mesh,
                    ventralCenter,
                    { a.x, -kFlakeRadius, a.z },
                    { b.x, -kFlakeRadius, b.z },
                    kFlakeVentral);
    }

    // Face DORSALE : une arete centrale, decalee vers l'arriere pour que l'eclat
    // ne soit pas symetrique (le talon est epais, la pointe est fine).
    const glm::vec3 dorsalApex{ -0.15f * kFlakeLength, kFlakeRadius, 0.0f };
    for (int i = 0; i < kSides; ++i) {
        const glm::vec3& a = rim[static_cast<std::size_t>(i)];
        const glm::vec3& b = rim[static_cast<std::size_t>((i + 1) % kSides)];
        appendFacet(mesh,
                    dorsalApex,
                    { b.x, -kFlakeRadius, b.z },
                    { a.x, -kFlakeRadius, a.z },
                    kFlakeDorsal);
    }

    return mesh;
}

} // namespace vercora
