# Components 内存资源模块（memory_resource.hpp）

## 原理
该模块提供一组基于 C++17 `std::pmr::memory_resource` 的内存资源实现，适用于 FreeRTOS 与 CMSIS-RTOS2 环境。提供统一的 PMR 接口进行内存分配与释放，支持多种分配策略（堆分配、TLSF 内存池、RTOS 内存池、静态固定块）。

## 核心设计

### 为什么使用 PMR（Polymorphic Memory Resources）
- **统一接口**：所有内存资源都继承自 `std::pmr::memory_resource`
- **可插拔**：可以轻松切换分配策略，无需修改使用代码
- **类型安全**：配合 `std::pmr::polymorphic_allocator` 实现类型安全的内存管理
- **嵌入式友好**：避免全局 `new`/`delete`，提供确定性内存行为

### 资源类型概览

| 类型 | 特性 | 线程安全 | 适用场景 |
|------|------|---------|---------|
| `portable_resource` | FreeRTOS 堆（单例） | ✅ 是 | 动态分配、不确定大小 |
| `unsynchronized_tlsf_resource` | TLSF 内存池 | ❌ 否 | 单线程、高性能分配 |
| `synchronized_tlsf_resource` | TLSF 内存池 + 互斥锁 | ✅ 是 | 多线程、高性能分配 |
| `os_memory_pool_resource` | CMSIS-RTOS2 内存池 | ✅ 是 | 固定大小块分配 |
| `fixed_block_resource<N>` | 静态 TLSF 内存池 | ❌ 否 | 静态内存、CCM RAM |

## 资源详解

### 1. portable_resource - FreeRTOS 堆分配器

基于 FreeRTOS 的 `pvPortMalloc`/`vPortFree` 实现的单例资源。

**特性：**
- 单例模式，全局唯一实例
- 自动对齐到 `portBYTE_ALIGNMENT`（通常 8 字节）
- 超出对齐要求返回 `nullptr`

**使用示例：**
```cpp
#include "memory_resource.hpp"

void use_freertos_heap() {
    // 获取 FreeRTOS 堆资源（单例）
    std::pmr::memory_resource *heap = 
        gdut::pmr::portable_resource::get_instance();
    
    // 分配 256 字节，对齐到 8 字节
    void *buffer = heap->allocate(256, 8);
    if (buffer != nullptr) {
        // 使用内存
        std::memset(buffer, 0, 256);
        
        // 释放内存
        heap->deallocate(buffer, 256, 8);
    }
}
```

### 2. unsynchronized_tlsf_resource - 非线程安全 TLSF 内存池

基于 TLSF（Two-Level Segregated Fit）算法的高性能内存池，适合单线程或外部已保证互斥的场景。

**特性：**
- O(1) 分配和释放
- 自动扩展（通过上游资源）
- 超大分配自动回退到上游资源
- 低碎片率

**使用示例：**
```cpp
void use_tlsf_pool() {
    // 使用 FreeRTOS 堆作为上游资源，创建 2KB 内存池
    gdut::pmr::unsynchronized_tlsf_resource pool(
        gdut::pmr::portable_resource::get_instance(),
        2048
    );
    
    // 检查池是否创建成功
    if (!pool) {
        // 创建失败
        return;
    }
    
    // 分配 128 字节
    void *mem = pool.allocate(128, alignof(std::max_align_t));
    if (mem != nullptr) {
        pool.deallocate(mem, 128, alignof(std::max_align_t));
    }
    
    // 超大分配会自动回退到上游资源
    void *large = pool.allocate(4096, 8);  // 超过 2048，来自 FreeRTOS 堆
    if (large != nullptr) {
        pool.deallocate(large, 4096, 8);
    }
}
```

### 3. synchronized_tlsf_resource - 线程安全 TLSF 内存池

在 `unsynchronized_tlsf_resource` 基础上增加互斥锁保护。

**使用示例：**
```cpp
// 全局共享内存池
gdut::pmr::synchronized_tlsf_resource g_shared_pool(
    gdut::pmr::portable_resource::get_instance(),
    4096
);

// 线程 1
void thread1_func() {
    void *mem = g_shared_pool.allocate(256, 8);
    // 使用内存
    g_shared_pool.deallocate(mem, 256, 8);
}

// 线程 2
void thread2_func() {
    void *mem = g_shared_pool.allocate(512, 8);
    // 使用内存（线程安全，自动加锁）
    g_shared_pool.deallocate(mem, 512, 8);
}

gdut::thread<512> t1(thread1_func);
gdut::thread<512> t2(thread2_func);
t1.join();
t2.join();
```

