#include "renderer/VulkanBuffer.hpp"

#include "renderer/VulkanContext.hpp"

#include <cstring>
#include <stdexcept>

namespace vercora {

VulkanBuffer::VulkanBuffer(VulkanContext& context,
                           VkDeviceSize size,
                           VkBufferUsageFlags usage,
                           VkMemoryPropertyFlags properties)
    : context_(context), size_(size) {
    VkDevice device = context_.device();

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size_;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(device, &bufferInfo, nullptr, &buffer_) != VK_SUCCESS) {
        throw std::runtime_error("Echec de la creation d'un buffer.");
    }

    VkMemoryRequirements memRequirements{};
    vkGetBufferMemoryRequirements(device, buffer_, &memRequirements);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex =
        context_.findMemoryType(memRequirements.memoryTypeBits, properties);

    if (vkAllocateMemory(device, &allocInfo, nullptr, &memory_) != VK_SUCCESS) {
        throw std::runtime_error("Echec de l'allocation memoire d'un buffer.");
    }

    vkBindBufferMemory(device, buffer_, memory_, 0);
}

VulkanBuffer::~VulkanBuffer() {
    VkDevice device = context_.device();
    if (mapped_ != nullptr) {
        vkUnmapMemory(device, memory_);
        mapped_ = nullptr;
    }
    if (buffer_ != VK_NULL_HANDLE) {
        vkDestroyBuffer(device, buffer_, nullptr);
    }
    if (memory_ != VK_NULL_HANDLE) {
        vkFreeMemory(device, memory_, nullptr);
    }
}

void VulkanBuffer::map() {
    if (mapped_ == nullptr) {
        vkMapMemory(context_.device(), memory_, 0, size_, 0, &mapped_);
    }
}

void VulkanBuffer::unmap() {
    if (mapped_ != nullptr) {
        vkUnmapMemory(context_.device(), memory_);
        mapped_ = nullptr;
    }
}

void VulkanBuffer::writeToBuffer(const void* data, VkDeviceSize dataSize) {
    map();
    std::memcpy(mapped_, data, static_cast<size_t>(dataSize));
}

void VulkanBuffer::copyFrom(const VulkanBuffer& src, VkCommandPool commandPool, VkQueue queue) {
    VkDevice device = context_.device();

    // Command buffer a usage unique pour le transfert.
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool = commandPool;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device, &allocInfo, &commandBuffer);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(commandBuffer, &beginInfo);

    VkBufferCopy copyRegion{};
    copyRegion.size = src.size();
    vkCmdCopyBuffer(commandBuffer, src.handle(), buffer_, 1, &copyRegion);

    vkEndCommandBuffer(commandBuffer);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;

    vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue); // simple et suffisant pour un transfert ponctuel a l'init.

    vkFreeCommandBuffers(device, commandPool, 1, &commandBuffer);
}

} // namespace vercora
