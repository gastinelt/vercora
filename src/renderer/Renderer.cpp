#include "renderer/Renderer.hpp"

#include "assets/DetailNoise.hpp"
#include "assets/TextureFile.hpp"

#include "core/Camera.hpp"
#include "core/Logger.hpp"
#include "core/Window.hpp"
#include "renderer/Mesh.hpp"
#include "renderer/MeshData.hpp"
#include "renderer/Pipeline.hpp"
#include "renderer/SwapChain.hpp"
#include "renderer/Texture.hpp"
#include "renderer/UniformBufferObject.hpp"
#include "renderer/VulkanBuffer.hpp"
#include "renderer/VulkanContext.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace vercora {

namespace {

// PHASE 32 : barriere d'EXECUTION placee AVANT les copies staging -> pool.
//
// Avant le pool, chaque copie ecrivait un buffer neuf, que rien n'avait jamais
// lu. Desormais une plage du pool est RECYCLEE : la copie ecrit des octets que
// des images precedentes ont lus comme sommets ou indices. Le delai de retrait
// (Renderer::removeMesh) garantit que ces images sont terminees, mais il le
// garantit cote CPU, par les fences ; la validation de SYNCHRONISATION signale
// a juste titre que rien ne l'exprime cote GPU (SYNC-HAZARD-WRITE-AFTER-READ,
// 414 messages en 30 s de survol avec --force-staging). Un danger ecriture
// apres lecture ne demande qu'une dependance d'EXECUTION : pas de masque
// d'acces, l'etage d'assemblage des sommets avant, le transfert apres. Le
// chemin d'ecriture directe (memoire unifiee) n'a pas de copie, donc pas de
// barriere.
void recordCopyAfterReadBarrier(VkCommandBuffer commandBuffer) {
    vkCmdPipelineBarrier(commandBuffer,
                         VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 0, nullptr);
}

// Paire de shaders associee a chaque materiau. L'ordre suit celui de l'enum
// Material : c'est ce tableau qui fait le lien entre le vocabulaire du gameplay
// ("cet objet est du terrain") et celui du rendu ("ces .spv").
const ShaderSet& shadersFor(Material material) {
    switch (material) {
        case Material::Terrain: return kTerrainShaders;
        case Material::Foliage: return kFoliageShaders;
        case Material::Object:  break;
    }
    return kObjectShaders;
}

} // namespace

Renderer::Renderer(VulkanContext& context, Window& window,
                   const RenderOptions& options)
    : context_(context), window_(window), options_(options) {
    swapChain_ = std::make_unique<SwapChain>(context_, window_, options_.vsync);

    // PHASE 32 : le pool de geometrie. En memoire unifiee (GPU integre,
    // lavapipe), ses blocs sont mappes et la geometrie y est ecrite DIRECTEMENT
    // : ni staging, ni copie, ni barriere. Sur un GPU discret, la memoire
    // device-local n'est pas (ou peu) mappable : chemin staging de la Phase 12.
    const bool directWrites = context_.preferDirectGeometryWrites() && !options_.forceStaging;
    geometryPool_ = std::make_unique<GpuBufferPool>(
        context_,
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, directWrites);
    logInfo(std::string("Geometrie : pool de blocs de ") +
            std::to_string(GpuBufferPool::kDefaultBlockSize / (1024 * 1024)) + " Mo, " +
            (directWrites ? "ecriture DIRECTE (memoire unifiee, sans staging)."
                          : (options_.forceStaging
                                 ? "televersement par staging (FORCE, --force-staging)."
                                 : "televersement par staging (memoire device-local non mappable).")));

    createCommandPool(); // requis avant les buffers (transferts staging)

    createDescriptorSetLayout();

    // Un pipeline par materiau, cree une fois pour toutes : changer de pipeline
    // pendant l'enregistrement d'un command buffer est bon marche, en creer un
    // ne l'est pas. Ils ne dependent pas de l'extent (viewport/scissor
    // dynamiques), donc le resize ne les recree pas non plus.
    for (uint32_t i = 0; i < kMaterialCount; ++i) {
        pipelines_[i] = std::make_unique<Pipeline>(
            context_.device(), swapChain_->renderPass(), descriptorSetLayout_,
            shadersFor(static_cast<Material>(i)));
    }
    precipitationPipeline_ = std::make_unique<Pipeline>(
        context_.device(), swapChain_->renderPass(), descriptorSetLayout_,
        kPrecipitationShaders);

    createUniformBuffers();
    createDescriptorPool();

    // Doit exister avant l'ecriture des descriptor sets : c'est elle qui occupe
    // le binding 1 tant qu'aucune orthophoto n'est chargee.
    whiteTexture_ = std::make_unique<Texture>(context_, commandPool_,
                                              context_.graphicsQueue(),
                                              Texture::whitePixelData());
    detailTexture_ = std::make_unique<Texture>(context_, commandPool_,
                                               context_.graphicsQueue(),
                                               makeDetailNoiseTexture(), true);

    createDescriptorSets();

    // Un emplacement de buffer d'instances par frame-in-flight ; les buffers
    // eux-memes sont alloues a la premiere frame qui en a besoin, avec la taille
    // qu'il faut. Une scene sans vegetation n'alloue donc rien.
    instanceBuffers_.resize(kMaxFramesInFlight);

    createCommandBuffers();
    createFrameSyncObjects();
    createImageSyncObjects();
}

Renderer::~Renderer() {
    VkDevice device = context_.device();

    // PHASE 32 : rien ne doit plus lire la geometrie quand ses plages sont
    // rendues au pool (les Mesh le font en se detruisant, apres ce corps).
    vkDeviceWaitIdle(device);
    pendingTerrainWrites_.clear();
    for (SharedIndices& shared : sharedIndices_) {
        geometryPool_->release(shared.range);
    }
    sharedIndices_.clear();

    for (VkSemaphore s : imageAvailableSemaphores_) {
        vkDestroySemaphore(device, s, nullptr);
    }
    for (VkFence f : inFlightFences_) {
        vkDestroyFence(device, f, nullptr);
    }
    destroyImageSyncObjects();

    if (descriptorPool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device, descriptorPool_, nullptr);
    }
    if (descriptorSetLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device, descriptorSetLayout_, nullptr);
    }

    if (commandPool_ != VK_NULL_HANDLE) {
        // Detruire le pool libere aussi les command buffers alloues dessus.
        vkDestroyCommandPool(device, commandPool_, nullptr);
    }

    // uniformBuffers_, meshes_, pipelines_, swapChain_ : detruits automatiquement
    // (unique_ptr) apres ce corps, device encore valide.
}

void Renderer::waitIdle() const {
    vkDeviceWaitIdle(context_.device());
}

MeshHandle Renderer::registerMesh(std::unique_ptr<Mesh> mesh) {
    ++liveMeshCount_;
    if (!freeMeshSlots_.empty()) {
        const MeshHandle handle = freeMeshSlots_.back();
        freeMeshSlots_.pop_back();
        meshes_[handle] = std::move(mesh);
        return handle;
    }
    meshes_.push_back(std::move(mesh));
    return static_cast<MeshHandle>(meshes_.size() - 1);
}

