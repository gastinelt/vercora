#pragma once

#include "renderer/InstanceData.hpp"

#include <glm/glm.hpp>

#include <cstdint>

namespace vercora {

// Identifie une geometrie enregistree aupres du Renderer (Renderer::addMesh).
// C'est un simple indice : le Renderer possede les Mesh, l'appelant ne manipule
// que des poignees, ce qui evite d'exposer Vulkan au code de gameplay.
using MeshHandle = uint32_t;

inline constexpr MeshHandle kInvalidMesh = static_cast<MeshHandle>(-1);

// Quel couple de shaders utiliser pour dessiner un objet.
// Volontairement un enum ferme et non un ShaderSet libre : le Renderer cree UN
// pipeline par valeur, et trie les draws par materiau pour limiter les
// changements de pipeline. Ajouter un materiau = ajouter une valeur ici et sa
// paire de shaders dans kMaterialShaders.
enum class Material : uint32_t {
    // Couleur de sommet + Blinn-Phong (Phase 2). Cube, meshes .obj, arbres.
    Object = 0,
    // Coloration par altitude et par pente (Phase 4). Reserve au terrain.
    Terrain = 1,
    // PHASE 11. Vegetation INSTANCIEE : arbres de decor et touffes d'herbe.
    // Un seul materiau pour les deux, et non deux : ils partagent exactement le
    // meme besoin (une geometrie, N transformations reduites, un eclairage
    // Blinn-Phong). Ce qui les distingue - l'ondulation, la teinte, la portee -
    // tient dans le maillage et dans les push constants, pas dans le pipeline.
    Foliage = 2,
};

inline constexpr uint32_t kMaterialCount = 3;

// Une instance a dessiner : une geometrie, une transformation, un materiau.
//
// La model matrix voyage par PUSH CONSTANT (et non plus par l'UBO comme en
// Phases 1 a 4) : elle change d'un objet a l'autre a l'interieur d'une meme
// frame, alors que l'UBO est ecrit une seule fois par frame. 64 octets, donc
// tres en dessous des 128 garantis par la spec Vulkan pour maxPushConstantsSize.
//
// CONTRAINTE : la partie lineaire de `model` doit rester une rotation
// eventuellement suivie d'une echelle UNIFORME. Le vertex shader utilise
// mat3(model) comme normal matrix, ce qui n'est exact que dans ce cas (une
// echelle uniforme ne change que la longueur de la normale, absorbee par le
// normalize() du fragment shader). Une echelle non uniforme imposerait de
// calculer transpose(inverse(mat3(model))) cote CPU et de la pousser en plus.
struct DrawItem {
    MeshHandle mesh = kInvalidMesh;
    glm::mat4  model{1.0f};
    Material   material = Material::Object;

    // Parametres libres pousses au vertex shader a cote de la model matrix.
    //
    // ATTENTION : leur signification depend du MATERIAU. C'est le meme bloc de
    // push constants pour tous les pipelines - c'est ce qui permet a un seul
    // pipeline layout de les servir tous - mais chaque paire de shaders
    // l'interprete a sa facon. Deux conventions coexistent aujourd'hui :
    //
    //   Material::Terrain (PHASE 10, shaders/terrain.vert)
    //     x = debut de la bande de geomorphing, y = fin, en unites monde
    //     (metres) mesurees depuis la camera ; zw reserves.
    //     Une bande VIDE (y <= x, le defaut) desactive le morphing. C'est ce qui
    //     permet a toute geometrie qui n'est pas une tuile de quadtree - cube,
    //     arbres, terrain d'un seul bloc en --no-lod - de traverser le meme
    //     pipeline sans rien changer.
    //
    //   Material::Object (PHASE 15, shaders/cube.vert et cube.frag)
    //     x = force de la teinte de temoin de l'editeur, 0 = aucune ;
    //     yzw = couleur de cette teinte.
    //     Sert a distinguer le fantome de pose et l'objet selectionne du meme
    //     modele pose normalement, SANS transparence donc sans tri en
    //     profondeur. Une force nulle - le cas de tous les DrawItem du jeu -
    //     laisse le rendu strictement identique a celui d'avant la Phase 15.
    glm::vec4  params{0.0f, 0.0f, 0.0f, 0.0f};

    // PHASE 30, Material::Terrain seulement : ou se trouve le maillage dans la
    // grille de la heightmap, pour que terrain.vert y retrouve la position
    // horizontale et l'UV de chaque sommet a partir de gl_VertexIndex au lieu
    // de les lire dans le buffer (voir renderer/GpuVertex.hpp, TerrainVertex).
    //   x, y = indices de grille (ix, iz) du premier sommet
    //   z    = pas, en echantillons de grille, entre deux sommets voisins
    //   w    = sommets par rangee
    // Une tuile de quadtree pousse (node.ix0, node.iz0, node.stride, 65) ; le
    // terrain d'un seul bloc (--no-lod) pousse (0, 0, 1, resolution).
    // Ignore par les autres materiaux.
    glm::ivec4 grid{0, 0, 1, 1};
};

// Ce que le Renderer pousse REELLEMENT en push constant, dans cet ordre.
// Duplique volontairement les deux membres correspondants de DrawItem : ce type
// decrit le bloc `push_constant` des shaders (donc un layout impose de
// l'exterieur), la ou DrawItem decrit une intention de rendu cote gameplay. Les
// melanger obligerait a garder `mesh` et `material` - inutiles au GPU - dans un
// budget de push constants qui ne fait que 128 octets garantis.
//
// 96 octets depuis la Phase 30 (le bloc `grid` du terrain) : toujours en
// dessous des 128 garantis par la spec Vulkan pour maxPushConstantsSize.
struct DrawItemPush {
    glm::mat4 model{1.0f};
    glm::vec4 params{0.0f, 0.0f, 0.0f, 0.0f};
    glm::ivec4 grid{0, 0, 1, 1};
};
static_assert(sizeof(DrawItemPush) == 96 && sizeof(DrawItemPush) <= 128,
              "Le bloc de push constants doit tenir dans les 128 octets garantis.");

// ---------------------------------------------------------------------------
// PHASE 11 : dessin INSTANCIE
// ---------------------------------------------------------------------------
// Une geometrie dessinee N fois en un seul appel, chaque exemplaire lisant sa
// transformation dans un tableau d'InstanceData (voir InstanceData.hpp).
//
// Les instances ne sont PAS possedees ici : le pointeur doit rester valide
// jusqu'au retour de drawFrame, qui les recopie dans un buffer de la frame. Ce
// choix evite une allocation par frame et par especes de vegetation, alors que
// l'appelant, lui, produit deja ses instances dans un vecteur qu'il reutilise.
struct InstancedDrawItem {
    MeshHandle mesh = kInvalidMesh;
    const InstanceData* instances = nullptr;
    uint32_t count = 0;
    Material material = Material::Foliage;

    // Pousses au vertex shader comme pour un DrawItem.
    // PHASE 11 : x = amplitude d'ondulation en metres (0 pour un arbre),
    // y = temps en secondes, z = distance de rendu (pour information du shader),
    // w = reserve.
    glm::vec4 params{0.0f, 0.0f, 0.0f, 0.0f};
};

} // namespace vercora
