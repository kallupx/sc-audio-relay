#include "sho/AudioPipeline.hpp"
#include "sho/PipeWireCapture.hpp"
#include "sho/TritonTransport.hpp"

#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace {

volatile std::sig_atomic_t stopping = 0;

void handleSignal(int) { stopping = 1; }

struct Options {
  std::string device;
  sho::PipeWireMode pipeWireMode{sho::PipeWireMode::Mirror};
  sho::ControllerPreference controller{sho::ControllerPreference::Auto};
  float gain{0.5F};
  bool stats{true};
  bool help{};
};

constexpr std::string_view usage =
    "Usage: sc-audio-relay [--mirror-default | --mirror-device NAME | --virtual-output]\n"
    "                            [--gain 0..4] [--controller auto|wired|wireless]\n"
    "                            [--stats | --no-stats]\n";

std::string_view nextValue(int& index, int argc, char** argv, std::string_view option) {
  if (++index >= argc) {
    throw std::runtime_error(std::string{option} + " requires a value");
  }
  return argv[index];
}

Options parseArgs(int argc, char** argv) {
  Options options;
  bool sourceChosen = false;
  for (int index = 1; index < argc; ++index) {
    const std::string_view arg = argv[index];
    if (arg == "--help" || arg == "-h") {
      options.help = true;
    } else if (arg == "--mirror-default") {
      if (sourceChosen) {
        throw std::runtime_error("choose only one audio source");
      }
      sourceChosen = true;
    } else if (arg == "--mirror-device") {
      if (sourceChosen) {
        throw std::runtime_error("choose only one audio source");
      }
      options.device = nextValue(index, argc, argv, arg);
      if (options.device.empty()) {
        throw std::runtime_error("--mirror-device cannot be empty");
      }
      sourceChosen = true;
    } else if (arg == "--gain") {
      const auto value = nextValue(index, argc, argv, arg);
      const auto result = std::from_chars(value.data(), value.data() + value.size(), options.gain);
      if (result.ec != std::errc{} || result.ptr != value.data() + value.size() ||
          options.gain < 0.0F || options.gain > 4.0F) {
        throw std::runtime_error("--gain must be a number from 0 to 4");
      }
    } else if (arg == "--controller") {
      const auto value = nextValue(index, argc, argv, arg);
      if (value == "auto") {
        options.controller = sho::ControllerPreference::Auto;
      } else if (value == "wired") {
        options.controller = sho::ControllerPreference::Wired;
      } else if (value == "wireless") {
        options.controller = sho::ControllerPreference::Wireless;
      } else {
        throw std::runtime_error("--controller must be auto, wired, or wireless");
      }
    } else if (arg == "--stats") {
      options.stats = true;
    } else if (arg == "--no-stats") {
      options.stats = false;
    } else if (arg == "--virtual-output") {
      if (sourceChosen) {
        throw std::runtime_error("choose only one audio source");
      }
      options.pipeWireMode = sho::PipeWireMode::VirtualOutput;
      sourceChosen = true;
    } else {
      throw std::runtime_error("unknown option: " + std::string{arg});
    }
  }
  return options;
}

} // namespace

int main(int argc, char** argv) {
  try {
    std::cout << std::unitbuf;
    const auto options = parseArgs(argc, argv);
    if (options.help) {
      std::cout << usage;
      return 0;
    }

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    sho::CaptureRing capturedAudio;
    sho::OutputRing controllerAudio;
    sho::StreamStatistics stats;
    sho::TritonTransport controller(options.controller);
    const auto link = controller.link();
    if (!controller.configure(link)) {
      throw std::runtime_error("controller: PCM setup failed");
    }

    sho::AudioPipeline pipeline(capturedAudio, controllerAudio, stats, options.gain);
    sho::PipeWireCapture capture(capturedAudio, stats, options.pipeWireMode, options.device);
    sho::ControllerStreamer streamer(controllerAudio, controller, link, stats);
    pipeline.start();
    capture.start();
    streamer.start();

    std::cout << "Streaming "
              << (options.pipeWireMode == sho::PipeWireMode::VirtualOutput
                      ? "Steam Controller Haptics"
                      : options.device.empty() ? "the default output" : options.device)
              << " to the "
              << (link == sho::ControllerLink::Wired16Bit ? "wired" : "wireless")
              << " controller; "
              << (options.pipeWireMode == sho::PipeWireMode::VirtualOutput
                      ? "route an app to it, then press Ctrl+C to stop.\n"
                      : "press Ctrl+C to stop.\n");

    std::uint64_t previousCaptured = 0;
    std::uint64_t previousResampled = 0;
    while (stopping == 0 && controller.connected() && !capture.failed()) {
      std::this_thread::sleep_for(std::chrono::seconds{2});
      if (!options.stats) {
        continue;
      }
      const auto captured = stats.capturedFrames.load(std::memory_order_relaxed);
      const auto resampled = stats.resampledFrames.load(std::memory_order_relaxed);
      std::cout << "capture=" << (captured - previousCaptured) / 2 << "/s resampled="
                << (resampled - previousResampled) / 2
                << "/s queue=" << stats.outputQueueFrames.load(std::memory_order_relaxed)
                << " underruns=" << stats.underrunPackets.load(std::memory_order_relaxed)
                << " late=" << stats.latePackets.load(std::memory_order_relaxed) << '\n';
      previousCaptured = captured;
      previousResampled = resampled;
    }

    capture.stop();
    pipeline.stop();
    streamer.stop();
    controller.disable();
    if (capture.failed()) {
      std::cerr << "sc-audio-relay: PipeWire stream disconnected\n";
      return 1;
    }
    return controller.connected() ? 0 : 1;
  } catch (const std::exception& error) {
    std::cerr << "sc-audio-relay: " << error.what() << '\n' << usage;
    return 1;
  }
}
