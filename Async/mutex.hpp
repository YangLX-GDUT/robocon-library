#ifndef GDUT_MUTEX_CMSIS_RTOS2_INCLUDED
#define GDUT_MUTEX_CMSIS_RTOS2_INCLUDED

#include <cmsis_os2.h>
#include <exception>
#include <memory>
#include <utility>

namespace gdut {

struct empty_mutex_t {
  explicit empty_mutex_t() = default;
};
inline constexpr empty_mutex_t empty_mutex{};

/**
 * @brief CMSIS-RTOS2 互斥锁的 RAII 包装
 *
 * 该类提供了 C++ 风格的互斥锁包装，基于 CMSIS-RTOS2 osMutex。
 * 特性：
 * - 支持递归互斥锁和优先级继承
 * - 支持移动语义
 *
 * 线程安全：所有方法都是线程安全的。
 *
 * 重要：互斥锁创建可能失败，原因如下：
 * - 系统资源（FreeRTOS 堆）耗尽
 * - osKernelInitialize() 尚未被调用
 * 使用 valid() 方法或布尔转换检查互斥锁是否成功创建。
 * 若互斥锁无效，lock() 会调用 std::terminate()，try_lock() 返回 false。
 */
class mutex {
public:
  mutex() : m_mutex_id(nullptr) {
    osMutexAttr_t attr = {"GDUT", osMutexRecursive | osMutexPrioInherit, 0, 0};
    m_mutex_id = osMutexNew(&attr);
  }
  explicit mutex(empty_mutex_t) : m_mutex_id(nullptr) {}

  /**
   * @brief 从已存在的 CMSIS-RTOS2 互斥锁 ID 构造包装器。
   *
   * 此构造函数取得 @p mutex_id 的所有权：销毁或移动时会调用
   * @c osMutexDelete 传递句柄后，不要在其他地方删除或管理互斥锁。
   * 允许传递 @c nullptr 结果是一个无效的互斥锁对象，
   * 等价于用 ::gdut::empty_mutex 构造。此时
   * valid() 和 operator bool() 返回 false，加锁操作返回错误码。
   */
  explicit mutex(osMutexId_t mutex_id) : m_mutex_id(mutex_id) {}

  mutex(const mutex &) = delete;
  mutex &operator=(const mutex &) = delete;

  mutex(mutex &&other) noexcept
      : m_mutex_id(std::exchange(other.m_mutex_id, nullptr)) {}
  mutex &operator=(mutex &&other) noexcept {
    if (this != std::addressof(other)) {
      if (m_mutex_id != nullptr) {
        osMutexDelete(m_mutex_id);
      }
      m_mutex_id = std::exchange(other.m_mutex_id, nullptr);
    }
    return *this;
  }

  ~mutex() noexcept {
    if (m_mutex_id != nullptr) {
      osMutexDelete(m_mutex_id);
    }
  }

  void lock() {
    if (m_mutex_id == nullptr) {
      std::terminate();
    }
    osMutexAcquire(m_mutex_id, osWaitForever);
  }

  bool try_lock() {
    if (m_mutex_id == nullptr) {
      std::terminate();
    }
    return osMutexAcquire(m_mutex_id, 0) == osOK;
  }

  void unlock() {
    if (m_mutex_id == nullptr) {
      std::terminate();
    }
    osMutexRelease(m_mutex_id);
  }

  /**
   * @brief 检查互斥锁是否成功创建
   * @return 若互斥锁有效且可使用，返回 true
   */
  bool valid() const noexcept { return m_mutex_id != nullptr; }

  /**
   * @brief 布尔转换操作符，用于检查有效性
   *
   * 允许在条件语句中使用，例如：
   *   if (mutex_obj) { ... }
   *
   * @return 若互斥锁有效，返回 true
   */
  explicit operator bool() const noexcept { return valid(); }

private:
  osMutexId_t m_mutex_id;
};

} // namespace gdut

#endif
