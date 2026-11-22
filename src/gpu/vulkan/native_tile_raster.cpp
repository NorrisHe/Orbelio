#include "native_tile_raster.hpp"
#include <algorithm>
#include <cstring>
#include <bit>

namespace orbelio {
namespace {
constexpr std::uint32_t tileWidth = 16;
constexpr std::uint32_t groupSize = 256;
constexpr std::uint32_t minimumPairCapacity = 65536;
constexpr std::uint32_t maximumPairCapacity = 8'000'000;
constexpr std::uint32_t pairsPerGaussian = 8;
constexpr std::size_t controlWords = 36;
constexpr std::size_t controlBytes = controlWords * sizeof(std::uint32_t);
constexpr VkDeviceSize cooperativeDispatchOffset = 29 * sizeof(std::uint32_t);
constexpr VkDeviceSize cooperativeEmitOffset = 32 * sizeof(std::uint32_t);
constexpr VkDeviceSize emitDispatchOffset = 24 * sizeof(std::uint32_t);
constexpr std::uint32_t gpuSourceCountOption = 1;
constexpr std::uint32_t binningScaleShift = 2;
constexpr std::uint32_t scanLevelShift = 4;
constexpr std::uint32_t maximumSourceCount = groupSize * groupSize * groupSize;
constexpr VkDeviceSize rasterDispatchOffset = 13 * sizeof(std::uint32_t);
constexpr std::uint32_t reservedCountWord = 12;
constexpr std::uint32_t overflowFlag = 1;
constexpr std::uint32_t selectedFlag = 2;

void computeBarrier(VkCommandBuffer command) {
    nativeMemoryBarrier(command,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_ACCESS_SHADER_WRITE_BIT,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
                        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                            VK_ACCESS_INDIRECT_COMMAND_READ_BIT);
}

void stampTile(VkCommandBuffer command, NativeTileQueries queries, std::uint32_t phase) {
    if (queries.pool) {
        vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, queries.pool, queries.first + phase);
    }
}

std::uint32_t pairCapacity(const FrameState& frame, std::size_t maximumCount) {
    const auto requested = frame.renderSettings.tilePairCapacity;
    if (requested > maximumPairCapacity) {
        throw std::invalid_argument("Tile pair capacity exceeds 8000000");
    }
    return requested ? requested
                     : std::uint32_t(std::clamp(std::uint64_t(maximumCount) * pairsPerGaussian,
                                                std::uint64_t(minimumPairCapacity),
                                                std::uint64_t(maximumPairCapacity)));
}
}

struct alignas(16) NativeTileRaster::Parameters {
    VkDeviceAddress projected;
    std::array<std::uint32_t, 2> viewport;
    std::uint32_t count;
    std::uint32_t capacity;
    std::uint32_t columns;
    std::uint32_t options;
};

NativeTileRaster::NativeTileRaster(DeviceContext context,
                                   VmaAllocator allocator,
                                   NativeBufferCounts& counts,
                                   std::string_view directory)
    : context_(context), allocator_(allocator), counts_(counts),
      sorter_(context, allocator, counts, directory, NativeSortMode::GpuCount), shaderDirectory_(directory) {
    try {
        createPipelines(directory);
    } catch (...) {
        destroy();
        throw;
    }
}

NativeTileRaster::~NativeTileRaster() { destroy(); }

void NativeTileRaster::destroy() noexcept {
    for (auto* pipeline : {&sourceDispatch_,
                           &bin_,
                           &scan_,
                           &emit_,
                           &dispatch_,
                           &cooperativeDispatch_,
                           &cooperativeCount_,
                           &cooperativeEmit_,
                           &rangesPipeline_}) {
        destroyPipeline(context_, *pipeline);
    }
    for (auto& variants : rasterPipelines_) {
        for (auto& pipeline : variants) {
            destroyPipeline(context_, pipeline);
        }
    }
    for (auto* buffer : {&pairCounts_,
                         &bounds_,
                         &offsets_,
                         &groupSums_,
                         &groupOffsets_,
                         &chunkSums_,
                         &chunkOffsets_,
                         &total_,
                         &orders_,
                         &tiles_,
                         &ranges_,
                         &control_,
                         &readback_}) {
        destroyBuffer(allocator_, *buffer);
    }
}