### 4. os_memory_pool_resource - CMSIS-RTOS2 内存池

封装 CMSIS-RTOS2 的 `osMemoryPool`，提供固定大小块分配。

**特性：**
- 固定块大小，分配速度快
- 超出块大小的分配返回 `nullptr`
- 支持移动语义
- RAII 自动管理生命周期

**使用示例：**
```cpp
struct packet {
    uint32_t id;
    uint8_t data[64];
};

void use_os_pool() {
    // 创建 16 个块，每块大小为 sizeof(packet)
    gdut::pmr::os_memory_pool_resource pool(16, sizeof(packet));
    
    if (!pool) {
        // 创建失败
        return;
    }
    
    // 使用 PMR allocator 分配对象
    std::pmr::polymorphic_allocator<packet> alloc(&pool);
    
    packet *pkt = alloc.allocate(1);
    if (pkt != nullptr) {
        // 构造对象
        std::construct_at(pkt, packet{.id = 1});
        
        // 使用对象
        pkt->data[0] = 0xAA;
        
        // 销毁并释放
        std::destroy_at(pkt);
        alloc.deallocate(pkt, 1);
    }
}
```

### 5. fixed_block_resource<N> - 静态固定块资源

使用静态数组实现的 TLSF 内存池，适合放置在 CCM RAM 或其他特殊内存区域。

**特性：**
- 完全静态，无动态分配
- 可放置在 CCM RAM（使用 `GDUT_CCMRAM` 宏）
- 编译期确定大小
- O(1) 分配和释放

**使用示例：**
```cpp
// 创建 4KB 静态内存池（可放在 CCM RAM）
GDUT_CCMRAM gdut::pmr::fixed_block_resource<4096> static_pool;

void use_static_pool() {
    if (!static_pool) {
        // 创建失败（几乎不会发生）
        return;
    }
    
    void *mem = static_pool.allocate(256, 8);
    if (mem != nullptr) {
        // 使用内存
        static_pool.deallocate(mem, 256, 8);
    }
}

// 也可以用在线程内存池
struct thread_memory_resource {
    static constexpr size_t pool_size = 4096;
    GDUT_CCMRAM inline static gdut::pmr::fixed_block_resource<pool_size>
        pool_resource{};
};
```

## 配合 std::pmr 容器使用

### 使用 polymorphic_allocator
```cpp
#include <vector>
#include <memory_resource>

void use_pmr_containers() {
    // 创建内存池
    gdut::pmr::synchronized_tlsf_resource pool(
        gdut::pmr::portable_resource::get_instance(),
        2048
    );
    
    // 使用 PMR vector
    std::pmr::vector<int> numbers(&pool);
    
    for (int i = 0; i < 100; i++) {
        numbers.push_back(i);  // 内存来自 pool
    }
    
    // vector 销毁时自动释放内存回 pool
}
```

### 自定义分配器
```cpp
template <typename T>
using pool_allocator = std::pmr::polymorphic_allocator<T>;

struct sensor_data {
    uint32_t timestamp;
    float values[4];
};

void allocate_with_custom_pool() {
    gdut::pmr::os_memory_pool_resource pool(32, sizeof(sensor_data));
    
    pool_allocator<sensor_data> alloc(&pool);
    
    // 分配多个对象
    sensor_data *data = alloc.allocate(10);
    
    for (int i = 0; i < 10; i++) {
        std::construct_at(&data[i]);
        data[i].timestamp = i;
    }
    
    // 清理
    for (int i = 0; i < 10; i++) {
        std::destroy_at(&data[i]);
    }
    alloc.deallocate(data, 10);
}
```

## 实际应用示例

### 消息队列内存管理
```cpp
#include <queue>

// 使用自定义内存池管理消息队列
class message_queue_with_pool {
public:
    message_queue_with_pool() 
        : m_pool(gdut::pmr::portable_resource::get_instance(), 4096),
          m_queue(&m_pool) {
        if (!m_pool) {
            // 处理错误
        }
    }
    
    void push(int message) {
        m_queue.push(message);  // 内存来自 m_pool
    }
    
    int pop() {
        int msg = m_queue.front();
        m_queue.pop();
        return msg;
    }

private:
    gdut::pmr::synchronized_tlsf_resource m_pool;
    std::pmr::queue<int> m_queue;
};
```

