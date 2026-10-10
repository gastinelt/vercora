#pragma once

#include "renderer/Vertex.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace vercora {

// Geometrie cote CPU, independante de Vulkan : ce que produit un loader
// (ObjLoader) ou un generateur (CubeMesh), et ce que consomme la classe Mesh
// pour remplir ses buffers GPU.
//
// Separer ce type de Mesh permet de valider/inspecter/transformer la geometrie
// AVANT de toucher au GPU (et donc de refuser un fichier invalide sans avoir
// alloue quoi que ce soit cote Vulkan).
struct MeshData {
    std::vector<Vertex>   vertices;
    std::vector<uint32_t> indices;

    // Nom lisible (fichier source ou libelle) : sert aux logs et au titre de
    // la fenetre, pas au rendu.
    std::string name;

    bool empty() const { return vertices.empty() || indices.empty(); }

    uint32_t triangleCount() const {
        return static_cast<uint32_t>(indices.size() / 3);
    }
};

} // namespace vercora