void NativeTileRaster::createPipelines(std::string_view directory) {
    static_assert(sizeof(Parameters) == 32);
    createNativeCompute(
        context_, sourceDispatch_, directory, "native_tile_source_dispatch.comp.spv", 2, sizeof(Parameters));
    createNativeCompute(context_, bin_, directory, "native_tile_bin.comp.spv", 5, sizeof(Parameters));
    createNativeCompute(
        context_, dispatch_, directory, "native_tile_dispatch.comp.spv", 2, sizeof(Parameters));
    createNativeCompute(
        context_, rangesPipeline_, directory, "native_tile_ranges.comp.spv", 3, sizeof(Parameters));
    createNativeCompute(context_, scan_, directory, "native_tile_scan.comp.spv", 4, sizeof(Parameters));
    createNativeCompute(context_, emit_, directory, "native_tile_emit.comp.spv", 7, sizeof(Parameters));
    createNativeCompute(
        context_, cooperativeDispatch_, directory, "native_tile_cooperative_dispatch.comp.spv", 1, 0);
    createNativeCompute(
        context_, cooperativeCount_, directory, "native_tile_cooperative.comp.spv", 3, sizeof(Parameters));
    createNativeCompute(context_,
                        cooperativeEmit_,
                        directory,
                        "native_tile_cooperative_emit.comp.spv",
                        7,
                        sizeof(Parameters));
    createRasterPipelines(directory, false);
}

