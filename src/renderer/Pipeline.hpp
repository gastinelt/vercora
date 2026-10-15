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

    // true si le vertex shader lit le format de sommet du TERRAIN
    // (TerrainVertex, 16 octets) ; false pour le format des objets et de la
    // vegetation (ObjectVertex, 44 octets). Voir renderer/GpuVertex.hpp.
    //
    // PHASE 32 : jusqu'ici ce drapeau (usesMorphAttributes) ne faisait que
    // TRONQUER la description d'un format unique de 68 octets, pour ne pas
    // declarer aux shaders d'objets les attributs de morphing qu'ils ne lisent
    // pas (zero message de validation). Les deux familles ont desormais chacune
    // leur format, et les octets inutiles ont quitte les buffers eux-memes.
    bool terrainVertexLayout = false;

    // PHASE 11. true si le pipeline doit decrire le second binding de sommets,
    // cadence par instance (voir VertexFormat.hpp). C'est une CORRECTION, pas
    // une optimisation (zero message de validation). Decrire
    // un binding d'instances a un pipeline non instancie obligerait a lier un
    // buffer d'instances avant de dessiner un cube, et ne pas le decrire a un
    // pipeline instancie ferait echouer la validation sur les locations 6 a 8.
    bool usesInstancing = false;

    // PHASE 27 : passe PLEIN ECRAN. Aucune entree de sommet (le vertex shader
    // fabrique un triangle couvrant a partir de gl_VertexIndex), pas de test
    // ni d'ecriture de profondeur, melange par alpha.
    bool fullscreenOverlay = false;

    // PHASE 32 : elimine les faces ARRIERE avant le tramage. Reserve au
    // terrain, et c'est une propriete de la geometrie, pas un reglage : un
    // champ de hauteurs a toutes ses normales geometriques vers le haut, et un
    // rayon parti d'un oeil situe AU-DESSUS du relief rencontre toujours une
    // face avant en premier - une face arriere n'est donc jamais le pixel
    // visible, elle ne fait que couter du tramage et des tests de profondeur.
    // (Image identique au bit pres, verifiee - NOTES-PHASE32.md.) Les objets
    // et la vegetation ne s'y pretent pas : l'herbe est un quad vu des deux
    // cotes, et rien ne garantit l'enroulement des maillages .obj.
    //
    // MESURE (lavapipe, NOTES-PHASE32.md) : -10 % de temps de rendu sur le
    // panorama. L'image n'est PAS identique au bit pres : 315 pixels isoles
    // (0,03 %) changent, tous sur des parois quasi verticales vues en rasant,
    // la ou une face arriere l'emportait d'un cheveu sur la face avant par
    // erreur d'arrondi de profondeur. C'est desormais toujours la face avant.
    bool cullBackFaces = false;
};

// Shaders de la Phase 2 : couleur par sommet + Blinn-Phong. Utilises par le
// cube en dur et par les meshes .obj.
inline const ShaderSet kObjectShaders{ "cube.vert.spv", "cube.frag.spv" };

// Shaders de la Phase 4 : coloration par altitude et par pente + Blinn-Phong.
inline const ShaderSet kTerrainShaders{ "terrain.vert.spv", "terrain.frag.spv", true,
                                        false, false, true };

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
