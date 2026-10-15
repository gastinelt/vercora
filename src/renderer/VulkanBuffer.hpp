#pragma once

#include "renderer/VulkanContext.hpp" // GpuAllocation

#include <vulkan/vulkan.h>

#include <memory>

namespace vercora {

class VulkanContext;
class VulkanBuffer;

// PHASE 12. Destinataire d'une copie staging -> device-local qui ne veut PAS
// attendre le GPU.
//
// Jusqu'ici tout upload passait par VulkanBuffer::copyFrom, qui soumet un
// command buffer jetable puis fait un vkQueueWaitIdle : une synchronisation
// complete avec le GPU, a chaque tuile de terrain. C'etait le "verrou n 1"
// identifie en Phase 10, celui qui imposait un budget de 4 tuiles construites
// par frame.
//
// L'implementation reelle est le Renderer : il accumule les copies demandees et
// les enregistre en tete du command buffer de la frame suivante, suivies d'UNE
// barriere transfert -> vertex input. Le buffer staging lui est CEDE, parce que
// sa duree de vie ne se termine plus au retour de l'appel mais a la fin de la
// frame qui aura execute la copie.
//
// L'interface existe pour que Mesh n'ait pas a connaitre le Renderer (qui, lui,
// possede des Mesh : la dependance inverse serait circulaire).
class BufferUploader {
public:
    virtual ~BufferUploader() = default;

    // Enregistre une copie de `staging` (integralite de sa taille) vers
    // `destination`, a partir de l'octet `destinationOffset` (PHASE 32 : la
    // destination est une PLAGE d'un bloc partage, voir GpuBufferPool). Prend
    // possession de `staging`, qui sera detruit quand la frame ayant execute la
    // copie sera terminee.
    virtual void enqueueCopy(std::unique_ptr<VulkanBuffer> staging,
                             VkBuffer destination, VkDeviceSize destinationOffset) = 0;
};

// Buffer Vulkan reutilisable : creation, allocation memoire, bind, map,
// ecriture et copie (staging). Evite de dupliquer le code d'allocation partout.
class VulkanBuffer {
public:
    VulkanBuffer(VulkanContext& context,
                 VkDeviceSize size,
                 VkBufferUsageFlags usage,
                 VkMemoryPropertyFlags properties);
    ~VulkanBuffer();

    VulkanBuffer(const VulkanBuffer&) = delete;
    VulkanBuffer& operator=(const VulkanBuffer&) = delete;

    VkBuffer     handle() const { return buffer_; }
    VkDeviceSize size() const   { return size_; }

    // Mappe la memoire de facon persistante (buffers host-visible uniquement).
    void  map();
    void  unmap();
    void* mapped() const { return mapped_; }

    // Copie 'dataSize' octets depuis 'data' vers la memoire mappee.
    void writeToBuffer(const void* data, VkDeviceSize dataSize);

    // Copie GPU->GPU depuis 'src' vers ce buffer via un command buffer jetable,
    // SUIVIE D'UN vkQueueWaitIdle : l'appel ne revient qu'une fois la copie
    // reellement terminee.
    //
    // PHASE 12 - a n'utiliser que HORS du chemin critique de rendu, c'est-a-dire
    // avant la premiere frame (orthophoto, textures). Pour tout ce qui est
    // televerse pendant que le jeu tourne - les tuiles du streaming - passer par
    // un BufferUploader : le vkQueueWaitIdle ci-dessous est exactement ce qui
    // plafonnait le streaming a 4 tuiles par frame.
    void copyFrom(const VulkanBuffer& src, VkCommandPool commandPool, VkQueue queue);

private:
    VulkanContext& context_;
    VkDeviceSize   size_ = 0;
    VkBuffer       buffer_ = VK_NULL_HANDLE;
    GpuAllocation  allocation_;
    void*          mapped_ = nullptr;
};

} // namespace vercora
