#pragma once

#include "renderer/GpuVertex.hpp"
#include "renderer/InstanceData.hpp"

#include <vulkan/vulkan.h>

#include <array>
#include <cstddef>

namespace vercora {

// Description VULKAN du sommet : comment le lire dans le buffer, et quel attribut
// alimente quel location(...) du vertex shader.
//
// Volontairement separee de Vertex.hpp. Vertex/MeshData se presentent comme de la
// geometrie "cote CPU, independante de Vulkan", mais tant que ces deux fonctions
// vivaient dans la struct, le simple fait d'inclure MeshData.hpp tirait
// <vulkan/vulkan.h> : toute la logique de jeu (world/, core/) devenait donc
// incompilable sans le SDK Vulkan installe, alors qu'elle ne depend que de GLM.
// Ce fichier est le SEUL endroit du dossier renderer/ ou le format de sommet
// rencontre Vulkan, et seul Pipeline.cpp en a besoin.
//
// Consequence concrete : la logique de jeu se compile et se teste sans GPU (voir
// tests/logic_tests.cpp), y compris sur une machine ou le SDK n'est pas installe.

// PHASE 30 : DEUX formats de sommet, un par famille de shaders (voir
// renderer/GpuVertex.hpp) - le format de travail CPU `Vertex` n'est plus
// televerse tel quel. Un seul binding entrelace dans les deux cas.

// --- Objets et vegetation : ObjectVertex, 44 octets -------------------------
inline VkVertexInputBindingDescription objectBindingDescription() {
    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(ObjectVertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    return binding;
}

// Locations 0 a 3 : position, couleur, normale, UV - les memes que depuis la
// Phase 3, seul le stride a change (les 24 octets de morphing, qu'aucun de ces
// shaders ne lisait, ne sont plus dans le buffer).
inline std::array<VkVertexInputAttributeDescription, 4> objectAttributeDescriptions() {
    std::array<VkVertexInputAttributeDescription, 4> attrs{};

    attrs[0].binding = 0;
    attrs[0].location = 0;
    attrs[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attrs[0].offset = offsetof(ObjectVertex, pos);

    attrs[1].binding = 0;
    attrs[1].location = 1;
    attrs[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    attrs[1].offset = offsetof(ObjectVertex, color);

    attrs[2].binding = 0;
    attrs[2].location = 2;
    attrs[2].format = VK_FORMAT_R32G32B32_SFLOAT;
    attrs[2].offset = offsetof(ObjectVertex, normal);

    // Deux composantes seulement -> format R32G32.
    attrs[3].binding = 0;
    attrs[3].location = 3;
    attrs[3].format = VK_FORMAT_R32G32_SFLOAT;
    attrs[3].offset = offsetof(ObjectVertex, uv);

    return attrs;
}

// --- Terrain : TerrainVertex, 16 octets --------------------------------------
inline VkVertexInputBindingDescription terrainBindingDescription() {
    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(TerrainVertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    return binding;
}

// Deux altitudes en flottant, deux normales en snorm16 (x et z, y reconstruit
// par terrain.vert). Tout le reste se deduit de gl_VertexIndex.
inline std::array<VkVertexInputAttributeDescription, 4> terrainAttributeDescriptions() {
    std::array<VkVertexInputAttributeDescription, 4> attrs{};

    attrs[0].binding = 0;
    attrs[0].location = 0;
    attrs[0].format = VK_FORMAT_R32_SFLOAT;
    attrs[0].offset = offsetof(TerrainVertex, height);

    attrs[1].binding = 0;
    attrs[1].location = 1;
    attrs[1].format = VK_FORMAT_R32_SFLOAT;
    attrs[1].offset = offsetof(TerrainVertex, morphHeight);

    attrs[2].binding = 0;
    attrs[2].location = 2;
    attrs[2].format = VK_FORMAT_R16G16_SNORM;
    attrs[2].offset = offsetof(TerrainVertex, normal);

    attrs[3].binding = 0;
    attrs[3].location = 3;
    attrs[3].format = VK_FORMAT_R16G16_SNORM;
    attrs[3].offset = offsetof(TerrainVertex, morphNormal);

    return attrs;
}

// ---------------------------------------------------------------------------
// PHASE 11 : binding d'INSTANCES (binding 1)
// ---------------------------------------------------------------------------
// Second flux de sommets, cadence par INSTANCE et non par sommet. Le pipeline
// n'en est decrit que pour les materiaux instancies (ShaderSet::usesInstancing) :
// declarer un binding qu'aucun shader ne lit ferait echouer la validation, et
// surtout obligerait a lier un buffer d'instances au moment de dessiner un cube.

inline VkVertexInputBindingDescription instanceBindingDescription() {
    VkVertexInputBindingDescription binding{};
    binding.binding = 1;
    binding.stride = sizeof(InstanceData);
    binding.inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
    return binding;
}

// Locations 6 a 8 : les locations basses appartiennent au sommet (voir plus haut),
// et un pipeline instancie decrit les deux flux dans le meme espace de numeros.
// Les trois flottants scalaires (scale, yaw, variation, fade) sont regroupes en
// deux attributs plutot qu'en quatre : moins d'attributs a decrire, et le shader
// les lit comme des vec2, ce qui se lit aussi bien.
inline std::array<VkVertexInputAttributeDescription, 3> instanceAttributeDescriptions() {
    std::array<VkVertexInputAttributeDescription, 3> attrs{};

    attrs[0].binding = 1;
    attrs[0].location = 6;
    attrs[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attrs[0].offset = offsetof(InstanceData, position);

    // scale et yaw sont contigus dans la struct : un seul vec2 les porte.
    attrs[1].binding = 1;
    attrs[1].location = 7;
    attrs[1].format = VK_FORMAT_R32G32_SFLOAT;
    attrs[1].offset = offsetof(InstanceData, scale);

    // variation et fade, contigus de la meme facon.
    attrs[2].binding = 1;
    attrs[2].location = 8;
    attrs[2].format = VK_FORMAT_R32G32_SFLOAT;
    attrs[2].offset = offsetof(InstanceData, variation);

    return attrs;
}

// Verifications que les regroupements ci-dessus supposent. Sans elles, un ajout
// de membre dans InstanceData decalerait silencieusement yaw ou fade.
static_assert(offsetof(InstanceData, yaw) == offsetof(InstanceData, scale) + sizeof(float),
              "scale et yaw doivent rester contigus : ils sont lus comme un vec2.");
static_assert(offsetof(InstanceData, fade) == offsetof(InstanceData, variation) + sizeof(float),
              "variation et fade doivent rester contigus : ils sont lus comme un vec2.");

} // namespace vercora
