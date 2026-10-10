#include "world/GrassMesh.hpp"

#include <cmath>
#include <stdexcept>

namespace vercora {
namespace {

constexpr float kPi = 3.14159265358979323846f;

// Largeur d'un quad au pied, en fraction de la hauteur.
//
// PHASE 18 : elargi de 0,26 a 0,42, et l'effilement du quad presque supprime
// (0,06 -> 0,30). Ce n'est pas un changement de gout mais la consequence directe
// de la decoupe alpha : le quad n'est plus le brin, il est la PLANCHE dans
// laquelle shaders/foliage.frag decoupe trois brins effiles. Garder l'ancien
// quad etroit et deja effile aurait donne trois brins minuscules dans un
// triangle, c'est-a-dire moins de matiere qu'avant. C'est desormais le MASQUE
// qui porte la silhouette, et le quad qui doit lui laisser la place.
constexpr float kBaseHalfWidth = 0.42f;

// Largeur a la pointe. Un leger retrecissement subsiste : au-dela de la portee
// de la decoupe (voir foliage.vert), le quad redevient plein et c'est cette
// forme-la qu'on voit de loin ; un rectangle parfait s'y lirait comme un ruban.
constexpr float kTipHalfWidth = 0.30f;

// Teintes du pied et de la pointe. Un degrade vertical est ce qui donne le plus
// d'effet pour le moins de travail : le pied dans l'ombre de la touffe est plus
// sombre et plus froid, la pointe au soleil plus claire et plus jaune. Sans lui
// une touffe est un aplat vert qui se lit comme du carton.
constexpr float kBaseColor[3] = { 0.16f, 0.26f, 0.11f };
constexpr float kTipColor[3]  = { 0.47f, 0.58f, 0.24f };

} // namespace

MeshData grassTuftMeshData(int blades) {
    if (blades < 1) {
        throw std::runtime_error("grassTuftMeshData : au moins un quad est requis.");
    }

    MeshData mesh;
    mesh.name = "touffe d'herbe (Phase 11, decoupee en brins Phase 18)";
    mesh.vertices.reserve(static_cast<std::size_t>(blades) * 4);
    mesh.indices.reserve(static_cast<std::size_t>(blades) * 6);

    for (int blade = 0; blade < blades; ++blade) {
        // Les quads sont repartis regulierement en azimut. Le lacet de
        // l'INSTANCE (voir InstanceData) fait ensuite tourner la touffe entiere,
        // donc deux touffes voisines ne presentent pas la meme face.
        const float angle = kPi * static_cast<float>(blade) / static_cast<float>(blades);
        const float dirX = std::cos(angle);
        const float dirZ = std::sin(angle);

        // Normale du quad : perpendiculaire a son plan, donc horizontale. Un
        // quad vertical eclaire par une lumiere venant d'en haut recevrait ainsi
        // presque rien. On la RELEVE donc vers le haut : ce n'est pas la normale
        // geometrique du polygone, c'est celle que le brin AURAIT s'il etait un
        // volume. C'est le meme artifice que sur n'importe quel feuillage en
        // billboard, et sans lui l'herbe est uniformement noire.
        const float nx = -dirZ * 0.45f;
        const float nz = dirX * 0.45f;
        const float ny = 0.89f;

        const auto index = static_cast<uint32_t>(mesh.vertices.size());

        Vertex v;
        v.normal = { nx, ny, nz };

        // Pied gauche / droit (uv.y = 0 : ne bouge pas sous l'ondulation).
        v.color = { kBaseColor[0], kBaseColor[1], kBaseColor[2] };
        v.pos = { -dirX * kBaseHalfWidth, 0.0f, -dirZ * kBaseHalfWidth };
        v.uv = { 0.0f, 0.0f };
        mesh.vertices.push_back(v);

        v.pos = { dirX * kBaseHalfWidth, 0.0f, dirZ * kBaseHalfWidth };
        v.uv = { 1.0f, 0.0f };
        mesh.vertices.push_back(v);

        // Pointes (uv.y = 1 : amplitude maximale de l'ondulation).
        v.color = { kTipColor[0], kTipColor[1], kTipColor[2] };
        v.pos = { dirX * kTipHalfWidth, 1.0f, dirZ * kTipHalfWidth };
        v.uv = { 1.0f, 1.0f };
        mesh.vertices.push_back(v);

        v.pos = { -dirX * kTipHalfWidth, 1.0f, -dirZ * kTipHalfWidth };
        v.uv = { 0.0f, 1.0f };
        mesh.vertices.push_back(v);

        // Deux triangles. L'orientation n'a pas d'importance : le pipeline
        // dessine sans culling de faces.
        mesh.indices.push_back(index + 0);
        mesh.indices.push_back(index + 1);
        mesh.indices.push_back(index + 2);
        mesh.indices.push_back(index + 0);
        mesh.indices.push_back(index + 2);
        mesh.indices.push_back(index + 3);
    }

    return mesh;
}

} // namespace vercora
