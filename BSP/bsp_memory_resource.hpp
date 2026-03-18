#ifndef BSP_MEMORY_RESOURCE_HPP
#define BSP_MEMORY_RESOURCE_HPP

#include "FreeRTOS.h"
#include "bsp_mutex.hpp"
#include "tlsf.h"
#include <cmsis_os2.h>
#include <cstddef>
#include <memory_resource>
#include <mutex>

namespace gdut::pmr {

class portable_resource : public std::pmr::memory_resource {
public:
  static memory_resource *get_instance() {
    static portable_resource instance;
    return &instance;
  }

private:
  void *do_allocate(size_t bytes, size_t alignment) override {
    // FreeRTOS 保证 pvPortMalloc 返回 portBYTE_ALIGNMENT 字节对齐。
    // 若请求更强的对齐，显式返回失败，
    // 以避免返回未对齐内存导致未定义行为。
    const size_t requested_alignment =
        alignment == 0 ? alignof(max_align_t) : alignment;

    if (requested_alignment > static_cast<size_t>(portBYTE_ALIGNMENT)) {
      return nullptr;
    }
    return pvPortMalloc(bytes);
  }
  void do_deallocate(void *p, size_t bytes, size_t alignment) override {
    (void)bytes;     // 该简化实现忽略 bytes
    (void)alignment; // 该简化实现忽略 alignment
    vPortFree(p);
  }
  bool do_is_equal(const memory_resource &other) const noexcept override {
    return true; // 所有 portable_resource 实例都视为相等
  }
};

// 非线程安全的内存池资源
class unsynchronized_tlsf_resource : public std::pmr::memory_resource {
  struct alloc_node {
    struct alloc_node *next;
  };

  memory_resource *m_upstream_resource{nullptr};
  tlsf_t m_pool_memory{nullptr};
  size_t m_default_pool_block_size{0};
  alloc_node *m_free_list_head{nullptr}; // 空闲块链表头指针

public:
  static constexpr size_t default_block_size() {
    return 512; // 内存池默认块大小
  }

  unsynchronized_tlsf_resource(
      memory_resource *upstream = portable_resource::get_instance(),
      std::size_t pool_block_size = default_block_size())
      : m_upstream_resource(upstream),
        m_default_pool_block_size(pool_block_size) {
    if (upstream == nullptr) {
      m_upstream_resource = portable_resource::get_instance();
      if (m_upstream_resource == nullptr) {
        m_pool_memory = nullptr;
        return;
      }
    }
    // 分配足够空间：节点头 + TLSF 控制结构 + 内存池
    const size_t tlsf_overhead = tlsf_size();
    void *mem = m_upstream_resource->allocate(
        sizeof(alloc_node) + tlsf_overhead + m_default_pool_block_size,
        alignof(std::max_align_t));
    if (mem) {
      m_free_list_head = static_cast<alloc_node *>(mem);
      m_free_list_head->next = nullptr; // 初始化空闲链表
      // tlsf_create_with_pool 需要：tlsf_size() + pool_bytes
      m_pool_memory =
          tlsf_create_with_pool(static_cast<char *>(mem) + sizeof(alloc_node),
                                tlsf_overhead + m_default_pool_block_size);
    } else {
      m_pool_memory = nullptr;
    }
  }

  unsynchronized_tlsf_resource(const unsynchronized_tlsf_resource &) = delete;
  unsynchronized_tlsf_resource &
  operator=(const unsynchronized_tlsf_resource &) = delete;

  ~unsynchronized_tlsf_resource() override {
    if (m_pool_memory == nullptr) {
      return;
    }
    tlsf_destroy(m_pool_memory);
    const size_t tlsf_overhead = tlsf_size();
    while (m_free_list_head != nullptr) {
      alloc_node *current = m_free_list_head;
      m_free_list_head = m_free_list_head->next;
      m_upstream_resource->deallocate(current,
                                      sizeof(alloc_node) + tlsf_overhead +
                                          m_default_pool_block_size,
                                      alignof(std::max_align_t));
    }
  }

