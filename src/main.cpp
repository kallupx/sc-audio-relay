#include "sho/AudioPipeline.hpp"
#include "sho/PipeWireCapture.hpp"
#include "sho/TritonTransport.hpp"

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

namespace {

volatile std::sig_atomic_t stopping = 0;

void handleSignal(int) { stopping = 1; }

struct Options {
  std::string device;
  sho::PipeWireMode pipeWireMode{sho::PipeWireMode::Mirror};
  sho::ControllerPreference controller{sho::ControllerPreference::Auto};
  float gain{0.5F};
  std::uint16_t cuePort{28491};
  bool stats{true};
  bool help{};
};

constexpr std::string_view usage =
    "Usage: sc-audio-relay [--mirror-default | --mirror-device NAME | --virtual-output]\n"
    "                            [--gain 0..4] [--controller auto|wired|wireless]\n"
    "                            [--cue-port 1..65535]\n"
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
    } else if (arg == "--cue-port") {
      const auto value = nextValue(index, argc, argv, arg);
      unsigned port = 0;
      const auto result = std::from_chars(value.data(), value.data() + value.size(), port);
      if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || port == 0 ||
          port > 65535) {
        throw std::runtime_error("--cue-port must be a number from 1 to 65535");
      }
      options.cuePort = static_cast<std::uint16_t>(port);
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

class GameCueReceiver {
public:
  GameCueReceiver(sho::CueRing& output, std::uint16_t port) : output_(output), port_(port) {}
  ~GameCueReceiver() { stop(); }

  void start() {
    socket_ = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (socket_ < 0) {
      throw std::system_error(errno, std::generic_category(), "game cue socket");
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port_);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(socket_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0) {
      const int error = errno;
      ::close(socket_);
      socket_ = -1;
      throw std::system_error(error, std::generic_category(), "game cue bind");
    }

    thread_ = std::jthread([this](std::stop_token stop) {
      std::array<char, 256> message{};
      pollfd event{socket_, POLLIN, 0};
      while (!stop.stop_requested()) {
        const int ready = ::poll(&event, 1, 100);
        if (ready <= 0) {
          continue;
        }
        const auto bytes = ::recv(socket_, message.data(), message.size(), 0);
        if (bytes > 0) {
          sho::enqueueGameCue(
              std::string_view{message.data(), static_cast<std::size_t>(bytes)}, output_);
        }
      }
    });
  }

  void stop() noexcept {
    if (thread_.joinable()) {
      thread_.request_stop();
      thread_.join();
    }
    if (socket_ >= 0) {
      ::close(socket_);
      socket_ = -1;
    }
  }

private:
  sho::CueRing& output_;
  std::uint16_t port_;
  int socket_{-1};
  std::jthread thread_;
};

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
    sho::CueRing gameCues;
    sho::OutputRing controllerAudio;
    sho::StreamStatistics stats;
    sho::TritonTransport controller(options.controller);
    const auto link = controller.link();
    if (!controller.configure(link)) {
      throw std::runtime_error("controller: PCM setup failed");
    }

    sho::AudioPipeline pipeline(capturedAudio, gameCues, controllerAudio, stats, options.gain);
    sho::PipeWireCapture capture(capturedAudio, stats, options.pipeWireMode, options.device);
    sho::ControllerStreamer streamer(controllerAudio, controller, link, stats);
    GameCueReceiver cueReceiver(gameCues, options.cuePort);
    pipeline.start();
    capture.start();
    streamer.start();
    cueReceiver.start();

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
    std::cout << "Listening for HL2 cues on 127.0.0.1:" << options.cuePort << ".\n";

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

    cueReceiver.stop();
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