void NativeTileRaster::createRasterPipelines(std::string_view directory, bool withNormals) {
    const std::array types{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                           VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                           VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                           VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                           VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                           VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                           VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                           VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                           VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
    const std::array shaders{std::array{"native_tile_raster.comp.spv",
                                        "native_tile_raster_diagnostics.comp.spv",
                                        "native_tile_raster_macro.comp.spv",
                                        "native_tile_raster_macro_diagnostics.comp.spv"},
                             std::array{"native_tile_raster_normals.comp.spv",
                                        "native_tile_raster_normals_diagnostics.comp.spv",
                                        "native_tile_raster_normals_macro.comp.spv",
                                        "native_tile_raster_normals_macro_diagnostics.comp.spv"}};
    auto& pipelines = rasterPipelines_[withNormals];
    try {
        for (std::size_t i = 0; i < pipelines.size(); ++i) {
            const auto bindings = (withNormals ? 8 : 6) + (i % 2);
            createNativeCompute(context_,
                                pipelines[i],
                                directory,
                                shaders[withNormals][i],
                                std::span(types).first(bindings),
                                sizeof(Parameters));
        }
    } catch (...) {
        for (auto& pipeline : pipelines) {
            destroyPipeline(context_, pipeline);
        }
        throw;
    }
}

void NativeTileRaster::reserve(const FrameState& frame, std::size_t maximumCount, bool withNormals) {
    if (maximumCount > maximumSourceCount) {
        throw std::invalid_argument("Tile scan supports at most 16777216 Gaussians");
    }
    const auto binWidth = frame.renderSettings.tileBinningWidth;
    if (binWidth != tileWidth && binWidth != 2 * tileWidth && binWidth != 4 * tileWidth) {
        throw std::invalid_argument("Tile binning width must be 16, 32, or 64");
    }
    capacity_ = pairCapacity(frame, maximumCount);
    reserveScan(maximumCount);
    for (auto* buffer : {&orders_, &tiles_}) {
        reserveBuffer(allocator_,
                      *buffer,
                      std::size_t(capacity_) * sizeof(std::uint32_t),
                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                      NativeHostAccess::None,
                      counts_);
    }
    const auto tiles =
        std::size_t((frame.width + tileWidth - 1) / tileWidth) * ((frame.height + tileWidth - 1) / tileWidth);
    reserveBuffer(allocator_,
                  ranges_,
                  tiles * 2 * sizeof(std::uint32_t),
                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                  NativeHostAccess::None,
                  counts_);
    reserveTelemetry(maximumCount);
    sorter_.reserve(capacity_);
    if (withNormals && !rasterPipelines_[true][0].handle) {
        createRasterPipelines(shaderDirectory_, true);
    }
}

void NativeTileRaster::reserveTelemetry(std::size_t maximumCount) {
    reserveBuffer(allocator_,
                  control_,
                  controlBytes + maximumCount * 2 * sizeof(std::uint32_t),
                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                      VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                  NativeHostAccess::None,
                  counts_);
    reserveBuffer(allocator_,
                  readback_,
                  controlBytes,
                  VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                  NativeHostAccess::Readback,
                  counts_);
}

void NativeTileRaster::reserveScan(std::size_t maximumCount) {
    const auto groups = (maximumCount + groupSize - 1) / groupSize;
    const auto chunks = (groups + groupSize - 1) / groupSize;
    const std::array<std::pair<NativeBuffer*, std::size_t>, 8> requests{{{&pairCounts_, maximumCount},
                                                                         {&bounds_, maximumCount * 4},
                                                                         {&offsets_, maximumCount},
                                                                         {&groupSums_, groups},
                                                                         {&groupOffsets_, groups},
                                                                         {&chunkSums_, chunks},
                                                                         {&chunkOffsets_, chunks},
                                                                         {&total_, 1}}};
    for (const auto& [buffer, words] : requests) {
        reserveBuffer(allocator_,
                      *buffer,
                      std::max(words, std::size_t(1)) * sizeof(std::uint32_t),
                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                      NativeHostAccess::None,
                      counts_);
    }
}

void NativeTileRaster::recordScan(VkCommandBuffer command,
                                  Parameters settings,
                                  ScanBuffers buffers,
                                  std::uint32_t count,
                                  std::uint32_t level) {
    settings.count = count;
    settings.options |= level << scanLevelShift;
    bindNativeBuffers(command,
                      scan_,
                      VK_PIPELINE_BIND_POINT_COMPUTE,
                      {&buffers.input, &buffers.offsets, &buffers.totals, &control_});
    vkCmdPushConstants(command, scan_.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(settings), &settings);
    vkCmdDispatchIndirect(command, control_.handle, level * sizeof(VkDispatchIndirectCommand));
    computeBarrier(command);
}

NativeTileRaster::Parameters NativeTileRaster::parameters(const FrameState& frame,
                                                          NativeRasterInput input,
                                                          bool depth) const {
    return {bufferAddress(context_, input.projected),
            {frame.width, frame.height},
            input.count,
            capacity_,
            (frame.width + frame.renderSettings.tileBinningWidth - 1) / frame.renderSettings.tileBinningWidth,
            (std::uint32_t(depth) << 1) | (input.visibility ? gpuSourceCountOption : 0) |
                (std::uint32_t(std::countr_zero(frame.renderSettings.tileBinningWidth / tileWidth))
                 << binningScaleShift)};
}

void NativeTileRaster::recordBinning(VkCommandBuffer command,
                                     const FrameState& frame,
                                     NativeRasterInput input,
                                     NativeTileQueries queries) {
    nativeMemoryBarrier(command,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdFillBuffer(command, control_.handle, 0, controlBytes, 0);
    vkCmdFillBuffer(command, ranges_.handle, 0, VK_WHOLE_SIZE, 0);
    nativeMemoryBarrier(command,
                        VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    const auto settings = parameters(frame, input);
    recordCounts(command, input, settings);
    stampTile(command, queries, 0);
    const auto groups = (input.count + groupSize - 1) / groupSize;
    const auto chunks = (groups + groupSize - 1) / groupSize;
    recordScan(command, settings, {pairCounts_, offsets_, groupSums_}, input.count, 0);
    recordScan(command, settings, {groupSums_, groupOffsets_, chunkSums_}, groups, 1);
    recordScan(command, settings, {chunkSums_, chunkOffsets_, total_}, chunks, 2);
    bindNativeBuffers(command, dispatch_, VK_PIPELINE_BIND_POINT_COMPUTE, {&total_, &control_});
    vkCmdPushConstants(
        command, dispatch_.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(settings), &settings);
    vkCmdDispatch(command, 1, 1, 1);
    computeBarrier(command);
    stampTile(command, queries, 1);
    recordEmission(command, settings);
    stampTile(command, queries, 2);
    // Prefix emission preserves global depth order; the only remaining sort is stable tile grouping.
    const auto binWidth = frame.renderSettings.tileBinningWidth;
    const auto tileCount = std::uint64_t(settings.columns) * ((frame.height + binWidth - 1) / binWidth);
    sorter_.record(command,
                   capacity_,
                   tiles_,
                   orders_,
                   &control_,
                   std::max(1u, static_cast<std::uint32_t>(std::bit_width(tileCount - 1))));
    stampTile(command, queries, 3);
    bindNativeBuffers(
        command, rangesPipeline_, VK_PIPELINE_BIND_POINT_COMPUTE, {&tiles_, &ranges_, &control_});
    vkCmdPushConstants(
        command, rangesPipeline_.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(settings), &settings);
    vkCmdDispatchIndirect(command, control_.handle, offsetof(NativeSortControl, histogramDispatch));
    computeBarrier(command);
}

void NativeTileRaster::recordCounts(VkCommandBuffer command,
                                    NativeRasterInput input,
                                    const Parameters& settings) {
    bindNativeBuffers(command,
                      sourceDispatch_,
                      VK_PIPELINE_BIND_POINT_COMPUTE,
                      {input.visibility ? input.visibility : &input.keys, &control_});
    vkCmdPushConstants(
        command, sourceDispatch_.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(settings), &settings);
    vkCmdDispatch(command, 1, 1, 1);
    computeBarrier(command);
    bindNativeBuffers(command,
                      bin_,
                      VK_PIPELINE_BIND_POINT_COMPUTE,
                      {&input.indices, &pairCounts_, &bounds_, &control_, &input.keys});
    vkCmdPushConstants(command, bin_.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(settings), &settings);
    vkCmdDispatchIndirect(command, control_.handle, 0);
    computeBarrier(command);
    bindNativeBuffers(command, cooperativeDispatch_, VK_PIPELINE_BIND_POINT_COMPUTE, {&control_});
    vkCmdDispatch(command, 1, 1, 1);
    computeBarrier(command);
    bindNativeBuffers(
        command, cooperativeCount_, VK_PIPELINE_BIND_POINT_COMPUTE, {&control_, &pairCounts_, &bounds_});
    vkCmdPushConstants(
        command, cooperativeCount_.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(settings), &settings);
    vkCmdDispatchIndirect(command, control_.handle, cooperativeDispatchOffset);
    computeBarrier(command);
}

void NativeTileRaster::recordEmission(VkCommandBuffer command, const Parameters& settings) {
    bindNativeBuffers(command,
                      emit_,
                      VK_PIPELINE_BIND_POINT_COMPUTE,
                      {&bounds_, &offsets_, &groupOffsets_, &chunkOffsets_, &orders_, &tiles_, &pairCounts_});
    vkCmdPushConstants(command, emit_.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(settings), &settings);
    vkCmdDispatchIndirect(command, control_.handle, emitDispatchOffset);
    computeBarrier(command);
    bindNativeBuffers(command,
                      cooperativeEmit_,
                      VK_PIPELINE_BIND_POINT_COMPUTE,
                      {&bounds_, &offsets_, &groupOffsets_, &chunkOffsets_, &orders_, &tiles_, &control_});
    vkCmdPushConstants(
        command, cooperativeEmit_.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(settings), &settings);
    vkCmdDispatchIndirect(command, control_.handle, cooperativeEmitOffset);
    computeBarrier(command);
}

void NativeTileRaster::bindRaster(VkCommandBuffer command,
                                  NativeRasterInput input,
                                  const NativeRenderTargets& targets,
                                  const NativePipeline& pipeline,
                                  bool diagnostics) {
    const bool withNormals = targets.hasNormals();
    const std::array<const NativeBuffer*, 9> buffers{&orders_,
                                                     &input.indices,
                                                     &input.keys,
                                                     &ranges_,
                                                     nullptr,
                                                     nullptr,
                                                     withNormals ? input.normals : &control_,
                                                     nullptr,
                                                     &control_};
    std::array<VkDescriptorBufferInfo, 9> bufferInfos{};
    const auto depthView =
        targets.hasDepth() ? targets.depthAccum.resource.view : targets.colorAccum.resource.view;
    const std::array<VkDescriptorImageInfo, 3> images{
        {{VK_NULL_HANDLE, targets.colorAccum.resource.view, VK_IMAGE_LAYOUT_GENERAL},
         {VK_NULL_HANDLE, depthView, VK_IMAGE_LAYOUT_GENERAL},
         {VK_NULL_HANDLE, targets.normals.resource.view, VK_IMAGE_LAYOUT_GENERAL}}};
    std::array<VkWriteDescriptorSet, 9> writes{};
    const std::uint32_t count = (withNormals ? 8 : 6) + std::uint32_t(diagnostics);
    for (std::uint32_t i = 0; i < count; ++i) {
        writes[i] = vkInfo<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        if (const auto* buffer = buffers[i]) {
            bufferInfos[i] = {buffer->handle, 0, buffer->capacity};
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].pBufferInfo = &bufferInfos[i];
        } else {
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[i].pImageInfo = &images[i == 7 ? 2 : i - 4];
        }
    }
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.handle);
    vkCmdPushDescriptorSetKHR(
        command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.layout, 0, count, writes.data());
}

void NativeTileRaster::transitionTargets(VkCommandBuffer command,
                                         const NativeRenderTargets& targets,
                                         bool writing) {
    for (const auto* target : {&targets.colorAccum, &targets.depthAccum, &targets.normals}) {
        if (!target->resource.image) {
            continue;
        }
        auto barrier = vkInfo<VkImageMemoryBarrier>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);
        barrier.image = target->resource.image;
        barrier.oldLayout = writing ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL;
        barrier.newLayout = writing ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask =
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | (writing ? 0 : VK_ACCESS_SHADER_WRITE_BIT);
        barrier.dstAccessMask =
            writing ? VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT : VK_ACCESS_SHADER_READ_BIT;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        nativeImageBarrier(command,
                           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           target == &targets.normals && !writing ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT
                                                                  : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           barrier);
    }
}

