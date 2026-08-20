#pragma once

#include <atomic>
#include <cstdint>

namespace sho {

struct StereoFrame {
  float left{};
  float right{};
};

struct StreamStatistics {
  std::atomic<std::uint64_t> capturedFrames{};
  std::atomic<std::uint64_t> resampledFrames{};
  std::atomic<std::uint64_t> sentPackets{};
  std::atomic<std::uint64_t> underrunPackets{};
  std::atomic<std::uint64_t> discardedFrames{};
  std::atomic<std::uint64_t> latePackets{};
  std::atomic<std::uint64_t> captureDiscontinuities{};
  std::atomic<std::uint64_t> hidErrors{};
  std::atomic<std::int32_t> correctionPpm{};
  std::atomic<std::uint32_t> inputQueueFrames{};
  std::atomic<std::uint32_t> outputQueueFrames{};
};

} // namespace sho