Renderer::RangeWrite Renderer::beginRangeWrite(const GpuRange& range) {
    RangeWrite write;
    write.range = range;
    if (range.mapped != nullptr) {
        // Memoire unifiee : on ecrit la ou le GPU lira. Sans danger pour la
        // synchronisation - une plage n'est rendue au pool qu'une fois que plus
        // aucune image en vol ne la lit (Renderer::removeMesh), et l'ecriture
        // hote est visible du GPU a la soumission suivante (memoire coherente).
        write.destination = range.mapped;
        return write;
    }
    write.staging = std::make_unique<VulkanBuffer>(
        context_, range.size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    write.staging->map();
    write.destination = write.staging->mapped();
    return write;
}

void Renderer::finishRangeWrite(RangeWrite& write) {
    if (write.staging) {
        write.staging->unmap();
        enqueueCopy(std::move(write.staging), write.range.buffer, write.range.offset);
    }
    write.destination = nullptr;
}

MeshHandle Renderer::addMesh(const MeshData& meshData, bool log) {
    // --- Validation AVANT toute allocation ----------------------------------
    // Un index hors limites ne produit pas d'erreur a la creation du buffer :
    // il se manifesterait au draw, sous forme de lecture hors du vertex buffer
    // (comportement indefini cote GPU). Mieux vaut refuser ici, avec un message
    // qui dit quoi. Si la geometrie est refusee, aucun emplacement n'a ete
    // consomme.
    if (meshData.vertices.empty()) {
        throw std::runtime_error("Mesh vide : aucun sommet (" + meshData.name + ").");
    }
    if (meshData.indices.empty()) {
        throw std::runtime_error("Mesh vide : aucun index (" + meshData.name + ").");
    }
    if (meshData.indices.size() % 3 != 0) {
        throw std::runtime_error(
            "Mesh non triangule : " + std::to_string(meshData.indices.size()) +
            " index, ce qui n'est pas un multiple de 3 (" + meshData.name + ").");
    }
    const auto vertexCount = static_cast<uint32_t>(meshData.vertices.size());
    for (uint32_t index : meshData.indices) {
        if (index >= vertexCount) {
            throw std::runtime_error(
                "Index hors limites dans le mesh : " + std::to_string(index) +
                " pour " + std::to_string(vertexCount) + " sommets (" + meshData.name + ").");
        }
    }
    const auto indexCount = static_cast<uint32_t>(meshData.indices.size());

    // PHASE 32 : sommets au format ObjectVertex (44 octets, conversion exacte)
    // et indices en uint16 des que le maillage a au plus 65 536 sommets - le
    // cas de TOUS les maillages d'objets du jeu. Les valeurs sont les memes,
    // seule leur largeur change.
    const bool shortIndices = vertexCount <= 65536u;
    const VkDeviceSize vertexBytes = static_cast<VkDeviceSize>(vertexCount) * sizeof(ObjectVertex);
    const VkDeviceSize indexBytes = static_cast<VkDeviceSize>(indexCount) *
                                    (shortIndices ? sizeof(uint16_t) : sizeof(uint32_t));

    // Le Mesh possede ses plages DES leur allocation : si la suite echoue
    // (memoire epuisee), il les rend au pool en se detruisant. Et aucune copie
    // n'est confiee a l'image suivante avant que les DEUX ecritures aient
    // reussi - sinon une copie en attente viserait une plage deja rendue.
    auto mesh = allocateMesh(VertexLayout::Object, vertexBytes, vertexCount, indexBytes,
                             shortIndices ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32,
                             indexCount);
    RangeWrite vertexWrite = beginRangeWrite(mesh->vertexRange());
    RangeWrite indexWrite = beginRangeWrite(mesh->indexRange());

    auto* vertices = static_cast<ObjectVertex*>(vertexWrite.destination);
    for (uint32_t v = 0; v < vertexCount; ++v) {
        vertices[v] = toObjectVertex(meshData.vertices[v]);
    }
    if (shortIndices) {
        auto* indices = static_cast<uint16_t*>(indexWrite.destination);
        for (uint32_t k = 0; k < indexCount; ++k) {
            indices[k] = static_cast<uint16_t>(meshData.indices[k]);
        }
    } else {
        std::memcpy(indexWrite.destination, meshData.indices.data(),
                    static_cast<std::size_t>(indexBytes));
    }
    finishRangeWrite(vertexWrite);
    finishRangeWrite(indexWrite);

    if (log) {
        logInfo("Mesh charge sur le GPU : " + meshData.name + " - " +
                std::to_string(vertexCount) + " sommets, " +
                std::to_string(indexCount / 3) + " triangles, " +
                std::to_string((vertexBytes + indexBytes + 1023) / 1024) + " Ko.");
    }
    return registerMesh(std::move(mesh));
}

std::unique_ptr<Mesh> Renderer::allocateMesh(VertexLayout layout, VkDeviceSize vertexBytes,
                                             uint32_t vertexCount, VkDeviceSize indexBytes,
                                             VkIndexType indexType, uint32_t indexCount) {
    const GpuRange vertexRange = geometryPool_->allocate(vertexBytes);
    GpuRange indexRange;
    try {
        indexRange = geometryPool_->allocate(indexBytes);
    } catch (...) {
        geometryPool_->release(vertexRange);
        throw;
    }
    return std::make_unique<Mesh>(*geometryPool_, layout, vertexRange, vertexCount,
                                  indexRange, indexType, indexCount, true);
}

MeshHandle Renderer::addTerrainMesh(const std::vector<TerrainVertex>& vertices,
                                    const std::vector<uint32_t>& indices,
                                    const std::string& name, bool log) {
    if (vertices.empty() || indices.empty() || indices.size() % 3 != 0) {
        throw std::runtime_error("Terrain : geometrie vide ou non triangulee (" + name + ").");
    }
    const auto vertexCount = static_cast<uint32_t>(vertices.size());
    for (uint32_t index : indices) {
        if (index >= vertexCount) {
            throw std::runtime_error("Terrain : index " + std::to_string(index) +
                                     " hors des " + std::to_string(vertexCount) +
                                     " sommets (" + name + ").");
        }
    }
    const VkDeviceSize vertexBytes = static_cast<VkDeviceSize>(vertexCount) * sizeof(TerrainVertex);
    const VkDeviceSize indexBytes = static_cast<VkDeviceSize>(indices.size()) * sizeof(uint32_t);
    // Meme ordre que addMesh : plages possedees d'abord, copies confiees ensuite.
    auto mesh = allocateMesh(VertexLayout::Terrain, vertexBytes, vertexCount, indexBytes,
                             VK_INDEX_TYPE_UINT32, static_cast<uint32_t>(indices.size()));
    RangeWrite vertexWrite = beginRangeWrite(mesh->vertexRange());
    RangeWrite indexWrite = beginRangeWrite(mesh->indexRange());
    std::memcpy(vertexWrite.destination, vertices.data(), static_cast<std::size_t>(vertexBytes));
    std::memcpy(indexWrite.destination, indices.data(), static_cast<std::size_t>(indexBytes));
    finishRangeWrite(vertexWrite);
    finishRangeWrite(indexWrite);

    if (log) {
        logInfo("Terrain charge sur le GPU : " + name + " - " + std::to_string(vertexCount) +
                " sommets, " + std::to_string(indices.size() / 3) + " triangles, " +
                std::to_string((vertexBytes + indexBytes) / (1024 * 1024)) + " Mo.");
    }
    return registerMesh(std::move(mesh));
}

uint32_t Renderer::addSharedIndices(const std::vector<uint32_t>& indices) {
    if (indices.empty() || indices.size() % 3 != 0) {
        throw std::runtime_error("Topologie partagee vide ou non triangulee.");
    }
    const uint32_t highest = *std::max_element(indices.begin(), indices.end());
    const bool shortIndices = highest <= 0xFFFFu;
    SharedIndices shared;
    shared.count = static_cast<uint32_t>(indices.size());
    shared.type = shortIndices ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
    shared.range = geometryPool_->allocate(static_cast<VkDeviceSize>(indices.size()) *
                                           (shortIndices ? sizeof(uint16_t) : sizeof(uint32_t)));
    RangeWrite write;
    try {
        write = beginRangeWrite(shared.range);
    } catch (...) {
        geometryPool_->release(shared.range);
        throw;
    }
    if (shortIndices) {
        auto* out = static_cast<uint16_t*>(write.destination);
        for (std::size_t k = 0; k < indices.size(); ++k) {
            out[k] = static_cast<uint16_t>(indices[k]);
        }
    } else {
        std::memcpy(write.destination, indices.data(), indices.size() * sizeof(uint32_t));
    }
    finishRangeWrite(write);
    sharedIndices_.push_back(shared);
    return static_cast<uint32_t>(sharedIndices_.size() - 1);
}

Renderer::TerrainMeshWrite Renderer::beginTerrainMesh(uint32_t vertexCount,
                                                      uint32_t sharedIndices) {
    if (sharedIndices >= sharedIndices_.size() || vertexCount == 0) {
        throw std::runtime_error("Tuile de terrain : topologie partagee inconnue.");
    }
    const SharedIndices& shared = sharedIndices_[sharedIndices];
    // Le Mesh possede sa plage avant l'ecriture : si le staging ne peut pas
    // etre cree, il la rend au pool en se detruisant.
    auto mesh = std::make_unique<Mesh>(
        *geometryPool_, VertexLayout::Terrain,
        geometryPool_->allocate(static_cast<VkDeviceSize>(vertexCount) * sizeof(TerrainVertex)),
        vertexCount, shared.range, shared.type, shared.count, false);
    RangeWrite write = beginRangeWrite(mesh->vertexRange());

    TerrainMeshWrite result;
    result.vertices = static_cast<TerrainVertex*>(write.destination);
    result.mesh = registerMesh(std::move(mesh));
    if (write.staging) {
        pendingTerrainWrites_.emplace(result.mesh, std::move(write));
    }
    return result;
}

void Renderer::endTerrainMesh(MeshHandle mesh) {
    const auto it = pendingTerrainWrites_.find(mesh);
    if (it == pendingTerrainWrites_.end()) {
        return; // ecriture directe : rien a livrer, le GPU lit deja ces octets
    }
    finishRangeWrite(it->second);
    pendingTerrainWrites_.erase(it);
}

void Renderer::setTerrainGrid(float origin, float spacing, uint32_t cells) {
    terrainGrid_ = glm::vec4(origin, spacing, static_cast<float>(cells), 0.0f);
}

std::string Renderer::geometryLine() const {
    return "Geometrie : " + geometryPool_->statsLine();
}

void Renderer::removeMesh(MeshHandle handle) {
    if (handle >= meshes_.size() || !meshes_[handle]) {
        return; // deja libere, ou poignee invalide : ne rien faire est sans risque
    }
    // PHASE 32 : une tuile commencee mais jamais livree (ecriture en echec)
    // abandonne son staging : il n'y a rien a copier.
    pendingTerrainWrites_.erase(handle);

    // Le mesh sort de la table tout de suite (sa poignee redevient disponible)
    // mais ses plages de geometrie survivent le temps que les frames deja
    // soumises aient fini de les lire. Voir le commentaire de declaration.
    retiredMeshes_.push_back({ std::move(meshes_[handle]), kMaxFramesInFlight + 1 });
    freeMeshSlots_.push_back(handle);
    --liveMeshCount_;
}

glm::mat4 Renderer::projectionMatrix() const {
    const VkExtent2D extent = swapChain_->extent();
    // Une hauteur nulle est possible pendant une minimisation : on evite la
    // division par zero, la matrice obtenue n'etant de toute facon pas utilisee
    // (aucune frame n'est presentee dans cet etat).
    const float height = (extent.height > 0) ? static_cast<float>(extent.height) : 1.0f;
    const float aspect = static_cast<float>(extent.width) / height;

    glm::mat4 proj = glm::perspective(glm::radians(60.0f), aspect,
                                      options_.nearPlane, options_.farPlane);
    // GLM a ete concu pour OpenGL (Y clip inverse par rapport a Vulkan) : on
    // reinverse l'axe Y de la projection.
    proj[1][1] *= -1.0f;
    return proj;
}

// ---------------------------------------------------------------------------
// Command pool / buffers
// ---------------------------------------------------------------------------
void Renderer::createCommandPool() {
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    // RESET_COMMAND_BUFFER : on reenregistre chaque buffer a chaque frame.
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = context_.queueFamilies().graphicsFamily.value();

    if (vkCreateCommandPool(context_.device(), &poolInfo, nullptr, &commandPool_) != VK_SUCCESS) {
        throw std::runtime_error("Echec de la creation du command pool.");
    }
}

void Renderer::createCommandBuffers() {
    commandBuffers_.resize(kMaxFramesInFlight);

    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = commandPool_;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = static_cast<uint32_t>(commandBuffers_.size());

    if (vkAllocateCommandBuffers(context_.device(), &allocInfo, commandBuffers_.data()) != VK_SUCCESS) {
        throw std::runtime_error("Echec de l'allocation des command buffers.");
    }
}

// ---------------------------------------------------------------------------
// UBO + descriptors
// ---------------------------------------------------------------------------
void Renderer::createUniformBuffers() {
    const VkDeviceSize bufferSize = sizeof(UniformBufferObject);
    uniformBuffers_.resize(kMaxFramesInFlight);

    for (int i = 0; i < kMaxFramesInFlight; ++i) {
        // Host-visible + coherent, mappe en permanence : mis a jour chaque frame.
        uniformBuffers_[i] = std::make_unique<VulkanBuffer>(
            context_, bufferSize,
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        uniformBuffers_[i]->map();
    }
}

void Renderer::createDescriptorSetLayout() {
    VkDescriptorSetLayoutBinding uboBinding{};
    uboBinding.binding = 0;
    uboBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    uboBinding.descriptorCount = 1;
    // Lu par le vertex shader (matrices MVP) ET par le fragment shader (viewPos,
    // pour le speculaire). Oublier FRAGMENT_BIT ici declenche une erreur de
    // validation des que le fragment shader accede au binding.
    uboBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    uboBinding.pImmutableSamplers = nullptr;

    // PHASE 11, binding 1 : orthophoto du terrain. Declare pour TOUS les
    // materiaux, y compris ceux qui ne l'echantillonnent pas (cube, arbres) : un
    // descriptor present dans le set mais non lu par un shader est parfaitement
    // legal, alors qu'avoir deux layouts differents obligerait a deux jeux de
    // pipelines et deux pools. Le binding pointe toujours sur une image valide -
    // un texel blanc tant qu'aucune photo n'est chargee.
    VkDescriptorSetLayoutBinding textureBinding{};
    textureBinding.binding = 1;
    textureBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    textureBinding.descriptorCount = 1;
    textureBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    textureBinding.pImmutableSamplers = nullptr;

    // PHASE 27, binding 2 : bruit periodique du micro-relief du sol. Toujours
    // relie (cree avant les descriptor sets), lu par terrain.frag seulement.
    VkDescriptorSetLayoutBinding detailBinding = textureBinding;
    detailBinding.binding = 2;

    const std::array<VkDescriptorSetLayoutBinding, 3> bindings{ uboBinding, textureBinding,
                                                                detailBinding };

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();

    if (vkCreateDescriptorSetLayout(context_.device(), &layoutInfo, nullptr,
                                    &descriptorSetLayout_) != VK_SUCCESS) {
        throw std::runtime_error("Echec de la creation du descriptor set layout.");
    }
}

void Renderer::createDescriptorPool() {
    std::array<VkDescriptorPoolSize, 2> poolSizes{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[0].descriptorCount = static_cast<uint32_t>(kMaxFramesInFlight);
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    // Deux images par set depuis la Phase 27 : orthophoto et bruit de detail.
    poolSizes[1].descriptorCount = static_cast<uint32_t>(2 * kMaxFramesInFlight);

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    poolInfo.maxSets = static_cast<uint32_t>(kMaxFramesInFlight);

    if (vkCreateDescriptorPool(context_.device(), &poolInfo, nullptr, &descriptorPool_) != VK_SUCCESS) {
        throw std::runtime_error("Echec de la creation du descriptor pool.");
    }
}

void Renderer::createDescriptorSets() {
    std::vector<VkDescriptorSetLayout> layouts(kMaxFramesInFlight, descriptorSetLayout_);

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = descriptorPool_;
    allocInfo.descriptorSetCount = static_cast<uint32_t>(kMaxFramesInFlight);
    allocInfo.pSetLayouts = layouts.data();

    descriptorSets_.resize(kMaxFramesInFlight);
    if (vkAllocateDescriptorSets(context_.device(), &allocInfo, descriptorSets_.data()) != VK_SUCCESS) {
        throw std::runtime_error("Echec de l'allocation des descriptor sets.");
    }

    // Chaque set pointe vers l'uniform buffer de sa frame-in-flight.
    for (int i = 0; i < kMaxFramesInFlight; ++i) {
        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = uniformBuffers_[i]->handle();
        bufferInfo.offset = 0;
        bufferInfo.range = sizeof(UniformBufferObject);

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = descriptorSets_[i];
        write.dstBinding = 0;
        write.dstArrayElement = 0;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        write.descriptorCount = 1;
        write.pBufferInfo = &bufferInfo;

        vkUpdateDescriptorSets(context_.device(), 1, &write, 0, nullptr);
    }

    writeTextureDescriptors();
}

void Renderer::writeTextureDescriptors() {
    // La texture de terrain si elle existe, le texel blanc sinon. Le binding
    // n'est JAMAIS laisse vide : un descriptor non ecrit auquel un shader accede
    // est une erreur de validation, et selon le driver un plantage.
    const Texture& texture = terrainTexture_ ? *terrainTexture_ : *whiteTexture_;

    VkDescriptorImageInfo imageInfo{};
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfo.imageView = texture.view();
    imageInfo.sampler = texture.sampler();

    for (int i = 0; i < kMaxFramesInFlight; ++i) {
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = descriptorSets_[i];
        write.dstBinding = 1;
        write.dstArrayElement = 0;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.descriptorCount = 1;
        write.pImageInfo = &imageInfo;

        vkUpdateDescriptorSets(context_.device(), 1, &write, 0, nullptr);

        // PHASE 27 : bruit de detail, binding 2. Reecrit ici avec le reste pour
        // qu'aucun chemin ne laisse le binding vide.
        VkDescriptorImageInfo detailInfo{};
        detailInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        detailInfo.imageView = detailTexture_->view();
        detailInfo.sampler = detailTexture_->sampler();
        VkWriteDescriptorSet detailWrite = write;
        detailWrite.dstBinding = 2;
        detailWrite.pImageInfo = &detailInfo;
        vkUpdateDescriptorSets(context_.device(), 1, &detailWrite, 0, nullptr);
    }
}

void Renderer::setTerrainTexture(const TextureData& data) {
    // Le GPU peut encore lire l'ancienne texture : on attend avant de la
    // remplacer. Sans consequence, l'appel ayant lieu a l'initialisation.
    waitIdle();

    terrainTexture_ = std::make_unique<Texture>(context_, commandPool_,
                                                context_.graphicsQueue(), data);
    writeTextureDescriptors();

    logInfo("Orthophoto reliee au terrain : " + std::to_string(data.width) + "x" +
            std::to_string(data.height) + ", " +
            std::to_string(terrainTexture_->deviceSize() / (1024 * 1024)) +
            " Mo sur le GPU.");
}

void Renderer::setTerrainTextureFromFile(const TextureData& header, const std::string& path) {
    waitIdle();

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Texture : fichier introuvable ou illisible : " + path);
    }
    terrainTexture_ = std::make_unique<Texture>(
        context_, commandPool_, context_.graphicsQueue(), header,
        [&file](void* destination, uint64_t offset, uint64_t bytes) {
            readTexturePayload(file, offset, bytes, destination);
        });
    writeTextureDescriptors();

    logInfo("Orthophoto reliee au terrain : " + std::to_string(header.width) + "x" +
            std::to_string(header.height) + ", " +
            std::to_string(terrainTexture_->deviceSize() / (1024 * 1024)) +
            " Mo sur le GPU, lue par tranches directement dans le staging.");
}

void Renderer::setOrthoMix(float mix) {
    options_.orthoMix = std::clamp(mix, 0.0f, 1.0f);
}

void Renderer::setSun(const glm::vec3& direction, float intensity, const glm::vec3& color,
                      float ambient) {
    const float length = glm::length(direction);
    if (std::isfinite(length) && length > 1e-4f) {
        const glm::vec3 unit = direction / length;
        sun_ = glm::vec4(unit, std::clamp(intensity, 0.0f, 1.0f));
    } else {
        // Direction degeneree : on garde l'ancienne et on ne met a jour que
        // l'intensite. Eteindre l'image serait une reaction disproportionnee a
        // une entree douteuse.
        sun_.w = std::clamp(intensity, 0.0f, 1.0f);
    }

    // PHASE 27 : le multiplicateur d'ambiante peut depasser 1 (ciel couvert :
    // la lumiere diffuse remplace le direct, voir weatherVisualsFor). Borne
    // haute a 4, garde-fou contre une valeur absurde, et non plus 1 - qui
    // avait silencieusement annule tout l'effet a la premiere capture.
    sunColor_ = glm::vec4(glm::clamp(color, glm::vec3(0.0f), glm::vec3(1.0f)),
                          std::isfinite(ambient) ? std::clamp(ambient, 0.0f, 4.0f) : 1.0f);
}

void Renderer::setWeatherVisuals(const glm::vec3& fogColor, float fogDensityPerMeter,
                                 float precipitation, bool snow,
                                 const glm::vec3& fallVelocity, float groundDarkening,
                                 float timeSeconds) {
    // Toute entree non finie est ramenee au NEUTRE (pas de brouillard, pas de
    // pluie) : une valeur douteuse ne doit ni noircir ni blanchir l'image.
    const auto finiteOr = [](float v, float fallback) {
        return std::isfinite(v) ? v : fallback;
    };
    fog_ = glm::vec4(glm::clamp(fogColor, glm::vec3(0.0f), glm::vec3(1.0f)),
                     std::max(0.0f, finiteOr(fogDensityPerMeter, 0.0f)));
    precip_ = glm::vec4(std::clamp(finiteOr(precipitation, 0.0f), 0.0f, 1.0f),
                        snow ? 1.0f : 0.0f,
                        finiteOr(timeSeconds, 0.0f),
                        std::clamp(finiteOr(groundDarkening, 0.0f), 0.0f, 1.0f));
    const bool fallOk = std::isfinite(fallVelocity.x) && std::isfinite(fallVelocity.y) &&
                        std::isfinite(fallVelocity.z);
    fall_ = glm::vec4(fallOk ? fallVelocity : glm::vec3(0.0f, -6.5f, 0.0f), 0.0f);
}

void Renderer::setGroundDetail(float strength, float fadeStart, float fadeEnd,
                               float patternMeters) {
    const float start = std::isfinite(fadeStart) ? std::max(0.0f, fadeStart) : 0.0f;
    const float end = std::isfinite(fadeEnd) ? std::max(start + 0.01f, fadeEnd) : start + 1.0f;
    detail_ = glm::vec4(std::isfinite(strength) ? std::clamp(strength, 0.0f, 1.0f) : 0.0f,
                        start, end,
                        std::isfinite(patternMeters) ? std::max(0.01f, patternMeters) : 1.0f);
}

void Renderer::updateUniformBuffer(uint32_t frameIndex, const Camera& camera) {
    UniformBufferObject ubo{};

    // La model matrix n'est plus ici : elle est propre a chaque objet et part
    // par push constant au moment de son draw (voir recordCommandBuffer).
    ubo.view = camera.viewMatrix();
    // Position de la camera : utilisee par le fragment shader pour le speculaire.
    ubo.viewPos = camera.position();
    // z = dosage orthophoto. Force a zero tant qu'aucune texture n'est reliee :
    // le binding porte alors un texel blanc, et laisser passer un dosage non nul
    // afficherait un terrain uniformement blanc au lieu du repli procedural.
    const float mix = terrainTexture_ ? options_.orthoMix : 0.0f;
    // w = force de l'indice adret/ubac (PHASE 25). Le champ etait marque
    // "reserve" depuis la Phase 11 ; il l'est resté jusqu'ici.
    ubo.terrain = glm::vec4(options_.heightRange.x, options_.heightRange.y, mix, aspectTint_);

    // Exactement la matrice que projectionMatrix() a rendue a l'appelant pour
    // son frustum culling : une seule definition, donc aucun risque que le
    // culling et le rendu ne voient pas le meme tronc de vision.
    ubo.proj = projectionMatrix();

    // PHASE 23 : la lumiere du cycle jour/nuit. Une seule ecriture par image,
    // dans le bloc qui portait deja tout ce qui est constant sur la frame.
    ubo.sun = sun_;
    ubo.sunColor = sunColor_;
    ubo.fog = fog_;
    ubo.precip = precip_;
    ubo.fall = fall_;
    ubo.detail = detail_;
    ubo.terrainGrid = terrainGrid_;

    std::memcpy(uniformBuffers_[frameIndex]->mapped(), &ubo, sizeof(ubo));
}

// ---------------------------------------------------------------------------
// Synchronisation
// ---------------------------------------------------------------------------
void Renderer::createFrameSyncObjects() {
    imageAvailableSemaphores_.resize(kMaxFramesInFlight);
    inFlightFences_.resize(kMaxFramesInFlight);

    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    // Cree signalee pour que la premiere frame n'attende pas indefiniment.
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    VkDevice device = context_.device();
    for (int i = 0; i < kMaxFramesInFlight; ++i) {
        if (vkCreateSemaphore(device, &semaphoreInfo, nullptr, &imageAvailableSemaphores_[i]) != VK_SUCCESS ||
            vkCreateFence(device, &fenceInfo, nullptr, &inFlightFences_[i]) != VK_SUCCESS) {
            throw std::runtime_error("Echec de la creation des objets de synchro (frame).");
        }
    }
}

void Renderer::createImageSyncObjects() {
    // Un semaphore "renderFinished" par image de swapchain : evite qu'un semaphore
    // soit reutilise alors qu'une presentation precedente ne l'a pas encore attendu.
    const uint32_t count = swapChain_->imageCount();
    renderFinishedSemaphores_.resize(count);

    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    for (uint32_t i = 0; i < count; ++i) {
        if (vkCreateSemaphore(context_.device(), &semaphoreInfo, nullptr,
                              &renderFinishedSemaphores_[i]) != VK_SUCCESS) {
            throw std::runtime_error("Echec de la creation des objets de synchro (image).");
        }
    }

    // Aucune image n'est occupee au demarrage.
    imagesInFlight_.assign(count, VK_NULL_HANDLE);
}

void Renderer::destroyImageSyncObjects() {
    for (VkSemaphore s : renderFinishedSemaphores_) {
        vkDestroySemaphore(context_.device(), s, nullptr);
    }
    renderFinishedSemaphores_.clear();
    imagesInFlight_.clear();
}

// ---------------------------------------------------------------------------
// Transferts asynchrones (PHASE 12)
// ---------------------------------------------------------------------------
// Rappel du probleme (Phase 10) : chaque tuile de terrain televersee faisait un
// vkQueueWaitIdle, c'est-a-dire vidait la queue graphique. Quatre tuiles par
// frame suffisaient a manger le budget d'image, d'ou le plafond artificiel
// maxUploadsPerFrame = 4 et les trous a l'horizon en deplacement rapide.
//
// Ce GPU (Intel UHD 770) n'expose AUCUNE famille de transfert dediee : famille 0
// graphics/compute/transfer, famille 1 video sans bit transfert (journalise au
// demarrage par VulkanContext). Les transferts partagent donc forcement la queue
// graphique, et une "soumission asynchrone separee sur la meme queue" n'y
// gagnerait rien de plus que ce qui suit, tout en ajoutant une soumission et une
// fence par frame.
//
// Choix retenu : les copies sont enregistrees DANS le command buffer de la
// frame, en tete, avant vkCmdBeginRenderPass, suivies d'une seule barriere
// memoire transfert -> vertex input. Proprietes :
//   - aucune attente CPU nulle part sur le chemin critique ;
//   - une seule soumission par frame, comme avant ;
//   - la dependance memoire est EXPLICITE et verifiable par la validation de
//     synchronisation, au lieu de reposer sur un vidage de queue ;
//   - aucun transfert de propriete de famille de queue a ecrire (meme famille),
//     donc pas de code de synchronisation impossible a eprouver ici ;
//   - la tuile est lisible DES CETTE FRAME : le repli sur l'ancetre ne sert donc
//     qu'aux tuiles pas encore CONSTRUITES (budget), pas a une latence de
//     transfert.
void Renderer::enqueueCopy(std::unique_ptr<VulkanBuffer> staging, VkBuffer destination,
                           VkDeviceSize destinationOffset) {
    if (!staging || destination == VK_NULL_HANDLE) {
        return;
    }
    PendingCopy copy;
    copy.size = staging->size();
    copy.destination = destination;
    copy.destinationOffset = destinationOffset;
    copy.staging = std::move(staging);
    stagingBytesInFlight_ += static_cast<std::size_t>(copy.size);
    pendingCopies_.push_back(std::move(copy));

    // Garde-fou memoire : un televersement geant (le maillage d'un bloc entier
    // sur le chemin --no-lod) ne doit pas garder des gigaoctets de staging le
    // temps de trois frames. Voir kMaxPendingStagingBytes.
    if (stagingBytesInFlight_ > kMaxPendingStagingBytes) {
        flushCopiesNow();
    }
}

void Renderer::flushCopiesNow() {
    if (pendingCopies_.empty()) {
        return;
    }
    VkDevice device = context_.device();

    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool = commandPool_;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(device, &allocInfo, &commandBuffer) != VK_SUCCESS) {
        return; // les copies restent en attente : elles partiront avec la frame
    }

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(commandBuffer, &beginInfo);

    recordCopyAfterReadBarrier(commandBuffer);
    for (const PendingCopy& copy : pendingCopies_) {
        VkBufferCopy region{};
        region.dstOffset = copy.destinationOffset;
        region.size = copy.size;
        vkCmdCopyBuffer(commandBuffer, copy.staging->handle(), copy.destination,
                        1, &region);
    }
    vkEndCommandBuffer(commandBuffer);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;

    vkQueueSubmit(context_.graphicsQueue(), 1, &submitInfo, VK_NULL_HANDLE);
    // Le seul vkQueueWaitIdle qui subsiste sur un upload de geometrie, et il
    // n'est atteint que par un maillage de plus de 128 Mo, donc jamais par une
    // tuile de streaming. Meme forme que VulkanBuffer::copyFrom.
    vkQueueWaitIdle(context_.graphicsQueue());

    vkFreeCommandBuffers(device, commandPool_, 1, &commandBuffer);

    std::size_t freed = 0;
    for (const PendingCopy& copy : pendingCopies_) {
        freed += static_cast<std::size_t>(copy.size);
    }
    stagingBytesInFlight_ -= freed;
    pendingCopies_.clear(); // detruit les staging : le GPU a fini de les lire

    logInfo("Transfert synchrone de secours : " +
            std::to_string(freed / (1024 * 1024)) +
            " Mo de staging depassaient le plafond, copie attendue puis liberee.");
}

void Renderer::recordPendingCopies(VkCommandBuffer commandBuffer) {
    if (pendingCopies_.empty()) {
        return;
    }

    recordCopyAfterReadBarrier(commandBuffer);
    for (PendingCopy& copy : pendingCopies_) {
        VkBufferCopy region{};
        region.dstOffset = copy.destinationOffset;
        region.size = copy.size;
        vkCmdCopyBuffer(commandBuffer, copy.staging->handle(), copy.destination,
                        1, &region);
        // Le staging passe en attente de fin d'execution. Meme delai que les
        // meshes retires : kMaxFramesInFlight + 1 vieillissements, ce qui
        // garantit que la frame qui porte cette copie est terminee.
        retiredStaging_.push_back({ std::move(copy.staging), kMaxFramesInFlight + 1 });
    }
    pendingCopies_.clear();

    // UNE barriere pour toutes les copies : elles ecrivent toutes des buffers de
    // geometrie, et elles sont toutes lues au meme endroit (l'assemblage des
    // sommets). Une barriere par buffer serait exact aussi, mais ferait N
    // barrieres pour une seule dependance reelle.
    //
    // VK_ACCESS_INDEX_READ_BIT autant que VERTEX_ATTRIBUTE_READ : un Mesh
    // televerse ses sommets ET ses indices.
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT;

    vkCmdPipelineBarrier(commandBuffer,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
                         0,
                         1, &barrier,
                         0, nullptr,
                         0, nullptr);
}

// ---------------------------------------------------------------------------
// Enregistrement des commandes
// ---------------------------------------------------------------------------
void Renderer::recordCommandBuffer(VkCommandBuffer commandBuffer, uint32_t imageIndex,
                                   const std::vector<DrawItem>& items,
                                   const std::vector<InstancedDrawItem>& instanced) {
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

    if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
        throw std::runtime_error("Echec du debut d'enregistrement du command buffer.");
    }

    // PHASE 12 : transferts d'abord, HORS du render pass. Un vkCmdCopyBuffer est
    // interdit a l'interieur d'un render pass, et la barriere doit de toute
    // facon precede tout draw qui lirait la geometrie fraichement copiee.
    recordPendingCopies(commandBuffer);

    // Deux valeurs de clear : couleur (attache 0) et profondeur (attache 1).
    std::array<VkClearValue, 2> clearValues{};
    clearValues[0].color = { { options_.clearColor.r,
                               options_.clearColor.g,
                               options_.clearColor.b, 1.0f } };
    clearValues[1].depthStencil = { 1.0f, 0 }; // 1.0 = plan lointain

    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = swapChain_->renderPass();
    renderPassInfo.framebuffer = swapChain_->framebuffer(imageIndex);
    renderPassInfo.renderArea.offset = { 0, 0 };
    renderPassInfo.renderArea.extent = swapChain_->extent();
    renderPassInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
    renderPassInfo.pClearValues = clearValues.data();

    vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

    // Viewport et scissor dynamiques : adaptes a l'extent courant. Fixes une
    // seule fois : ils font partie de l'etat du command buffer, pas du pipeline.
    const VkExtent2D extent = swapChain_->extent();
    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(extent.width);
    viewport.height = static_cast<float>(extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.offset = { 0, 0 };
    scissor.extent = extent;
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

    // Un passage par materiau plutot qu'un parcours dans l'ordre d'arrivee :
    // les objets d'un meme materiau sont dessines a la suite, donc le pipeline
    // n'est lie qu'une fois par materiau reellement utilise (au plus deux fois
    // par frame ici). Avec quelques dizaines d'objets, ce tri "par balayage"
    // coute moins cher qu'un vrai tri, et evite de copier la liste.
    //
    // PHASE 32 : le TERRAIN en dernier. Son shader de fragment est le plus
    // lourd du jeu (orthophoto, micro-relief, adret/ubac, brouillard) ; dessine
    // apres les troncs, les houppiers et l'herbe, il trouve leur profondeur
    // deja ecrite et le test de profondeur PRECOCE ecarte ses fragments caches
    // avant de les ombrer. Tout est opaque et le test est LESS : l'ordre ne
    // change l'image qu'a profondeur RIGOUREUSEMENT EGALE - le pied d'un brin
    // ou d'un tronc, pose exactement sur la surface du sol. Mesure : 24 pixels
    // sur 921 600 en foret, et une variante qui rend ces egalites au terrain
    // redonne l'image de reference au bit pres (NOTES-PHASE32.md). Gain : -26 %
    // de temps de rendu en foret (lavapipe).
    static constexpr std::array<Material, kMaterialCount> kDrawOrder{
        Material::Object, Material::Foliage, Material::Terrain };

    // PHASE 32 : liaisons de buffers EVITEES quand elles ne changent rien. Les
    // tuiles de terrain partagent une seule topologie : elle est liee une fois
    // par image au lieu d'une fois par tuile. Les liaisons survivent aux
    // changements de pipeline (etat du command buffer, pas du pipeline).
    VkBuffer boundVertexBuffer = VK_NULL_HANDLE;
    VkDeviceSize boundVertexOffset = 0;
    VkBuffer boundIndexBuffer = VK_NULL_HANDLE;
    VkDeviceSize boundIndexOffset = 0;
    VkIndexType boundIndexType = VK_INDEX_TYPE_MAX_ENUM;
    // Decalage de LIAISON plutot que vertexOffset dans le draw : gl_VertexIndex
    // reste ainsi l'indice LOCAL du sommet dans son maillage, ce dont
    // terrain.vert se sert pour retrouver sa place dans la grille de la tuile.
    const auto bindMesh = [&](const Mesh& mesh) {
        const GpuRange& vertices = mesh.vertexRange();
        if (vertices.buffer != boundVertexBuffer || vertices.offset != boundVertexOffset) {
            vkCmdBindVertexBuffers(commandBuffer, 0, 1, &vertices.buffer, &vertices.offset);
            boundVertexBuffer = vertices.buffer;
            boundVertexOffset = vertices.offset;
        }
        const GpuRange& indices = mesh.indexRange();
        if (indices.buffer != boundIndexBuffer || indices.offset != boundIndexOffset ||
            mesh.indexType() != boundIndexType) {
            vkCmdBindIndexBuffer(commandBuffer, indices.buffer, indices.offset, mesh.indexType());
            boundIndexBuffer = indices.buffer;
            boundIndexOffset = indices.offset;
            boundIndexType = mesh.indexType();
        }
    };

    for (const Material material : kDrawOrder) {
        const uint32_t m = static_cast<uint32_t>(material);
        bool pipelineBound = false;
        // PHASE 32 : un maillage de terrain (16 octets par sommet) ne se lit
        // qu'avec le pipeline du terrain, et reciproquement.
        const VertexLayout expectedLayout = (material == Material::Terrain)
            ? VertexLayout::Terrain : VertexLayout::Object;

        for (const DrawItem& item : items) {
            if (item.material != material) {
                continue;
            }
            if (item.mesh >= meshes_.size() || !meshes_[item.mesh]) {
                // Poignee invalide, ou emplacement libere par removeMesh depuis
                // que la liste a ete construite : on ignore l'objet plutot que
                // de laisser le driver lire hors du tableau (ou de dereferencer
                // un pointeur nul). Signaler serait bruyant a 60 Hz.
                continue;
            }
            if (meshes_[item.mesh]->layout() != expectedLayout) {
                continue; // meme raison : le GPU lirait des octets d'un autre format
            }

            if (!pipelineBound) {
                vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  pipelines_[m]->handle());
                // Descriptor set (UBO) de la frame-in-flight courante. Relie
                // apres chaque changement de pipeline : les layouts des deux
                // pipelines sont identiques donc compatibles, mais s'appuyer
                // sur cette compatibilite serait fragile au moindre ajout.
                vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        pipelines_[m]->layout(), 0, 1,
                                        &descriptorSets_[currentFrame_], 0, nullptr);
                pipelineBound = true;
            }

            // Transformation et parametres propres a cet objet. Tous sont
            // pousses d'un seul coup : ils sont contigus dans DrawItemPush,
            // dont le layout reproduit exactement le bloc push_constant des
            // shaders (voir DrawItem.hpp).
            const DrawItemPush push{ item.model, item.params, item.grid };
            vkCmdPushConstants(commandBuffer, pipelines_[m]->layout(),
                               VK_SHADER_STAGE_VERTEX_BIT, 0,
                               static_cast<uint32_t>(sizeof(DrawItemPush)), &push);

            const Mesh& mesh = *meshes_[item.mesh];
            bindMesh(mesh);
            mesh.draw(commandBuffer);
        }

        // --- Geometrie INSTANCIEE du meme materiau (Phase 11) ---------------
        // Dans le meme passage par materiau que ci-dessus, pour que le pipeline
        // ne soit lie qu'une fois, y compris quand un materiau porte a la fois
        // des objets simples et des instances.
        for (std::size_t index = 0; index < instanced.size(); ++index) {
            const InstancedDrawItem& item = instanced[index];
            if (item.material != material || item.count == 0) {
                continue;
            }
            if (item.mesh >= meshes_.size() || !meshes_[item.mesh] ||
                meshes_[item.mesh]->layout() != expectedLayout) {
                continue;
            }

            if (!pipelineBound) {
                vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  pipelines_[m]->handle());
                vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        pipelines_[m]->layout(), 0, 1,
                                        &descriptorSets_[currentFrame_], 0, nullptr);
                pipelineBound = true;
            }

            const DrawItemPush push{ glm::mat4(1.0f), item.params };
            vkCmdPushConstants(commandBuffer, pipelines_[m]->layout(),
                               VK_SHADER_STAGE_VERTEX_BIT, 0,
                               static_cast<uint32_t>(sizeof(DrawItemPush)), &push);

            const Mesh& mesh = *meshes_[item.mesh];
            bindMesh(mesh);

            // Binding 1 : la tranche du buffer d'instances qui appartient a cet
            // objet. L'offset a ete calcule par uploadInstances, dans le meme
            // ordre que cette liste.
            const VkBuffer instanceBuffer = instanceBuffers_[currentFrame_]->handle();
            const VkDeviceSize offset = instanceOffsets_[index];
            vkCmdBindVertexBuffers(commandBuffer, 1, 1, &instanceBuffer, &offset);

            mesh.drawInstanced(commandBuffer, item.count);
        }
    }

    // --- PHASE 27 : precipitation, en DERNIER --------------------------------
    // Apres toute la geometrie, un triangle plein ecran melange par alpha, sans
    // test ni ecriture de profondeur. SAUTEE ENTIEREMENT par temps sec : pas de
    // pipeline lie, pas de draw, donc un cout rigoureusement nul les jours sans
    // precipitation.
    if (precip_.x > 0.0f) {
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          precipitationPipeline_->handle());
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                precipitationPipeline_->layout(), 0, 1,
                                &descriptorSets_[currentFrame_], 0, nullptr);
        vkCmdDraw(commandBuffer, 3, 1, 0, 0);
    }

    vkCmdEndRenderPass(commandBuffer);

    if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
        throw std::runtime_error("Echec de la fin d'enregistrement du command buffer.");
    }
}