  explicit operator bool() const { return m_pool_memory != nullptr; }

private:
  void *do_allocate(size_t bytes, size_t alignment) override {
    if (m_pool_memory == nullptr) {
      return nullptr;
    }
    // 若请求过大，回退到上游资源
    if (bytes > m_default_pool_block_size) {
      return m_upstream_resource->allocate(bytes, alignment);
    }
    if (void *mem = tlsf_memalign(m_pool_memory, alignment, bytes);
        mem != nullptr) {
      return mem;
    }
    // 添加新的内存池（tlsf_add_pool 不需要 tlsf_size() 额外开销）
    void *new_mem = m_upstream_resource->allocate(sizeof(alloc_node) +
                                                      m_default_pool_block_size,
                                                  alignof(std::max_align_t));
    if (new_mem == nullptr) {
      return nullptr;
    }
    if (tlsf_add_pool(m_pool_memory,
                      static_cast<char *>(new_mem) + sizeof(alloc_node),
                      m_default_pool_block_size) == 0) {
      m_upstream_resource->deallocate(
          new_mem, sizeof(alloc_node) + m_default_pool_block_size,
          alignof(std::max_align_t));
      return nullptr;
    }
    static_cast<alloc_node *>(new_mem)->next =
        m_free_list_head; // 将新块加入空闲链表
    m_free_list_head = static_cast<alloc_node *>(new_mem);
    return tlsf_memalign(m_pool_memory, alignment, bytes);
  }
  void do_deallocate(void *p, size_t bytes, size_t alignment) override {
    // 若为超大分配，则来自上游资源
    if (bytes > m_default_pool_block_size) {
      m_upstream_resource->deallocate(p, bytes, alignment);
    } else if (m_pool_memory != nullptr) {
      tlsf_free(m_pool_memory, p);
    }
  }
  bool do_is_equal(const memory_resource &other) const noexcept override {
    return this == std::addressof(other);
  }
};

class synchronized_tlsf_resource : public std::pmr::memory_resource {
  unsynchronized_tlsf_resource m_pool;
  mutable mutex m_mutex;

public:
  synchronized_tlsf_resource(
      memory_resource *upstream = portable_resource::get_instance(),
      std::size_t pool_block_size =
          unsynchronized_tlsf_resource::default_block_size())
      : m_pool(upstream, pool_block_size) {}

  synchronized_tlsf_resource(const synchronized_tlsf_resource &) = delete;
  synchronized_tlsf_resource &
  operator=(const synchronized_tlsf_resource &) = delete;

  explicit operator bool() const {
    std::lock_guard lock(m_mutex);
    return static_cast<bool>(m_pool);
  }

private:
  void *do_allocate(size_t bytes, size_t alignment) override {
    std::lock_guard lock(m_mutex);
    return m_pool.allocate(bytes, alignment);
  }

  void do_deallocate(void *p, size_t bytes, size_t alignment) override {
    std::lock_guard lock(m_mutex);
    m_pool.deallocate(p, bytes, alignment);
  }

  bool do_is_equal(const memory_resource &other) const noexcept override {
    return this == std::addressof(other);
  }
};

struct empty_os_memory_pool_resource_t {
  explicit empty_os_memory_pool_resource_t() = default;
};
inline constexpr empty_os_memory_pool_resource_t
    empty_os_memory_pool_resource{};

class os_memory_pool_resource : public std::pmr::memory_resource {
  osMemoryPoolId_t m_pool_id{nullptr};
  size_t m_block_size{0};

public:
  os_memory_pool_resource(size_t block_count, size_t block_size)
      : m_block_size(block_size) {
    m_pool_id = osMemoryPoolNew(block_count, block_size, nullptr);
  }

  explicit os_memory_pool_resource(empty_os_memory_pool_resource_t) {}

  /**
   * @brief 从已有的 CMSIS-RTOS2 内存池构造包装器。
   *
   * 此构造函数取得 @p pool_id 的所有权：销毁或移动时会调用
   * @c osMemoryPoolDelete 传递句柄后，不要在其他地方删除或管理该内存池。
   * 允许传入 @c nullptr 得到一个不拥有资源且无效的包装器。
   */
  explicit os_memory_pool_resource(osMemoryPoolId_t pool_id)
      : m_pool_id(pool_id),
        m_block_size(pool_id != nullptr ? osMemoryPoolGetBlockSize(pool_id)
                                        : 0) {}

  os_memory_pool_resource(const os_memory_pool_resource &) = delete;
  os_memory_pool_resource &operator=(const os_memory_pool_resource &) = delete;

  os_memory_pool_resource(os_memory_pool_resource &&other) noexcept
      : m_pool_id(std::exchange(other.m_pool_id, nullptr)),
        m_block_size(std::exchange(other.m_block_size, 0)) {}

