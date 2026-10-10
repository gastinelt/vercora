#pragma once

#include <glm/glm.hpp>

namespace vercora {

// Un sommet : position, couleur, normale, coordonnee de texture, plus la cible
// de geomorphing ajoutee en Phase 10.
//
// L'UV n'est pas encore utilise par les shaders (pas de texturing avant une
// phase ulterieure) mais il est stocke des maintenant : le loader .obj de la
// Phase 3 le lit quand le fichier le fournit, et changer le format de sommet
// plus tard obligerait a retoucher pipeline + shaders + tous les meshes.
// Valeur par defaut (0,0) : la geometrie codee en dur (CubeMesh) ne definit pas
// d'UV, l'initialisation par agregat laisse donc ce membre a zero de facon
// deterministe plutot que non initialise.
//
// PHASE 10 - morphPos / morphNormal : ou ce sommet doit ALLER quand la tuile de
// terrain qui le porte passe au niveau de detail superieur (voir
// renderer/TerrainNodeMesh.hpp). Le vertex shader du terrain interpole entre
// (pos, normal) et (morphPos, morphNormal) selon la distance a la camera, ce qui
// supprime le "popping" au changement de LOD ET les fissures entre tuiles
// voisines de niveaux differents.
//
// Ces deux membres coutent 24 octets sur TOUS les sommets, y compris ceux du
// cube et des arbres qui ne s'en servent pas. C'est assume : un second format de
// sommet imposerait un second binding, un second pipeline layout et un second
// jeu de shaders, pour economiser quelques kilo-octets sur des meshes minuscules.
// Sur le terrain, ou le volume est reel, le quadtree fait par ailleurs baisser la
// quantite de geometrie residente d'un ordre de grandeur - largement de quoi
// payer ces 24 octets.
//
// Valeur par defaut nulle : sans effet, car le shader ne morphe que lorsque la
// bande [morphStart, morphEnd] poussee par le DrawItem est non vide, ce qui n'est
// le cas que pour les tuiles du quadtree.
//
// Ce header ne connait PAS Vulkan : c'est de la donnee pure. La description du
// format a l'attention du pipeline vit dans renderer/VertexFormat.hpp, qui est
// le seul fichier a inclure <vulkan/vulkan.h> pour ce sujet. Voir l'explication
// detaillee la-bas.
struct Vertex {
    glm::vec3 pos;
    glm::vec3 color;
    glm::vec3 normal;
    glm::vec2 uv{0.0f, 0.0f};
    glm::vec3 morphPos{0.0f, 0.0f, 0.0f};
    glm::vec3 morphNormal{0.0f, 0.0f, 0.0f};
};

} // namespace vercora
