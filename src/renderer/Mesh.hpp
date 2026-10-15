#pragma once

#include "renderer/GpuBufferPool.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>

namespace vercora {

// PHASE 32. Format des sommets d'un maillage, tel que son pipeline les lit
// (voir renderer/GpuVertex.hpp). Un maillage de terrain ne peut etre dessine
// qu'avec Material::Terrain, un maillage d'objet qu'avec les autres : le
// Renderer ecarte tout DrawItem qui melangerait les deux, plutot que de laisser
// le GPU lire 16 octets la ou il en attend 44.
enum class VertexLayout : uint8_t {
    Object,  // ObjectVertex, 44 octets
    Terrain, // TerrainVertex, 16 octets
};

// Un maillage pret a etre dessine : une plage de sommets et une plage d'indices
// dans les blocs partages du GpuBufferPool, et leurs compteurs.
//
// PHASE 32 : un Mesh ne possede PLUS de buffer Vulkan. Il possede des PLAGES,
// rendues au pool a sa destruction - laquelle est toujours differee par le
// Renderer de kMaxFramesInFlight + 1 images, exactement comme l'etaient les
// buffers jusqu'ici (voir Renderer::removeMesh). La topologie peut aussi etre
// PARTAGEE (les tuiles de terrain en ont une seule pour toutes) : le Mesh ne la
// rend alors pas, c'est le Renderer qui la possede.
//
// Le televersement (conversion des sommets, ecriture directe ou staging) est
// fait par le Renderer avant de construire le Mesh : cette classe ne fait que
// decrire et dessiner.
class Mesh {
public:
    Mesh(GpuBufferPool& pool, VertexLayout layout,
         const GpuRange& vertices, uint32_t vertexCount,
         const GpuRange& indices, VkIndexType indexType, uint32_t indexCount,
         bool ownsIndices);
    ~Mesh();

    Mesh(const Mesh&) = delete;
    Mesh& operator=(const Mesh&) = delete;

    // Emet le draw indexe. A appeler apres avoir lie les plages du maillage
    // (Renderer::recordCommandBuffer, qui evite les liaisons redondantes), les
    // descriptor sets et le pipeline.
    void draw(VkCommandBuffer commandBuffer) const;

    // PHASE 11 : meme dessin, repete `instanceCount` fois. Les instances lisent
    // leurs attributs dans le binding 1, que l'appelant doit avoir lie (le Mesh
    // ne connait que sa propre geometrie, pas le buffer d'instances, qui vit
    // dans le Renderer et change a chaque frame).
    void drawInstanced(VkCommandBuffer commandBuffer, uint32_t instanceCount) const;

    uint32_t vertexCount() const { return vertexCount_; }
    uint32_t indexCount() const  { return indexCount_; }
    VertexLayout layout() const  { return layout_; }

    const GpuRange& vertexRange() const { return vertices_; }
    const GpuRange& indexRange() const  { return indices_; }
    VkIndexType indexType() const       { return indexType_; }

    // Octets de geometrie PROPRES a ce maillage (la topologie partagee n'est
    // pas comptee : elle n'existe qu'une fois).
    VkDeviceSize ownedBytes() const {
        return vertices_.size + (ownsIndices_ ? indices_.size : 0);
    }

private:
    GpuBufferPool& pool_;
    VertexLayout layout_ = VertexLayout::Object;
    GpuRange vertices_;
    GpuRange indices_;
    VkIndexType indexType_ = VK_INDEX_TYPE_UINT32;
    uint32_t vertexCount_ = 0;
    uint32_t indexCount_ = 0;
    bool ownsIndices_ = true;
};

} // namespace vercora
