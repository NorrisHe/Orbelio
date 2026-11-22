#include "native_rasterizer.hpp"

namespace orbelio {
namespace {
struct GraphicsParameters {
    VkDeviceAddress projected;
    std::array<float, 2> viewport;
};

static_assert(sizeof(GraphicsParameters) == 16);

VkPipelineColorBlendAttachmentState blendState() {
    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_DST_ALPHA;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                           VK_COLOR_COMPONENT_A_BIT;
    return blend;
}

}

bool nativeAllocationFailed(const VulkanError& error) {
    return error.result() == VK_ERROR_OUT_OF_DEVICE_MEMORY || error.result() == VK_ERROR_OUT_OF_HOST_MEMORY;
}

NativeRasterizer::NativeRasterizer(DeviceContext context,
                                   VmaAllocator allocator,
                                   NativeBufferCounts& counts,
                                   const NativeRenderTargets& targets,
                                   std::string_view directory)
    : context_(context), allocator_(allocator), counts_(counts), targets_(targets),
      shaderDirectory_(directory) {
    try {
        createGraphicsPipeline(raster_, false, false);
        createGraphicsPipeline(rasterDepth_, true, false);
    } catch (...) {
        destroy();
        throw;
    }
}

NativeRasterizer::~NativeRasterizer() { destroy(); }

void NativeRasterizer::destroy() noexcept {
    for (auto* resource : {&raster_, &rasterDepth_, &rasterNormals_, &rasterDepthNormals_}) {
        destroyPipeline(context_, *resource);
    }
}

void NativeRasterizer::ensureNormals() {
    if (!targets_.hasNormals() || rasterNormals_.handle) {
        return;
    }
    try {
        createGraphicsPipeline(rasterNormals_, false, true);
        createGraphicsPipeline(rasterDepthNormals_, true, true);
    } catch (...) {
        destroyPipeline(context_, rasterNormals_);
        destroyPipeline(context_, rasterDepthNormals_);
        throw;
    }
}

void NativeRasterizer::createGraphicsPipeline(NativePipeline& pipeline, bool withDepth, bool withNormals) {
    const std::array types{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                           VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                           VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
    const auto attachmentCount = 1 + std::uint32_t(withDepth) + std::uint32_t(withNormals);
    createNativeLayout(context_,
                       pipeline,
                       std::span(types).first(attachmentCount),
                       VK_SHADER_STAGE_VERTEX_BIT,
                       sizeof(GraphicsParameters));
    std::string stem = "native_quad";
    if (withDepth) {
        stem += "_depth";
    }
    if (withNormals) {
        stem += "_normals";
    }
    const auto vertex = loadNativeShader(context_, shaderDirectory_, stem + ".vert.spv");
    VkShaderModule fragment = VK_NULL_HANDLE;
    try {
        fragment = loadNativeShader(context_, shaderDirectory_, stem + ".frag.spv");
        createGraphicsStages(vertex, fragment, pipeline, withDepth, withNormals);
    } catch (...) {
        vkDestroyShaderModule(context_.device, vertex, nullptr);
        vkDestroyShaderModule(context_.device, fragment, nullptr);
        throw;
    }
    vkDestroyShaderModule(context_.device, vertex, nullptr);
    vkDestroyShaderModule(context_.device, fragment, nullptr);
}

void NativeRasterizer::createGraphicsStages(VkShaderModule vertex,
                                            VkShaderModule fragment,
                                            NativePipeline& pipeline,
                                            bool withDepth,
                                            bool withNormals) {
    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    for (auto& stage : stages) {
        stage = vkInfo<VkPipelineShaderStageCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
        stage.pName = "main";
    }
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment;
    auto input = vkInfo<VkPipelineVertexInputStateCreateInfo>(
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO);
    auto assembly = vkInfo<VkPipelineInputAssemblyStateCreateInfo>(
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO);
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    auto viewport =
        vkInfo<VkPipelineViewportStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO);
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    auto rasterization = vkInfo<VkPipelineRasterizationStateCreateInfo>(
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO);
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.cullMode = VK_CULL_MODE_NONE;
    rasterization.lineWidth = 1;
    auto sampling = vkInfo<VkPipelineMultisampleStateCreateInfo>(
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO);
    sampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    const std::array attachments{blendState(), blendState(), blendState()};
    auto blending =
        vkInfo<VkPipelineColorBlendStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO);
    blending.attachmentCount = 1 + std::uint32_t(withDepth) + std::uint32_t(withNormals);
    blending.pAttachments = attachments.data();
    const std::array dynamicStates{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    auto dynamic =
        vkInfo<VkPipelineDynamicStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO);
    dynamic.dynamicStateCount = static_cast<std::uint32_t>(dynamicStates.size());
    dynamic.pDynamicStates = dynamicStates.data();
    auto info = vkInfo<VkGraphicsPipelineCreateInfo>(VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO);
    info.stageCount = static_cast<std::uint32_t>(stages.size());
    info.pStages = stages.data();
    info.pVertexInputState = &input;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &rasterization;
    info.pMultisampleState = &sampling;
    info.pColorBlendState = &blending;
    info.pDynamicState = &dynamic;
    info.layout = pipeline.layout;
    info.renderPass = targets_.pass(withDepth, withNormals);
    vkCheck(vkCreateGraphicsPipelines(context_.device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline.handle),
            "vkCreateGraphicsPipelines native");
}