  os_memory_pool_resource &operator=(os_memory_pool_resource &&other) noexcept {
    if (this != std::addressof(other)) {
      if (m_pool_id != nullptr) {
        osMemoryPoolDelete(m_pool_id);
      }
      m_pool_id = std::exchange(other.m_pool_id, nullptr);
      m_block_size = std::exchange(other.m_block_size, 0);
    }
    return *this;
  }

  ~os_memory_pool_resource() override {
    if (m_pool_id != nullptr) {
      osMemoryPoolDelete(m_pool_id);
    }
  }

  explicit operator bool() const { return m_pool_id != nullptr; }

private:
  void *do_allocate(size_t bytes, size_t alignment) override {
    (void)alignment; // 该实现忽略 alignment
    if (m_pool_id == nullptr || bytes == 0) {
      return nullptr;
    }
    // 校验请求大小是否适配内存池块大小
    if (bytes > m_block_size) {
      return nullptr;
    }
    return osMemoryPoolAlloc(m_pool_id, osWaitForever);
  }

  void do_deallocate(void *p, size_t bytes, size_t alignment) override {
    (void)bytes;     // 该实现忽略 bytes
    (void)alignment; // 该实现忽略 alignment
    if (m_pool_id != nullptr && p != nullptr) {
      osMemoryPoolFree(m_pool_id, p);
    }
  }

  bool do_is_equal(const memory_resource &other) const noexcept override {
    return this == std::addressof(other);
  }
};

/**
 * @brief 基于 TLSF 的固定大小静态内存资源。
 *
 * 该 memory_resource 使用 TLSF（Two-Level Segregated Fit）分配器实现
 * 单一、静态分配的内存池，大小为 @p BlockSize 字节。所有分配都来自
 * 内部 @p m_block 缓冲区；内存池不可增长，不会从系统堆或 FreeRTOS
 * 申请动态内存。
 *
 * 与本文件中的其他内存资源相比，该类：
 * - 使用固定大小的静态缓冲区，而非动态获得的内存；
 * - 提供 TLSF 的 O(1) 分配/释放特性；
 * - 适合需要可预测内存占用的嵌入式场景。
 *
 * 重要限制：
 * - 任意单次分配请求大于 @p BlockSize 时会被拒绝。
 * - 可用容量小于 @p BlockSize 因为 TLSF 会在缓冲区内存储元数据。
 * - 当内部内存池耗尽或碎片过多无法满足请求时，@c do_allocate()
 *   内部返回 @c nullptr 但本项目使用 @c -fno-exceptions 构建，
 *   标准 PMR 接口 @c std::pmr::memory_resource::allocate()
 *   (以及 @c std::pmr::polymorphic_allocator ) 在看到 @c nullptr
 *   前会调用 @c std::terminate() 请将内存池耗尽视为致命且不可恢复
 *   的错误。
 */
template <std::size_t BlockSize>
class fixed_block_resource : public std::pmr::memory_resource {
  static_assert(BlockSize > 0, "Block size must be greater than zero.");

public:
  fixed_block_resource() {
    m_pool_memory = tlsf_create_with_pool(m_block, BlockSize);
  }

  fixed_block_resource(const fixed_block_resource &) = delete;
  fixed_block_resource &operator=(const fixed_block_resource &) = delete;
  fixed_block_resource(fixed_block_resource &&) = delete;
  fixed_block_resource &operator=(fixed_block_resource &&) = delete;

  ~fixed_block_resource() override {
    if (m_pool_memory != nullptr) {
      tlsf_destroy(m_pool_memory);
    }
  }

  explicit operator bool() const { return m_pool_memory != nullptr; }

private:
  void *do_allocate(size_t bytes, size_t alignment) override {
    if (m_pool_memory == nullptr || bytes == 0) {
      return nullptr;
    }
    // 校验请求大小是否适配块大小
    if (bytes > BlockSize - tlsf_size()) {
      return nullptr;
    }
    return tlsf_memalign(m_pool_memory, alignment, bytes);
  }

  void do_deallocate(void *p, size_t bytes, size_t alignment) override {
    (void)bytes;     // 该实现忽略 bytes
    (void)alignment; // 该实现忽略 alignment
    if (m_pool_memory != nullptr && p != nullptr) {
      tlsf_free(m_pool_memory, p);
    }
  }

  bool do_is_equal(const memory_resource &other) const noexcept override {
    return this == std::addressof(other);
  }

private:
  tlsf_t m_pool_memory{nullptr};
  alignas(std::max_align_t) char m_block[BlockSize]{};
};

} // namespace gdut::pmr

#endif // BSP_MEMORY_RESOURCE_HPP