// ---------------------------------------------------------------------------
// Boucle de rendu
// ---------------------------------------------------------------------------
void Renderer::uploadInstances(const std::vector<InstancedDrawItem>& instanced) {
    instanceOffsets_.assign(instanced.size(), 0);
    lastInstanceCount_ = 0;

    VkDeviceSize total = 0;
    for (const InstancedDrawItem& item : instanced) {
        if (item.instances == nullptr || item.count == 0) {
            continue;
        }
        total += static_cast<VkDeviceSize>(item.count) * sizeof(InstanceData);
        lastInstanceCount_ += item.count;
    }
    if (total == 0) {
        return;
    }

    std::unique_ptr<VulkanBuffer>& buffer = instanceBuffers_[currentFrame_];
    if (!buffer || buffer->size() < total) {
        // Agrandissement avec de la marge (x1,5) : la vegetation visible varie
        // continuement, et reallouer a chaque poignee d'instances de plus
        // couterait une allocation Vulkan par frame.
        //
        // Remplacer le buffer ICI est sans risque : drawFrame a deja attendu la
        // fence de cette frame-in-flight, donc plus aucune commande soumise ne
        // lit l'ancien buffer.
        const VkDeviceSize capacity = std::max<VkDeviceSize>(
            static_cast<VkDeviceSize>(static_cast<double>(total) * 1.5),
            64u * 1024u);
        buffer = std::make_unique<VulkanBuffer>(
            context_, capacity,
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        buffer->map();
        logInfo("Instances : buffer porte a " +
                std::to_string(capacity / 1024) + " Ko (frame " +
                std::to_string(currentFrame_) + ").");
    }

    auto* destination = static_cast<unsigned char*>(buffer->mapped());
    VkDeviceSize offset = 0;
    for (std::size_t index = 0; index < instanced.size(); ++index) {
        const InstancedDrawItem& item = instanced[index];
        if (item.instances == nullptr || item.count == 0) {
            continue;
        }
        const VkDeviceSize bytes = static_cast<VkDeviceSize>(item.count) * sizeof(InstanceData);
        std::memcpy(destination + offset, item.instances, static_cast<std::size_t>(bytes));
        instanceOffsets_[index] = offset;
        offset += bytes;
    }
}

void Renderer::drawFrame(const Camera& camera, const std::vector<DrawItem>& items) {
    static const std::vector<InstancedDrawItem> kNone;
    drawFrame(camera, items, kNone);
}

void Renderer::drawFrame(const Camera& camera, const std::vector<DrawItem>& items,
                         const std::vector<InstancedDrawItem>& instanced) {
    VkDevice device = context_.device();

    // 1. Attendre que la frame-in-flight courante soit libre.
    vkWaitForFences(device, 1, &inFlightFences_[currentFrame_], VK_TRUE, UINT64_MAX);

    // Vieillissement des meshes retires (Phase 10, streaming par tuiles) : place
    // ICI, apres l'attente de la fence, donc a un instant ou l'on sait qu'une
    // frame de plus est reellement terminee. Un compteur atteint zero => plus
    // aucune frame en vol ne peut referencer ces buffers, ils sont detruits.
    for (std::size_t i = retiredMeshes_.size(); i-- > 0;) {
        if (--retiredMeshes_[i].framesRemaining <= 0) {
            if (i + 1 != retiredMeshes_.size()) {
                retiredMeshes_[i] = std::move(retiredMeshes_.back());
            }
            retiredMeshes_.pop_back();
        }
    }

    // PHASE 12 : meme vieillissement pour les buffers staging dont la copie a
    // ete soumise. A zero, la frame qui portait la copie est forcement terminee,
    // donc plus personne ne lit le staging : il peut partir.
    for (std::size_t i = retiredStaging_.size(); i-- > 0;) {
        if (--retiredStaging_[i].framesRemaining <= 0) {
            stagingBytesInFlight_ -= static_cast<std::size_t>(
                retiredStaging_[i].staging->size());
            if (i + 1 != retiredStaging_.size()) {
                retiredStaging_[i] = std::move(retiredStaging_.back());
            }
            retiredStaging_.pop_back();
        }
    }

    // 1 bis. Recopier les instances de la frame. Place ICI, apres l'attente de
    // la fence, parce que le buffer d'instances de cette frame-in-flight peut
    // alors etre reecrit (ou remplace par un plus grand) sans qu'aucune commande
    // en vol ne le lise.
    uploadInstances(instanced);

    // 2. Acquerir la prochaine image.
    uint32_t imageIndex = 0;
    VkResult result = vkAcquireNextImageKHR(
        device, swapChain_->handle(), UINT64_MAX,
        imageAvailableSemaphores_[currentFrame_], VK_NULL_HANDLE, &imageIndex);

    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        recreateSwapChain();
        return; // on repart proprement a la frame suivante.
    }
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        throw std::runtime_error("Echec de l'acquisition d'une image de swapchain.");
    }

    // Si une frame precedente utilise encore cette image, attendre sa fence.
    if (imagesInFlight_[imageIndex] != VK_NULL_HANDLE) {
        vkWaitForFences(device, 1, &imagesInFlight_[imageIndex], VK_TRUE, UINT64_MAX);
    }
    imagesInFlight_[imageIndex] = inFlightFences_[currentFrame_];

    // Mise a jour de l'UBO de cette frame (matrices MVP a partir de la camera).
    updateUniformBuffer(currentFrame_, camera);

    // 3. Enregistrer les commandes.
    vkResetCommandBuffer(commandBuffers_[currentFrame_], 0);
    recordCommandBuffer(commandBuffers_[currentFrame_], imageIndex, items, instanced);

    // 4. Soumettre a la graphics queue.
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

    const VkSemaphore waitSemaphores[] = { imageAvailableSemaphores_[currentFrame_] };
    const VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = waitSemaphores;
    submitInfo.pWaitDstStageMask = waitStages;

    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffers_[currentFrame_];

    // Signale le semaphore associe a l'image (et non a la frame-in-flight).
    const VkSemaphore signalSemaphores[] = { renderFinishedSemaphores_[imageIndex] };
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = signalSemaphores;

    // Reset de la fence juste avant la soumission qui va la re-signaler.
    vkResetFences(device, 1, &inFlightFences_[currentFrame_]);

    if (vkQueueSubmit(context_.graphicsQueue(), 1, &submitInfo,
                      inFlightFences_[currentFrame_]) != VK_SUCCESS) {
        throw std::runtime_error("Echec de la soumission du command buffer.");
    }

    // 5. Presenter.
    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = signalSemaphores;

    const VkSwapchainKHR swapChains[] = { swapChain_->handle() };
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = swapChains;
    presentInfo.pImageIndices = &imageIndex;

    result = vkQueuePresentKHR(context_.presentQueue(), &presentInfo);

    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR ||
        window_.wasResized()) {
        window_.resetResizedFlag();
        recreateSwapChain();
    } else if (result != VK_SUCCESS) {
        throw std::runtime_error("Echec de la presentation d'une image.");
    }

    currentFrame_ = (currentFrame_ + 1) % kMaxFramesInFlight;
}

// ---------------------------------------------------------------------------
// Resize : swapchain ET depth buffer recrees ensemble (le depth vit dans SwapChain)
// ---------------------------------------------------------------------------
void Renderer::recreateSwapChain() {
    // Si la fenetre est minimisee (taille 0), on attend qu'elle reprenne une taille.
    int width = 0;
    int height = 0;
    window_.getFramebufferSize(width, height);
    while (width == 0 || height == 0) {
        window_.getFramebufferSize(width, height);
        window_.waitEvents();
    }

    // Attendre que le GPU n'utilise plus les ressources qu'on va recreer.
    vkDeviceWaitIdle(context_.device());

    swapChain_->recreate();

    // Le nombre d'images peut changer : on recree les objets de synchro par image.
    destroyImageSyncObjects();
    createImageSyncObjects();
}

} // namespace vercora
