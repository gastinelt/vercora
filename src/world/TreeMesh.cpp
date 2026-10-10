#include "world/TreeMesh.hpp"

#include <glm/glm.hpp>

#include <cmath>
#include <stdexcept>
#include <string>
#include <utility>

namespace vercora {
namespace {

constexpr float kTwoPi = 6.28318530718f;

// Teintes des sommets. Les meshes d'arbre passent par le materiau "Object"
// (cube.vert/frag) : leur couleur vient donc de la COULEUR DE SOMMET, pas du
// shader. Aucune texture, aucun fichier - c'est le point de cette phase.
const glm::vec3 kBarkColor    {0.30f, 0.21f, 0.14f}; // ecorce
const glm::vec3 kCutWoodColor {0.74f, 0.61f, 0.42f}; // bois frais coupe
const glm::vec3 kFoliageLow   {0.13f, 0.30f, 0.16f}; // etage bas, a l'ombre
const glm::vec3 kFoliageHigh  {0.19f, 0.40f, 0.20f}; // etage haut, expose
const glm::vec3 kBranchColor  {0.24f, 0.17f, 0.11f};

// PHASE 26 : le feuillu. Teintes franchement plus CLAIRES et plus JAUNES que
// celles du conifere - un hetre ne rend pas le vert bleute d'un epicea, et c'est
// le second indice de lecture apres la silhouette. Elles restent dans la meme
// famille de valeurs (rien de fluorescent) : ce qui doit se voir est une
// dominante de versant, pas un damier.
const glm::vec3 kBroadleafLow  {0.22f, 0.36f, 0.15f};
const glm::vec3 kBroadleafHigh {0.38f, 0.55f, 0.21f};

// Elargissement du houppier du feuillu par rapport a celui du conifere. 1,45 :
// un houppier de feuillu est nettement plus large que haut, et c'est ce rapport
// qui porte la silhouette de loin, quand les facettes ne se distinguent plus.
constexpr float kBroadleafCrownWidening = 1.45f;

// Anneaux du houppier arrondi. 5 anneaux + le cone de fermeture donnent une
// boule lisible pour ~100 triangles : le meme ordre de grandeur que le conifere
// (qui en fait 32 pour son houppier), donc aucun poste de cout nouveau. En
// dessous de 4 la boule se lit comme un tonneau.
constexpr uint32_t kBroadleafRings = 5;

// Hauteur relative, dans la bande du houppier, ou commence le premier anneau.
// Non nulle : le profil elliptique vaut zero a t = 0, et un houppier qui
// commencerait par un rayon nul laisserait voir le raccord au tronc.
constexpr float kBroadleafBaseT = 0.18f;

// Ajoute la SURFACE LATERALE d'un tronc de cone d'axe Y, entre les cercles
// (y0, r0) et (y1, r1). Sommets partages entre secteurs voisins et normales
// radiales : le tronc parait rond malgre ses 8 cotes, ce qui est exactement ce
// qu'on veut pour un cylindre (a l'inverse du houppier, volontairement facette).
//
// Normale exacte de la surface reglee, et non (cos, 0, sin) : sur un cone la
// normale est inclinee. La negliger donnerait un eclairage faux sur un tronc
// conique, d'autant plus visible que la lumiere est rasante.
void appendTube(MeshData& mesh, float y0, float r0, float y1, float r1,
                uint32_t sides, const glm::vec3& color) {
    const auto base = static_cast<uint32_t>(mesh.vertices.size());

    const float dy = y1 - y0;
    const float dr = r1 - r0;

    for (uint32_t i = 0; i < sides; ++i) {
        const float angle = kTwoPi * static_cast<float>(i) / static_cast<float>(sides);
        const float c = std::cos(angle);
        const float s = std::sin(angle);

        // Voir la derivation dans le commentaire ci-dessus : la normale sortante
        // vaut (dy*cos, -dr, dy*sin), normalisee.
        const glm::vec3 normal = glm::normalize(glm::vec3(dy * c, -dr, dy * s));

        Vertex bottom{};
        bottom.pos = { r0 * c, y0, r0 * s };
        bottom.color = color;
        bottom.normal = normal;
        mesh.vertices.push_back(bottom);

        Vertex top{};
        top.pos = { r1 * c, y1, r1 * s };
        top.color = color;
        top.normal = normal;
        mesh.vertices.push_back(top);
    }

    // Deux sommets par secteur : indice 2*i = bas, 2*i+1 = haut. Le modulo
    // referme l'anneau sur le premier secteur sans dupliquer de sommet (aucune
    // couture a gerer : ces meshes n'ont pas d'UV).
    for (uint32_t i = 0; i < sides; ++i) {
        const uint32_t next = (i + 1) % sides;
        const uint32_t b0 = base + 2 * i;
        const uint32_t t0 = b0 + 1;
        const uint32_t b1 = base + 2 * next;
        const uint32_t t1 = b1 + 1;

        // Enroulement anti-horaire vu de l'EXTERIEUR (meme convention que le
        // cube de la Phase 1). Le culling est desactive dans Pipeline, donc
        // l'ordre n'est pas critique, mais il reste correct pour l'activer.
        mesh.indices.insert(mesh.indices.end(), { b0, t0, t1 });
        mesh.indices.insert(mesh.indices.end(), { b0, t1, b1 });
    }
}

// Ajoute un disque plein d'axe Y a l'altitude y. upward = true : le disque
// regarde vers +Y (dessus de souche), false : vers -Y (dessous d'un houppier).
// Sans ces disques, le culling etant desactive, on verrait l'interieur du cone,
// eclaire par des normales tournees a l'envers.
void appendDisc(MeshData& mesh, float y, float radius, uint32_t sides,
                bool upward, const glm::vec3& color) {
    const auto base = static_cast<uint32_t>(mesh.vertices.size());
    const glm::vec3 normal = upward ? glm::vec3(0.0f, 1.0f, 0.0f)
                                    : glm::vec3(0.0f, -1.0f, 0.0f);

    Vertex center{};
    center.pos = { 0.0f, y, 0.0f };
    center.color = color;
    center.normal = normal;
    mesh.vertices.push_back(center);

    for (uint32_t i = 0; i < sides; ++i) {
        const float angle = kTwoPi * static_cast<float>(i) / static_cast<float>(sides);
        Vertex rim{};
        rim.pos = { radius * std::cos(angle), y, radius * std::sin(angle) };
        rim.color = color;
        rim.normal = normal;
        mesh.vertices.push_back(rim);
    }

    for (uint32_t i = 0; i < sides; ++i) {
        const uint32_t a = base + 1 + i;
        const uint32_t b = base + 1 + (i + 1) % sides;
        // (centre, i, i+1) tourne dans le sens anti-horaire vu de DESSOUS ;
        // l'ordre s'inverse donc pour un disque tourne vers le haut.
        if (upward) {
            mesh.indices.insert(mesh.indices.end(), { base, b, a });
        } else {
            mesh.indices.insert(mesh.indices.end(), { base, a, b });
        }
    }
}

// Ajoute un cone d'axe Y, base (yBase, radius) et sommet (yApex, 0), FACETTE :
// chaque triangle possede ses trois sommets et une normale unique. Contrairement
// au tronc, on veut ici des facettes nettes - c'est ce qui donne au houppier sa
// silhouette bas-poly lisible plutot qu'une bougie fondue. Ca resout aussi le
// probleme de la normale au sommet du cone, qui n'est pas definie si on partage
// le sommet entre tous les secteurs.
void appendFacetedCone(MeshData& mesh, float yBase, float radius, float yApex,
                       uint32_t sides, const glm::vec3& color) {
    for (uint32_t i = 0; i < sides; ++i) {
        const float a0 = kTwoPi * static_cast<float>(i) / static_cast<float>(sides);
        const float a1 = kTwoPi * static_cast<float>(i + 1) / static_cast<float>(sides);

        const glm::vec3 p0{ radius * std::cos(a0), yBase, radius * std::sin(a0) };
        const glm::vec3 p1{ radius * std::cos(a1), yBase, radius * std::sin(a1) };
        const glm::vec3 apex{ 0.0f, yApex, 0.0f };

        // Normale de la facette. L'ordre (p0, apex, p1) est anti-horaire vu de
        // l'exterieur, donc ce produit vectoriel sort du cone.
        const glm::vec3 normal = glm::normalize(glm::cross(apex - p0, p1 - p0));

        const auto base = static_cast<uint32_t>(mesh.vertices.size());
        for (const glm::vec3& position : { p0, apex, p1 }) {
            Vertex vertex{};
            vertex.pos = position;
            vertex.color = color;
            vertex.normal = normal;
            mesh.vertices.push_back(vertex);
        }
        mesh.indices.insert(mesh.indices.end(), { base, base + 1, base + 2 });
    }
}

// Bascule un mesh genere le long de +Y pour qu'il s'etende le long de +X.
// Rotation de -90 degres autour de Z : (x, y) -> (y, -x). Appliquee aux
// positions ET aux normales (c'est une rotation pure, donc la meme matrice
// convient aux deux - c'est precisement l'argument de la normal matrix).
// Permet de reutiliser appendTube/appendDisc pour une buche couchee.
void layDownAlongX(MeshData& mesh) {
    for (Vertex& vertex : mesh.vertices) {
        vertex.pos = { vertex.pos.y, -vertex.pos.x, vertex.pos.z };
        vertex.normal = { vertex.normal.y, -vertex.normal.x, vertex.normal.z };
    }
}

void validateProportions(const TreeProportions& p) {
    if (p.sides < 3) {
        throw std::runtime_error("TreeMesh : il faut au moins 3 cotes pour former un volume.");
    }
    if (p.trunkRadius <= 0.0f || p.crownRadius <= 0.0f) {
        throw std::runtime_error("TreeMesh : rayons de tronc et de houppier strictement positifs attendus.");
    }
    if (p.stumpHeight <= 0.0f || p.stumpHeight >= p.crownBase || p.crownBase >= 1.0f) {
        throw std::runtime_error(
            "TreeMesh : l'ordre 0 < stumpHeight < crownBase < 1 doit etre respecte "
            "(la charniere de chute doit se situer sous le houppier).");
    }
}

// Cylindre couche le long de X, centre sur l'origine, avec ses deux faces de
// coupe. Facteur commun a la buche et a la branche.
MeshData lyingCylinder(float length, float radius, uint32_t sides,
                       const glm::vec3& sideColor, const glm::vec3& capColor,
                       std::string name) {
    MeshData mesh;
    mesh.name = std::move(name);

    const float half = 0.5f * length;
    appendTube(mesh, -half, radius, half, radius, sides, sideColor);
    appendDisc(mesh, half, radius, sides, /*upward=*/true, capColor);
    appendDisc(mesh, -half, radius, sides, /*upward=*/false, capColor);

    layDownAlongX(mesh);
    return mesh;
}

} // namespace

MeshData treeMeshData(const TreeProportions& p) {
    validateProportions(p);

    MeshData mesh;
    mesh.name = "arbre synthetique (Phase 5)";

    // --- Tronc --------------------------------------------------------------
    // Il monte legerement AU-DELA du bas du houppier : sans ce recouvrement, le
    // raccord entre le cone et le tronc laisse voir un anneau de ciel des que la
    // camera passe au-dessus.
    const float trunkTop = p.crownBase + 0.08f;
    appendTube(mesh, 0.0f, p.trunkRadius, trunkTop, p.topRadius, p.sides, kBarkColor);

    // --- Houppier : deux cones empietant l'un sur l'autre --------------------
    // Deux etages plutot qu'un seul cone : la silhouette d'un conifere se lit a
    // la rupture de pente entre les etages, et ca ne coute que 8 triangles.
    const float crownSpan = 1.0f - p.crownBase;

    const float lowerApex = p.crownBase + crownSpan * 0.62f;
    appendFacetedCone(mesh, p.crownBase, p.crownRadius, lowerApex, p.sides, kFoliageLow);
    appendDisc(mesh, p.crownBase, p.crownRadius, p.sides, /*upward=*/false, kFoliageLow);

    const float upperBase = p.crownBase + crownSpan * 0.45f;
    const float upperRadius = p.crownRadius * 0.62f;
    appendFacetedCone(mesh, upperBase, upperRadius, 1.0f, p.sides, kFoliageHigh);
    appendDisc(mesh, upperBase, upperRadius, p.sides, /*upward=*/false, kFoliageHigh);

    return mesh;
}

MeshData broadleafTreeMeshData(const TreeProportions& p) {
    validateProportions(p);

    MeshData mesh;
    mesh.name = "feuillu synthetique (Phase 26)";

    // --- Tronc : STRICTEMENT celui du conifere ------------------------------
    // Meme appel, memes rayons, meme recouvrement de 0,08. C'est ce qui permet a
    // UNE souche de servir les deux essences (voir TreeMesh.hpp) : la souche
    // interpole son rayon de coupe sur cette droite-la, et sur aucune autre.
    const float trunkTop = p.crownBase + 0.08f;
    appendTube(mesh, 0.0f, p.trunkRadius, trunkTop, p.topRadius, p.sides, kBarkColor);

    // --- Houppier : anneaux empiles sur un profil elliptique ----------------
    // Le houppier occupe la meme bande verticale que celui du conifere, mais il
    // est PLUS LARGE (facteur ci-dessous) et surtout son rayon maximal est
    // atteint au MILIEU de la bande, non a sa base. C'est toute la difference de
    // lecture entre un cone et une boule : le conifere est le plus large en bas,
    // le feuillu au centre.
    const float crownSpan = 1.0f - p.crownBase;
    const float maxRadius = p.crownRadius * kBroadleafCrownWidening;

    // Le profil r(t) = maxRadius * sqrt(1 - (2t - 1)^2) est un demi-cercle
    // ecrase sur la bande : nul aux deux bouts, maximal au milieu. On ne l'evalue
    // PAS jusqu'a t = 0 : un houppier de rayon nul a sa base laisserait voir le
    // tronc nu sur toute la hauteur de raccord. On demarre donc a un rayon
    // franc, ce que fait le decalage kBroadleafBaseT.
    float previousY = 0.0f;
    float previousRadius = 0.0f;
    for (uint32_t ring = 0; ring <= kBroadleafRings; ++ring) {
        const float t = kBroadleafBaseT +
                        (1.0f - kBroadleafBaseT) *
                            (static_cast<float>(ring) / static_cast<float>(kBroadleafRings));
        const float y = p.crownBase + crownSpan * t;
        const float centred = 2.0f * t - 1.0f;
        const float radius = maxRadius * std::sqrt(std::max(0.0f, 1.0f - centred * centred));

        if (ring == 0) {
            // Disque tourne vers le BAS qui ferme le houppier par en dessous :
            // le culling etant desactive, sans lui on verrait l'interieur des
            // anneaux, eclaire par des normales retournees.
            appendDisc(mesh, y, radius, p.sides, /*upward=*/false, kBroadleafLow);
        } else {
            // Teinte interpolee du bas vers le haut : l'etage bas est a l'ombre
            // du houppier, l'etage haut est expose. Meme intention que les deux
            // teintes du conifere, en degrade continu puisque la forme l'est.
            const glm::vec3 color = glm::mix(kBroadleafLow, kBroadleafHigh, t);
            appendTube(mesh, previousY, previousRadius, y, radius, p.sides, color);
        }
        previousY = y;
        previousRadius = radius;
    }

    // Le dernier anneau a un rayon petit mais non nul : on le referme par un
    // cone jusqu'a y = 1.0 exactement, pour que la convention unite tienne au
    // sommet comme elle tient au pied.
    appendFacetedCone(mesh, previousY, previousRadius, 1.0f, p.sides, kBroadleafHigh);

    return mesh;
}

MeshData stumpMeshData(const TreeProportions& p) {
    validateProportions(p);

    MeshData mesh;
    mesh.name = "souche synthetique (Phase 5)";

    // Le rayon au sommet de la souche est interpole sur la meme droite que le
    // tronc de l'arbre : la souche est litteralement le bas de ce tronc, donc
    // les diametres coincident au point de coupe.
    const float trunkTop = p.crownBase + 0.08f;
    const float t = p.stumpHeight / trunkTop;
    const float cutRadius = p.trunkRadius + (p.topRadius - p.trunkRadius) * t;

    // Legerement enfoncee sous 0 : sur un terrain en pente, une souche posee
    // pile a y = 0 laisse un croissant de vide du cote amont.
    appendTube(mesh, -0.02f, p.trunkRadius, p.stumpHeight, cutRadius, p.sides, kBarkColor);
    appendDisc(mesh, p.stumpHeight, cutRadius, p.sides, /*upward=*/true, kCutWoodColor);

    return mesh;
}

MeshData logMeshData() {
    return lyingCylinder(kLogLength, kLogRadius, 8, kBarkColor, kCutWoodColor,
                         "buche synthetique (Phase 5)");
}

MeshData branchMeshData() {
    return lyingCylinder(kBranchLength, kBranchRadius, 6, kBranchColor, kBranchColor,
                         "branche synthetique (Phase 5)");
}

} // namespace vercora
