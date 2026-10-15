#include "renderer/GpuBufferPool.hpp"

#include <cstdio>
#include <stdexcept>

namespace vercora {

GpuBufferPool::GpuBufferPool(VulkanContext& context, VkBufferUsageFlags usage,
                             VkMemoryPropertyFlags properties, bool mapped,
                             VkDeviceSize blockSize)
    : context_(context), usage_(usage), properties_(properties), mapped_(mapped),
      blockSize_(blockSize) {
    if (mapped_) {
        properties_ |= VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    }
}

GpuBufferPool::~GpuBufferPool() {
    for (std::unique_ptr<Block>& block : blocks_) {
        if (block) {
            destroyBlock(*block);
        }
    }
}

std::unique_ptr<GpuBufferPool::Block> GpuBufferPool::createBlock(VkDeviceSize capacity,
                                                                 bool dedicated) {
    auto block = std::make_unique<Block>(capacity);
    block->dedicated = dedicated;

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = capacity;
    bufferInfo.usage = usage_;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(context_.device(), &bufferInfo, nullptr, &block->buffer) != VK_SUCCESS) {
        throw std::runtime_error("GpuBufferPool : echec de la creation d'un bloc de " +
                                 std::to_string(capacity) + " octets.");
    }

    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(context_.device(), block->buffer, &requirements);
    try {
        block->memory = context_.allocateMemory(requirements, properties_, mapped_);
    } catch (...) {
        vkDestroyBuffer(context_.device(), block->buffer, nullptr);
        throw;
    }
    vkBindBufferMemory(context_.device(), block->buffer, block->memory.memory, 0);
    capacityBytes_ += capacity;
    return block;
}

void GpuBufferPool::destroyBlock(Block& block) {
    capacityBytes_ -= block.ranges.capacity();
    vkDestroyBuffer(context_.device(), block.buffer, nullptr);
    block.buffer = VK_NULL_HANDLE;
    context_.freeMemory(block.memory);
}

GpuRange GpuBufferPool::allocate(VkDeviceSize size) {
    if (size == 0) {
        throw std::runtime_error("GpuBufferPool : allocation de taille nulle.");
    }

    uint32_t index = UINT32_MAX;
    uint64_t offset = RangeAllocator::kInvalid;

    if (size > blockSize_ / 2) {
        // Bloc DEDIE, a la taille exacte : un tres gros maillage ne doit ni
        // gacher un bloc standard, ni l'empecher de servir aux autres.
        for (uint32_t k = 0; k <= blocks_.size(); ++k) {
            if (k == blocks_.size() || !blocks_[k]) {
                index = k;
                break;
            }
        }
        auto block = createBlock(size, true);
        offset = block->ranges.allocate(size, kAlignment);
        if (index == blocks_.size()) {
            blocks_.push_back(std::move(block));
        } else {
            blocks_[index] = std::move(block);
        }
    } else {
        // Premier bloc standard qui a la place.
        for (uint32_t k = 0; k < blocks_.size(); ++k) {
            Block* block = blocks_[k].get();
            if (block == nullptr || block->dedicated) {
                continue;
            }
            offset = block->ranges.allocate(size, kAlignment);
            if (offset != RangeAllocator::kInvalid) {
                index = k;
                break;
            }
        }
        if (offset == RangeAllocator::kInvalid) {
            for (uint32_t k = 0; k <= blocks_.size(); ++k) {
                if (k == blocks_.size() || !blocks_[k]) {
                    index = k;
                    break;
                }
            }
            auto block = createBlock(blockSize_, false);
            offset = block->ranges.allocate(size, kAlignment);
            if (index == blocks_.size()) {
                blocks_.push_back(std::move(block));
            } else {
                blocks_[index] = std::move(block);
            }
        }
    }

    Block& block = *blocks_[index];
    GpuRange range;
    range.buffer = block.buffer;
    range.offset = offset;
    range.size = size;
    range.block = index;
    if (block.memory.mapped != nullptr) {
        range.mapped = static_cast<unsigned char*>(block.memory.mapped) + offset;
    }
    usedBytes_ += size;
    return range;
}

void GpuBufferPool::release(const GpuRange& range) {
    if (!range.valid() || range.block >= blocks_.size() || !blocks_[range.block]) {
        return;
    }
    Block& block = *blocks_[range.block];
    block.ranges.release(range.offset, range.size);
    usedBytes_ -= range.size;
    if (!block.ranges.empty()) {
        return;
    }

    // Bloc vide. Un bloc dedie part toujours ; un bloc standard part seulement
    // s'il existe deja un AUTRE bloc standard vide - en survol, les tuiles
    // evincees et construites se croisent sans cesse, et rendre puis
    // reallouer 4 Mo a chaque passage a vide ramenerait les allocations que ce
    // pool supprime.
    bool keep = !block.dedicated;
    if (keep) {
        for (uint32_t k = 0; k < blocks_.size(); ++k) {
            if (k != range.block && blocks_[k] && !blocks_[k]->dedicated &&
                blocks_[k]->ranges.empty()) {
                keep = false;
                break;
            }
        }
    }
    if (!keep) {
        destroyBlock(block);
        blocks_[range.block].reset();
    }
}

uint32_t GpuBufferPool::blockCount() const {
    uint32_t count = 0;
    for (const std::unique_ptr<Block>& block : blocks_) {
        count += block ? 1u : 0u;
    }
    return count;
}

std::string GpuBufferPool::statsLine() const {
    char text[160];
    std::snprintf(text, sizeof(text),
                  "%.1f Mo utilises sur %.1f Mo alloues en %u bloc(s), %s",
                  static_cast<double>(usedBytes_) / (1024.0 * 1024.0),
                  static_cast<double>(capacityBytes_) / (1024.0 * 1024.0), blockCount(),
                  mapped_ ? "ecriture directe (memoire unifiee)" : "televersement par staging");
    return text;
}

} // namespace vercora
