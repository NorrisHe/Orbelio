#include "bootstrap_renderer.hpp"
#include "static_gs_renderer.hpp"
#ifdef ORBELIO_TRACY_ENABLED
#include <tracy/Tracy.hpp>
#endif
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <vector>

namespace orbelio {
namespace {
using Clock = std::chrono::steady_clock;

constexpr std::size_t bufferPoolBudgetBytes = 32 * 1024 * 1024;
constexpr std::size_t placeholderScratchBytes = 4096;
constexpr std::uint32_t imageBucketPixels = 64;
constexpr std::uint32_t imageLimitPixels = 8192;
constexpr std::uint64_t submissionWaitNs = 5'000'000'000ULL;
constexpr std::uint64_t memorySampleIntervalFrames = 30;
constexpr std::uint32_t timestampQueryCount = 2;
constexpr std::uint32_t timestampFirstQuery = 0;
constexpr std::uint32_t timestampLastQuery = 1;
constexpr double nanosecondsPerMillisecond = 1'000'000.0;

double elapsedMs(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

void clearSceneReport(FrameReport& report) {
    FrameStats frame{};
    frame.frameIndex = report.frame.frameIndex;
    BackendStats backend{};
    backend.imageAllocations = report.backend.imageAllocations;
    backend.imageReservedBytes = report.backend.imageReservedBytes;
    backend.allocatorPeakBytes = report.backend.allocatorPeakBytes;
    report.frame = frame;
    report.backend = backend;
}

// One image transition: the six empty pipeline-barrier arguments never vary here.
void applyImageBarrier(VkCommandBuffer command,
                       VkPipelineStageFlags sourceStage,
                       VkPipelineStageFlags destinationStage,
                       const VkImageMemoryBarrier& barrier) {
    vkCmdPipelineBarrier(command, sourceStage, destinationStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

// Barrier for the whole-target transition. `hadContent` selects the previous layout:
// an undefined target on the first frame, readable afterwards.
VkImageMemoryBarrier targetBarrier(VkImage image, bool hadContent) {
    auto barrier = vkInfo<VkImageMemoryBarrier>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);
    barrier.srcAccessMask = hadContent ? VK_ACCESS_SHADER_READ_BIT : 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.oldLayout = hadContent ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return barrier;
}

struct TargetImage {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VmaAllocation memory = VK_NULL_HANDLE;
    VkDeviceSize bytes = 0;
};

TargetImage createTargetImage(VmaAllocator allocator,
                              DeviceContext context,
                              std::uint32_t width,
                              std::uint32_t height) {
    auto info = vkInfo<VkImageCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.extent = {width, height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                 VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo memory{};
    memory.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

    TargetImage created;
    VmaAllocationInfo memoryInfo{};
    vkCheck(vmaCreateImage(allocator, &info, &memory, &created.image, &created.memory, &memoryInfo),
            "vmaCreateImage");
    created.bytes = memoryInfo.size;

    auto view = vkInfo<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
    view.image = created.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = info.format;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    const VkResult viewResult = vkCreateImageView(context.device, &view, nullptr, &created.view);
    if (viewResult != VK_SUCCESS) {
        vmaDestroyImage(allocator, created.image, created.memory);
        created.image = VK_NULL_HANDLE;
        created.memory = VK_NULL_HANDLE;
        vkCheck(viewResult, "create viewport image view");
    }
    return created;
}
} // namespace

BootstrapRenderer::BootstrapRenderer(DeviceContext context) : context_(context) {
    try {
        createAllocator();
        buffers_ = std::make_unique<GpuBufferPool>(allocator_, bufferPoolBudgetBytes);
        createCommandResources();
        detectTimestampSupport();
    } catch (...) {
        destroy();
        throw;
    }
}

BootstrapRenderer::~BootstrapRenderer() { destroy(); }

void BootstrapRenderer::waitIdle() noexcept {
    if (context_.device) {
        vkDeviceWaitIdle(context_.device);
    }
}

void BootstrapRenderer::destroy() noexcept {
    // Shutdown only. Normal frames use a fence, never vkDeviceWaitIdle.
    waitIdle();
    gs_.reset();
    buffers_.reset();
    destroyTargetImage();
    if (queries_) {
        vkDestroyQueryPool(context_.device, queries_, nullptr);
    }
    if (fence_) {
        vkDestroyFence(context_.device, fence_, nullptr);
    }
    if (commands_) {
        vkDestroyCommandPool(context_.device, commands_, nullptr);
    }
    if (allocator_) {
        vmaDestroyAllocator(allocator_);
    }
}

void BootstrapRenderer::destroyTargetImage() noexcept {
    if (imageView_) {
        vkDestroyImageView(context_.device, imageView_, nullptr);
    }
    if (image_) {
        vmaDestroyImage(allocator_, image_, imageAllocation_);
    }
    imageView_ = VK_NULL_HANDLE;
    image_ = VK_NULL_HANDLE;
    imageAllocation_ = VK_NULL_HANDLE;
}

void BootstrapRenderer::createAllocator() {
    VmaVulkanFunctions functions{};
    functions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    functions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;

    VmaAllocatorCreateInfo info{};
    info.instance = context_.instance;
    info.physicalDevice = context_.physicalDevice;
    info.device = context_.device;
    info.vulkanApiVersion = VK_API_VERSION_1_3;
    info.pVulkanFunctions = &functions;
    vkCheck(vmaCreateAllocator(&info, &allocator_), "vmaCreateAllocator");
}

void BootstrapRenderer::createCommandResources() {
    auto poolInfo = vkInfo<VkCommandPoolCreateInfo>(VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
    poolInfo.queueFamilyIndex = context_.queueFamily;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    vkCheck(vkCreateCommandPool(context_.device, &poolInfo, nullptr, &commands_), "vkCreateCommandPool");

    auto allocation = vkInfo<VkCommandBufferAllocateInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
    allocation.commandPool = commands_;
    allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocation.commandBufferCount = 1;
    vkCheck(vkAllocateCommandBuffers(context_.device, &allocation, &command_), "vkAllocateCommandBuffers");

    auto fenceInfo = vkInfo<VkFenceCreateInfo>(VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);
    vkCheck(vkCreateFence(context_.device, &fenceInfo, nullptr, &fence_), "vkCreateFence");
}

void BootstrapRenderer::detectTimestampSupport() {
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(context_.physicalDevice, &properties);
    timestampPeriod_ = properties.limits.timestampPeriod;

    std::uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(context_.physicalDevice, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(context_.physicalDevice, &count, families.data());
    timestampBits_ = families.at(context_.queueFamily).timestampValidBits;
    if (!timestampBits_) {
        return;
    }

    auto queryInfo = vkInfo<VkQueryPoolCreateInfo>(VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO);
    queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
    queryInfo.queryCount = timestampQueryCount;
    vkCheck(vkCreateQueryPool(context_.device, &queryInfo, nullptr, &queries_), "vkCreateQueryPool");
}

void BootstrapRenderer::waitSubmission() {
    if (!fencePending_) {
        return;
    }
    const auto start = Clock::now();
    // Finite wait: device loss/stalls become diagnostics instead of an infinite hang.
    vkCheck(vkWaitForFences(context_.device, 1, &fence_, VK_TRUE, submissionWaitNs),
            "vkWaitForFences (5 seconds)");
    report_.frame.gpuWaitMs += elapsedMs(start);
    fencePending_ = false;
}

bool BootstrapRenderer::resize(std::uint32_t width, std::uint32_t height) {
    guard_.check();
    width = std::max(1u, width);
    height = std::max(1u, height);
    if (image_ && width <= width_ && height <= height_) {
        return false;
    }
    // The integration deletes its old QSGTexture reference before calling resize.
    if (fencePending_) {
        throw std::logic_error("Resize before frame completion");
    }
    if (width > imageLimitPixels || height > imageLimitPixels) {
        throw std::runtime_error("Bootstrap viewport exceeds 8192 pixel limit");
    }

    // Single-slot grow-only image cache, rounded to 64 pixels; no frame churn.
    const auto bucket = [](std::uint32_t requested) {
        return (requested + imageBucketPixels - 1u) / imageBucketPixels * imageBucketPixels;
    };
    const auto nextWidth = bucket(std::max(width, width_));
    const auto nextHeight = bucket(std::max(height, height_));
    const auto next = createTargetImage(allocator_, context_, nextWidth, nextHeight);

    destroyTargetImage();
    image_ = next.image;
    imageView_ = next.view;
    imageAllocation_ = next.memory;
    width_ = nextWidth;
    height_ = nextHeight;
    imageHasContent_ = false;

    ++report_.backend.imageAllocations;
    report_.backend.imageReservedBytes = next.bytes;
    return true;
}

void BootstrapRenderer::render(const FrameState& state) {
#ifdef ORBELIO_TRACY_ENABLED
    ZoneScoped;
#endif
    guard_.check();
    if (fencePending_ || !image_) {
        throw std::logic_error("Invalid frame submission state");
    }
    report_.frame.gpuWaitMs = 0;
    frameStart_ = Clock::now();
    if (state.scene) {
        renderGaussians(state);
    } else {
        if (gs_) {
            // The previous complete-frame fence covers both Engine and Qt use of these resources.
            gs_.reset();
            clearSceneReport(report_);
        }
        renderPlaceholder(state);
    }
    report_.frame.sceneVersion = state.sceneVersion;
    report_.frame.viewportWidth = state.width;
    report_.frame.viewportHeight = state.height;
}

void BootstrapRenderer::renderGaussians(const FrameState& state) {
    if (!gs_) {
        gs_ = std::make_unique<StaticGsRenderer>(context_, allocator_);
    }
    gs_->render(state, image_, imageView_, imageHasContent_, report_);
    imageHasContent_ = true;
    timestampsRecorded_ = false;
    report_.frame.cpuSubmitMs = elapsedMs(frameStart_); // includes blocking backend fence waits
}

void BootstrapRenderer::renderPlaceholder(const FrameState& state) {
    auto scratch = buffers_->acquire(placeholderScratchBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    try {
        beginOneTimeCommands();
        if (queries_) {
            vkCmdResetQueryPool(command_, queries_, timestampFirstQuery, timestampQueryCount);
            vkCmdWriteTimestamp(command_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries_, timestampFirstQuery);
        }
        vkCmdFillBuffer(command_, scratch.buffer, 0, placeholderScratchBytes, 0);

        const auto toTransferDst = targetBarrier(image_, imageHasContent_);
        const auto sourceStage =
            imageHasContent_ ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        applyImageBarrier(command_, sourceStage, VK_PIPELINE_STAGE_TRANSFER_BIT, toTransferDst);

        const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(state.timestampSeconds));
        const VkClearColorValue idleColor{
            {0.025f + pulse * 0.015f, 0.045f + pulse * 0.025f, 0.075f + pulse * 0.04f, 1.0f}};
        vkCmdClearColorImage(command_,
                             image_,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             &idleColor,
                             1,
                             &toTransferDst.subresourceRange);

        auto toReadable = toTransferDst;
        toReadable.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toReadable.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        toReadable.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toReadable.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        applyImageBarrier(
            command_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, toReadable);

        if (queries_) {
            vkCmdWriteTimestamp(command_, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries_, timestampLastQuery);
        }
        vkCheck(vkEndCommandBuffer(command_), "vkEndCommandBuffer");
        submitPlaceholderFrame();

        report_.frame.cpuSubmitMs = elapsedMs(frameStart_);
        waitSubmission();
        imageHasContent_ = true;
        timestampsRecorded_ = static_cast<bool>(queries_);
        buffers_->release(scratch);
    } catch (...) {
        // The pool is torn down only after device idle if a submitted frame failed.
        if (!fencePending_) {
            buffers_->release(scratch);
        }
        throw;
    }
}

void BootstrapRenderer::beginOneTimeCommands() {
    vkCheck(vkResetCommandPool(context_.device, commands_, 0), "vkResetCommandPool");
    auto begin = vkInfo<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkCheck(vkBeginCommandBuffer(command_, &begin), "vkBeginCommandBuffer");
}

void BootstrapRenderer::submitPlaceholderFrame() {
    vkCheck(vkResetFences(context_.device, 1, &fence_), "vkResetFences");
    auto submit = vkInfo<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command_;
    vkCheck(vkQueueSubmit(context_.queue, 1, &submit, fence_), "vkQueueSubmit bootstrap");
    fencePending_ = true;
}

FrameReport BootstrapRenderer::finishFrame() {
    guard_.check();
    submitCompletionFence();
    readGpuTimestamps();
    report_.backend.buffers = buffers_->stats();
    ++report_.frame.frameIndex;
    sampleHeapBudgets();

    const auto completed = Clock::now();
    auto& frame = report_.frame;
    frame.completeFrameMs = std::chrono::duration<double, std::milli>(completed - frameStart_).count();
    if (hasPreviousCompletion_) {
        frame.frameIntervalMs =
            std::chrono::duration<double, std::milli>(completed - previousCompletion_).count();
    }
    previousCompletion_ = completed;
    hasPreviousCompletion_ = true;
    return report_;
}

void BootstrapRenderer::submitCompletionFence() {
    // This empty submission is after Qt's actual submission on the SAME queue.
    // Its fence covers both the native target and Qt's texture sampling.
    vkCheck(vkResetFences(context_.device, 1, &fence_), "vkResetFences after Qt");
    vkCheck(vkQueueSubmit(context_.queue, 0, nullptr, fence_), "vkQueueSubmit frame-completion fence");
    fencePending_ = true;
    waitSubmission();
}

void BootstrapRenderer::readGpuTimestamps() {
    if (!timestampsRecorded_) {
        return;
    }
    std::array<std::uint64_t, timestampQueryCount> ticks{};
    vkCheck(vkGetQueryPoolResults(context_.device,
                                  queries_,
                                  timestampFirstQuery,
                                  timestampQueryCount,
                                  sizeof(ticks),
                                  ticks.data(),
                                  sizeof(std::uint64_t),
                                  VK_QUERY_RESULT_64_BIT),
            "vkGetQueryPoolResults");
    const auto mask = timestampBits_ >= 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << timestampBits_) - 1;
    report_.frame.gpuRenderMs =
        static_cast<double>((ticks[timestampLastQuery] - ticks[timestampFirstQuery]) & mask) *
        timestampPeriod_ / nanosecondsPerMillisecond;
}

void BootstrapRenderer::sampleHeapBudgets() {
    // Cached counters, no per-allocation walk. Memory display is sampled every 30 frames.
    if (report_.frame.gaussianCount && report_.frame.frameIndex != 1 &&
        report_.frame.frameIndex % memorySampleIntervalFrames != 0) {
        return;
    }
    VmaBudget heaps[VK_MAX_MEMORY_HEAPS]{};
    vmaGetHeapBudgets(allocator_, heaps);
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(context_.physicalDevice, &properties);
    auto& backend = report_.backend;
    backend.allocatorUsedBytes = 0;
    backend.allocatorReservedBytes = 0;
    for (std::uint32_t heap = 0; heap < properties.memoryHeapCount; ++heap) {
        backend.allocatorUsedBytes += heaps[heap].statistics.allocationBytes;
        backend.allocatorReservedBytes += heaps[heap].statistics.blockBytes;
    }
    backend.allocatorPeakBytes = std::max(backend.allocatorPeakBytes, backend.allocatorUsedBytes);
}
} // namespace orbelio