### 对象池模式
```cpp
template <typename T, size_t MaxObjects>
class object_pool {
public:
    object_pool() : m_pool(MaxObjects, sizeof(T)) {}
    
    T* allocate() {
        if (!m_pool) return nullptr;
        
        void *mem = m_pool.allocate(sizeof(T), alignof(T));
        if (mem == nullptr) return nullptr;
        
        return std::construct_at(static_cast<T*>(mem));
    }
    
    void deallocate(T* obj) {
        if (obj == nullptr) return;
        
        std::destroy_at(obj);
        m_pool.deallocate(obj, sizeof(T), alignof(T));
    }

private:
    gdut::pmr::os_memory_pool_resource m_pool;
};

// 使用
struct task_context {
    uint32_t id;
    uint8_t priority;
    char name[16];
};

object_pool<task_context, 10> task_pool;

void create_task() {
    task_context *ctx = task_pool.allocate();
    if (ctx != nullptr) {
        ctx->id = 1;
        ctx->priority = 5;
        strcpy(ctx->name, "Worker");
        
        // 使用 context
        
        task_pool.deallocate(ctx);
    }
}
```

### 分层内存管理
```cpp
class memory_manager {
public:
    memory_manager() 
        : m_small_pool(
            gdut::pmr::portable_resource::get_instance(), 1024),
          m_large_pool(
            gdut::pmr::portable_resource::get_instance(), 4096) {}
    
    void* allocate(size_t size) {
        if (size <= 256) {
            return m_small_pool.allocate(size, alignof(std::max_align_t));
        } else {
            return m_large_pool.allocate(size, alignof(std::max_align_t));
        }
    }
    
    void deallocate(void *ptr, size_t size) {
        if (size <= 256) {
            m_small_pool.deallocate(ptr, size, alignof(std::max_align_t));
        } else {
            m_large_pool.deallocate(ptr, size, alignof(std::max_align_t));
        }
    }

private:
    gdut::pmr::synchronized_tlsf_resource m_small_pool;
    gdut::pmr::synchronized_tlsf_resource m_large_pool;
};
```

### CCM RAM 使用
```cpp
// 定义在 CCM RAM 的内存池（高速访问）
GDUT_CCMRAM gdut::pmr::fixed_block_resource<8192> ccm_pool;

void use_ccm_memory() {
    // 从 CCM RAM 分配（比普通 RAM 快）
    void *fast_mem = ccm_pool.allocate(1024, 8);
    
    if (fast_mem != nullptr) {
        // 在 CCM RAM 中进行计算
        float *data = static_cast<float*>(fast_mem);
        for (int i = 0; i < 256; i++) {
            data[i] = i * 0.5f;
        }
        
        ccm_pool.deallocate(fast_mem, 1024, 8);
    }
}
```

### 临时缓冲区分配器
```cpp
class scoped_buffer {
public:
    scoped_buffer(std::pmr::memory_resource *resource, size_t size)
        : m_resource(resource), m_size(size) {
        m_buffer = m_resource->allocate(size, alignof(std::max_align_t));
    }
    
    ~scoped_buffer() {
        if (m_buffer != nullptr) {
            m_resource->deallocate(m_buffer, m_size, 
                                   alignof(std::max_align_t));
        }
    }
    
    void* get() { return m_buffer; }
    size_t size() const { return m_size; }

private:
    std::pmr::memory_resource *m_resource;
    void *m_buffer;
    size_t m_size;
};

void process_data() {
    gdut::pmr::synchronized_tlsf_resource pool(
        gdut::pmr::portable_resource::get_instance(), 2048);
    
    {
        scoped_buffer temp(&pool, 512);
        // 使用临时缓冲区
        std::memset(temp.get(), 0, temp.size());
    }  // 自动释放
}
```

## 选择合适的内存资源

### 决策树

