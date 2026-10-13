#include "renderer/SwapChain.hpp"

#include "core/Logger.hpp"
#include "core/Window.hpp"
#include "renderer/VulkanContext.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace vercora {
namespace {

// Nom lisible d'un present mode, pour le log de demarrage.
const char* presentModeName(VkPresentModeKHR mode) {
    switch (mode) {
        case VK_PRESENT_MODE_IMMEDIATE_KHR:    return "IMMEDIATE (V-Sync off, tearing possible)";
        case VK_PRESENT_MODE_MAILBOX_KHR:      return "MAILBOX (triple buffering, V-Sync on)";
        case VK_PRESENT_MODE_FIFO_KHR:         return "FIFO (V-Sync on, FPS plafonnes)";
        case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "FIFO_RELAXED";
        default:                               return "inconnu";
    }
}

} // namespace

SwapChain::SwapChain(VulkanContext& context, Window& window)
    : context_(context), window_(window) {
    // Le format de profondeur est fixe une fois pour toutes (il ne depend pas de
    // la taille) : le render pass, cree une seule fois, s'y refere.
    depthFormat_ = context_.findDepthFormat();

    createSwapChain();
    createImageViews();
    createRenderPass();
    createDepthResources();
    createFramebuffers();
}

SwapChain::~SwapChain() {
    cleanupSizeDependent();
    if (renderPass_ != VK_NULL_HANDLE) {
        vkDestroyRenderPass(context_.device(), renderPass_, nullptr);
    }
}

void SwapChain::recreate() {
    cleanupSizeDependent();
    createSwapChain();
    createImageViews();
    createDepthResources();
    createFramebuffers();
}

void SwapChain::cleanupSizeDependent() {
    VkDevice device = context_.device();

    // Depth buffer (dependant de la taille).
    if (depthImageView_ != VK_NULL_HANDLE) {
        vkDestroyImageView(device, depthImageView_, nullptr);
        depthImageView_ = VK_NULL_HANDLE;
    }
    if (depthImage_ != VK_NULL_HANDLE) {
        vkDestroyImage(device, depthImage_, nullptr);
        depthImage_ = VK_NULL_HANDLE;
    }
    context_.freeMemory(depthAllocation_);

    for (VkFramebuffer fb : framebuffers_) {
        vkDestroyFramebuffer(device, fb, nullptr);
    }
    framebuffers_.clear();

    for (VkImageView view : imageViews_) {
        vkDestroyImageView(device, view, nullptr);
    }
    imageViews_.clear();

    if (swapChain_ != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device, swapChain_, nullptr);
        swapChain_ = VK_NULL_HANDLE;
    }
    images_.clear();
}

// ---------------------------------------------------------------------------
// Choix des parametres de la swapchain
// ---------------------------------------------------------------------------
VkSurfaceFormatKHR SwapChain::chooseSurfaceFormat(
    const std::vector<VkSurfaceFormatKHR>& formats) const {
    for (const auto& format : formats) {
        if (format.format == VK_FORMAT_B8G8R8A8_SRGB &&
            format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return format;
        }
    }
    return formats.front();
}

