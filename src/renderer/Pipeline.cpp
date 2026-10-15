#include "renderer/Pipeline.hpp"

#include "core/Logger.hpp"
#include "core/Paths.hpp"
#include "renderer/DrawItem.hpp"
#include "renderer/VertexFormat.hpp"

#include <glm/glm.hpp>

#include <array>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace vercora {

Pipeline::Pipeline(VkDevice device, VkRenderPass renderPass, VkDescriptorSetLayout descriptorSetLayout,
                   const ShaderSet& shaders)
    : device_(device) {
    // PHASE 21 : le dossier shaders\ a cote de l'executable s'il existe (cas du
    // package distribue), sinon SHADERS_OUTPUT_DIR defini par CMake (cas du
    // developpement). Voir core/Paths.hpp.
    const std::string shaderDir = shadersDir() + "/";
    const std::vector<char> vertCode = readFile(shaderDir + shaders.vertexSpv);
    const std::vector<char> fragCode = readFile(shaderDir + shaders.fragmentSpv);

    VkShaderModule vertModule = createShaderModule(vertCode);
    VkShaderModule fragModule = createShaderModule(fragCode);

    VkPipelineShaderStageCreateInfo vertStage{};
    vertStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vertStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertStage.module = vertModule;
    vertStage.pName = "main";

    VkPipelineShaderStageCreateInfo fragStage{};
    fragStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fragStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragStage.module = fragModule;
    fragStage.pName = "main";

    const std::array<VkPipelineShaderStageCreateInfo, 2> stages = { vertStage, fragStage };

    // Vertex input : un binding entrelace, au format de la famille de shaders
    // (PHASE 32, voir renderer/GpuVertex.hpp) - ObjectVertex pour les objets
    // et la vegetation, TerrainVertex pour le terrain.
    const VkVertexInputBindingDescription bindingDescription =
        shaders.terrainVertexLayout ? terrainBindingDescription()
                                    : objectBindingDescription();
    const std::array<VkVertexInputAttributeDescription, 4> attributeDescriptions =
        shaders.terrainVertexLayout ? terrainAttributeDescriptions()
                                    : objectAttributeDescriptions();

    // PHASE 11 : un materiau instancie decrit DEUX bindings (sommets et
    // instances) et concatene leurs attributs. Les tableaux sont construits ici
    // meme pour que leur duree de vie couvre l'appel a vkCreateGraphicsPipelines.
    const auto instanceBinding = instanceBindingDescription();
    const auto instanceAttributes = instanceAttributeDescriptions();

    std::vector<VkVertexInputBindingDescription> bindings{ bindingDescription };
    std::vector<VkVertexInputAttributeDescription> attributes(
        attributeDescriptions.begin(), attributeDescriptions.end());

    // PHASE 27 : une passe plein ecran ne lit AUCUN buffer de sommets.
    if (shaders.fullscreenOverlay) {
        bindings.clear();
        attributes.clear();
    }

    if (shaders.usesInstancing) {
        bindings.push_back(instanceBinding);
        attributes.insert(attributes.end(),
                          instanceAttributes.begin(), instanceAttributes.end());
    }

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = static_cast<uint32_t>(bindings.size());
    vertexInput.pVertexBindingDescriptions = bindings.data();
    vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size());
    vertexInput.pVertexAttributeDescriptions = attributes.data();

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    inputAssembly.primitiveRestartEnable = VK_FALSE;

    // Viewport/scissor dynamiques : leur contenu est fixe au moment du rendu.
    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_NONE; // pas de culling : triangle toujours visible
    rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;
    if (shaders.cullBackFaces) {
        // PHASE 32 : voir ShaderSet::cullBackFaces. Le terrain est enroule
        // (v00, v01, v11) avec Z vers le sud ; une fois l'axe Y de la
        // projection retourne (Renderer::projectionMatrix), une face vue
        // d'en haut est tramee dans le sens ANTIHORAIRE du framebuffer.
        rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
        rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    }
    rasterizer.depthBiasEnable = VK_FALSE;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.sampleShadingEnable = VK_FALSE;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_FALSE;
    if (shaders.fullscreenOverlay) {
        // Melange "over" classique : la precipitation se pose sur l'image.
        colorBlendAttachment.blendEnable = VK_TRUE;
        colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
        colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
    }

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.logicOpEnable = VK_FALSE;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;

    // Depth testing : on garde le fragment le plus proche (compareOp LESS) et on
    // ecrit sa profondeur. Indispensable pour un rendu 3D correct.
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
    if (shaders.fullscreenOverlay) {
        depthStencil.depthTestEnable = VK_FALSE;
        depthStencil.depthWriteEnable = VK_FALSE;
    }
    depthStencil.depthBoundsTestEnable = VK_FALSE;
    depthStencil.stencilTestEnable = VK_FALSE;

    const std::array<VkDynamicState, 2> dynamicStates = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR
    };
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();

    // Un descriptor set (l'UBO : view/proj/viewPos/terrain, constant sur la
    // frame) + un push constant (la model matrix, propre a chaque objet dessine).
    //
    // Push constant plutot qu'un second UBO ou un offset dynamique : depuis la
    // Phase 5 une frame dessine plusieurs objets (terrain, arbres, souches,
    // buches) et chacun a sa propre transformation. 80 octets depuis la Phase 10
    // (la bande de geomorphing de la tuile s'y ajoute) : toujours largement dans
    // les 128 que la spec Vulkan garantit sur toute implementation.
    // Stage VERTEX uniquement : le fragment shader n'en a pas besoin (il
    // travaille sur des varyings deja en espace monde).
    VkPushConstantRange modelRange{};
    modelRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    modelRange.offset = 0;
    // La plage couvre model + params (voir DrawItemPush). Elle est la MEME pour
    // les deux materiaux, meme si cube.vert n'utilise pas params : une plage
    // identique garde les deux pipeline layouts compatibles, ce dont depend le
    // fait de ne relier le descriptor set qu'une fois par materiau.
    modelRange.size = static_cast<uint32_t>(sizeof(DrawItemPush));

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &descriptorSetLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &modelRange;

    if (vkCreatePipelineLayout(device_, &layoutInfo, nullptr, &pipelineLayout_) != VK_SUCCESS) {
        throw std::runtime_error("Echec de la creation du pipeline layout.");
    }

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = static_cast<uint32_t>(stages.size());
    pipelineInfo.pStages = stages.data();
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = pipelineLayout_;
    pipelineInfo.renderPass = renderPass;
    pipelineInfo.subpass = 0;

    if (vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr,
                                  &graphicsPipeline_) != VK_SUCCESS) {
        throw std::runtime_error("Echec de la creation du pipeline graphique.");
    }

    // Les modules ne sont plus necessaires une fois le pipeline construit.
    vkDestroyShaderModule(device_, fragModule, nullptr);
    vkDestroyShaderModule(device_, vertModule, nullptr);

    logInfo("Pipeline graphique cree (" + shaders.vertexSpv + " + " +
            shaders.fragmentSpv + ").");
}

Pipeline::~Pipeline() {
    if (graphicsPipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, graphicsPipeline_, nullptr);
    }
    if (pipelineLayout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
    }
}

VkShaderModule Pipeline::createShaderModule(const std::vector<char>& code) const {
    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = code.size();
    createInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());

    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device_, &createInfo, nullptr, &module) != VK_SUCCESS) {
        throw std::runtime_error("Echec de la creation d'un shader module.");
    }
    return module;
}

std::vector<char> Pipeline::readFile(const std::string& path) {
    // ate : on se place a la fin pour connaitre la taille directement.
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Impossible d'ouvrir le fichier shader : " + path);
    }
    const size_t fileSize = static_cast<size_t>(file.tellg());
    std::vector<char> buffer(fileSize);
    file.seekg(0);
    file.read(buffer.data(), static_cast<std::streamsize>(fileSize));
    file.close();
    return buffer;
}

} // namespace vercora
