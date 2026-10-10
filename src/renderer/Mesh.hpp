#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>

namespace vercora {

class VulkanContext;
class VulkanBuffer;
class BufferUploader;
struct MeshData;

// Un mesh pret a etre dessine : vertex buffer + index buffer device-local et
// leurs compteurs. Interchangeable par construction : le Renderer ne sait pas
// d'ou vient la geometrie (fichier .obj, cube code en dur, terrain genere plus
// tard), il recoit un Mesh et l'affiche.
//
// L'upload passe par VulkanBuffer (staging host-visible -> device-local), le
// meme chemin que la Phase 1, factorise ici en une seule fonction.
//
// PHASE 12 : cet upload n'ATTEND PLUS le GPU. La copie est confiee a un
// BufferUploader (le Renderer), qui l'enregistre en tete du command buffer de la
// frame suivante ; le constructeur revient des que la geometrie a ete recopiee
// dans un buffer staging, sans aucune synchronisation. Consequence a garder en
// tete : les buffers du Mesh ne contiennent la geometrie qu'a partir de
// l'execution de cette frame - c'est-a-dire avant tout draw qui pourrait les
// lire, puisque la copie est enregistree AVANT le render pass et separee de lui
// par une barriere.
class Mesh {
public:
    // `uploader` recoit les copies staging -> device-local. Il doit vivre plus
    // longtemps que la frame en cours (en pratique c'est le Renderer lui-meme).
    // Lance std::runtime_error si la geometrie est vide ou incoherente.
    // `log` a false pour les geometries creees en masse (les tuiles du quadtree
    // de la Phase 10) : une ligne de journal par tuile noierait toute autre
    // information et couterait plus cher que l'upload lui-meme.
    Mesh(VulkanContext& context,
         BufferUploader& uploader,
         const MeshData& data,
         bool log = true);
    ~Mesh();

    Mesh(const Mesh&) = delete;
    Mesh& operator=(const Mesh&) = delete;

    // Lie les buffers du mesh au command buffer courant.
    void bind(VkCommandBuffer commandBuffer) const;

    // Emet le draw indexe. A appeler apres bind() et apres avoir lie les
    // descriptor sets / le pipeline.
    void draw(VkCommandBuffer commandBuffer) const;

    // PHASE 11 : meme dessin, repete `instanceCount` fois. Les instances lisent
    // leurs attributs dans le binding 1, que l'appelant doit avoir lie (le Mesh
    // ne connait que sa propre geometrie, pas le buffer d'instances, qui vit
    // dans le Renderer et change a chaque frame).
    void drawInstanced(VkCommandBuffer commandBuffer, uint32_t instanceCount) const;

    uint32_t vertexCount() const { return vertexCount_; }
    uint32_t indexCount() const  { return indexCount_; }

private:
    // Cree un buffer device-local et CONFIE a l'uploader la copie depuis un
    // staging temporaire. Commun au vertex buffer et a l'index buffer.
    static std::unique_ptr<VulkanBuffer> createDeviceLocalBuffer(
        VulkanContext& context,
        BufferUploader& uploader,
        const void* data,
        VkDeviceSize size,
        VkBufferUsageFlags usage);

    std::unique_ptr<VulkanBuffer> vertexBuffer_;
    std::unique_ptr<VulkanBuffer> indexBuffer_;

    uint32_t vertexCount_ = 0;
    uint32_t indexCount_ = 0;
};

} // namespace vercora
