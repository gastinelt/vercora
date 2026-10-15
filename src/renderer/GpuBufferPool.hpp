#pragma once

#include "renderer/RangeAllocator.hpp"
#include "renderer/VulkanContext.hpp" // GpuAllocation

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace vercora {

// PHASE 32. Une plage de geometrie dans un bloc partage.
struct GpuRange {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    VkDeviceSize size = 0;
    // Non nul quand le bloc est mappe (memoire unifiee) : l'appelant ECRIT LA,
    // et le GPU lit exactement ces octets - ni staging, ni copie.
    void* mapped = nullptr;
    uint32_t block = UINT32_MAX; // interne au pool

    bool valid() const { return buffer != VK_NULL_HANDLE; }
};

// PHASE 32. Sous-allocation de TOUTE la geometrie du moteur (sommets et
// indices, objets et tuiles de terrain) dans quelques gros blocs Vulkan.
//
// LE PROBLEME MESURE : un vkAllocateMemory par buffer, soit deux par maillage
// plus deux de staging par televersement - ~17 000 appels en 30 s de survol
// (NOTES-PHASE32.md). Chaque appel est un aller-retour noyau sous Windows,
// arrondi a la granularite d'allocation du systeme, et le nombre de blocs
// vivants est plafonne par maxMemoryAllocationCount (souvent 4 096) : c'est ce
// plafond qui imposait les 512 tuiles residentes au plus.
//
// LE PRINCIPE : des blocs de kBlockSize octets, chacun un VkBuffer unique dans
// lequel les maillages occupent des PLAGES (RangeAllocator, premier
// ajustement). Une demande plus grande que la moitie d'un bloc recoit un bloc
// DEDIE a sa taille exacte (le terrain d'un seul bloc de --no-lod, ~400 Mo).
// Un bloc qui se vide est rendu au pilote, sauf le dernier bloc vide, garde
// pour amortir le va-et-vient du streaming.
//
// SYNCHRONISATION : AUCUNE ici. release() rend la plage IMMEDIATEMENT ; c'est
// a l'appelant de ne la rendre que lorsque plus aucune image en vol ne la lit
// - ce que fait deja le Renderer pour tout maillage retire (retiredMeshes_,
// kMaxFramesInFlight + 1 images, voir Renderer::removeMesh).
class GpuBufferPool {
public:
    // `mapped` = true : les blocs sont alloues HOST_VISIBLE | HOST_COHERENT et
    // mappes en permanence (memoire unifiee), les plages renvoyees portent
    // alors un pointeur d'ecriture directe.
    GpuBufferPool(VulkanContext& context, VkBufferUsageFlags usage,
                  VkMemoryPropertyFlags properties, bool mapped,
                  VkDeviceSize blockSize = kDefaultBlockSize);
    ~GpuBufferPool();

    GpuBufferPool(const GpuBufferPool&) = delete;
    GpuBufferPool& operator=(const GpuBufferPool&) = delete;

    // 4 Mo : 60 tuiles de terrain par bloc ; la perte moyenne au pic (un
    // demi-bloc) reste sous les 2 Mo.
    static constexpr VkDeviceSize kDefaultBlockSize = 4ull * 1024 * 1024;

    // 256 : couvre l'alignement de tout format de sommet et d'indice, et le
    // nonCoherentAtomSize des GPU courants.
    static constexpr VkDeviceSize kAlignment = 256;

    // Lance std::runtime_error si le pilote refuse un nouveau bloc.
    GpuRange allocate(VkDeviceSize size);
    void release(const GpuRange& range);

    bool mapped() const { return mapped_; }

    // Bilan : octets reserves par des plages, octets alloues au pilote, blocs.
    VkDeviceSize usedBytes() const { return usedBytes_; }
    VkDeviceSize capacityBytes() const { return capacityBytes_; }
    uint32_t blockCount() const;
    std::string statsLine() const;

private:
    struct Block {
        VkBuffer buffer = VK_NULL_HANDLE;
        GpuAllocation memory;
        RangeAllocator ranges;
        bool dedicated = false;

        explicit Block(VkDeviceSize capacity) : ranges(capacity) {}
    };

    std::unique_ptr<Block> createBlock(VkDeviceSize capacity, bool dedicated);
    void destroyBlock(Block& block);

    VulkanContext& context_;
    VkBufferUsageFlags usage_ = 0;
    VkMemoryPropertyFlags properties_ = 0;
    bool mapped_ = false;
    VkDeviceSize blockSize_ = kDefaultBlockSize;

    // Emplacements stables : GpuRange::block est un indice dans ce tableau, un
    // bloc rendu laisse un trou (nullptr) reutilise par le suivant.
    std::vector<std::unique_ptr<Block>> blocks_;

    VkDeviceSize usedBytes_ = 0;
    VkDeviceSize capacityBytes_ = 0;
};

} // namespace vercora
