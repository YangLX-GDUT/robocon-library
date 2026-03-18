#ifndef BSP_EVENT_HPP
#define BSP_EVENT_HPP

#include <chrono>
#include <cmsis_os2.h>
#include <utility>

#include "bsp_type_traits.hpp"

namespace gdut {

struct empty_event_flags_t {
  explicit empty_event_flags_t() = default;
};
inline constexpr empty_event_flags_t empty_event_flags{};

class event_flags {
public:
  event_flags() { m_id = osEventFlagsNew(nullptr); }

  explicit event_flags(empty_event_flags_t) {}

  // 从已有的 CMSIS-RTOS 事件标志 ID 构造。
  // 允许传入 nullptr，会得到一个无效的 event_flags 对象
  // （valid() 返回 false）；成员函数会根据现有的 nullptr 检查
  // 返回错误码或 0。
  explicit event_flags(osEventFlagsId_t id) : m_id(id) {}

  ~event_flags() {
    if (m_id != nullptr) {
      osEventFlagsDelete(m_id);
    }
  }

  event_flags(const event_flags &) = delete;
  event_flags &operator=(const event_flags &) = delete;

  event_flags(event_flags &&other) noexcept
      : m_id(std::exchange(other.m_id, nullptr)) {}
  event_flags &operator=(event_flags &&other) noexcept {
    if (this != std::addressof(other)) {
      if (m_id != nullptr) {
        osEventFlagsDelete(m_id);
      }
      m_id = std::exchange(other.m_id, nullptr);
    }
    return *this;
  }

  uint32_t set(uint32_t flags) {
    if (m_id == nullptr)
      return osFlagsErrorParameter;
    return osEventFlagsSet(m_id, flags);
  }

  uint32_t clear(uint32_t flags) {
    if (m_id == nullptr)
      return osFlagsErrorParameter;
    return osEventFlagsClear(m_id, flags);
  }

  uint32_t get() const {
    if (m_id == nullptr)
      return 0;
    return osEventFlagsGet(m_id);
  }

  uint32_t
  wait(uint32_t flags,
       std::chrono::milliseconds timeout = std::chrono::milliseconds::max(),
       bool wait_all = false, bool no_clear = false) {
    if (m_id == nullptr)
      return osFlagsErrorParameter;

    uint32_t options = 0;
    if (wait_all)
      options |= osFlagsWaitAll;
    if (no_clear)
      options |= osFlagsNoClear;
    return osEventFlagsWait(m_id, flags, options, time_to_ticks(timeout));
  }

  bool valid() const noexcept { return m_id != nullptr; }
  explicit operator bool() const noexcept { return valid(); }

private:
  osEventFlagsId_t m_id{nullptr};
};

} // namespace gdut

#endif // BSP_EVENT_HPP