VkPresentModeKHR SwapChain::choosePresentMode(
    const std::vector<VkPresentModeKHR>& modes) const {
    // Phase 2 : on veut mesurer les FPS reels du moteur, donc pas de V-Sync.
    // Ordre de preference :
    //   IMMEDIATE : presente sans attendre le rafraichissement ecran. FPS non
    //               plafonnes (ce qu'on veut ici), au prix d'un tearing possible.
    //   MAILBOX   : triple buffering, faible latence mais toujours cale sur
    //               l'ecran -> FPS plafonnes au taux de rafraichissement.
    //   FIFO      : seul mode dont Vulkan garantit la presence. Fallback ultime.
    // A rebasculer sur MAILBOX/FIFO quand le jeu sera jouable : le tearing n'est
    // acceptable que pour du profilage.
    for (VkPresentModeKHR wanted : { VK_PRESENT_MODE_IMMEDIATE_KHR,
                                     VK_PRESENT_MODE_MAILBOX_KHR }) {
        if (std::find(modes.begin(), modes.end(), wanted) != modes.end()) {
            return wanted;
        }
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

VkExtent2D SwapChain::chooseExtent(const VkSurfaceCapabilitiesKHR& capabilities) const {
    if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
        return capabilities.currentExtent;
    }
    int width = 0;
    int height = 0;
    window_.getFramebufferSize(width, height);

    VkExtent2D actual{ static_cast<uint32_t>(width), static_cast<uint32_t>(height) };
    actual.width = std::clamp(actual.width,
        capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
    actual.height = std::clamp(actual.height,
        capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
    return actual;
}

// ---------------------------------------------------------------------------
// Swapchain
// ---------------------------------------------------------------------------
void SwapChain::createSwapChain() {
    const SwapChainSupportDetails support = context_.querySwapChainSupport();

    const VkSurfaceFormatKHR surfaceFormat = chooseSurfaceFormat(support.formats);
    const VkPresentModeKHR presentMode = choosePresentMode(support.presentModes);
    const VkExtent2D extent = chooseExtent(support.capabilities);

    // createSwapChain() est aussi appelee a chaque resize : on ne log que si le
    // mode change reellement, pour ne pas spammer la console pendant un drag.
    if (presentMode != presentMode_) {
        logInfo(std::string("Present mode retenu : ") + presentModeName(presentMode));
        presentMode_ = presentMode;
    }

    // On demande une image de plus que le minimum pour eviter d'attendre le driver.
    uint32_t imageCount = support.capabilities.minImageCount + 1;
    if (support.capabilities.maxImageCount > 0 &&
        imageCount > support.capabilities.maxImageCount) {
        imageCount = support.capabilities.maxImageCount;
    }

    VkSwapchainCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    createInfo.surface = context_.surface();
    createInfo.minImageCount = imageCount;
    createInfo.imageFormat = surfaceFormat.format;
    createInfo.imageColorSpace = surfaceFormat.colorSpace;
    createInfo.imageExtent = extent;
    createInfo.imageArrayLayers = 1;
    createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    const QueueFamilyIndices& indices = context_.queueFamilies();
    const uint32_t queueFamilyIndices[] = {
        indices.graphicsFamily.value(),
        indices.presentFamily.value()
    };

    if (indices.graphicsFamily != indices.presentFamily) {
        // Familles distinctes : partage concurrent (plus simple, un peu moins rapide).
        createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        createInfo.queueFamilyIndexCount = 2;
        createInfo.pQueueFamilyIndices = queueFamilyIndices;
    } else {
        createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    createInfo.preTransform = support.capabilities.currentTransform;
    createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    createInfo.presentMode = presentMode;
    createInfo.clipped = VK_TRUE;
    createInfo.oldSwapchain = VK_NULL_HANDLE;

    if (vkCreateSwapchainKHR(context_.device(), &createInfo, nullptr, &swapChain_) != VK_SUCCESS) {
        throw std::runtime_error("Echec de la creation de la swapchain.");
    }

    // Recupere les images effectivement creees (peut differer de imageCount demande).
    vkGetSwapchainImagesKHR(context_.device(), swapChain_, &imageCount, nullptr);
    images_.resize(imageCount);
    vkGetSwapchainImagesKHR(context_.device(), swapChain_, &imageCount, images_.data());

    imageFormat_ = surfaceFormat.format;
    extent_ = extent;
}

void SwapChain::createImageViews() {
    imageViews_.resize(images_.size());
    for (size_t i = 0; i < images_.size(); ++i) {
        VkImageViewCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        createInfo.image = images_[i];
        createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        createInfo.format = imageFormat_;
        createInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        createInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        createInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        createInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
        createInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        createInfo.subresourceRange.baseMipLevel = 0;
        createInfo.subresourceRange.levelCount = 1;
        createInfo.subresourceRange.baseArrayLayer = 0;
        createInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(context_.device(), &createInfo, nullptr, &imageViews_[i]) != VK_SUCCESS) {
            throw std::runtime_error("Echec de la creation d'une image view.");
        }
    }
}

void SwapChain::createRenderPass() {
    // Attache 0 : couleur.
    VkAttachmentDescription colorAttachment{};
    colorAttachment.format = imageFormat_;
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;   // efface a chaque frame
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE; // conserve pour la presentation
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference colorRef{};
    colorRef.attachment = 0;
    colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    // Attache 1 : profondeur. Pas besoin de la conserver (storeOp DONT_CARE).
    VkAttachmentDescription depthAttachment{};
    depthAttachment.format = depthFormat_;
    depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference depthRef{};
    depthRef.attachment = 1;
    depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    subpass.pDepthStencilAttachment = &depthRef;

    // Dependance : synchronise couleur ET profondeur avec l'exterieur du subpass.
    //
    // PHASE 12, CORRECTION. srcAccessMask valait 0, ce qui n'ordonne QUE
    // l'execution, pas les ecritures entre elles. Or il n'y a qu'UN SEUL buffer
    // de profondeur pour les deux frames-in-flight : le clear de la frame N+1
    // (une ecriture, au vkCmdBeginRenderPass) court alors sans dependance
    // contre le storeOp de la frame N (une ecriture aussi). La validation de
    // SYNCHRONISATION - qui n'etait activee dans aucune phase precedente -
    // signale exactement ca : "WRITE_AFTER_WRITE hazard, vkCmdBeginRenderPass
    // ... previously written ... by the attachment storeOp".
    //
    // Le defaut est ANTERIEUR au transfert asynchrone : il se reproduit sur la
    // scene `cube` et avec --no-lod, donc sans le moindre upload en vol. Il est
    // corrige ici parce qu'un message d'erreur permanent dans le journal de
    // synchronisation rendrait invisible un VRAI probleme de transfert.
    //
    // La forme correcte nomme les ecritures de la frame precedente cote source.
    // L'ecriture du storeOp de profondeur a lieu au stage LATE_FRAGMENT_TESTS,
    // celle de la couleur au stage COLOR_ATTACHMENT_OUTPUT.
    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependency.srcAccessMask =
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependency.dstStageMask =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.dstAccessMask =
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    const std::array<VkAttachmentDescription, 2> attachments = { colorAttachment, depthAttachment };

    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
    renderPassInfo.pAttachments = attachments.data();
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = 1;
    renderPassInfo.pDependencies = &dependency;

    if (vkCreateRenderPass(context_.device(), &renderPassInfo, nullptr, &renderPass_) != VK_SUCCESS) {
        throw std::runtime_error("Echec de la creation du render pass.");
    }
}

void SwapChain::createDepthResources() {
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = extent_.width;
    imageInfo.extent.height = extent_.height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = depthFormat_;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkDevice device = context_.device();
    if (vkCreateImage(device, &imageInfo, nullptr, &depthImage_) != VK_SUCCESS) {
        throw std::runtime_error("Echec de la creation de l'image de profondeur.");
    }

    VkMemoryRequirements memRequirements{};
    vkGetImageMemoryRequirements(device, depthImage_, &memRequirements);

    // PHASE 30 : par le contexte, qui tient le compte de la memoire GPU.
    depthAllocation_ = context_.allocateMemory(memRequirements,
                                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkBindImageMemory(device, depthImage_, depthAllocation_.memory, 0);

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = depthImage_;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = depthFormat_;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    if (vkCreateImageView(device, &viewInfo, nullptr, &depthImageView_) != VK_SUCCESS) {
        throw std::runtime_error("Echec de la creation de la vue de profondeur.");
    }
}

void SwapChain::createFramebuffers() {
    framebuffers_.resize(imageViews_.size());
    for (size_t i = 0; i < imageViews_.size(); ++i) {
        // Ordre = ordre des attaches du render pass : couleur puis profondeur.
        std::array<VkImageView, 2> attachments = { imageViews_[i], depthImageView_ };

        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = renderPass_;
        framebufferInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
        framebufferInfo.pAttachments = attachments.data();
        framebufferInfo.width = extent_.width;
        framebufferInfo.height = extent_.height;
        framebufferInfo.layers = 1;

        if (vkCreateFramebuffer(context_.device(), &framebufferInfo, nullptr, &framebuffers_[i]) != VK_SUCCESS) {
            throw std::runtime_error("Echec de la creation d'un framebuffer.");
        }
    }
}

} // namespace vercora
