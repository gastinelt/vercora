#pragma once

#include <vulkan/vulkan.h>

#include <string>
#include <vector>

namespace vercora {

// Paire de shaders a charger, designee par le NOM des fichiers .spv produits
// par CMake dans SHADERS_OUTPUT_DIR (et non par un chemin complet : le
// repertoire est une affaire de build, pas d'appelant).
struct ShaderSet {
    std::string vertexSpv;
    std::string fragmentSpv;

    // true si le vertex shader consomme les attributs de geomorphing
    // (locations 4 et 5, voir Vertex.hpp). Seul terrain.vert le fait.
    //
    // Ce drapeau n'est pas une optimisation mais une CORRECTION : declarer dans
    // le vertex input un attribut qu'aucun shader ne lit fait emettre aux
    // couches de validation "Vertex attribute at location 4 not consumed by
    // vertex shader", et le contrat du projet est zero message de validation. Le
    // format de sommet reste unique cote C++ ; c'est seulement sa DESCRIPTION au
    // pipeline qui est tronquee pour les materiaux qui n'en lisent qu'une partie
    // (ce que Vulkan autorise : un binding peut avoir un stride plus grand que
    // la somme des attributs decrits).
    bool usesMorphAttributes = false;

    // PHASE 11. true si le pipeline doit decrire le second binding de sommets,
    // cadence par instance (voir VertexFormat.hpp). Meme raisonnement que
    // usesMorphAttributes : c'est une CORRECTION, pas une optimisation. Decrire
    // un binding d'instances a un pipeline non instancie obligerait a lier un
    // buffer d'instances avant de dessiner un cube, et ne pas le decrire a un
    // pipeline instancie ferait echouer la validation sur les locations 6 a 8.
    bool usesInstancing = false;

    // PHASE 27 : passe PLEIN ECRAN. Aucune entree de sommet (le vertex shader
    // fabrique un triangle couvrant a partir de gl_VertexIndex), pas de test
    // ni d'ecriture de profondeur, melange par alpha.
    bool fullscreenOverlay = false;
};

// Shaders de la Phase 2 : couleur par sommet + Blinn-Phong. Utilises par le
// cube en dur et par les meshes .obj.
inline const ShaderSet kObjectShaders{ "cube.vert.spv", "cube.frag.spv" };

// Shaders de la Phase 4 : coloration par altitude et par pente + Blinn-Phong.
inline const ShaderSet kTerrainShaders{ "terrain.vert.spv", "terrain.frag.spv", true };

// Shaders de la Phase 11 : vegetation instanciee (arbres de decor et herbe).
// Ne lit PAS les attributs de geomorphing, mais lit le binding d'instances.
inline const ShaderSet kFoliageShaders{ "foliage.vert.spv", "foliage.frag.spv",
                                        false, true };

// PHASE 27 : pluie et neige a l'ecran. Voir shaders/precip.frag.
inline const ShaderSet kPrecipitationShaders{ "precip.vert.spv", "precip.frag.spv",
                                              false, false, true };

// Charge les shaders SPIR-V et construit le pipeline graphique.
// Viewport et scissor sont dynamiques : le pipeline n'a donc PAS besoin d'etre
// recree au resize (on les fixe dans le command buffer).
class Pipeline {
public:
    Pipeline(VkDevice device, VkRenderPass renderPass, VkDescriptorSetLayout descriptorSetLayout,
             const ShaderSet& shaders);
    ~Pipeline();

    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    VkPipeline       handle() const { return graphicsPipeline_; }
    VkPipelineLayout layout() const { return pipelineLayout_; }

private:
    VkShaderModule createShaderModule(const std::vector<char>& code) const;
    static std::vector<char> readFile(const std::string& path);

    VkDevice device_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline graphicsPipeline_ = VK_NULL_HANDLE;
};

} // namespace vercora
