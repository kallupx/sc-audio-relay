#pragma once

#include "sho/Controller.hpp"

#include <memory>

namespace sho {

enum class ControllerPreference { Auto, Wired, Wireless };

class TritonTransport final : public IControllerTransport {
public:
  explicit TritonTransport(ControllerPreference preference);
  ~TritonTransport() override;
  bool configure(ControllerLink link) override;
  bool send(const ControllerPcmPacket& packet) override;
  bool connected() const noexcept override;
  void disable() noexcept override;
  [[nodiscard]] ControllerLink link() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace sho
