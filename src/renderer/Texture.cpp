#include "renderer/Texture.hpp"

#include "core/Logger.hpp"
#include "renderer/VulkanBuffer.hpp"
#include "renderer/VulkanContext.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace vercora {
namespace {

VkFormat toVkFormat(TextureFormat format) {
    switch (format) {
        case TextureFormat::BC1:
            // ..._UNORM_BLOCK et non _SRGB_BLOCK : la swapchain de ce moteur est
            // en UNORM et l'eclairage est calcule tel quel depuis la Phase 2.
            // Prendre une variante sRGB ici ferait subir a la seule orthophoto
            // une conversion que rien d'autre dans le pipeline n'applique, donc
            // la rendrait plus claire que les couleurs de sommet voisines.
            return VK_FORMAT_BC1_RGB_UNORM_BLOCK;
        case TextureFormat::RGBA8:
            break;
    }
    return VK_FORMAT_R8G8B8A8_UNORM;
}

// Barriere de transition de layout couvrant TOUTE la chaine de mipmaps. Les
// niveaux sont copies d'un bloc, donc il n'y a jamais besoin de transitionner un
// niveau isolement (ce que ferait une generation de mipmaps par blit).
void transitionLayout(VkCommandBuffer commandBuffer, VkImage image, uint32_t mipLevels,
                      VkImageLayout oldLayout, VkImageLayout newLayout,
                      VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                      VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage) {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = mipLevels;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;

    vkCmdPipelineBarrier(commandBuffer, srcStage, dstStage, 0,
                         0, nullptr, 0, nullptr, 1, &barrier);
}

} // namespace

TextureData Texture::whitePixelData() {
    TextureData data;
    data.width = 1;
    data.height = 1;
    data.format = TextureFormat::RGBA8;
    data.pixels = { 255, 255, 255, 255 };
    data.mips.push_back({ 1, 1, 0, 4 });
    data.label = "blanc 1x1";
    // Emprise volontairement degeneree : cette texture n'est PAS georeferencee,
    // et personne ne doit croire qu'elle recouvre quoi que ce soit.
    data.epsg = 0;
    return data;
}

Texture::Texture(VulkanContext& context, VkCommandPool commandPool, VkQueue queue,
                 const TextureData& data, bool repeat)
    : Texture(context, commandPool, queue, data,
              [&data](void* destination, uint64_t offset, uint64_t bytes) {
                  std::memcpy(destination, data.pixels.data() + offset,
                              static_cast<std::size_t>(bytes));
              },
              repeat) {
}

