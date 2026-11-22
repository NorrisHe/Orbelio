#pragma once
#include "native_raster_input.hpp"
#include "renderer_stats.hpp"

namespace orbelio {

class NativeFrameCommands {
public:
    enum class Stamp : std::uint32_t {
        ProjectionBegin,
        ProjectionEnd,
        ShadingEnd,
        SortEnd,
        BinningEnd,
        RasterEnd,
        CompositionEnd,
        Count
    };
    explicit NativeFrameCommands(DeviceContext context);
    ~NativeFrameCommands();
    NativeFrameCommands(const NativeFrameCommands&) = delete;
    NativeFrameCommands& operator=(const NativeFrameCommands&) = delete;

    VkCommandBuffer begin();
    void stamp(Stamp stage);
    double submitAndWait();
    NativeTileQueries tileQueries(bool capture) const;

    VkQueryPool fragmentQuery(bool capture) const { return capture ? statisticsQueries_ : VK_NULL_HANDLE; }

    void readTimestamps(const FrameState& state, FrameReport& report, bool tiled, bool captured);
    void readStatistics(BackendStats& stats, bool captured);

private:
    static constexpr auto frameTimestampCount = static_cast<std::uint32_t>(Stamp::Count);
    void createCommands();
    void createQueries();
    void createStatisticsQueries();
    void readTicks(std::uint32_t first, std::span<std::uint64_t> values);
    void readTileTimings(BackendStats& stats, const std::array<std::uint64_t, frameTimestampCount>& frame);
    void destroy() noexcept;

    DeviceContext context_;
    VkCommandPool commands_ = VK_NULL_HANDLE;
    VkCommandBuffer command_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    VkQueryPool queries_ = VK_NULL_HANDLE;
    VkQueryPool statisticsQueries_ = VK_NULL_HANDLE;
    double timestampPeriod_ = 0;
    std::uint64_t timestampMask_ = 0;
};
}
