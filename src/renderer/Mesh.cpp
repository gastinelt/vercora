#include "renderer/Mesh.hpp"

namespace vercora {

Mesh::Mesh(GpuBufferPool& pool, VertexLayout layout,
           const GpuRange& vertices, uint32_t vertexCount,
           const GpuRange& indices, VkIndexType indexType, uint32_t indexCount,
           bool ownsIndices)
    : pool_(pool), layout_(layout), vertices_(vertices), indices_(indices),
      indexType_(indexType), vertexCount_(vertexCount), indexCount_(indexCount),
      ownsIndices_(ownsIndices) {}

Mesh::~Mesh() {
    pool_.release(vertices_);
    if (ownsIndices_) {
        pool_.release(indices_);
    }
}

void Mesh::draw(VkCommandBuffer commandBuffer) const {
    vkCmdDrawIndexed(commandBuffer, indexCount_, 1, 0, 0, 0);
}

void Mesh::drawInstanced(VkCommandBuffer commandBuffer, uint32_t instanceCount) const {
    if (instanceCount == 0) {
        return; // un draw a zero instance est legal mais inutile
    }
    vkCmdDrawIndexed(commandBuffer, indexCount_, instanceCount, 0, 0, 0);
}

} // namespace vercora
