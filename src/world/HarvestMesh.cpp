#include "world/HarvestMesh.hpp"

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

// Teintes de sommet. Comme pour le galet et l'eclat, la couleur vient du SOMMET
// et non d'une texture : le materiau Object (cube.vert/frag) l'utilise telle
// quelle.
const glm::vec3 kClayColor  {0.51f, 0.35f, 0.26f}; // argile humide, ocre-rouge
const glm::vec3 kSandColor  {0.76f, 0.68f, 0.50f}; // sable clair, un peu jaune
const glm::vec3 kResinColor {0.85f, 0.60f, 0.18f}; // resine ambree
const glm::vec3 kStemColor  {0.36f, 0.44f, 0.22f}; // tige verte, mate
const glm::vec3 kBindColor  {0.44f, 0.38f, 0.24f}; // le lien du fagot

// Meme fonction que StoneMesh.cpp, et volontairement RECOPIEE plutot que
// partagee : la sortir dans un en-tete commun obligerait StoneMesh a dependre
// d'un troisieme fichier pour zero gain - c'est douze lignes sans etat, pas un
// systeme. Le jour ou un cinquieme generateur en aura besoin, ce sera le moment
// de la remonter.
void appendFacet(MeshData& mesh, const glm::vec3& a, const glm::vec3& b,
                 const glm::vec3& c, const glm::vec3& color) {
    const glm::vec3 rawNormal = glm::cross(b - a, c - a);
    const float length = glm::length(rawNormal);
    if (length < 1e-9f) {
        return; // triangle degenere : rien a dessiner, sa normale n'existe pas
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

using EdgeKey = std::pair<uint32_t, uint32_t>;

uint32_t midpointIndex(std::vector<glm::vec3>& points,
                       std::map<EdgeKey, uint32_t>& cache,
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

// Octaedre subdivise une fois (32 faces), sommets UNIQUES bruites puis mis aux
// dimensions voulues. Meme construction que le galet, et pour la meme raison :
// bruiter les triangles independamment ouvrirait le volume.
std::vector<std::array<uint32_t, 3>> subdividedOctahedron(std::vector<glm::vec3>& points) {
    points = {
        { 1.0f,  0.0f,  0.0f}, {-1.0f,  0.0f,  0.0f},
        { 0.0f,  1.0f,  0.0f}, { 0.0f, -1.0f,  0.0f},
        { 0.0f,  0.0f,  1.0f}, { 0.0f,  0.0f, -1.0f},
    };
    const std::vector<std::array<uint32_t, 3>> faces = {
        {0, 2, 4}, {2, 1, 4}, {1, 3, 4}, {3, 0, 4},
        {2, 0, 5}, {1, 2, 5}, {3, 1, 5}, {0, 3, 5},
    };

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
    return subdivided;
}

} // namespace

MeshData clayLumpMeshData() {
    MeshData mesh;
    mesh.name = "motte d'argile (Phase 22)";

    std::vector<glm::vec3> points;
    const auto faces = subdividedOctahedron(points);

    // Bruitage NETTEMENT plus faible que celui du galet (0,74-1,00) : l'argile
    // se petrit, elle n'a pas de cassures. C'est cette seule difference de
    // fourchette qui separe visuellement une motte d'un caillou de meme taille.
    std::mt19937 rng(0xC1A1Fu);
    std::uniform_real_distribution<float> bump(0.90f, 1.0f);

    for (glm::vec3& point : points) {
        point *= bump(rng);
        point.x *= kClayWidth;
        point.z *= kClayWidth;
        point.y *= kClayRadius;
    }
    for (const auto& face : faces) {
        appendFacet(mesh, points[face[0]], points[face[1]], points[face[2]], kClayColor);
    }
    return mesh;
}

MeshData sandPileMeshData() {
    MeshData mesh;
    mesh.name = "poignee de sable (Phase 22)";

    // Un cone tres surbaisse, pose sur sa base. Le sable n'a pas de forme
    // propre : sa seule geometrie est son ANGLE DE TALUS, et c'est exactement ce
    // que ce cone represente (kSandRadius / kSandWidth ~ 0,23, soit 13 degres -
    // volontairement plus plat que les ~34 degres d'un vrai talus de sable sec,
    // pour qu'une poignee versee au sol se lise comme une trace et non comme un
    // petit volcan).
    constexpr int kSides = 9;
    const glm::vec3 apex{ 0.0f, kSandRadius, 0.0f };
    const glm::vec3 baseCenter{ 0.0f, -kSandRadius, 0.0f };

    std::mt19937 rng(0x5A11Du);
    std::uniform_real_distribution<float> wobble(0.82f, 1.0f);

    std::vector<glm::vec3> rim;
    rim.reserve(kSides);
    for (int i = 0; i < kSides; ++i) {
        const float angle = kTwoPi * static_cast<float>(i) / static_cast<float>(kSides);
        const float r = kSandWidth * wobble(rng);
        rim.push_back({ r * std::cos(angle), -kSandRadius, r * std::sin(angle) });
    }

    for (int i = 0; i < kSides; ++i) {
        const glm::vec3& a = rim[static_cast<std::size_t>(i)];
        const glm::vec3& b = rim[static_cast<std::size_t>((i + 1) % kSides)];
        appendFacet(mesh, apex, a, b, kSandColor);
        // La base est fermee : sans elle, le tas serait vu de dessous comme un
        // trou des que le joueur le regarde depuis un talus en contrebas.
        appendFacet(mesh, baseCenter, b, a, kSandColor);
    }
    return mesh;
}

MeshData resinBlobMeshData() {
    MeshData mesh;
    mesh.name = "perle de resine (Phase 22)";

    std::vector<glm::vec3> points;
    const auto faces = subdividedOctahedron(points);

    std::mt19937 rng(0x235112u);
    std::uniform_real_distribution<float> bump(0.88f, 1.0f);

    for (glm::vec3& point : points) {
        point *= bump(rng);
        point.x *= kResinWidth;
        point.z *= kResinWidth;
        // Goutte : la moitie HAUTE est etiree, la moitie basse reste ronde.
        // C'est la silhouette d'une coulee figee, pas d'une bille.
        point.y *= (point.y > 0.0f) ? (1.9f * kResinRadius) : kResinRadius;
    }
    for (const auto& face : faces) {
        appendFacet(mesh, points[face[0]], points[face[1]], points[face[2]], kResinColor);
    }
    return mesh;
}

MeshData fiberBundleMeshData() {
    MeshData mesh;
    mesh.name = "fagot de tiges a fibres (Phase 22)";

    // Quelques tiges couchees le long de +X - meme convention que la buche, la
    // branche et l'eclat, ce qui rend GroundItem::yaw interchangeable entre tous
    // les objets au sol.
    constexpr int kStems = 5;
    const float halfLength = 0.5f * kFiberBundleLength;
    const float stemHalf = 0.35f * kFiberBundleRadius;

    std::mt19937 rng(0xF1BE12u);
    std::uniform_real_distribution<float> jitter(-1.0f, 1.0f);

    for (int i = 0; i < kStems; ++i) {
        const float offsetZ = jitter(rng) * (kFiberBundleRadius - stemHalf);
        const float offsetY = jitter(rng) * (kFiberBundleRadius - stemHalf);
        // Les deux bouts ne sont pas alignes : un fagot n'est pas un fagot si
        // toutes les tiges finissent au meme endroit.
        const float x0 = -halfLength * (0.75f + 0.25f * std::fabs(jitter(rng)));
        const float x1 = halfLength * (0.75f + 0.25f * std::fabs(jitter(rng)));

        // Une tige = un prisme triangulaire, soit six triangles. Trois faces
        // suffisent a ce qu'une tige lise comme un cylindre a cette taille.
        const std::array<glm::vec2, 3> section = {
            glm::vec2{ 0.0f, stemHalf },
            glm::vec2{ -0.866f * stemHalf, -0.5f * stemHalf },
            glm::vec2{ 0.866f * stemHalf, -0.5f * stemHalf },
        };
        for (int k = 0; k < 3; ++k) {
            const glm::vec2& p = section[static_cast<std::size_t>(k)];
            const glm::vec2& q = section[static_cast<std::size_t>((k + 1) % 3)];
            const glm::vec3 a{ x0, offsetY + p.y, offsetZ + p.x };
            const glm::vec3 b{ x1, offsetY + p.y, offsetZ + p.x };
            const glm::vec3 c{ x1, offsetY + q.y, offsetZ + q.x };
            const glm::vec3 d{ x0, offsetY + q.y, offsetZ + q.x };
            appendFacet(mesh, a, b, c, kStemColor);
            appendFacet(mesh, a, c, d, kStemColor);
        }
    }

    // Le LIEN : un anneau plat au milieu du fagot. C'est ce qui fait la
    // difference entre "des tiges par terre" et "des tiges recoltees".
    constexpr int kBindSides = 6;
    const float bindRadius = 1.15f * kFiberBundleRadius;
    const float bindHalf = 0.035f * kFiberBundleLength;
    for (int i = 0; i < kBindSides; ++i) {
        const float a0 = kTwoPi * static_cast<float>(i) / static_cast<float>(kBindSides);
        const float a1 = kTwoPi * static_cast<float>(i + 1) / static_cast<float>(kBindSides);
        const glm::vec3 p0{ -bindHalf, bindRadius * std::sin(a0), bindRadius * std::cos(a0) };
        const glm::vec3 p1{ -bindHalf, bindRadius * std::sin(a1), bindRadius * std::cos(a1) };
        const glm::vec3 q0{ bindHalf, bindRadius * std::sin(a0), bindRadius * std::cos(a0) };
        const glm::vec3 q1{ bindHalf, bindRadius * std::sin(a1), bindRadius * std::cos(a1) };
        appendFacet(mesh, p0, q0, q1, kBindColor);
        appendFacet(mesh, p0, q1, p1, kBindColor);
    }

    return mesh;
}

MeshData fiberPlantMeshData() {
    MeshData mesh;
    mesh.name = "plante a fibres sur pied (Phase 22)";

    // A l'echelle UNITE : une tige d'un metre, pied a l'origine. Le semis donne
    // sa taille a chaque instance (meme convention que la touffe d'herbe).
    //
    // Six tiges dressees et DIVERGENTES, chacune un prisme triangulaire qui
    // s'affine vers la pointe. La divergence est ce qui distingue une ronce
    // d'une touffe d'herbe a distance : l'herbe part droit, la ronce s'ecarte.
    constexpr int kStems = 6;
    constexpr float kBaseHalf = 0.020f; // demi-epaisseur au pied
    constexpr float kSpread = 0.28f;    // ecartement de la pointe, en unites

    std::mt19937 rng(0x1207125u);
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);

    for (int i = 0; i < kStems; ++i) {
        const float angle = kTwoPi * (static_cast<float>(i) + 0.35f * unit(rng))
                          / static_cast<float>(kStems);
        const float height = 0.55f + 0.45f * unit(rng);
        const float lean = kSpread * (0.45f + 0.55f * unit(rng));

        const glm::vec3 foot{ 0.06f * std::cos(angle), 0.0f, 0.06f * std::sin(angle) };
        const glm::vec3 tip{ foot.x + lean * std::cos(angle), height,
                             foot.z + lean * std::sin(angle) };

        const std::array<glm::vec2, 3> section = {
            glm::vec2{ 0.0f, kBaseHalf },
            glm::vec2{ -0.866f * kBaseHalf, -0.5f * kBaseHalf },
            glm::vec2{ 0.866f * kBaseHalf, -0.5f * kBaseHalf },
        };
        for (int k = 0; k < 3; ++k) {
            const glm::vec2& p = section[static_cast<std::size_t>(k)];
            const glm::vec2& q = section[static_cast<std::size_t>((k + 1) % 3)];
            // La pointe est ramenee a un POINT unique : la tige s'y termine en
            // fuseau au lieu de finir carrement, ce qui coute deux triangles de
            // moins par face et se lit mieux.
            // La section est REMONTEE de la moitie de kBaseHalf, qui est
            // exactement la profondeur du plus bas de ses trois sommets
            // (-0,5 x kBaseHalf, cf. `section`). Sans ce decalage la plante
            // s'enfoncerait d'un centimetre sous le sol a chaque instance ;
            // avec un decalage plein, elle flotterait d'autant. Le contrat du
            // mesh unite est que son pied est a y = 0, exactement.
            const glm::vec3 a{ foot.x + p.x, foot.y + p.y + 0.5f * kBaseHalf, foot.z };
            const glm::vec3 b{ foot.x + q.x, foot.y + q.y + 0.5f * kBaseHalf, foot.z };
            appendFacet(mesh, a, b, tip, kStemColor);
        }
    }

    return mesh;
}

} // namespace vercora
