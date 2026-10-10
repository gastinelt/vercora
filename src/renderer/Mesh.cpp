#include "renderer/Mesh.hpp"

#include "core/Logger.hpp"
#include "renderer/MeshData.hpp"
#include "renderer/VulkanBuffer.hpp"
#include "renderer/VulkanContext.hpp"

#include <stdexcept>
#include <string>

namespace vercora {

Mesh::Mesh(VulkanContext& context,
           BufferUploader& uploader,
           const MeshData& data,
           bool log) {
    // --- Validation AVANT toute allocation Vulkan ---------------------------
    // Un index hors limites ne produit pas d'erreur a la creation du buffer :
    // il se manifesterait au draw, sous forme de lecture hors du vertex buffer
    // (comportement indefini cote GPU). Mieux vaut refuser ici, avec un message
    // qui dit quoi.
    if (data.vertices.empty()) {
        throw std::runtime_error("Mesh vide : aucun sommet (" + data.name + ").");
    }
    if (data.indices.empty()) {
        throw std::runtime_error("Mesh vide : aucun index (" + data.name + ").");
    }
    if (data.indices.size() % 3 != 0) {
        throw std::runtime_error(
            "Mesh non triangule : " + std::to_string(data.indices.size()) +
            " index, ce qui n'est pas un multiple de 3 (" + data.name + ").");
    }

    const auto vertexCount = static_cast<uint32_t>(data.vertices.size());
    for (uint32_t index : data.indices) {
        if (index >= vertexCount) {
            throw std::runtime_error(
                "Index hors limites dans le mesh : " + std::to_string(index) +
                " pour " + std::to_string(vertexCount) + " sommets (" + data.name + ").");
        }
    }

    vertexCount_ = vertexCount;
    indexCount_ = static_cast<uint32_t>(data.indices.size());

    // --- Upload GPU ---------------------------------------------------------
    vertexBuffer_ = createDeviceLocalBuffer(
        context, uploader,
        data.vertices.data(),
        sizeof(data.vertices[0]) * data.vertices.size(),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);

    indexBuffer_ = createDeviceLocalBuffer(
        context, uploader,
        data.indices.data(),
        sizeof(data.indices[0]) * data.indices.size(),
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT);

    if (log) {
        logInfo("Mesh charge sur le GPU : " + data.name + " - " +
                std::to_string(vertexCount_) + " sommets, " +
                std::to_string(indexCount_ / 3) + " triangles.");
    }
}

Mesh::~Mesh() = default; // les buffers se detruisent avec leurs unique_ptr

std::unique_ptr<VulkanBuffer> Mesh::createDeviceLocalBuffer(
    VulkanContext& context,
    BufferUploader& uploader,
    const void* data,
    VkDeviceSize size,
    VkBufferUsageFlags usage) {
    // 1. Staging host-visible : accessible au CPU, on y ecrit la geometrie.
    auto staging = std::make_unique<VulkanBuffer>(
        context, size,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    staging->writeToBuffer(data, size);

    // 2. Buffer final device-local : rapide pour le GPU, non mappable.
    auto buffer = std::make_unique<VulkanBuffer>(
        context, size,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT | usage,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    // 3. PHASE 12 : la copie est CONFIEE, pas attendue. Le staging part avec
    //    elle - il doit survivre jusqu'a l'execution de la frame qui portera la
    //    commande de copie, ce qui n'est plus une duree connue ici. C'est tout
    //    le changement : avant, cette ligne bloquait le thread jusqu'a ce que le
    //    GPU ait vide sa queue.
    uploader.enqueueCopy(std::move(staging), buffer->handle());

    return buffer;
}

void Mesh::bind(VkCommandBuffer commandBuffer) const {
    const VkBuffer buffers[] = { vertexBuffer_->handle() };
    const VkDeviceSize offsets[] = { 0 };
    vkCmdBindVertexBuffers(commandBuffer, 0, 1, buffers, offsets);

    // uint32 : suffisant pour de gros meshes, indispensable pour du terrain
    // LiDAR plus tard (uint16 plafonnerait a 65536 sommets).
    vkCmdBindIndexBuffer(commandBuffer, indexBuffer_->handle(), 0, VK_INDEX_TYPE_UINT32);
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
