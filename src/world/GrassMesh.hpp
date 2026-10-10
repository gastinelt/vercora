#pragma once

#include "renderer/MeshData.hpp"

namespace vercora {

// Touffe d'herbe : quelques quads croises, generes en code comme les autres
// geometries synthetiques (voir world/TreeMesh.hpp).
//
// CONVENTION UNITE, la meme que l'arbre : pied a l'origine, hauteur exactement
// 1.0, largeurs en fraction de la hauteur. L'instance ne porte donc qu'une
// echelle uniforme, ce qui autorise le vertex shader a transporter la normale
// par la seule rotation (voir renderer/InstanceData.hpp).
//
// L'UV.y vaut 0 au pied du brin et 1 a sa pointe : c'est ce que lit
// shaders/foliage.vert pour faire plier la touffe SANS decoller son pied du sol.
//
// POURQUOI DES QUADS CROISES ET NON DE VRAIS BRINS. Un brin modelise en volume
// coute une dizaine de triangles ; il en faudrait des centaines par touffe pour
// que ce soit lisible, donc des dizaines de millions a l'ecran. Trois quads
// croises a 120 degres donnent une silhouette correcte depuis n'importe quel
// azimut pour 12 triangles, et le rendu se fait sans culling de faces (deja le
// cas dans Pipeline.cpp), donc les deux cotes de chaque quad sont visibles.
//
// LIMITE ASSUMEE de cette premiere version : les quads sont OPAQUES et
// rectangulaires, sans texture ni decoupe alpha. Une touffe est donc un
// eventail de rubans verts plutot qu'un bouquet de brins. C'est ce qui permet de
// se passer entierement de melange alpha - donc de tri en profondeur - au prix
// d'un aspect plus schematique de pres. Voir NOTES-PHASE11.md.
//
// `blades` = nombre de quads croises (3 par defaut).
MeshData grassTuftMeshData(int blades = 3);

} // namespace vercora
