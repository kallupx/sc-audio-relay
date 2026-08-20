#include "sho/TritonTransport.hpp"

#include <TritonController.h>
#include <TritonFinder.h>

#include <cstring>
#include <memory>
#include <stdexcept>

namespace sho {

struct TritonTransport::Impl {
  explicit Impl(ControllerPreference preference) : finder(true), controller(finder.getController()) {
    if (!controller) {
      throw std::runtime_error("TritonLib: no supported Steam Controller found");
    }
    link = controller->pairType == ETritonPairType::k_ETritonPairType_Wired
               ? ControllerLink::Wired16Bit
               : ControllerLink::WirelessMuLaw;
    if ((preference == ControllerPreference::Wired && link != ControllerLink::Wired16Bit) ||
        (preference == ControllerPreference::Wireless &&
         link != ControllerLink::WirelessMuLaw)) {
      throw std::runtime_error("TritonLib: connected controller does not match --controller");
    }
  }

  TritonFinder finder;
  std::unique_ptr<TritonController> controller;
  ControllerLink link{};
};

TritonTransport::TritonTransport(ControllerPreference preference)
try : impl_(std::make_unique<Impl>(preference)) {
} catch (const char* error) {
  throw std::runtime_error(std::string{"TritonLib: "} + error);
}

TritonTransport::~TritonTransport() { disable(); }

bool TritonTransport::configure(ControllerLink link) {
  if (link != impl_->link || !connected()) {
    return false;
  }
  impl_->controller->setupPCMStreaming(
      link == ControllerLink::Wired16Bit ? TritonPCMMode::Khz8_16Bit
                                         : TritonPCMMode::Khz8_8Bit_ulaw);
  return connected();
}

bool TritonTransport::send(const ControllerPcmPacket& packet) {
  if (!connected()) {
    return false;
  }
  MsgHapticPCMStereo native{};
  native.length = packet.length;
  std::memcpy(native.left, packet.left.data(), packet.left.size());
  std::memcpy(native.right, packet.right.data(), packet.right.size());
  return impl_->controller->sendPCMStereo(&native) >= 0;
}

bool TritonTransport::connected() const noexcept {
  return impl_ && impl_->controller && !impl_->controller->disconnected.load();
}

void TritonTransport::disable() noexcept {
  if (connected()) {
    impl_->controller->setupPCMStreaming(TritonPCMMode::None);
  }
}

ControllerLink TritonTransport::link() const noexcept { return impl_->link; }

} // namespace sho
