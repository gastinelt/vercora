#pragma once

#include <vulkan/vulkan.h>

#include <optional>
#include <vector>

namespace vercora {

class Window;

// Indices des familles de queues dont on a besoin.
struct QueueFamilyIndices {
    std::optional<uint32_t> graphicsFamily;
    std::optional<uint32_t> presentFamily;

    // PHASE 12. Famille de transfert DEDIEE, c'est-a-dire une famille qui expose
    // VK_QUEUE_TRANSFER_BIT sans VK_QUEUE_GRAPHICS_BIT. Renseignee seulement si
    // le GPU en propose une : elle est OPTIONNELLE en Vulkan, et beaucoup de GPU
    // integres n'exposent qu'une seule famille universelle.
    //
    // Elle n'est PAS utilisee pour les transferts du streaming (voir la note
    // d'architecture dans NOTES-PHASE12.md et Renderer::enqueueCopy) : elle est
    // detectee et journalisee parce que c'est cette detection qui justifie le
    // choix fait, et parce qu'un futur portage sur GPU discret voudra la
    // relire. Utiliser une famille differente pour ecrire un buffer puis le lire
    // au rendu impose un TRANSFERT DE PROPRIETE de famille (barriere de release
    // puis d'acquire) : du code qu'on ne peut pas eprouver sur une machine qui
    // n'a qu'une famille, donc du code qu'on n'ecrit pas.
    std::optional<uint32_t> dedicatedTransferFamily;

    bool isComplete() const {
        return graphicsFamily.has_value() && presentFamily.has_value();
    }
};

// Capacites de la surface pour la construction de la swapchain.
struct SwapChainSupportDetails {
    VkSurfaceCapabilitiesKHR capabilities{};
    std::vector<VkSurfaceFormatKHR> formats;
    std::vector<VkPresentModeKHR> presentModes;
};

// Regroupe l'etat "global" Vulkan qui ne change pas au resize :
// instance, debug messenger, surface, GPU physique, device logique, queues.
class VulkanContext {
public:
    explicit VulkanContext(Window& window);
    ~VulkanContext();

    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;

    VkInstance       instance() const       { return instance_; }
    VkSurfaceKHR     surface() const         { return surface_; }
    VkPhysicalDevice physicalDevice() const  { return physicalDevice_; }
    VkDevice         device() const          { return device_; }
    VkQueue          graphicsQueue() const   { return graphicsQueue_; }
    VkQueue          presentQueue() const    { return presentQueue_; }

    const QueueFamilyIndices& queueFamilies() const { return queueIndices_; }

    // PHASE 11. true si samplerAnisotropy a ete ACTIVEE sur le device logique.
    // Un echantillonneur ne peut demander l'anisotropie que dans ce cas : la
    // demander sans l'avoir activee est une erreur de validation immediate. La
    // feature est optionnelle en Vulkan, donc le moteur s'en passe si le GPU ne
    // la propose pas (le terrain est alors plus flou en vue rasante, rien de
    // plus).
    bool anisotropyEnabled() const { return anisotropyEnabled_; }

    // (Re)interroge le support swapchain du GPU choisi. Appele au (re)build de
    // la swapchain, car les capacites (extent courant) peuvent avoir change.
    SwapChainSupportDetails querySwapChainSupport() const;

    // Trouve un type de memoire satisfaisant a la fois un filtre (bits issus des
    // memory requirements) et un jeu de proprietes (host-visible, device-local...).
    uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const;

    // Premier format de la liste supporte par le GPU pour un tiling/feature donne.
    VkFormat findSupportedFormat(const std::vector<VkFormat>& candidates,
                                 VkImageTiling tiling,
                                 VkFormatFeatureFlags features) const;

    // Format de profondeur retenu (D32 de preference, avec fallbacks).
    VkFormat findDepthFormat() const;

private:
    void createInstance();
    void setupDebugMessenger();
    void createSurface(Window& window);
    void pickPhysicalDevice();
    void createLogicalDevice();

    bool isDeviceSuitable(VkPhysicalDevice device) const;
    bool checkDeviceExtensionSupport(VkPhysicalDevice device) const;
    QueueFamilyIndices findQueueFamilies(VkPhysicalDevice device) const;
    SwapChainSupportDetails querySwapChainSupport(VkPhysicalDevice device) const;

    std::vector<const char*> getRequiredInstanceExtensions() const;
    bool checkValidationLayerSupport() const;

    VkInstance               instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger_ = VK_NULL_HANDLE;
    VkSurfaceKHR             surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice         physicalDevice_ = VK_NULL_HANDLE;
    VkDevice                 device_ = VK_NULL_HANDLE;
    VkQueue                  graphicsQueue_ = VK_NULL_HANDLE;
    VkQueue                  presentQueue_ = VK_NULL_HANDLE;

    QueueFamilyIndices queueIndices_;

    bool anisotropyEnabled_ = false;
};

} // namespace vercora