void NativeTileRaster::recordRaster(VkCommandBuffer command,
                                    const FrameState& frame,
                                    NativeRasterInput input,
                                    const NativeRenderTargets& targets,
                                    bool diagnostics) {
    transitionTargets(command, targets, true);
    const bool macro = frame.renderSettings.tileBinningWidth > tileWidth;
    const auto slot = 2 * std::size_t(macro) + std::size_t(diagnostics);
    const auto& pipeline = rasterPipelines_[targets.hasNormals()][slot];
    bindRaster(command, input, targets, pipeline, diagnostics);
    const auto settings = parameters(frame, input, targets.hasDepth());
    vkCmdPushConstants(command, pipeline.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(settings), &settings);
    vkCmdDispatchIndirect(command, control_.handle, rasterDispatchOffset);
    transitionTargets(command, targets, false);
}

void NativeTileRaster::recordFallbackDraw(VkCommandBuffer command) const {
    vkCmdDrawIndirect(command, control_.handle, sizeof(NativeSortControl), 1, sizeof(VkDrawIndirectCommand));
}

void NativeTileRaster::recordTelemetry(VkCommandBuffer command) {
    nativeMemoryBarrier(command,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_ACCESS_SHADER_WRITE_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_ACCESS_TRANSFER_READ_BIT);
    const VkBufferCopy copy{0, 0, controlBytes};
    vkCmdCopyBuffer(command, control_.handle, readback_.handle, 1, &copy);
}

