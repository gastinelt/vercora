#include "world/AnimalMesh.hpp"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>

namespace vercora {
namespace {

// Teintes de sommet, comme pour la pierre et l'arbre : le materiau Object
// (cube.vert/frag) les utilise telles quelles, il n'y a pas de texture.
const glm::vec3 kCoatColor {0.42f, 0.31f, 0.20f}; // pelage fauve
const glm::vec3 kBellyColor{0.58f, 0.48f, 0.36f}; // ventre et croupe, plus clairs
const glm::vec3 kLegColor  {0.30f, 0.22f, 0.15f}; // pattes, plus sombres
const glm::vec3 kHeadColor {0.36f, 0.27f, 0.18f};

// Meme fonction que StoneMesh::appendFacet : trois sommets qui appartiennent au
// triangle, une seule normale, donc une arete franche.
void appendFacet(MeshData& mesh, const glm::vec3& a, const glm::vec3& b,
                 const glm::vec3& c, const glm::vec3& color) {
    const glm::vec3 rawNormal = glm::cross(b - a, c - a);
    const float length = glm::length(rawNormal);
    if (length < 1e-9f) {
        return; // triangle degenere : sa normale n'existe pas
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

// Une boite quelconque decrite par ses deux coins opposes, en 12 triangles
// facettes. L'ordre des sommets de chaque face est choisi pour que la normale
// calculee par appendFacet sorte du volume (sens trigonometrique vu de
// l'exterieur) : une face a l'envers serait eclairee par derriere et
// apparaitrait noire.
//
// Une boite suffit pour toutes les parties du corps : ce qui rend l'animal
// lisible a trente metres est sa SILHOUETTE (encolure inclinee, pattes fines,
// corps allonge), pas le nombre de facettes de son flanc.
void appendBox(MeshData& mesh, const glm::vec3& min, const glm::vec3& max,
               const glm::vec3& color) {
    const glm::vec3 p000{min.x, min.y, min.z};
    const glm::vec3 p100{max.x, min.y, min.z};
    const glm::vec3 p110{max.x, max.y, min.z};
    const glm::vec3 p010{min.x, max.y, min.z};
    const glm::vec3 p001{min.x, min.y, max.z};
    const glm::vec3 p101{max.x, min.y, max.z};
    const glm::vec3 p111{max.x, max.y, max.z};
    const glm::vec3 p011{min.x, max.y, max.z};

    // -Z et +Z
    appendFacet(mesh, p000, p110, p100, color);
    appendFacet(mesh, p000, p010, p110, color);
    appendFacet(mesh, p001, p101, p111, color);
    appendFacet(mesh, p001, p111, p011, color);
    // -X et +X
    appendFacet(mesh, p000, p001, p011, color);
    appendFacet(mesh, p000, p011, p010, color);
    appendFacet(mesh, p100, p111, p101, color);
    appendFacet(mesh, p100, p110, p111, color);
    // -Y et +Y
    appendFacet(mesh, p000, p100, p101, color);
    appendFacet(mesh, p000, p101, p001, color);
    appendFacet(mesh, p010, p011, p111, color);
    appendFacet(mesh, p010, p111, p110, color);
}

// Une boite INCLINEE, decrite par deux sections rectangulaires successives le
// long d'un segment. C'est ce qui donne l'encolure : une boite droite y
// dessinerait une marche d'escalier entre le corps et la tete.
void appendTaperedLink(MeshData& mesh, const glm::vec3& from, float fromHalfWidth,
                       float fromHalfHeight, const glm::vec3& to, float toHalfWidth,
                       float toHalfHeight, const glm::vec3& color) {
    // Les sections restent verticales (leur normale n'est pas l'axe du
    // segment) : approximation assumee, invisible sur un tronc de cone de
    // 30 cm, et elle evite d'avoir a construire un repere orthonorme complet.
    const auto section = [](const glm::vec3& centre, float halfWidth, float halfHeight) {
        return std::array<glm::vec3, 4>{
            glm::vec3{centre.x, centre.y - halfHeight, centre.z - halfWidth},
            glm::vec3{centre.x, centre.y - halfHeight, centre.z + halfWidth},
            glm::vec3{centre.x, centre.y + halfHeight, centre.z + halfWidth},
            glm::vec3{centre.x, centre.y + halfHeight, centre.z - halfWidth},
        };
    };

    const auto a = section(from, fromHalfWidth, fromHalfHeight);
    const auto b = section(to, toHalfWidth, toHalfHeight);

    for (int i = 0; i < 4; ++i) {
        const int j = (i + 1) % 4;
        appendFacet(mesh, a[static_cast<std::size_t>(i)], b[static_cast<std::size_t>(i)],
                    b[static_cast<std::size_t>(j)], color);
        appendFacet(mesh, a[static_cast<std::size_t>(i)], b[static_cast<std::size_t>(j)],
                    a[static_cast<std::size_t>(j)], color);
    }
    // Bouchons : sans eux, on voit l'interieur du volume par les deux bouts.
    appendFacet(mesh, a[0], a[2], a[1], color);
    appendFacet(mesh, a[0], a[3], a[2], color);
    appendFacet(mesh, b[0], b[1], b[2], color);
    appendFacet(mesh, b[0], b[2], b[3], color);
}

} // namespace

MeshData animalMeshData() {
    MeshData mesh;
    mesh.name = "cervide synthetique (Phase 17)";

    // Repere : +X vers l'avant (museau), +Y vers le haut, y = 0 aux sabots.
    // Toutes les cotes sont en metres et tiennent dans l'enveloppe annoncee par
    // kAnimalLength / kAnimalHeight / kAnimalWidth.
    constexpr float kLegHeight = 0.46f;   // hauteur du ventre
    constexpr float kBodyTop   = 0.78f;   // ligne du dos
    constexpr float kHalfWidth = 0.5f * kAnimalWidth;
    constexpr float kLegHalf   = 0.045f;  // demi-section d'une patte

    // --- Corps : de la croupe (x negatif) au poitrail --------------------
    appendBox(mesh, { -0.62f, kLegHeight, -kHalfWidth },
                    {  0.24f, kBodyTop,    kHalfWidth }, kCoatColor);
    // Ventre plus clair, en tranche mince sous le corps : c'est ce contraste
    // qui empeche le corps de se lire comme une simple caisse.
    appendBox(mesh, { -0.60f, kLegHeight - 0.03f, -kHalfWidth + 0.04f },
                    {  0.22f, kLegHeight + 0.06f,  kHalfWidth - 0.04f }, kBellyColor);

    // --- Encolure : du poitrail vers l'avant et le haut ------------------
    appendTaperedLink(mesh, { 0.20f, 0.70f, 0.0f }, 0.10f, 0.11f,
                            { 0.46f, 0.92f, 0.0f }, 0.07f, 0.07f, kCoatColor);

    // --- Tete : petite boite allongee, museau vers +X --------------------
    appendBox(mesh, { 0.40f, 0.86f, -0.065f },
                    { 0.68f, 1.02f,  0.065f }, kHeadColor);
    // Oreilles : deux ecailles plates de part et d'autre du crane. Trois
    // triangles chacune, et elles font enormement pour la lisibilite de
    // l'espece a distance.
    for (const float side : { -1.0f, 1.0f }) {
        const glm::vec3 root{ 0.46f, 1.00f, side * 0.06f };
        const glm::vec3 tip { 0.42f, kAnimalHeight, side * 0.15f };
        const glm::vec3 back{ 0.52f, 1.00f, side * 0.06f };
        appendFacet(mesh, root, tip, back, kHeadColor);
        appendFacet(mesh, root, back, tip, kHeadColor); // dos de l'oreille
    }

    // --- Quatre pattes ----------------------------------------------------
    // Anterieures sous le poitrail, posterieures sous la croupe, ecartees de
    // part et d'autre de l'axe. Une patte est une boite verticale mince : a
    // cette echelle, un membre articule ne se verrait pas.
    for (const float x : { 0.10f, -0.48f }) {
        for (const float side : { -1.0f, 1.0f }) {
            const float z = side * (kHalfWidth - 0.07f);
            appendBox(mesh, { x - kLegHalf, 0.0f,       z - kLegHalf },
                            { x + kLegHalf, kLegHeight + 0.02f, z + kLegHalf },
                      kLegColor);
        }
    }

    // --- Queue : courte, claire, dressee ---------------------------------
    // Le miroir blanc du chevreuil. C'est le detail qui rend une FUITE lisible :
    // c'est la tache qu'on voit s'eloigner.
    // Elle s'arrete NET avant le museau en valeur absolue (0,66 contre 0,68) :
    // c'est ce qui permet de dire, en lisant les seuls sommets, de quel cote
    // l'animal regarde - et c'est ce que verifie le test de convention.
    appendBox(mesh, { -0.66f, kBodyTop - 0.14f, -0.05f },
                    { -0.56f, kBodyTop + 0.02f,  0.05f }, kBellyColor);

    return mesh;
}

} // namespace vercora
