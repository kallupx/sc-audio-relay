#include "sho/PipeWireCapture.hpp"

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw-utils.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <utility>

namespace sho {

struct PipeWireCapture::Impl {
  Impl(CaptureRing& outputRing, StreamStatistics& streamStats, std::string targetName)
      : output(outputRing), stats(streamStats), target(std::move(targetName)) {}

  ~Impl() {
    stop();
    if (stream != nullptr) {
      pw_stream_destroy(stream);
    }
    if (loop != nullptr) {
      pw_main_loop_destroy(loop);
    }
    if (initialized) {
      pw_deinit();
    }
  }

  static void stateChanged(void* data, pw_stream_state, pw_stream_state state,
                           const char*) noexcept {
    if (state == PW_STREAM_STATE_ERROR || state == PW_STREAM_STATE_UNCONNECTED) {
      static_cast<Impl*>(data)->failed.store(true, std::memory_order_relaxed);
    }
  }

  static void paramChanged(void* data, std::uint32_t id, const spa_pod* param) noexcept {
    if (id != SPA_PARAM_Format || param == nullptr) {
      return;
    }
    spa_audio_info_raw info{};
    const bool valid = spa_format_audio_raw_parse(param, &info) >= 0 &&
                       info.format == SPA_AUDIO_FORMAT_F32 && info.rate == 48000 &&
                       info.channels == 2;
    static_cast<Impl*>(data)->formatValid.store(valid, std::memory_order_release);
  }

  static void process(void* data) noexcept {
    auto& self = *static_cast<Impl*>(data);
    pw_buffer* pipewireBuffer = pw_stream_dequeue_buffer(self.stream);
    if (pipewireBuffer == nullptr) {
      return;
    }

    spa_buffer* buffer = pipewireBuffer->buffer;
    if (!self.formatValid.load(std::memory_order_acquire) || buffer == nullptr ||
        buffer->n_datas == 0) {
      pw_stream_queue_buffer(self.stream, pipewireBuffer);
      return;
    }

    const spa_data& audio = buffer->datas[0];
    if (audio.data == nullptr || audio.chunk == nullptr) {
      pw_stream_queue_buffer(self.stream, pipewireBuffer);
      return;
    }

    const auto offset = std::min(audio.chunk->offset, audio.maxsize);
    const auto bytes = std::min(audio.chunk->size, audio.maxsize - offset);
    const auto stride = audio.chunk->stride >= static_cast<std::int32_t>(sizeof(StereoFrame))
                            ? static_cast<std::size_t>(audio.chunk->stride)
                            : sizeof(StereoFrame);
    const auto frames = static_cast<std::size_t>(bytes) / stride;
    const auto* source = static_cast<const std::byte*>(audio.data) + offset;
    std::size_t captured = 0;
    for (; captured < frames; ++captured) {
      StereoFrame frame{};
      std::memcpy(&frame.left, source + captured * stride, sizeof(float));
      std::memcpy(&frame.right, source + captured * stride + sizeof(float), sizeof(float));
      if (!self.output.push(frame)) {
        self.stats.captureDiscontinuities.fetch_add(1, std::memory_order_relaxed);
        break;
      }
    }
    self.stats.capturedFrames.fetch_add(frames, std::memory_order_relaxed);
    self.stats.inputQueueFrames.store(static_cast<std::uint32_t>(self.output.size()),
                                      std::memory_order_relaxed);
    pw_stream_queue_buffer(self.stream, pipewireBuffer);
  }

  void start() {
    if (thread.joinable()) {
      return;
    }
    pw_init(nullptr, nullptr);
    initialized = true;
    loop = pw_main_loop_new(nullptr);
    if (loop == nullptr) {
      throw std::runtime_error("PipeWire: could not create main loop");
    }

    pw_properties* properties = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE,
        "Accessibility", PW_KEY_STREAM_CAPTURE_SINK, "true", PW_KEY_NODE_NAME,
        "steam-haptics-output", nullptr);
    if (properties == nullptr) {
      throw std::runtime_error("PipeWire: could not allocate stream properties");
    }
    if (!target.empty()) {
      pw_properties_set(properties, PW_KEY_TARGET_OBJECT, target.c_str());
    }

    static constexpr pw_stream_events events{PW_VERSION_STREAM_EVENTS,
                                              nullptr,
                                              stateChanged,
                                              nullptr,
                                              nullptr,
                                              paramChanged,
                                              nullptr,
                                              nullptr,
                                              process,
                                              nullptr,
                                              nullptr,
                                              nullptr};
    stream = pw_stream_new_simple(pw_main_loop_get_loop(loop), "Steam Controller Haptics Mirror",
                                  properties, &events, this);
    if (stream == nullptr) {
      throw std::runtime_error("PipeWire: could not create capture stream");
    }

    std::uint8_t podBuffer[1024]{};
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(podBuffer, sizeof(podBuffer));
    spa_audio_info_raw format{};
    format.format = SPA_AUDIO_FORMAT_F32;
    format.rate = 48000;
    format.channels = 2;
    format.position[0] = SPA_AUDIO_CHANNEL_FL;
    format.position[1] = SPA_AUDIO_CHANNEL_FR;
    const spa_pod* params[]{
        spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &format),
    };
    const auto flags = static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT |
                                                    PW_STREAM_FLAG_MAP_BUFFERS |
                                                    PW_STREAM_FLAG_RT_PROCESS);
    if (pw_stream_connect(stream, PW_DIRECTION_INPUT, PW_ID_ANY, flags, params, 1) < 0) {
      throw std::runtime_error("PipeWire: could not connect to the sink monitor");
    }
    thread = std::jthread([this] { pw_main_loop_run(loop); });
  }

  void stop() noexcept {
    if (thread.joinable()) {
      pw_main_loop_quit(loop);
      thread.join();
    }
  }

  CaptureRing& output;
  StreamStatistics& stats;
  std::string target;
  pw_main_loop* loop{};
  pw_stream* stream{};
  std::jthread thread;
  std::atomic<bool> formatValid{};
  std::atomic<bool> failed{};
  bool initialized{};
};

PipeWireCapture::PipeWireCapture(CaptureRing& output, StreamStatistics& stats,
                                 std::string target)
    : impl_(std::make_unique<Impl>(output, stats, std::move(target))) {}

PipeWireCapture::~PipeWireCapture() = default;

void PipeWireCapture::start() { impl_->start(); }

void PipeWireCapture::stop() noexcept { impl_->stop(); }

bool PipeWireCapture::failed() const noexcept {
  return impl_->failed.load(std::memory_order_relaxed);
}

} // namespace sho
