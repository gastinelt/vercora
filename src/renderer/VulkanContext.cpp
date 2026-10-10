#include "renderer/VulkanContext.hpp"

#include "core/Logger.hpp"
#include "core/Window.hpp"

#include <cstring>
#include <set>
#include <stdexcept>
#include <string>

namespace vercora {

namespace {

// Validation layers actives uniquement en Debug (voir VERCORA_DEBUG).
#ifdef VERCORA_DEBUG
constexpr bool kEnableValidationLayers = true;
#else
constexpr bool kEnableValidationLayers = false;
#endif

const std::vector<const char*> kValidationLayers = {
    "VK_LAYER_KHRONOS_validation"
};

const std::vector<const char*> kDeviceExtensions = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME
};

// Callback appele par les validation layers. Route les messages vers notre logger.
VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT /*type*/,
    const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
    void* /*userData*/) {

    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        logError(std::string("[Validation] ") + callbackData->pMessage);
    } else if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        logWarn(std::string("[Validation] ") + callbackData->pMessage);
    } else {
        logInfo(std::string("[Validation] ") + callbackData->pMessage);
    }
    return VK_FALSE;
}

// vkCreate/DestroyDebugUtilsMessengerEXT ne sont pas charges statiquement :
// il faut passer par vkGetInstanceProcAddr.
VkResult createDebugUtilsMessengerEXT(
    VkInstance instance,
    const VkDebugUtilsMessengerCreateInfoEXT* createInfo,
    const VkAllocationCallbacks* allocator,
    VkDebugUtilsMessengerEXT* messenger) {
    auto func = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
    if (func != nullptr) {
        return func(instance, createInfo, allocator, messenger);
    }
    return VK_ERROR_EXTENSION_NOT_PRESENT;
}

void destroyDebugUtilsMessengerEXT(
    VkInstance instance,
    VkDebugUtilsMessengerEXT messenger,
    const VkAllocationCallbacks* allocator) {
    auto func = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
    if (func != nullptr) {
        func(instance, messenger, allocator);
    }
}

void populateDebugMessengerCreateInfo(VkDebugUtilsMessengerCreateInfoEXT& info) {
    info = {};
    info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    info.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    info.messageType =
        VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    info.pfnUserCallback = debugCallback;
}

} // namespace

VulkanContext::VulkanContext(Window& window) {
    createInstance();
    setupDebugMessenger();
    createSurface(window);
    pickPhysicalDevice();
    createLogicalDevice();
}

VulkanContext::~VulkanContext() {
    if (device_ != VK_NULL_HANDLE) {
        vkDestroyDevice(device_, nullptr);
    }
    if (surface_ != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
    }
    if (kEnableValidationLayers && debugMessenger_ != VK_NULL_HANDLE) {
        destroyDebugUtilsMessengerEXT(instance_, debugMessenger_, nullptr);
    }
    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
    }
}

// ---------------------------------------------------------------------------
// Instance
// ---------------------------------------------------------------------------
void VulkanContext::createInstance() {
    if (kEnableValidationLayers && !checkValidationLayerSupport()) {
        throw std::runtime_error("Validation layers demandees mais indisponibles.");
    }

    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "Vercora";
    appInfo.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    appInfo.pEngineName = "Vercora Engine";
    appInfo.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    appInfo.apiVersion = VK_API_VERSION_1_2;

    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;

    const auto extensions = getRequiredInstanceExtensions();
    createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();

    // Ce debug messenger "temporaire" couvre la creation/destruction de l'instance
    // elle-meme (avant/apres le messenger principal).
    VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo{};
    if (kEnableValidationLayers) {
        createInfo.enabledLayerCount = static_cast<uint32_t>(kValidationLayers.size());
        createInfo.ppEnabledLayerNames = kValidationLayers.data();
        populateDebugMessengerCreateInfo(debugCreateInfo);
        createInfo.pNext = &debugCreateInfo;
    } else {
        createInfo.enabledLayerCount = 0;
    }

    if (vkCreateInstance(&createInfo, nullptr, &instance_) != VK_SUCCESS) {
        throw std::runtime_error("Echec de la creation de l'instance Vulkan.");
    }
    logInfo("Instance Vulkan creee.");
}