void NativeRasterizer::recordQuads(VkCommandBuffer command, VkExtent2D extent, NativeRasterInput input) {
    nativeMemoryBarrier(command,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_ACCESS_SHADER_WRITE_BIT,
                        VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
                        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT);
    const auto* normalBuffer = input.normals;
    const auto& pipeline = normalBuffer ? (targets_.hasDepth() ? rasterDepthNormals_ : rasterNormals_)
                                        : (targets_.hasDepth() ? rasterDepth_ : raster_);
    const VkClearValue clear{{{0, 0, 0, 1}}};
    const std::array clears{clear, clear, clear};
    auto pass = vkInfo<VkRenderPassBeginInfo>(VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO);
    pass.renderPass = targets_.pass(targets_.hasDepth(), targets_.hasNormals());
    pass.framebuffer = targets_.framebuffer();
    pass.renderArea.extent = extent;
    pass.clearValueCount = 1 + std::uint32_t(targets_.hasDepth()) + std::uint32_t(targets_.hasNormals());
    pass.pClearValues = clears.data();
    vkCmdBeginRenderPass(command, &pass, VK_SUBPASS_CONTENTS_INLINE);
    if (targets_.hasDepth()) {
        bindNativeBuffers(
            command, pipeline, VK_PIPELINE_BIND_POINT_GRAPHICS, {&input.indices, &input.keys}, normalBuffer);
    } else {
        bindNativeBuffers(command, pipeline, VK_PIPELINE_BIND_POINT_GRAPHICS, {&input.indices}, normalBuffer);
    }
    const GraphicsParameters settings{bufferAddress(context_, input.projected),
                                      {float(extent.width), float(extent.height)}};
    vkCmdPushConstants(command, pipeline.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(settings), &settings);
    const VkViewport viewport{0, 0, float(extent.width), float(extent.height), 0, 1};
    const VkRect2D scissor{{0, 0}, extent};
    vkCmdSetViewport(command, 0, 1, &viewport);
    vkCmdSetScissor(command, 0, 1, &scissor);
    if (tiler_) {
        tiler_->recordFallbackDraw(command);
    } else {
        vkCmdDrawIndirect(command,
                          input.visibility->handle,
                          offsetof(NativeVisibilityControl, draw),
                          1,
                          sizeof(VkDrawIndirectCommand));
    }
    vkCmdEndRenderPass(command);
}

bool NativeRasterizer::prepare(const FrameState& frame,
                               std::size_t targetCount,
                               std::size_t normalStorageBytes) {
    const auto mode = frame.renderSettings.rasterMode;
    const bool wanted = mode == RasterMode::TiledCompute;
    if (!wanted) {
        tiler_.reset();
        rejectedTile_.reset();
        return false;
    }
    const std::array configuration{targetCount,
                                   std::size_t(frame.width),
                                   std::size_t(frame.height),
                                   std::size_t(frame.renderSettings.tilePairCapacity),
                                   targets_.bytes() + normalStorageBytes};
    if (rejectedTile_ == configuration) {
        return false;
    }
    const bool created = !tiler_;
    try {
        if (created) {
            tiler_ = std::make_unique<NativeTileRaster>(context_, allocator_, counts_, shaderDirectory_);
        }
        tiler_->reserve(frame, targetCount, targets_.hasNormals());
    } catch (const VulkanError& error) {
        if (!nativeAllocationFailed(error)) {
            throw;
        }
        tiler_.reset();
        rejectedTile_ = configuration;
        return false;
    }
    rejectedTile_.reset();
    return created;
}

void NativeRasterizer::recordBinning(VkCommandBuffer command,
                                     const FrameState& frame,
                                     NativeRasterInput input,
                                     NativeTileQueries queries) {
    if (tiler_) {
        tiler_->recordBinning(command, frame, input, queries);
    }
}

void NativeRasterizer::record(VkCommandBuffer command,
                              const FrameState& frame,
                              NativeRasterInput input,
                              VkQueryPool statisticsQueries,
                              bool diagnostics) {
    if (statisticsQueries) {
        vkCmdResetQueryPool(command, statisticsQueries, 0, 1);
        vkCmdBeginQuery(command, statisticsQueries, 0, 0);
    }
    recordQuads(command, {frame.width, frame.height}, input);
    if (statisticsQueries) {
        vkCmdEndQuery(command, statisticsQueries, 0);
    }
    if (tiler_) {
        tiler_->recordRaster(command, frame, input, targets_, diagnostics);
    }
}

void NativeRasterizer::recordTelemetry(VkCommandBuffer command) {
    if (tiler_) {
        tiler_->recordTelemetry(command);
    }
}

void NativeRasterizer::readTelemetry(bool diagnostics) {
    if (tiler_) {
        tiler_->readTelemetry(diagnostics);
    }
}

void NativeRasterizer::fillStats(const FrameState& frame, BackendStats& stats) const {
    stats.tileAllocationFallback = rejectedTile_.has_value();
    stats.rasterizer = tiler_ ? "native-vulkan-hybrid-tiles" : "native-vulkan-ewa-quads";
    if (tiler_) {
        tiler_->fillStats(stats);
        stats.tileBinningWidth = frame.renderSettings.tileBinningWidth;
    } else {
        stats.tilePairsReserved.reset();
        stats.cooperativeGaussians.reset();
        stats.tilePairsDemandLowerBound.reset();
        stats.tilePairOverflow.reset();
        stats.tiledRasterization.reset();
        stats.tilePairCapacity = 0;
        stats.tileBinningWidth = 0;
        stats.tileDiagnostics.reset();
    }
}

std::size_t NativeRasterizer::sortingBytes() const { return tiler_ ? tiler_->workspaceBytes() : 0; }

std::size_t NativeRasterizer::telemetryBytes() const { return tiler_ ? tiler_->telemetryBytes() : 0; }
}