Texture::Texture(VulkanContext& context, VkCommandPool commandPool, VkQueue queue,
                 const TextureData& data, const PixelSource& source, bool repeat)
    : context_(context) {
    if (data.mips.empty() || data.payloadSize() == 0 ||
        (!data.pixels.empty() && data.pixels.size() < data.payloadSize())) {
        throw std::runtime_error("Texture : donnees vides.");
    }

    const VkFormat format = toVkFormat(data.format);

    // Verification AVANT toute allocation : sur un GPU sans textureCompressionBC,
    // creer l'image echouerait de toute facon, mais avec un message du driver
    // au lieu d'une explication. Le repli existe et il est nomme.
    VkFormatProperties properties{};
    vkGetPhysicalDeviceFormatProperties(context_.physicalDevice(), format, &properties);
    if (!(properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
        throw std::runtime_error(
            "Texture : ce GPU ne sait pas echantillonner le format demande" +
            std::string(data.format == TextureFormat::BC1
                ? " (BC1). Regenerer la texture avec "
                  "tools/make_ortho_texture.py --format rgba."
                : "."));
    }

    const uint32_t mipLevels = static_cast<uint32_t>(data.mips.size());
    createImage(data.width, data.height, mipLevels, format);
    uploadAndTransition(commandPool, queue, data, source);
    createView(format, mipLevels);
    createSampler(mipLevels, repeat);
}

Texture::~Texture() {
    VkDevice device = context_.device();
    if (sampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(device, sampler_, nullptr);
    }
    if (view_ != VK_NULL_HANDLE) {
        vkDestroyImageView(device, view_, nullptr);
    }
    if (image_ != VK_NULL_HANDLE) {
        vkDestroyImage(device, image_, nullptr);
    }
    context_.freeMemory(allocation_);
}

void Texture::createImage(uint32_t width, uint32_t height, uint32_t mipLevels,
                          VkFormat format) {
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent = { width, height, 1 };
    imageInfo.mipLevels = mipLevels;
    imageInfo.arrayLayers = 1;
    imageInfo.format = format;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;

    VkDevice device = context_.device();
    if (vkCreateImage(device, &imageInfo, nullptr, &image_) != VK_SUCCESS) {
        throw std::runtime_error("Texture : echec de la creation de l'image.");
    }

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device, image_, &requirements);

    // PHASE 32 : par le contexte, qui tient le compte de la memoire GPU.
    allocation_ = context_.allocateMemory(requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkBindImageMemory(device, image_, allocation_.memory, 0);
    deviceSize_ = requirements.size;
}

void Texture::uploadAndTransition(VkCommandPool commandPool, VkQueue queue,
                                  const TextureData& data, const PixelSource& source) {
    VkDevice device = context_.device();
    const VkDeviceSize total = static_cast<VkDeviceSize>(data.payloadSize());
    const uint32_t mipLevels = static_cast<uint32_t>(data.mips.size());

    // PHASE 32 : un staging de 4 Mo au plus, REUTILISE tranche apres tranche,
    // au lieu d'un staging de la taille de toute la chaine. Pour l'orthophoto
    // de la zone, c'etait 42 Mo de memoire GPU transitoire en plus de l'image
    // elle-meme : le pic de memoire GPU de TOUTE la partie se situait la, au
    // chargement. Les texels sont ecrits dans le staging par `source` -
    // directement depuis le fichier pour l'orthophoto.
    constexpr VkDeviceSize kStagingChunk = 4ull * 1024 * 1024;
    const VkDeviceSize stagingSize = std::min(total, kStagingChunk);
    VulkanBuffer staging(context_, stagingSize,
                         VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    staging.map();
    auto* stagingBytes = static_cast<unsigned char*>(staging.mapped());

    // --- Decoupe en BANDES de rangees de blocs ------------------------------
    // Une bande = des rangees completes d'un niveau, assez peu pour tenir dans
    // le staging. BC1 se range par rangees de blocs de 4 texels de haut ; une
    // bande qui n'atteint pas le bas d'un niveau a donc une hauteur multiple de
    // 4, comme l'exige une copie vers un format compresse.
    struct Band {
        uint32_t level = 0;
        uint32_t y = 0;
        uint32_t height = 0;
        uint64_t payloadOffset = 0;
        uint64_t bytes = 0;
    };
    std::vector<Band> bands;
    const bool blocks = (data.format == TextureFormat::BC1);
    const uint32_t texelsPerRow = blocks ? 4u : 1u;
    for (uint32_t level = 0; level < mipLevels; ++level) {
        const TextureMipLevel& mip = data.mips[level];
        const uint64_t rowBytes = blocks ? static_cast<uint64_t>((mip.width + 3u) / 4u) * 8u
                                         : static_cast<uint64_t>(mip.width) * 4u;
        const uint32_t rows = (mip.height + texelsPerRow - 1u) / texelsPerRow;
        const uint32_t rowsPerBand = static_cast<uint32_t>(
            std::max<uint64_t>(1u, stagingSize / rowBytes));
        for (uint32_t row = 0; row < rows; row += rowsPerBand) {
            const uint32_t count = std::min(rowsPerBand, rows - row);
            Band band;
            band.level = level;
            band.y = row * texelsPerRow;
            band.height = std::min(count * texelsPerRow, mip.height - band.y);
            band.payloadOffset = mip.offset + static_cast<uint64_t>(row) * rowBytes;
            band.bytes = static_cast<uint64_t>(count) * rowBytes;
            bands.push_back(band);
        }
    }

    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool = commandPool;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device, &allocInfo, &commandBuffer);

    // --- Un lot de bandes par remplissage du staging ------------------------
    std::vector<VkBufferImageCopy> regions;
    std::size_t next = 0;
    bool first = true;
    while (next < bands.size()) {
        regions.clear();
        VkDeviceSize used = 0;
        while (next < bands.size() && used + bands[next].bytes <= stagingSize) {
            const Band& band = bands[next];
            source(stagingBytes + used, band.payloadOffset, band.bytes);

            VkBufferImageCopy region{};
            region.bufferOffset = used;
            // 0 = "serre sur l'extent de la copie". Indispensable pour un format
            // compresse, ou bufferRowLength se compterait en TEXELS et non en
            // blocs : le calculer a la main serait une occasion d'erreur
            // gratuite.
            region.bufferRowLength = 0;
            region.bufferImageHeight = 0;
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.mipLevel = band.level;
            region.imageSubresource.baseArrayLayer = 0;
            region.imageSubresource.layerCount = 1;
            region.imageOffset = { 0, static_cast<int32_t>(band.y), 0 };
            region.imageExtent = { data.mips[band.level].width, band.height, 1 };
            regions.push_back(region);

            used += band.bytes;
            ++next;
        }
        const bool last = (next == bands.size());

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(commandBuffer, &beginInfo);

        if (first) {
            transitionLayout(commandBuffer, image_, mipLevels,
                             VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             0, VK_ACCESS_TRANSFER_WRITE_BIT,
                             VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        }
        vkCmdCopyBufferToImage(commandBuffer, staging.handle(), image_,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               static_cast<uint32_t>(regions.size()), regions.data());
        if (last) {
            transitionLayout(commandBuffer, image_, mipLevels,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        }
        vkEndCommandBuffer(commandBuffer);

        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffer;
        vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
        // Transfert a l'initialisation : attendre est simple et sans
        // consequence sur la boucle de rendu, qui n'a pas encore demarre - et
        // c'est ce qui rend le staging reutilisable pour la tranche suivante.
        vkQueueWaitIdle(queue);
        first = false;
    }

    vkFreeCommandBuffers(device, commandPool, 1, &commandBuffer);
}

void Texture::createView(VkFormat format, uint32_t mipLevels) {
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image_;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = mipLevels;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    if (vkCreateImageView(context_.device(), &viewInfo, nullptr, &view_) != VK_SUCCESS) {
        throw std::runtime_error("Texture : echec de la creation de la vue d'image.");
    }
}

void Texture::createSampler(uint32_t mipLevels, bool repeat) {
    VkPhysicalDeviceProperties deviceProperties{};
    vkGetPhysicalDeviceProperties(context_.physicalDevice(), &deviceProperties);

    VkPhysicalDeviceFeatures features{};
    vkGetPhysicalDeviceFeatures(context_.physicalDevice(), &features);

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    // CLAMP_TO_EDGE et non REPEAT : la texture recouvre EXACTEMENT l'emprise du
    // terrain. En repetition, un sommet legerement hors bornes (l'ecretage de la
    // heightmap en produit) irait chercher la couleur du bord oppose, soit
    // plusieurs kilometres plus loin.
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (repeat) {
        // PHASE 27 : texture periodique (bruit de detail), echantillonnee bien
        // au-dela de [0, 1] - la repetition est ici le comportement voulu.
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }

    // L'anisotropie n'est demandee que si la feature a ete ACTIVEE sur le device
    // logique (voir VulkanContext::createLogicalDevice). La demander sans
    // l'avoir activee est une erreur de validation immediate.
    if (context_.anisotropyEnabled()) {
        samplerInfo.anisotropyEnable = VK_TRUE;
        samplerInfo.maxAnisotropy = deviceProperties.limits.maxSamplerAnisotropy;
    } else {
        samplerInfo.anisotropyEnable = VK_FALSE;
        samplerInfo.maxAnisotropy = 1.0f;
    }

    samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.mipLodBias = 0.0f;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = static_cast<float>(mipLevels);

    if (vkCreateSampler(context_.device(), &samplerInfo, nullptr, &sampler_) != VK_SUCCESS) {
        throw std::runtime_error("Texture : echec de la creation de l'echantillonneur.");
    }
}

} // namespace vercora