std::vector<const char*> VulkanContext::getRequiredInstanceExtensions() const {
    // GLFW indique les extensions necessaires au fenetrage (surface + plateforme).
    uint32_t glfwExtensionCount = 0;
    const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);

    std::vector<const char*> extensions(glfwExtensions, glfwExtensions + glfwExtensionCount);

    if (kEnableValidationLayers) {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }
    return extensions;
}

bool VulkanContext::checkValidationLayerSupport() const {
    uint32_t layerCount = 0;
    vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
    std::vector<VkLayerProperties> available(layerCount);
    vkEnumerateInstanceLayerProperties(&layerCount, available.data());

    for (const char* wanted : kValidationLayers) {
        bool found = false;
        for (const auto& props : available) {
            if (std::strcmp(wanted, props.layerName) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Debug messenger
// ---------------------------------------------------------------------------
void VulkanContext::setupDebugMessenger() {
    if (!kEnableValidationLayers) {
        return;
    }
    VkDebugUtilsMessengerCreateInfoEXT createInfo{};
    populateDebugMessengerCreateInfo(createInfo);
    if (createDebugUtilsMessengerEXT(instance_, &createInfo, nullptr, &debugMessenger_) != VK_SUCCESS) {
        throw std::runtime_error("Echec de la mise en place du debug messenger.");
    }
    logInfo("Validation layers activees.");
}

// ---------------------------------------------------------------------------
// Surface
// ---------------------------------------------------------------------------
void VulkanContext::createSurface(Window& window) {
    surface_ = window.createSurface(instance_);
}

// ---------------------------------------------------------------------------
// Selection du GPU physique
// ---------------------------------------------------------------------------
void VulkanContext::pickPhysicalDevice() {
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(instance_, &deviceCount, nullptr);
    if (deviceCount == 0) {
        throw std::runtime_error("Aucun GPU compatible Vulkan trouve.");
    }
    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(instance_, &deviceCount, devices.data());

    for (const auto& device : devices) {
        if (isDeviceSuitable(device)) {
            physicalDevice_ = device;
            queueIndices_ = findQueueFamilies(device);
            break;
        }
    }

    if (physicalDevice_ == VK_NULL_HANDLE) {
        throw std::runtime_error("Aucun GPU ne remplit les criteres requis.");
    }

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physicalDevice_, &props);
    logInfo(std::string("GPU selectionne : ") + props.deviceName);
}

bool VulkanContext::isDeviceSuitable(VkPhysicalDevice device) const {
    const QueueFamilyIndices indices = findQueueFamilies(device);
    const bool extensionsSupported = checkDeviceExtensionSupport(device);

    bool swapChainAdequate = false;
    if (extensionsSupported) {
        const SwapChainSupportDetails support = querySwapChainSupport(device);
        swapChainAdequate = !support.formats.empty() && !support.presentModes.empty();
    }

    return indices.isComplete() && extensionsSupported && swapChainAdequate;
}

bool VulkanContext::checkDeviceExtensionSupport(VkPhysicalDevice device) const {
    uint32_t extensionCount = 0;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr);
    std::vector<VkExtensionProperties> available(extensionCount);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, available.data());

    std::set<std::string> required(kDeviceExtensions.begin(), kDeviceExtensions.end());
    for (const auto& ext : available) {
        required.erase(ext.extensionName);
    }
    return required.empty();
}

QueueFamilyIndices VulkanContext::findQueueFamilies(VkPhysicalDevice device) const {
    QueueFamilyIndices indices;

    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, families.data());

    // PHASE 12 : le balayage va desormais jusqu'au bout (et non plus jusqu'a
    // isComplete()), parce qu'une famille de transfert dediee est generalement
    // DERRIERE la famille graphique dans la liste. Il y a moins d'une dizaine de
    // familles sur n'importe quel GPU : le cout est nul et c'est fait une fois.
    for (uint32_t i = 0; i < queueFamilyCount; ++i) {
        const VkQueueFlags flags = families[i].queueFlags;

        if ((flags & VK_QUEUE_GRAPHICS_BIT) && !indices.graphicsFamily.has_value()) {
            indices.graphicsFamily = i;
        }
        VkBool32 presentSupport = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface_, &presentSupport);
        if (presentSupport == VK_TRUE && !indices.presentFamily.has_value()) {
            indices.presentFamily = i;
        }
        // Dediee = sait transferer, ne sait PAS faire de graphique. Le bit
        // TRANSFER est implicite pour une famille graphique ou compute, d'ou le
        // test sur les deux autres bits plutot que sur le seul bit TRANSFER.
        if ((flags & VK_QUEUE_TRANSFER_BIT) && !(flags & VK_QUEUE_GRAPHICS_BIT) &&
            !indices.dedicatedTransferFamily.has_value()) {
            indices.dedicatedTransferFamily = i;
        }
    }
    return indices;
}

