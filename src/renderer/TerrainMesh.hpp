#pragma once

#include "renderer/GpuVertex.hpp"
#include "renderer/MeshData.hpp"

#include <cstdint>
#include <string>
#include <vector>

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

// PHASE 32. Le meme terrain d'un seul bloc, au format GPU du terrain
// (TerrainVertex, 16 octets) : c'est ce que televerse le chemin --no-lod, sans
// construire au passage le MeshData de 68 octets par sommet (1,7 Go sur la zone
// au pas de 1 m). vertices[v] == toTerrainVertex(terrainMeshData(h).vertices[v])
// au bit pres et les indices sont identiques (verifie par les tests). Les
// sommets n'ont pas de cible de geomorphing : elle vaut le sommet lui-meme.
struct TerrainGpuMesh {
    std::vector<TerrainVertex> vertices;
    std::vector<uint32_t> indices;
    std::string name;
};
TerrainGpuMesh terrainGpuMeshData(const Heightmap& heightmap);

} // namespace vercora
