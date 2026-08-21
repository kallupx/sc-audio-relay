#pragma once

#include "sho/Audio.hpp"
#include "sho/Controller.hpp"
#include "sho/SpscRing.hpp"

#include <array>
#include <cstddef>
#include <span>
#include <string_view>
#include <thread>

struct SRC_STATE_tag;

namespace sho {

using CaptureRing = SpscRing<StereoFrame, 16384>;
using CueRing = SpscRing<StereoFrame, 16384>;

bool enqueueGameCue(std::string_view message, CueRing& output) noexcept;

class StreamingResampler {
public:
  StreamingResampler();
  ~StreamingResampler();
  StreamingResampler(const StreamingResampler&) = delete;
  StreamingResampler& operator=(const StreamingResampler&) = delete;

  std::size_t push(std::span<const StereoFrame> input) noexcept;
  std::size_t process(double ratio, std::span<StereoFrame> output);
  void reset() noexcept;
  [[nodiscard]] std::size_t inputSpace() const noexcept;

private:
  static constexpr std::size_t inputCapacity = 2048;
  SRC_STATE_tag* state_{};
  std::array<float, inputCapacity * 2> input_{};
  std::size_t inputFrames_{};
};

class AudioPipeline {
public:
  AudioPipeline(CaptureRing& input, CueRing& cues, OutputRing& output,
                StreamStatistics& stats, float gain);
  ~AudioPipeline();
  void start();
  void stop();

private:
  void run(std::stop_token stop);

  CaptureRing& input_;
  CueRing& cues_;
  OutputRing& output_;
  StreamStatistics& stats_;
  float gain_;
  std::jthread thread_;
};

class IAudioCaptureSource {
public:
  virtual ~IAudioCaptureSource() = default;
  virtual void start() = 0;
  virtual void stop() noexcept = 0;
};

} // namespace sho