// ---------------------------------------------------------------------------
// Device logique + queues
// ---------------------------------------------------------------------------
void VulkanContext::createLogicalDevice() {
    // graphics et present peuvent pointer sur la meme famille -> set pour dedup.
    std::set<uint32_t> uniqueFamilies = {
        queueIndices_.graphicsFamily.value(),
        queueIndices_.presentFamily.value()
    };

    std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
    const float queuePriority = 1.0f;
    for (uint32_t family : uniqueFamilies) {
        VkDeviceQueueCreateInfo queueInfo{};
        queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueInfo.queueFamilyIndex = family;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &queuePriority;
        queueCreateInfos.push_back(queueInfo);
    }

    // PHASE 11 : samplerAnisotropy pour l'orthophoto. Le terrain est un plan
    // regarde en rasant depuis la hauteur d'oeil - le cas ou un filtrage
    // trilineaire seul rend une bouillie au-dela de quelques dizaines de metres.
    // La feature est OPTIONNELLE en Vulkan : on l'active si le GPU la propose et
    // on s'en passe sinon, plutot que d'en faire un critere de selection du GPU.
    // La demander sans qu'elle soit supportee ferait echouer la creation du
    // device, et l'utiliser sans l'avoir activee est une erreur de validation.
    VkPhysicalDeviceFeatures supported{};
    vkGetPhysicalDeviceFeatures(physicalDevice_, &supported);

    VkPhysicalDeviceFeatures deviceFeatures{};
    anisotropyEnabled_ = (supported.samplerAnisotropy == VK_TRUE);
    deviceFeatures.samplerAnisotropy = anisotropyEnabled_ ? VK_TRUE : VK_FALSE;
    if (!anisotropyEnabled_) {
        logWarn("Ce GPU ne propose pas samplerAnisotropy : le terrain texture "
                "sera plus flou en vue rasante.");
    }

    VkDeviceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
    createInfo.pQueueCreateInfos = queueCreateInfos.data();
    createInfo.pEnabledFeatures = &deviceFeatures;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(kDeviceExtensions.size());
    createInfo.ppEnabledExtensionNames = kDeviceExtensions.data();

    // Les layers de device sont depreciees depuis Vulkan 1.0 : les validation
    // layers recentes exigent enabledLayerCount == 0. Seules les layers d'instance
    // comptent (voir createInstance). On laisse donc ce champ a 0.
    createInfo.enabledLayerCount = 0;

    if (vkCreateDevice(physicalDevice_, &createInfo, nullptr, &device_) != VK_SUCCESS) {
        throw std::runtime_error("Echec de la creation du device logique.");
    }

    vkGetDeviceQueue(device_, queueIndices_.graphicsFamily.value(), 0, &graphicsQueue_);
    vkGetDeviceQueue(device_, queueIndices_.presentFamily.value(), 0, &presentQueue_);
    logInfo("Device logique et queues cree(e)s.");

    // PHASE 12 : inventaire des familles de queues. Journalise parce que c'est
    // l'information qui DECIDE de l'architecture de transfert (voir
    // QueueFamilyIndices::dedicatedTransferFamily et NOTES-PHASE12.md) : sans
    // cette ligne, relire le choix six mois plus tard obligerait a le refaire.
    {
        uint32_t count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &count, families.data());

        std::string text = "Familles de queues : " + std::to_string(count) + " -";
        for (uint32_t i = 0; i < count; ++i) {
            text += " [" + std::to_string(i) + "] x" +
                    std::to_string(families[i].queueCount);
            const VkQueueFlags flags = families[i].queueFlags;
            if (flags & VK_QUEUE_GRAPHICS_BIT)       text += " graphics";
            if (flags & VK_QUEUE_COMPUTE_BIT)        text += " compute";
            if (flags & VK_QUEUE_TRANSFER_BIT)       text += " transfer";
            if (flags & VK_QUEUE_SPARSE_BINDING_BIT) text += " sparse";
        }
        logInfo(text);

        if (queueIndices_.dedicatedTransferFamily.has_value()) {
            logInfo("Famille de transfert dediee disponible (indice " +
                    std::to_string(queueIndices_.dedicatedTransferFamily.value()) +
                    "), NON utilisee : les transferts de tuiles sont enregistres "
                    "dans le command buffer de la frame (voir NOTES-PHASE12.md).");
        } else {
            logInfo("Aucune famille de transfert dediee sur ce GPU : les "
                    "transferts partagent forcement la queue graphique.");
        }
    }
}