void NativeTileRaster::readTelemetry(bool diagnostics) {
    vkCheck(vmaInvalidateAllocation(allocator_, readback_.allocation, 0, controlBytes),
            "invalidate tile telemetry");
    std::array<std::uint32_t, controlWords> control{};
    std::memcpy(control.data(), readback_.mapped, controlBytes);
    reserved_ = control[reservedCountWord];
    demand_ = control[27];
    cooperative_ = control[28];
    overflow_ = (control[1] & overflowFlag) != 0;
    selected_ = (control[1] & selectedFlag) != 0;
    diagnostics_.reset();
    if (diagnostics && *selected_) {
        TileDiagnostics values{};
        values.testedPixelGaussians = std::uint64_t(control[16]) | (std::uint64_t(control[17]) << 32);
        values.contributingPixelGaussians = std::uint64_t(control[18]) | (std::uint64_t(control[19]) << 32);
        values.zeroTransmittancePixels = control[20];
        values.nearOpaquePixels = control[21];
        values.testedBelowTransmittance1e4 = std::uint64_t(control[22]) | (std::uint64_t(control[23]) << 32);
        diagnostics_ = values;
    }
}

void NativeTileRaster::fillStats(BackendStats& stats) const {
    stats.tilePairsReserved = reserved_;
    stats.cooperativeGaussians = cooperative_;
    stats.tilePairsDemandLowerBound = demand_;
    stats.tilePairOverflow = overflow_;
    stats.tiledRasterization = selected_;
    stats.tilePairCapacity = capacity_;
    stats.tileDiagnostics = diagnostics_;
}

std::size_t NativeTileRaster::workspaceBytes() const {
    std::size_t bytes = sorter_.bytes();
    for (auto* buffer : {&pairCounts_,
                         &bounds_,
                         &offsets_,
                         &groupSums_,
                         &groupOffsets_,
                         &chunkSums_,
                         &chunkOffsets_,
                         &total_,
                         &orders_,
                         &tiles_,
                         &ranges_,
                         &control_}) {
        bytes += buffer->capacity;
    }
    return bytes;
}
}
