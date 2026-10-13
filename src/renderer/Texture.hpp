#pragma once

// TextureData est inclus (et non declare en avant) parce que whitePixelData()
// le renvoie par valeur. L'en-tete reste sans cout : assets/TextureFile.hpp ne
// depend pas de Vulkan.
#include "assets/TextureFile.hpp"
#include "renderer/VulkanContext.hpp" // GpuAllocation

#include <vulkan/vulkan.h>

#include <cstdint>

namespace vercora {

class VulkanContext;

// Une texture 2D sur le GPU : image, memoire, vue et echantillonneur, avec sa
// chaine de mipmaps DEJA CALCULEE cote CPU.
//
// PHASE 11. Premiere texture du moteur : jusqu'ici tout etait colore par
// sommet ou calcule dans le fragment shader.
//
// POURQUOI LES MIPMAPS NE SONT PAS GENEREES ICI. Le chemin classique
// (vkCmdBlitImage en cascade depuis le niveau 0) ne fonctionne QUE sur un format
// non compresse : on ne sait pas filtrer des blocs BC1. Comme la texture
// d'orthophoto est compressee - c'est ce qui la fait tenir en 45 Mo au lieu de
// 357 - les niveaux arrivent tout faits dans le .vcto et cette classe se contente
// d'un vkCmdCopyBufferToImage par niveau. Le code est plus court que la version
// qui blitte, pas plus long.
//
// L'ANISOTROPIE compte particulierement ici : le terrain est un plan regarde en
// rasant depuis la hauteur d'oeil, exactement le cas ou un filtrage trilineaire
// seul rend une bouillie au-dela de quelques dizaines de metres. Elle est
// demandee au maximum permis par le GPU, et silencieusement abandonnee si la
// feature n'est pas disponible (le rendu reste correct, seulement plus flou de
// loin).
class Texture {
public:
    // Envoie `data` sur le GPU. Lance std::runtime_error si le format n'est pas
    // supporte par le GPU (BC1 sur un materiel sans textureCompressionBC) ou si
    // une ressource Vulkan ne peut pas etre creee.
    // repeat : adressage en REPETITION (PHASE 27, texture de bruit de detail,
    // qui se raccorde bord a bord). Par defaut CLAMP_TO_EDGE, le bon choix pour
    // l'orthophoto - voir createSampler.
    Texture(VulkanContext& context, VkCommandPool commandPool, VkQueue queue,
            const TextureData& data, bool repeat = false);
    ~Texture();

    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;

    VkImageView view() const    { return view_; }
    VkSampler   sampler() const { return sampler_; }

    // Octets reellement alloues sur le GPU pour l'image (mipmaps comprises).
    VkDeviceSize deviceSize() const { return deviceSize_; }

    // Donnees d'une texture 1x1 blanche opaque, a passer au constructeur.
    //
    // Le descriptor set du Renderer declare toujours son binding de texture ; le
    // laisser non ecrit serait une erreur de validation des qu'un shader du set
    // y accede. Plutot que de multiplier les layouts et les pipelines selon
    // qu'une orthophoto est chargee ou non, on relie toujours QUELQUE CHOSE, et
    // c'est un facteur neutre (blanc) quand il n'y a pas de photo. Renvoyer des
    // donnees CPU plutot qu'une Texture construite evite d'avoir a rendre cette
    // classe deplacable pour un seul cas d'usage.
    static TextureData whitePixelData();

private:
    void createImage(uint32_t width, uint32_t height, uint32_t mipLevels, VkFormat format);
    void uploadAndTransition(VkCommandPool commandPool, VkQueue queue,
                             const TextureData& data);
    void createView(VkFormat format, uint32_t mipLevels);
    void createSampler(uint32_t mipLevels, bool repeat);

    VulkanContext& context_;
    VkImage        image_ = VK_NULL_HANDLE;
    GpuAllocation  allocation_;
    VkImageView    view_ = VK_NULL_HANDLE;
    VkSampler      sampler_ = VK_NULL_HANDLE;
    VkDeviceSize   deviceSize_ = 0;
};

} // namespace vercora
