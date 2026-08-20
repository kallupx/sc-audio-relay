#pragma once

#include "sho/Audio.hpp"
#include "sho/SpscRing.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <thread>

namespace sho {

enum class ControllerLink { Wired16Bit, WirelessMuLaw };

struct ControllerPcmPacket {
  std::uint8_t length{};
  std::array<std::uint8_t, 31> left{};
  std::array<std::uint8_t, 31> right{};
};

class IControllerTransport {
public:
  virtual ~IControllerTransport() = default;
  virtual bool configure(ControllerLink link) = 0;
  virtual bool send(const ControllerPcmPacket& packet) = 0;
  virtual bool connected() const noexcept = 0;
  virtual void disable() noexcept = 0;
};

std::int16_t floatToInt16(float sample) noexcept;
std::uint8_t linearToMuLaw(std::int16_t sample) noexcept;
ControllerPcmPacket buildWiredPacket(std::span<const StereoFrame, 15> frames) noexcept;
ControllerPcmPacket buildWirelessPacket(std::span<const StereoFrame, 31> frames) noexcept;

using OutputRing = SpscRing<StereoFrame, 1024>;

class ControllerStreamer {
public:
  ControllerStreamer(OutputRing& audio, IControllerTransport& transport,
                     ControllerLink link, StreamStatistics& stats);
  ~ControllerStreamer();
  void start();
  void stop();

private:
  void run(std::stop_token stop);

  OutputRing& audio_;
  IControllerTransport& transport_;
  ControllerLink link_;
  StreamStatistics& stats_;
  std::jthread thread_;
};

} // namespace sho
