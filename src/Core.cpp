#include "sho/AudioPipeline.hpp"

#include <samplerate.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <thread>

namespace sho {

std::int16_t floatToInt16(float sample) noexcept {
  const auto clipped = std::clamp(sample, -1.0F, 1.0F);
  const auto scale = clipped < 0.0F ? 32768.0F : 32767.0F;
  return static_cast<std::int16_t>(std::lround(clipped * scale));
}

std::uint8_t linearToMuLaw(std::int16_t sample) noexcept {
  constexpr int bias = 0x84;
  constexpr int clip = 32635;
  int value = sample;
  const int sign = value < 0 ? 0x80 : 0;
  if (value < 0) {
    value = -value;
  }
  value = std::min(value, clip) + bias;

  int exponent = 7;
  for (int mask = 0x4000; exponent > 0 && (value & mask) == 0; mask >>= 1) {
    --exponent;
  }
  const int mantissa = (value >> (exponent + 3)) & 0x0F;
  return static_cast<std::uint8_t>(~(sign | (exponent << 4) | mantissa));
}

ControllerPcmPacket buildWiredPacket(std::span<const StereoFrame, 15> frames) noexcept {
  ControllerPcmPacket packet{};
  packet.length = 30;
  for (std::size_t i = 0; i < frames.size(); ++i) {
    const auto left = static_cast<std::uint16_t>(floatToInt16(frames[i].left));
    const auto right = static_cast<std::uint16_t>(floatToInt16(frames[i].right));
    packet.left[i * 2] = static_cast<std::uint8_t>(left & 0xFFU);
    packet.left[i * 2 + 1] = static_cast<std::uint8_t>(left >> 8U);
    packet.right[i * 2] = static_cast<std::uint8_t>(right & 0xFFU);
    packet.right[i * 2 + 1] = static_cast<std::uint8_t>(right >> 8U);
  }
  return packet;
}

ControllerPcmPacket buildWirelessPacket(std::span<const StereoFrame, 31> frames) noexcept {
  ControllerPcmPacket packet{};
  packet.length = 31;
  for (std::size_t i = 0; i < frames.size(); ++i) {
    packet.left[i] = linearToMuLaw(floatToInt16(frames[i].left));
    packet.right[i] = linearToMuLaw(floatToInt16(frames[i].right));
  }
  return packet;
}

ControllerStreamer::ControllerStreamer(OutputRing& audio, IControllerTransport& transport,
                                       ControllerLink link, StreamStatistics& stats)
    : audio_(audio), transport_(transport), link_(link), stats_(stats) {}

ControllerStreamer::~ControllerStreamer() { stop(); }

void ControllerStreamer::start() {
  if (!thread_.joinable()) {
    thread_ = std::jthread([this](std::stop_token stop) { run(stop); });
  }
}

void ControllerStreamer::stop() {
  if (thread_.joinable()) {
    thread_.request_stop();
    thread_.join();
  }
}

void ControllerStreamer::run(std::stop_token stop) {
  using Clock = std::chrono::steady_clock;
  const auto period = link_ == ControllerLink::Wired16Bit
                          ? std::chrono::nanoseconds{1'875'000}
                          : std::chrono::nanoseconds{3'875'000};
  auto deadline = Clock::now();
  auto capturedFrames = stats_.capturedFrames.load(std::memory_order_relaxed);
  auto lastCapture = deadline;
  bool captureActive = capturedFrames != 0;

  while (!stop.stop_requested() && transport_.connected()) {
    std::this_thread::sleep_until(deadline);
    const auto now = Clock::now();
    const auto captured = stats_.capturedFrames.load(std::memory_order_relaxed);
    if (captured != capturedFrames) {
      capturedFrames = captured;
      lastCapture = now;
      captureActive = true;
    } else if (captureActive && now - lastCapture >= std::chrono::milliseconds{100}) {
      // ponytail: this short window avoids backend-specific idle plumbing; expose
      // capture activity explicitly if diagnostics need exact stop times.
      captureActive = false;
    }
    if (now > deadline + period * 3) {
      ++stats_.latePackets;
      deadline = now;
    }

    bool underrun = false;
    bool sent = false;
    if (link_ == ControllerLink::Wired16Bit) {
      std::array<StereoFrame, 15> frames{};
      for (auto& frame : frames) {
        underrun |= !audio_.pop(frame);
      }
      sent = transport_.send(buildWiredPacket(frames));
    } else {
      std::array<StereoFrame, 31> frames{};
      for (auto& frame : frames) {
        underrun |= !audio_.pop(frame);
      }
      sent = transport_.send(buildWirelessPacket(frames));
    }

    if (underrun && captureActive) {
      ++stats_.underrunPackets;
    }
    if (!sent) {
      ++stats_.hidErrors;
      break;
    }
    ++stats_.sentPackets;
    stats_.outputQueueFrames.store(static_cast<std::uint32_t>(audio_.size()),
                                   std::memory_order_relaxed);
    deadline += period;
  }
}

StreamingResampler::StreamingResampler() {
  int error = 0;
  state_ = src_new(SRC_SINC_FASTEST, 2, &error);
  if (state_ == nullptr) {
    throw std::runtime_error(std::string{"libsamplerate: "} + src_strerror(error));
  }
}

StreamingResampler::~StreamingResampler() {
  if (state_ != nullptr) {
    src_delete(state_);
  }
}

std::size_t StreamingResampler::push(std::span<const StereoFrame> input) noexcept {
  const auto accepted = std::min(input.size(), inputSpace());
  for (std::size_t i = 0; i < accepted; ++i) {
    input_[(inputFrames_ + i) * 2] = input[i].left;
    input_[(inputFrames_ + i) * 2 + 1] = input[i].right;
  }
  inputFrames_ += accepted;
  return accepted;
}

std::size_t StreamingResampler::process(double ratio, std::span<StereoFrame> output) {
  if (inputFrames_ == 0 || output.empty()) {
    return 0;
  }

  std::array<float, 512> converted{};
  const auto outputFrames = std::min(output.size(), converted.size() / 2);
  SRC_DATA data{};
  data.data_in = input_.data();
  data.data_out = converted.data();
  data.input_frames = static_cast<long>(inputFrames_);
  data.output_frames = static_cast<long>(outputFrames);
  data.src_ratio = ratio;
  if (const int error = src_process(state_, &data); error != 0) {
    throw std::runtime_error(std::string{"libsamplerate: "} + src_strerror(error));
  }

  const auto consumed = static_cast<std::size_t>(data.input_frames_used);
  inputFrames_ -= consumed;
  std::memmove(input_.data(), input_.data() + consumed * 2,
               inputFrames_ * 2 * sizeof(float));

  const auto generated = static_cast<std::size_t>(data.output_frames_gen);
  for (std::size_t i = 0; i < generated; ++i) {
    output[i] = {converted[i * 2], converted[i * 2 + 1]};
  }
  return generated;
}

void StreamingResampler::reset() noexcept {
  src_reset(state_);
  inputFrames_ = 0;
}

std::size_t StreamingResampler::inputSpace() const noexcept {
  return inputCapacity - inputFrames_;
}

AudioPipeline::AudioPipeline(CaptureRing& input, OutputRing& output,
                             StreamStatistics& stats, float gain)
    : input_(input), output_(output), stats_(stats), gain_(gain) {}

AudioPipeline::~AudioPipeline() { stop(); }

void AudioPipeline::start() {
  if (!thread_.joinable()) {
    thread_ = std::jthread([this](std::stop_token stop) { run(stop); });
  }
}

void AudioPipeline::stop() {
  if (thread_.joinable()) {
    thread_.request_stop();
    thread_.join();
  }
}

void AudioPipeline::run(std::stop_token stop) {
  constexpr double ratio = 8000.0 / 48000.0;
  constexpr float dcCoefficient = 0.995F;
  StreamingResampler resampler;
  std::array<StereoFrame, 512> inputFrames{};
  std::array<StereoFrame, 256> outputFrames{};
  StereoFrame previousInput{};
  StereoFrame previousOutput{};

  const auto processSample = [this](float sample, float& oldInput, float& oldOutput) {
    const float dcBlocked = sample - oldInput + dcCoefficient * oldOutput;
    oldInput = sample;
    oldOutput = dcBlocked;
    const float amplified = dcBlocked * gain_;
    const float magnitude = std::abs(amplified);
    if (magnitude <= 0.95F) {
      return amplified;
    }
    return std::copysign(0.95F + 0.05F * std::tanh((magnitude - 0.95F) / 0.05F),
                         amplified);
  };

  while (!stop.stop_requested()) {
    bool worked = false;
    const auto wanted = std::min(inputFrames.size(), resampler.inputSpace());
    std::size_t count = 0;
    while (count < wanted && input_.pop(inputFrames[count])) {
      auto& frame = inputFrames[count++];
      frame.left = processSample(frame.left, previousInput.left, previousOutput.left);
      frame.right = processSample(frame.right, previousInput.right, previousOutput.right);
    }
    if (count != 0) {
      resampler.push(std::span{inputFrames}.first(count));
      worked = true;
    }

    const auto room = std::min(outputFrames.size(), output_.freeSpace());
    if (room != 0) {
      const auto generated = resampler.process(ratio, std::span{outputFrames}.first(room));
      for (std::size_t i = 0; i < generated; ++i) {
        static_cast<void>(output_.push(outputFrames[i]));
      }
      if (generated != 0) {
        stats_.resampledFrames.fetch_add(generated, std::memory_order_relaxed);
        worked = true;
      }
    }

    stats_.inputQueueFrames.store(static_cast<std::uint32_t>(input_.size()),
                                  std::memory_order_relaxed);
    stats_.outputQueueFrames.store(static_cast<std::uint32_t>(output_.size()),
                                   std::memory_order_relaxed);
    if (!worked) {
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
  }
}

} // namespace sho