```
需要固定大小块? 
  ├─ 是 → os_memory_pool_resource
  └─ 否 → 需要动态大小?
      ├─ 是 → 多线程访问?
      │   ├─ 是 → synchronized_tlsf_resource
      │   └─ 否 → unsynchronized_tlsf_resource
      └─ 否 → 静态大小?
          ├─ 是 → fixed_block_resource<N>
          └─ 否 → portable_resource
```

### 性能对比

| 资源类型 | 分配速度 | 释放速度 | 碎片率 | 内存开销 |
|---------|---------|---------|-------|---------|
| portable_resource | 慢（O(n)） | 快 | 高 | 低 |
| unsynchronized_tlsf | 快（O(1)） | 快（O(1)） | 低 | 中等 |
| synchronized_tlsf | 快（O(1)+锁） | 快（O(1)+锁） | 低 | 中等 |
| os_memory_pool | 快（O(1)） | 快（O(1)） | 无 | 固定 |
| fixed_block_resource | 快（O(1)） | 快（O(1)） | 低 | 固定 |

## 与代码规范的对应
- 基于标准库 PMR 接口，类型安全
- RAII 自动资源管理
- 移动语义支持
- 蛇形命名约定
- 禁止拷贝，避免资源重复管理

## 注意事项/坑点
- ⚠️ **portable_resource 对齐限制**：仅保证 `portBYTE_ALIGNMENT` 对齐（通常 8 字节），超出对齐要求返回 `nullptr`
- ⚠️ **unsynchronized_tlsf_resource 非线程安全**：多线程使用必须外部同步或使用 `synchronized_tlsf_resource`
- ⚠️ **超大分配回退**：TLSF 资源对于超出池大小的分配会回退到上游资源
- ⚠️ **os_memory_pool_resource 固定块**：仅支持不超过 `block_size` 的分配，超出返回 `nullptr`
- ⚠️ **fixed_block_resource 静态大小**：编译期固定，运行时不可扩展
- ⚠️ **PMR 容器生命周期**：PMR 容器必须在其资源销毁前销毁
- ⚠️ **分配失败**：本项目使用 `-fno-exceptions`，分配失败会调用 `std::terminate()`
- ⚠️ **TLSF 元数据开销**：实际可用容量小于池大小（约 256 字节开销）
- ⚠️ **移动后失效**：移动后的源对象不再持有资源

## 内存池大小指南

### unsynchronized/synchronized_tlsf_resource
- **小型池**（512-1024 字节）：适合小对象频繁分配
- **中型池**（2048-4096 字节）：通用场景
- **大型池**（8192+ 字节）：大对象或容器使用

### os_memory_pool_resource
- **块数量**：根据并发需求（通常 8-32 个）
- **块大小**：对象大小 + 少量余量

### fixed_block_resource
- **CCM RAM**：受硬件限制（通常 64KB）
- **栈分配**：避免超过 1-2KB

## 调试技巧

### 检测内存泄漏
```cpp
class debug_memory_resource : public std::pmr::memory_resource {
public:
    debug_memory_resource(std::pmr::memory_resource *upstream)
        : m_upstream(upstream), m_allocated_bytes(0), m_allocation_count(0) {}
    
    size_t allocated_bytes() const { return m_allocated_bytes; }
    size_t allocation_count() const { return m_allocation_count; }

private:
    void *do_allocate(size_t bytes, size_t alignment) override {
        void *ptr = m_upstream->allocate(bytes, alignment);
        if (ptr != nullptr) {
            m_allocated_bytes += bytes;
            m_allocation_count++;
            printf("Allocate: %p, %zu bytes (total: %zu)\n", 
                   ptr, bytes, m_allocated_bytes);
        }
        return ptr;
    }
    
    void do_deallocate(void *p, size_t bytes, size_t alignment) override {
        m_upstream->deallocate(p, bytes, alignment);
        m_allocated_bytes -= bytes;
        m_allocation_count--;
        printf("Deallocate: %p, %zu bytes (total: %zu)\n", 
               p, bytes, m_allocated_bytes);
    }
    
    bool do_is_equal(const memory_resource &other) const noexcept override {
        return this == std::addressof(other);
    }
    
    std::pmr::memory_resource *m_upstream;
    size_t m_allocated_bytes;
    size_t m_allocation_count;
};
```

相关源码：[Middlewares/GDUT_RC_Library/Components/memory_resource.hpp](../../Middlewares/GDUT_RC_Library/Components/memory_resource.hpp)