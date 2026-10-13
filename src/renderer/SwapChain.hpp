#pragma once

#include "renderer/VulkanContext.hpp" // GpuAllocation

#include <vulkan/vulkan.h>

#include <vector>

namespace vercora {

class VulkanContext;
class Window;

// Encapsule la swapchain et tout ce qui en depend directement :
// image views, render pass et framebuffers. Concue pour etre recreee au resize.
// Le render pass, lui, est conserve entre deux recreations (son format ne change
// pas), ce qui evite d'invalider le pipeline graphique.
class SwapChain {
public:
    SwapChain(VulkanContext& context, Window& window);
    ~SwapChain();

    SwapChain(const SwapChain&) = delete;
    SwapChain& operator=(const SwapChain&) = delete;

    // Detruit les ressources dependant de la taille et les reconstruit.
    void recreate();

    VkRenderPass  renderPass() const  { return renderPass_; }
    VkExtent2D    extent() const      { return extent_; }
    VkFormat      imageFormat() const { return imageFormat_; }
    VkFormat      depthFormat() const { return depthFormat_; }
    VkSwapchainKHR handle() const     { return swapChain_; }

    // Mode de presentation effectivement retenu (depend du support GPU/driver).
    VkPresentModeKHR presentMode() const { return presentMode_; }

    uint32_t imageCount() const { return static_cast<uint32_t>(images_.size()); }
    VkFramebuffer framebuffer(uint32_t index) const { return framebuffers_[index]; }

private:
    void createSwapChain();
    void createImageViews();
    void createRenderPass();
    void createDepthResources();
    void createFramebuffers();

    // Detruit uniquement les ressources dependant de la taille (pas le render pass).
    void cleanupSizeDependent();

    VkSurfaceFormatKHR chooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats) const;
    VkPresentModeKHR   choosePresentMode(const std::vector<VkPresentModeKHR>& modes) const;
    VkExtent2D         chooseExtent(const VkSurfaceCapabilitiesKHR& capabilities) const;

    VulkanContext& context_;
    Window& window_;

    VkSwapchainKHR swapChain_ = VK_NULL_HANDLE;
    VkFormat       imageFormat_ = VK_FORMAT_UNDEFINED;
    VkExtent2D     extent_{};

    // MAX_ENUM = "pas encore choisi", pour que le premier choix soit toujours logge.
    VkPresentModeKHR presentMode_ = VK_PRESENT_MODE_MAX_ENUM_KHR;

    std::vector<VkImage>       images_;
    std::vector<VkImageView>   imageViews_;
    std::vector<VkFramebuffer> framebuffers_;

    // Depth buffer : une seule image partagee (le rendu d'une frame est serialise
    // par le render pass). Recree en meme temps que la swapchain au resize.
    VkFormat       depthFormat_ = VK_FORMAT_UNDEFINED;
    VkImage        depthImage_ = VK_NULL_HANDLE;
    GpuAllocation  depthAllocation_;
    VkImageView    depthImageView_ = VK_NULL_HANDLE;

    VkRenderPass renderPass_ = VK_NULL_HANDLE;
};

} // namespace vercora
