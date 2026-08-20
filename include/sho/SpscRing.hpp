#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <type_traits>

namespace sho {

template <typename T, std::size_t Capacity>
class SpscRing {
  static_assert(std::has_single_bit(Capacity));
  static_assert(std::is_trivially_copyable_v<T>);

public:
  bool push(const T& value) noexcept {
    const auto head = head_.load(std::memory_order_relaxed);
    if (head - tail_.load(std::memory_order_acquire) == Capacity) {
      return false;
    }
    data_[head & (Capacity - 1)] = value;
    head_.store(head + 1, std::memory_order_release);
    return true;
  }

  bool pop(T& value) noexcept {
    const auto tail = tail_.load(std::memory_order_relaxed);
    if (tail == head_.load(std::memory_order_acquire)) {
      return false;
    }
    value = data_[tail & (Capacity - 1)];
    tail_.store(tail + 1, std::memory_order_release);
    return true;
  }

  std::size_t discard(std::size_t count) noexcept {
    const auto tail = tail_.load(std::memory_order_relaxed);
    const auto available = head_.load(std::memory_order_acquire) - tail;
    const auto removed = count < available ? count : available;
    tail_.store(tail + removed, std::memory_order_release);
    return removed;
  }

  void clear() noexcept { discard(size()); }

  [[nodiscard]] std::size_t size() const noexcept {
    return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire);
  }

  [[nodiscard]] std::size_t freeSpace() const noexcept { return Capacity - size(); }

private:
  std::array<T, Capacity> data_{};
  alignas(64) std::atomic<std::size_t> head_{};
  alignas(64) std::atomic<std::size_t> tail_{};
};

} // namespace sho
