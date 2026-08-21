#include "sho/AudioPipeline.hpp"
#include "sho/Controller.hpp"
#include "sho/SpscRing.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace {

void check(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string{message});
  }
}

void packetTests() {
  std::array<sho::StereoFrame, 15> wiredFrames{};
  wiredFrames[0] = {1.0F, -1.0F};
  wiredFrames[1] = {0.5F, -0.5F};
  const auto wired = sho::buildWiredPacket(wiredFrames);
  check(wired.length == 30, "wired byte count");
  check(wired.left[0] == 0xFF && wired.left[1] == 0x7F, "wired positive full scale");
  check(wired.right[0] == 0x00 && wired.right[1] == 0x80, "wired negative full scale");
  check(wired.left[2] == 0x00 && wired.left[3] == 0x40, "wired little endian");
  check(wired.right[2] == 0x00 && wired.right[3] == 0xC0, "wired planar channels");
  check(wired.left[4] == 0 && wired.right[4] == 0, "wired silence");

  std::array<sho::StereoFrame, 31> wirelessFrames{};
  wirelessFrames[0] = {1.0F, -1.0F};
  const auto wireless = sho::buildWirelessPacket(wirelessFrames);
  check(wireless.length == 31, "wireless byte count");
  check(wireless.left[0] == 0x80 && wireless.right[0] == 0x00,
        "wireless full scale");
  check(wireless.left[1] == 0xFF && wireless.right[30] == 0xFF,
        "wireless mu-law silence");
}

void muLawTests() {
  check(sho::linearToMuLaw(0) == 0xFF, "mu-law zero");
  check(sho::linearToMuLaw(1000) == 0xCE, "mu-law +1000 golden vector");
  check(sho::linearToMuLaw(-1000) == 0x4E, "mu-law -1000 golden vector");
  check(sho::linearToMuLaw(32767) == 0x80, "mu-law positive full scale");
  check(sho::linearToMuLaw(-32768) == 0x00, "mu-law negative full scale");
}

void queueTests() {
  sho::SpscRing<int, 4> queue;
  int value = 0;
  check(!queue.pop(value), "empty queue");
  check(queue.push(1) && queue.push(2) && queue.push(3) && queue.push(4), "fill queue");
  check(!queue.push(5), "full queue preserves unread data");
  check(queue.pop(value) && value == 1, "queue order");
  check(queue.push(5), "queue wrap");
  check(queue.discard(2) == 2, "discard oldest frames");
  check(queue.pop(value) && value == 4, "discard order");
  check(queue.pop(value) && value == 5, "wrapped value");
  check(!queue.pop(value), "queue empty after wrap");

  sho::SpscRing<std::uint32_t, 1024> stress;
  constexpr std::uint32_t iterations = 100'000;
  std::atomic<bool> reordered{};
  std::jthread producer([&] {
    for (std::uint32_t i = 0; i < iterations; ++i) {
      while (!stress.push(i)) {
        std::this_thread::yield();
      }
    }
  });
  std::jthread consumer([&] {
    for (std::uint32_t expected = 0; expected < iterations; ++expected) {
      std::uint32_t actual = 0;
      while (!stress.pop(actual)) {
        std::this_thread::yield();
      }
      if (actual != expected) {
        reordered.store(true);
      }
    }
  });
  producer.join();
  consumer.join();
  check(!reordered.load(), "threaded queue order");
}

void resamplerTest() {
  sho::StreamingResampler resampler;
  std::array<sho::StereoFrame, 480> input{};
  std::array<sho::StereoFrame, 256> output{};
  std::size_t generated = 0;
  for (std::size_t block = 0; block < 200; ++block) {
    for (std::size_t frame = 0; frame < input.size(); ++frame) {
      const auto sampleIndex = block * input.size() + frame;
      const auto sample = static_cast<float>(
          std::sin(2.0 * 3.141592653589793 * 440.0 * static_cast<double>(sampleIndex) /
                   48000.0));
      input[frame] = {sample, sample};
    }
    check(resampler.push(input) == input.size(), "resampler accepts streaming block");
    while (const auto count = resampler.process(8000.0 / 48000.0, output)) {
      generated += count;
    }
  }
  check(generated > 15'800 && generated <= 16'000, "48 kHz resamples to about 8 kHz");
  resampler.reset();
  check(resampler.inputSpace() == 2048, "resampler reset clears pending input");
}

void gameCueTest() {
  sho::CueRing cues;
  constexpr std::array names{"TEST", "PISTOL_FIRE", "SHOTGUN_FIRE",
                             "GRAVITY_GUN_PICKUP", "GRAVITY_GUN_LAUNCH"};
  for (const auto* name : names) {
    const std::string message = std::string{"SC_AUDIO/1 PLAY_CUE "} + name +
                                " gain=0.5 pan=0";
    check(sho::enqueueGameCue(message, cues), "known game cue accepted");
    check(cues.size() != 0, "game cue produced audio");
    cues.clear();
  }
  check(!sho::enqueueGameCue("SC_AUDIO/2 PLAY_CUE TEST gain=1 pan=0", cues),
        "unknown protocol rejected");
  check(!sho::enqueueGameCue("SC_AUDIO/1 PLAY_CUE UNKNOWN gain=1 pan=0", cues),
        "unknown cue rejected");
  check(!sho::enqueueGameCue("SC_AUDIO/1 PLAY_CUE TEST gain=99 pan=0", cues),
        "invalid gain rejected");
}

class FakeController final : public sho::IControllerTransport {
public:
  bool configure(sho::ControllerLink) override { return true; }
  bool send(const sho::ControllerPcmPacket&) override { return true; }
  bool connected() const noexcept override { return true; }
  void disable() noexcept override {}
};

void underrunTest() {
  const auto runEmptyStreamer = [](std::uint64_t capturedFrames) {
    sho::OutputRing audio;
    sho::StreamStatistics stats;
    stats.capturedFrames.store(capturedFrames);
    FakeController controller;
    sho::ControllerStreamer streamer(audio, controller, sho::ControllerLink::Wired16Bit, stats);
    streamer.start();
    for (int wait = 0; wait < 100 && stats.sentPackets.load() < 4; ++wait) {
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    streamer.stop();
    check(stats.sentPackets.load() >= 4, "controller streamer ran");
    return stats.underrunPackets.load();
  };

  check(runEmptyStreamer(0) == 0, "idle silence is not an underrun");
  check(runEmptyStreamer(1) != 0, "active capture starvation is an underrun");
}

} // namespace

int main() {
  try {
    packetTests();
    muLawTests();
    queueTests();
    resamplerTest();
    gameCueTest();
    underrunTest();
    std::cout << "packet, mu-law, SPSC queue, resampler, game cue, and underrun tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "test failure: " << error.what() << '\n';
    return 1;
  }
}
