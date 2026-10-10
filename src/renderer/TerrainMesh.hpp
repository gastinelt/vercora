#pragma once

#include "renderer/MeshData.hpp"

namespace vercora {

class Heightmap;

// Convertit une grille d'altitudes en geometrie indexee, dans le meme MeshData
// que le cube en dur (CubeMesh) et que le loader .obj (ObjLoader) : la classe
// Mesh, les buffers Vulkan et le Renderer ne voient aucune difference.
//
// Positions en espace MONDE directement (voir le commentaire sur la model
// matrix dans TerrainMesh.cpp), normales calculees par differences finies sur
// les voisins, UV normalises sur [0,1]^2, couleur laissee a blanc : la
// coloration du terrain est calculee par le fragment shader a partir de
// l'altitude et de la pente, la couleur de sommet ne sert que de teinte
// (neutre ici).
//
// Lance std::runtime_error si la grille ne peut pas produire un maillage sain.
MeshData terrainMeshData(const Heightmap& heightmap);

} // namespace vercora
