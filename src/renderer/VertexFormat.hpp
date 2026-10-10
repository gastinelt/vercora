#pragma once

#include "renderer/InstanceData.hpp"
#include "renderer/Vertex.hpp"

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

// Un seul binding, sommets entrelaces.
inline VkVertexInputBindingDescription vertexBindingDescription() {
    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(Vertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    return binding;
}

// Mapping des attributs vers les location(...) du vertex shader.
//
// Six attributs depuis la Phase 10 : les deux derniers portent la cible de
// geomorphing du terrain (voir Vertex.hpp) et ne sont decrits qu'aux pipelines
// dont le vertex shader les lit reellement.
// `includeMorph` a false pour un vertex shader qui ne lit pas les locations 4/5
// (cube.vert) : les decrire quand meme declencherait un avertissement de
// validation. Voir ShaderSet::usesMorphAttributes.
inline std::array<VkVertexInputAttributeDescription, 6> vertexAttributeDescriptions() {
    std::array<VkVertexInputAttributeDescription, 6> attrs{};

    attrs[0].binding = 0;
    attrs[0].location = 0;
    attrs[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attrs[0].offset = offsetof(Vertex, pos);

    attrs[1].binding = 0;
    attrs[1].location = 1;
    attrs[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    attrs[1].offset = offsetof(Vertex, color);

    attrs[2].binding = 0;
    attrs[2].location = 2;
    attrs[2].format = VK_FORMAT_R32G32B32_SFLOAT;
    attrs[2].offset = offsetof(Vertex, normal);

    // Deux composantes seulement -> format R32G32.
    attrs[3].binding = 0;
    attrs[3].location = 3;
    attrs[3].format = VK_FORMAT_R32G32_SFLOAT;
    attrs[3].offset = offsetof(Vertex, uv);

    // Position et normale du meme sommet dans la grille du niveau de detail
    // SUPERIEUR (plus grossier). Nulles sur toute geometrie qui n'est pas une
    // tuile de quadtree : sans effet, le shader ne morphe que si la bande de
    // morphing poussee par le DrawItem est non vide.
    attrs[4].binding = 0;
    attrs[4].location = 4;
    attrs[4].format = VK_FORMAT_R32G32B32_SFLOAT;
    attrs[4].offset = offsetof(Vertex, morphPos);

    attrs[5].binding = 0;
    attrs[5].location = 5;
    attrs[5].format = VK_FORMAT_R32G32B32_SFLOAT;
    attrs[5].offset = offsetof(Vertex, morphNormal);

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

// Locations 6 a 8 : les locations 0 a 5 appartiennent au sommet (voir plus haut),
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