// ---------------------------------------------------------------------------
// Support swapchain
// ---------------------------------------------------------------------------
SwapChainSupportDetails VulkanContext::querySwapChainSupport() const {
    return querySwapChainSupport(physicalDevice_);
}

uint32_t VulkanContext::findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const {
    VkPhysicalDeviceMemoryProperties memProperties{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &memProperties);

    for (uint32_t i = 0; i < memProperties.memoryTypeCount; ++i) {
        const bool typeOk = (typeFilter & (1u << i)) != 0;
        const bool propsOk =
            (memProperties.memoryTypes[i].propertyFlags & properties) == properties;
        if (typeOk && propsOk) {
            return i;
        }
    }
    throw std::runtime_error("Aucun type de memoire adapte trouve.");
}

VkFormat VulkanContext::findSupportedFormat(const std::vector<VkFormat>& candidates,
                                            VkImageTiling tiling,
                                            VkFormatFeatureFlags features) const {
    for (VkFormat format : candidates) {
        VkFormatProperties props{};
        vkGetPhysicalDeviceFormatProperties(physicalDevice_, format, &props);

        if (tiling == VK_IMAGE_TILING_LINEAR &&
            (props.linearTilingFeatures & features) == features) {
            return format;
        }
        if (tiling == VK_IMAGE_TILING_OPTIMAL &&
            (props.optimalTilingFeatures & features) == features) {
            return format;
        }
    }
    throw std::runtime_error("Aucun format supporte trouve parmi les candidats.");
}

VkFormat VulkanContext::findDepthFormat() const {
    return findSupportedFormat(
        { VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT },
        VK_IMAGE_TILING_OPTIMAL,
        VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT);
}

SwapChainSupportDetails VulkanContext::querySwapChainSupport(VkPhysicalDevice device) const {
    SwapChainSupportDetails details;

    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface_, &details.capabilities);

    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface_, &formatCount, nullptr);
    if (formatCount != 0) {
        details.formats.resize(formatCount);
        vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface_, &formatCount, details.formats.data());
    }

    uint32_t presentModeCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface_, &presentModeCount, nullptr);
    if (presentModeCount != 0) {
        details.presentModes.resize(presentModeCount);
        vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface_, &presentModeCount, details.presentModes.data());
    }

    return details;
}

} // namespace vercora
