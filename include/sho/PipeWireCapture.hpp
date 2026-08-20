#pragma once

#include "sho/AudioPipeline.hpp"

#include <memory>
#include <string>

namespace sho {

enum class PipeWireMode { Mirror, VirtualOutput };

class PipeWireCapture final : public IAudioCaptureSource {
public:
  PipeWireCapture(CaptureRing& output, StreamStatistics& stats, PipeWireMode mode,
                  std::string target = {});
  ~PipeWireCapture() override;
  void start() override;
  void stop() noexcept override;
  [[nodiscard]] bool failed() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace sho
