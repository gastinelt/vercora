#pragma once

#include "renderer/DrawItem.hpp"
#include "renderer/GpuBufferPool.hpp"
#include "renderer/GpuVertex.hpp"
#include "renderer/Pipeline.hpp" // pour ShaderSet
#include "renderer/VulkanBuffer.hpp" // pour BufferUploader

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace vercora {

class VulkanContext;
class Window;
class SwapChain;
class Camera;
class Mesh;
class Texture;
struct MeshData;
struct TextureData;

// Ce que le Renderer doit savoir de la scene, en plus de sa geometrie.
// Regroupe ici plutot qu'en parametres separes : la liste s'allongera (soleil,
// brouillard, ...) et un struct nomme reste lisible sur l'appel.
struct RenderOptions {
    // Altitudes min/max du terrain, transmises au fragment shader pour sa
    // coloration. Ignore par les shaders d'objet.
    glm::vec2 heightRange{0.0f, 1.0f};

    // Couleur d'effacement de l'attache couleur. Par defaut le bleu nuit des
    // phases precedentes, qui fait ressortir un objet isole ; un terrain, lui,
    // a besoin d'un ciel pour que la ligne d'horizon soit lisible.
    glm::vec3 clearColor{0.01f, 0.01f, 0.03f};

    // Plans de la projection perspective. Le terrain est bien plus etendu qu'un
    // objet isole : son far plane doit suivre, sinon l'horizon est tronque.
    float nearPlane = 0.1f;
    float farPlane = 100.0f;

    // PHASE 11. Dosage entre coloration PROCEDURALE (0 : altitude + pente, le
    // rendu des Phases 4 a 10) et ORTHOPHOTO (1 : la photo aerienne reelle).
    // Un dosage plutot qu'un booleen : la bascule sert au debug et aux captures
    // avant/apres, et une valeur intermediaire montre d'un coup d'oeil si les
    // deux se recouvrent bien. Sans texture chargee, la valeur est ignoree (le
    // binding pointe alors sur un texel blanc).
    float orthoMix = 0.0f;

    // PHASE 30. Force le televersement de la geometrie par STAGING meme en
    // memoire unifiee, ou elle est sinon ecrite directement la ou le GPU la
    // lit. Sert a eprouver sur la machine de developpement le chemin d'un GPU
    // discret (option --force-staging).
    bool forceStaging = false;

    // PHASE 30. Presentation calee sur l'ecran (FIFO, option --vsync) : la
    // cadence ne depasse plus le rafraichissement, et le CPU comme le GPU
    // dorment le reste du temps. Voir SwapChain::choosePresentMode.
    bool vsync = false;
};

// Orchestre le rendu : geometries enregistrees, UBO/descriptors, command
// pool/buffers, synchronisation, et la boucle drawFrame. Possede la SwapChain
// et les Pipeline. Gere le resize (swapchain + depth buffer ensemble).
//
// Depuis la Phase 5, le Renderer dessine PLUSIEURS objets par frame :
//   1. l'appelant enregistre ses geometries une fois pour toutes (addMesh) et
//      garde les MeshHandle retournes ;
//   2. a chaque frame il fournit la liste des DrawItem a dessiner.
// La geometrie (couteuse : upload GPU) est donc dissociee des instances
// (jetables : une matrice). Un meme mesh d'arbre sert ainsi a tous les arbres.
//
// PHASE 12 : le Renderer est aussi le BufferUploader du moteur. Les copies
// staging -> device-local demandees par les Mesh ne sont plus attendues : elles
// sont accumulees puis enregistrees en tete du command buffer de la frame
// suivante, avant le render pass, suivies d'UNE barriere transfert -> vertex
// input. Voir enqueueCopy et NOTES-PHASE12.md.
class Renderer : public BufferUploader {
public:
    Renderer(VulkanContext& context, Window& window,
             const RenderOptions& options = {});
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // Envoie une geometrie sur le GPU et renvoie sa poignee. La geometrie est
    // fournie deja construite (fichier .obj, cube en dur, terrain ou arbre
    // genere) : le Renderer ne fait aucune I/O disque ni aucune generation.
    // Lance std::runtime_error si la geometrie est vide ou incoherente.
    // `log` a false pour les geometries creees en masse (les tuiles du
    // quadtree, plusieurs par seconde) : une ligne de journal par tuile noierait
    // toute autre information.
    MeshHandle addMesh(const MeshData& meshData, bool log = true);

    // PHASE 30 : GEOMETRIE DE TERRAIN, au format compact TerrainVertex (16
    // octets, voir renderer/GpuVertex.hpp). Un tel maillage ne se dessine
    // qu'avec Material::Terrain et un DrawItem::grid qui dit ou il se trouve
    // dans la heightmap.
    //
    // Terrain d'un seul bloc (--no-lod) : sommets et topologie propres.
    MeshHandle addTerrainMesh(const std::vector<TerrainVertex>& vertices,
                              const std::vector<uint32_t>& indices,
                              const std::string& name, bool log = true);

    // Topologie PARTAGEE par plusieurs maillages de terrain : les tuiles du
    // quadtree ont toutes la meme (Phase 18), et la televerser une fois au lieu
    // d'une par tuile economise 96 Ko par tuile residente. Stockee en uint16
    // quand les indices le permettent (65 x 65 sommets par tuile). Vit aussi
    // longtemps que le Renderer.
    uint32_t addSharedIndices(const std::vector<uint32_t>& indices);

    // Tuile de terrain sur une topologie partagee, ECRITE PAR L'APPELANT :
    // beginTerrainMesh reserve la place et rend un pointeur sur `vertexCount`
    // sommets a remplir - directement la memoire que le GPU lira en memoire
    // unifiee, un staging sinon - et endTerrainMesh la livre. Entre les deux,
    // l'ecriture peut se faire depuis N'IMPORTE QUEL thread (aucun appel
    // Vulkan) : c'est ce qui laisse le maillage parallele de la Phase 19
    // ecrire sans copie intermediaire. begin et end, eux, restent sur le
    // thread de rendu, et chaque begin doit etre suivi de son end (ou d'un
    // removeMesh si l'ecriture a echoue).
    struct TerrainMeshWrite {
        MeshHandle mesh = kInvalidMesh;
        TerrainVertex* vertices = nullptr;
    };
    TerrainMeshWrite beginTerrainMesh(uint32_t vertexCount, uint32_t sharedIndices);
    void endTerrainMesh(MeshHandle mesh);

    // PHASE 30 : grille de la heightmap, lue par terrain.vert pour recalculer
    // la position horizontale et l'UV des sommets de terrain (voir
    // UniformBufferObject::terrainGrid). A appeler une fois, avant la premiere
    // image, des qu'un terrain est charge.
    void setTerrainGrid(float origin, float spacing, uint32_t cells);

    // PHASE 30 : bilan du pool de geometrie, pour le journal.
    std::string geometryLine() const;

    // Libere une geometrie et rend sa poignee reutilisable. Indispensable depuis
    // la Phase 10 : le streaming par tuiles cree et detruit des maillages en
    // permanence, alors que jusque-la une geometrie enregistree vivait aussi
    // longtemps que le Renderer.
    //
    // La destruction est DIFFEREE de kMaxFramesInFlight + 1 frames : le GPU peut
    // encore etre en train de lire les buffers d'une frame deja soumise, et les
    // detruire tout de suite serait une erreur de validation immediate (voire un
    // crash du driver). Detruire au bout de ce delai est correct sans avoir a
    // suivre quelle frame a reference quel mesh.
    void removeMesh(MeshHandle handle);

    // Matrice de projection de la frame a venir. Exposee parce que le frustum
    // culling se decide AVANT le rendu, cote appelant (voir core/Frustum.hpp) :
    // c'est le Renderer qui connait l'aspect de la swapchain et les plans
    // near/far, donc lui qui doit fournir la matrice - la recalculer ailleurs
    // reviendrait a dupliquer une convention (l'inversion de l'axe Y de GLM)
    // qu'une seule des deux copies penserait a mettre a jour.
    glm::mat4 projectionMatrix() const;

    // PHASE 11. Envoie l'orthophoto sur le GPU et la relie aux descriptor sets.
    // A appeler avant la premiere frame (elle attend la fin des transferts).
    // Sans appel, le binding de texture reste sur un texel blanc et seule la
    // coloration procedurale est visible.
    void setTerrainTexture(const TextureData& data);

    // PHASE 30. Meme chose, mais les texels sont lus dans `path` DIRECTEMENT
    // vers le staging de la texture : `header` vient de loadTextureFileHeader
    // et ne porte aucun texel. Evite de tenir l'orthophoto (42 Mo) en RAM en
    // plus du staging et de l'image GPU pendant le chargement.
    void setTerrainTextureFromFile(const TextureData& header, const std::string& path);

    // Bascule coloration procedurale <-> orthophoto (voir RenderOptions).
    // Sans effet visible si aucune texture n'a ete chargee.
    void setOrthoMix(float mix);
    float orthoMix() const { return options_.orthoMix; }

    // PHASE 23 - CYCLE JOUR/NUIT.
    //
    // Regle la lumiere directionnelle pour les images suivantes. Appele une fois
    // par image depuis main.cpp, a partir de la position solaire calculee par
    // world/Weather.hpp - le renderer ne connait ni l'heure, ni la meteo, il
    // recoit une lumiere deja faite. C'est la meme separation que pour tout le
    // reste du projet : le monde calcule, le renderer dessine.
    //
    //   direction : du fragment VERS la lumiere, unitaire, en repere monde
    //   intensity : intensite de la lumiere DIRECTE, dans [0, 1] (0 = nuit)
    //   color     : couleur de la lumiere
    //   ambient   : MULTIPLICATEUR de l'ambiante propre a chaque shader, dans
    //               [0, 1]. Voir renderer/UniformBufferObject.hpp pour la raison
    //               d'un multiplicateur plutot que d'une valeur absolue.
    //
    // Une direction nulle ou non finie est IGNOREE (l'ancienne est conservee) :
    // un cas degenere ne doit pas eteindre l'image.
    void setSun(const glm::vec3& direction, float intensity, const glm::vec3& color,
                float ambient);

    // Couleur d'effacement, c'est-a-dire le CIEL. Reglee par image avec la
    // lumiere (voir SunLight::skyColor) : sans elle, la nuit tombait sur le
    // relief mais pas sur le fond, et une capture de 3 h du matin montrait des
    // sapins noirs sur un ciel de midi.
    void setClearColor(const glm::vec3& color) {
        options_.clearColor = glm::clamp(color, glm::vec3(0.0f), glm::vec3(1.0f));
    }

    // PHASE 27 : METEO VISIBLE. Parametres produits par weatherVisualsFor()
    // (world/Weather.hpp) - le renderer ne sait rien de la meteo, il recoit des
    // nombres, comme pour setSun(). Valeurs neutres par defaut : sans appel,
    // l'image est celle de la Phase 26.
    //
    //   fogColor / fogDensityPerMeter : brouillard de distance, dans les trois
    //       shaders d'objets (sinon les arbres ressortiraient nets du voile)
    //   precipitation (0..1), snow : passe plein ecran de pluie ou de neige,
    //       sautee entierement quand precipitation vaut 0 (cout nul par beau)
    //   fallVelocity : vitesse de chute en monde, m/s, vent local compris
    //   groundDarkening : assombrissement du sol mouille, terrain seulement
    //   timeSeconds : horloge d'animation de la chute
    void setWeatherVisuals(const glm::vec3& fogColor, float fogDensityPerMeter,
                           float precipitation, bool snow, const glm::vec3& fallVelocity,
                           float groundDarkening, float timeSeconds);

    // PHASE 27, chantier 3 : micro-relief du sol vu de pres. strength = 0
    // desactive. fadeStart / fadeEnd en metres depuis la camera ; patternMeters
    // = taille du motif le plus grossier. Voir shaders/terrain.frag.
    void setGroundDetail(float strength, float fadeStart, float fadeEnd, float patternMeters);

    // Vrai si la passe de precipitation sera enregistree a la prochaine image.
    bool precipitationPassActive() const { return precip_.x > 0.0f; }

    // La lumiere courante, pour le journal et les captures.
    const glm::vec4& sun() const { return sun_; }
    const glm::vec4& sunColor() const { return sunColor_; }

    // PHASE 25 : FORCE DE L'INDICE VISUEL ADRET / UBAC, dans [0, 1].
    //
    // Ce scalaire porte TOUT ce qui, dans l'ecart adret/ubac, ne depend pas de
    // la geometrie locale : la hauteur du soleil et la nebulosite. Il est
    // calcule cote CPU par main.cpp, a partir des memes grandeurs et des memes
    // parametres que WeatherSystem::sampleAt - et c'est deliberement lui qui
    // les porte, pour qu'aucune de ces deux regles ne soit re-derivee dans un
    // shader ou elle pourrait diverger de la meteo.
    //
    // Le shader n'ajoute que ce que lui seul peut faire : le cosinus
    // d'incidence par fragment, a partir de la normale et de la MEME direction
    // solaire que l'eclairage (ubo.sun.xyz). Voir shaders/terrain.frag.
    //
    // 0 = aucun effet, et c'est le defaut : sans appel a setAspectTint(), le
    // rendu est exactement celui d'avant la Phase 25.
    void setAspectTint(float strength) {
        aspectTint_ = std::isfinite(strength) ? std::clamp(strength, 0.0f, 1.0f) : 0.0f;
    }
    float aspectTint() const { return aspectTint_; }

    // true si une vraie orthophoto est reliee (et non le texel blanc de repli).
    bool hasTerrainTexture() const { return terrainTexture_ != nullptr; }

    // Nombre de geometries actuellement vivantes. Sert aux logs de diagnostic du
    // streaming.
    uint32_t liveMeshCount() const { return liveMeshCount_; }

    // Rend et presente une frame : la vue vient de la camera, le contenu de la
    // liste d'objets. La liste n'est PAS conservee au-dela de l'appel.
    void drawFrame(const Camera& camera, const std::vector<DrawItem>& items);

    // PHASE 11. Meme frame, avec en plus de la geometrie INSTANCIEE (vegetation).
    // Les tableaux d'instances designes par les InstancedDrawItem sont recopies
    // dans un buffer de la frame pendant l'appel ; ils n'ont pas a survivre au
    // retour.
    void drawFrame(const Camera& camera, const std::vector<DrawItem>& items,
                   const std::vector<InstancedDrawItem>& instanced);

    // Instances reellement soumises a la derniere frame, tous materiaux
    // confondus. Sert au journal de diagnostic.
    uint32_t lastInstanceCount() const { return lastInstanceCount_; }

    // A appeler avant destruction pour que le GPU termine son travail en cours.
    void waitIdle() const;

    // --- BufferUploader -----------------------------------------------------
    // Accumule une copie staging -> device-local. Elle sera enregistree en tete
    // du prochain command buffer de frame, AVANT vkCmdBeginRenderPass, et
    // separee des draws par une barriere memoire. Rien n'est soumis ici, rien
    // n'est attendu : c'est la levee du "verrou n 1" de la Phase 10.
    //
    // Consequence pour l'appelant : la geometrie n'est lisible par le GPU qu'a
    // partir de la frame suivante - ce qui est exactement le moment ou elle
    // servira, puisque le seul chemin qui l'appelle (addMesh) est suivi d'un
    // drawFrame.
    //
    // `destination` est une poignee NUE : elle doit rester valide jusqu'a
    // l'enregistrement, c'est-a-dire jusqu'au prochain drawFrame. C'est
    // garanti par construction - le seul appelant est addMesh, et removeMesh
    // DIFFERE la destruction des buffers de kMaxFramesInFlight + 1 frames.
    void enqueueCopy(std::unique_ptr<VulkanBuffer> staging,
                     VkBuffer destination, VkDeviceSize destinationOffset) override;

    // Octets de staging en attente d'enregistrement + en attente d'execution.
    // Sert au journal de diagnostic du streaming.
    std::size_t stagingBytesInFlight() const { return stagingBytesInFlight_; }

    // Plafond de staging vivant simultanement. Au-dela, enqueueCopy bascule sur
    // une soumission SYNCHRONE (voir flushCopiesNow) plutot que de laisser la
    // memoire enfler.
    //
    // Pourquoi ce plafond existe : differer les copies veut dire garder les
    // buffers staging trois frames de plus au lieu de les liberer aussitot. Pour
    // le streaming c'est negligeable (64 tuiles ~ 25 Mo). Pour le chemin
    // --no-lod, qui televerse la zone de 25 km2 en UN SEUL maillage, le staging
    // pese 3,4 Go a lui tout seul : sans plafond, le pic memoire de ce chemin
    // passait de 5,6 a 6,9 Go, soit une regression de 23 % sur une mesure de la
    // Phase 10. 128 Mo laisse passer tout le streaming et n'attrape que les
    // televersements geants, qui sont par nature hors du chemin critique (ils
    // ont lieu avant la premiere frame).
    static constexpr VkDeviceSize kMaxPendingStagingBytes = 128ull * 1024 * 1024;

private:
    // Lumiere directionnelle courante. Initialisee aux valeurs de la Phase 2 :
    // sans appel a setSun(), le rendu est exactement celui d'avant la Phase 23.
    float aspectTint_ = 0.0f; // PHASE 25, voir setAspectTint()
    glm::vec4 sun_{0.3244f, 0.8111f, 0.4867f, 1.0f};
    glm::vec4 sunColor_{1.0f, 0.97f, 0.90f, 1.0f};
    // PHASE 27 - voir setWeatherVisuals() / setGroundDetail() et le detail des
    // champs dans renderer/UniformBufferObject.hpp.
    glm::vec4 fog_{0.52f, 0.66f, 0.82f, 0.0f};
    glm::vec4 precip_{0.0f, 0.0f, 0.0f, 0.0f};
    glm::vec4 fall_{0.0f, -6.5f, 0.0f, 0.0f};
    glm::vec4 detail_{0.0f, 0.0f, 1.0f, 1.0f};
    glm::vec4 terrainGrid_{0.0f, 1.0f, 1.0f, 0.0f}; // PHASE 30, voir setTerrainGrid()

    static constexpr int kMaxFramesInFlight = 2;

    void createCommandPool();
    void createCommandBuffers();
    void createFrameSyncObjects();
    void createImageSyncObjects();
    void destroyImageSyncObjects();

    // Ressources de rendu.
    void createUniformBuffers();
    void createDescriptorSetLayout();
    void createDescriptorPool();
    void createDescriptorSets();

    // (Re)ecrit le binding 1 de tous les descriptor sets vers la texture
    // actuellement retenue. Appele a la creation (texel blanc) puis a chaque
    // setTerrainTexture.
    void writeTextureDescriptors();

    void updateUniformBuffer(uint32_t frameIndex, const Camera& camera);
    void recordCommandBuffer(VkCommandBuffer commandBuffer, uint32_t imageIndex,
                             const std::vector<DrawItem>& items,
                             const std::vector<InstancedDrawItem>& instanced);

    // Enregistre toutes les copies en attente puis UNE barriere, dans un
    // command buffer deja commence et AVANT tout render pass.
    void recordPendingCopies(VkCommandBuffer commandBuffer);

    // Soumet immediatement les copies en attente et ATTEND le GPU, puis libere
    // leur staging. Chemin de secours uniquement (voir kMaxPendingStagingBytes) :
    // il bloque, ce que tout le reste de la Phase 12 s'emploie a eviter.
    void flushCopiesNow();

    // Recopie toutes les instances de la frame dans le buffer de la
    // frame-in-flight courante, en agrandissant celui-ci si besoin, et remplit
    // instanceOffsets_ (un offset par InstancedDrawItem, dans l'ordre).
    void uploadInstances(const std::vector<InstancedDrawItem>& instanced);
    void recreateSwapChain();

    // PHASE 30. Ecriture dans une plage du pool : `destination` pointe soit
    // dans la plage elle-meme (memoire unifiee), soit dans un staging, et
    // finishRangeWrite confie alors la copie a la prochaine image.
    struct RangeWrite {
        GpuRange range;
        void* destination = nullptr;
        std::unique_ptr<VulkanBuffer> staging;
    };
    RangeWrite beginRangeWrite(const GpuRange& range);
    void finishRangeWrite(RangeWrite& write);

    // Inscrit un Mesh dans la table et rend sa poignee (emplacement libre
    // reutilise s'il y en a un).
    MeshHandle registerMesh(std::unique_ptr<Mesh> mesh);

    VulkanContext& context_;
    Window& window_;
    RenderOptions options_;

    std::unique_ptr<SwapChain> swapChain_;

    // PHASE 30 : TOUTE la geometrie (sommets et indices, objets et terrain)
    // vit dans les blocs de ce pool. Declare AVANT les tables de maillages :
    // les membres etant detruits dans l'ordre inverse, il survit a tous les
    // Mesh, qui lui rendent leurs plages en se detruisant.
    std::unique_ptr<GpuBufferPool> geometryPool_;

    // Topologies partagees (addSharedIndices), rendues a la destruction.
    struct SharedIndices {
        GpuRange range;
        uint32_t count = 0;
        VkIndexType type = VK_INDEX_TYPE_UINT16;
    };
    std::vector<SharedIndices> sharedIndices_;

    // Tuiles commencees (beginTerrainMesh) dont l'ecriture passe par un
    // staging, en attente de leur endTerrainMesh.
    std::unordered_map<MeshHandle, RangeWrite> pendingTerrainWrites_;

    // Un pipeline par materiau, indexe par la valeur de l'enum Material. Tous
    // partagent le meme descriptor set layout et le meme push constant range :
    // seuls les shaders different.
    std::array<std::unique_ptr<Pipeline>, kMaterialCount> pipelines_;

    // PHASE 27 : passe PLEIN ECRAN de precipitation. Hors de la table par
    // materiau : elle ne dessine aucun DrawItem, un seul triangle couvrant.
    std::unique_ptr<Pipeline> precipitationPipeline_;

    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> commandBuffers_; // un par frame-in-flight

    // Geometries enregistrees (buffers device-local), indexees par MeshHandle.
    // Les Mesh sont possedes ici et vivent aussi longtemps que le Renderer :
    // les DrawItem d'une frame ne portent que des poignees, jamais de pointeur.
    std::vector<std::unique_ptr<Mesh>> meshes_;

    // Emplacements de meshes_ liberes par removeMesh, reutilisables par addMesh.
    // Sans cette liste, meshes_ grandirait indefiniment au fil du streaming.
    std::vector<MeshHandle> freeMeshSlots_;

    // Meshes retires mais pas encore detruits (voir removeMesh). Le compteur est
    // decremente a chaque frame ; a zero, le unique_ptr est relache.
    struct RetiredMesh {
        std::unique_ptr<Mesh> mesh;
        int framesRemaining = 0;
    };
    std::vector<RetiredMesh> retiredMeshes_;

    uint32_t liveMeshCount_ = 0;

    // PHASE 12 : copies demandees mais pas encore enregistrees dans un command
    // buffer. Videe a chaque recordCommandBuffer.
    struct PendingCopy {
        std::unique_ptr<VulkanBuffer> staging;
        VkBuffer destination = VK_NULL_HANDLE;
        VkDeviceSize destinationOffset = 0;
        VkDeviceSize size = 0;
    };
    std::vector<PendingCopy> pendingCopies_;

    // Buffers staging dont la copie EST enregistree et soumise, mais dont on ne
    // sait pas encore si le GPU l'a executee. Meme mecanisme de vieillissement
    // que retiredMeshes_ : detruire un staging encore lu par une frame en vol
    // serait une erreur de validation immediate.
    struct RetiredStaging {
        std::unique_ptr<VulkanBuffer> staging;
        int framesRemaining = 0;
    };
    std::vector<RetiredStaging> retiredStaging_;

    std::size_t stagingBytesInFlight_ = 0;

    // UBO : un buffer host-visible mappe en permanence, par frame-in-flight.
    std::vector<std::unique_ptr<VulkanBuffer>> uniformBuffers_;

    // Descriptors.
    VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> descriptorSets_; // un par frame-in-flight

    // Texture du terrain (binding 1). `whiteTexture_` existe toujours : c'est
    // elle qui occupe le binding tant qu'aucune orthophoto n'est chargee, ce qui
    // evite d'avoir deux layouts de descriptor set et deux jeux de pipelines
    // selon la presence d'une photo.
    std::unique_ptr<Texture> whiteTexture_;
    std::unique_ptr<Texture> terrainTexture_;
    // PHASE 27 : bruit periodique du micro-relief (binding 2). Genere au
    // demarrage par makeDetailNoiseTexture, 256x256 RGBA8 + mipmaps, 341 Ko.
    std::unique_ptr<Texture> detailTexture_;

    // PHASE 11 : buffer d'instances par frame-in-flight, host-visible et mappe
    // en permanence, comme l'UBO. Reecrit a chaque frame parce que son contenu
    // change a chaque frame (le joueur bouge, donc l'ensemble des arbres
    // visibles change) : un buffer device-local imposerait un staging et une
    // copie par frame pour une donnee qui ne sert qu'une fois.
    //
    // Il GRANDIT a la demande et ne retrecit jamais : le pic de vegetation
    // visible est atteint en quelques secondes de jeu, et liberer pour
    // reallouer ensuite ne ferait qu'ajouter des a-coups.
    std::vector<std::unique_ptr<VulkanBuffer>> instanceBuffers_;
    std::vector<VkDeviceSize> instanceOffsets_; // un par InstancedDrawItem de la frame

    uint32_t lastInstanceCount_ = 0;

    // Synchronisation.
    std::vector<VkSemaphore> imageAvailableSemaphores_; // par frame-in-flight
    std::vector<VkSemaphore> renderFinishedSemaphores_; // par image de swapchain
    std::vector<VkFence>     inFlightFences_;            // par frame-in-flight
    std::vector<VkFence>     imagesInFlight_;            // suivi : quelle fence occupe l'image

    uint32_t currentFrame_ = 0;
};

} // namespace vercora
