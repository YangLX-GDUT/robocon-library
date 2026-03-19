# Async 互斥锁模块（mutex.hpp）

## 原理
该模块基于 CMSIS-RTOS2 的 `osMutex` 提供 C++ RAII 互斥锁封装，确保临界区在异常或提前返回时仍能安全释放锁。支持递归互斥锁和优先级继承。

## 核心设计

### 类 `mutex`
- RAII 自动管理 CMSIS-RTOS2 互斥锁生命周期
- 支持递归锁（同一线程可多次加锁）
- 支持优先级继承（避免优先级反转）
- 移动语义支持，禁止拷贝
- 无效检查：`valid()` 和 `operator bool()`

### 关键特性
- **自动创建失败检测**：构造时可能因系统资源不足而失败
- **安全性保证**：对无效互斥锁调用 `lock()`/`unlock()` 会触发 `std::terminate()`
- **标准接口**：符合 C++11 `BasicLockable` 要求

## 如何使用

### 基础使用
```cpp
#include "mutex.hpp"

gdut::mutex g_mtx;
int g_shared_value = 0;

void critical_section() {
    g_mtx.lock();
    g_shared_value += 1;
    g_mtx.unlock();
}
```

### 使用 RAII 自动锁管理（推荐）
```cpp
#include "mutex.hpp"
#include <mutex>

gdut::mutex g_mtx;
int g_shared_counter = 0;

void increment() {
    std::lock_guard<gdut::mutex> lock(g_mtx);
    g_shared_counter++;
    // lock 析构时自动释放互斥锁
}

void task_function() {
    for (int i = 0; i < 100; i++) {
        increment();
        osDelay(10);
    }
}
```

### try_lock 非阻塞尝试
```cpp
gdut::mutex g_mtx;

void try_acquire_resource() {
    if (g_mtx.try_lock()) {
        // 成功获取锁
        access_shared_resource();
        g_mtx.unlock();
    } else {
        // 无法获取锁，执行降级操作
        perform_alternative_action();
    }
}
```

### unique_lock 灵活锁管理
```cpp
#include <mutex>

gdut::mutex g_mtx;

void flexible_locking() {
    std::unique_lock<gdut::mutex> lock(g_mtx, std::defer_lock);
    
    // 做一些不需要锁的准备工作
    prepare_data();
    
    // 现在加锁
    lock.lock();
    
    // 临界区
    modify_shared_data();
    
    // 提前释放锁
    lock.unlock();
    
    // 做一些不需要锁的后续工作
    cleanup();
}
```

### 递归锁（同一线程多次加锁）
```cpp
gdut::mutex g_mtx;  // 默认支持递归

void inner_function() {
    std::lock_guard<gdut::mutex> lock(g_mtx);
    // 访问共享资源
}

void outer_function() {
    std::lock_guard<gdut::mutex> lock(g_mtx);  // 第一次加锁
    
    // 调用也需要锁的函数
    inner_function();  // 第二次加锁（递归锁允许）
    
    // 两个锁都会在各自作用域结束时释放
}
```

## 实际应用示例

### 多任务共享数据保护
```cpp
struct sensor_data {
    int32_t temperature;
    int32_t pressure;
    uint32_t timestamp;
};

gdut::mutex g_data_mutex;
sensor_data g_current_data;

// 传感器读取任务
void sensor_task(void *arg) {
    while (true) {
        sensor_data new_data = read_sensor();
        
        {
            std::lock_guard<gdut::mutex> lock(g_data_mutex);
            g_current_data = new_data;
        }
        
        osDelay(100);
    }
}

// 数据处理任务
void processing_task(void *arg) {
    while (true) {
        sensor_data local_copy;
        
        {
            std::lock_guard<gdut::mutex> lock(g_data_mutex);
            local_copy = g_current_data;
        }
        
        process_data(local_copy);
        osDelay(50);
    }
}
```

### 条件变量配合使用
```cpp
#include <queue>

gdut::mutex g_queue_mutex;
std::queue<int> g_task_queue;

void producer_task(void *arg) {
    for (int i = 0; i < 100; i++) {
        int task_id = generate_task();
        
        {
            std::lock_guard<gdut::mutex> lock(g_queue_mutex);
            g_task_queue.push(task_id);
        }
        
        osDelay(50);
    }
}

void consumer_task(void *arg) {
    while (true) {
        int task_id = -1;
        
        {
            std::lock_guard<gdut::mutex> lock(g_queue_mutex);
            if (!g_task_queue.empty()) {
                task_id = g_task_queue.front();
                g_task_queue.pop();
            }
        }
        
        if (task_id != -1) {
            execute_task(task_id);
        }
        
        osDelay(10);
    }
}
```

### 资源池管理
```cpp
class resource_pool {
public:
    resource_pool(int size) : m_available(size) {}
    
    bool acquire_resource() {
        std::lock_guard<gdut::mutex> lock(m_mutex);
        if (m_available > 0) {
            m_available--;
            return true;
        }
        return false;
    }
    
    void release_resource() {
        std::lock_guard<gdut::mutex> lock(m_mutex);
        m_available++;
    }
    
    int available_count() {
        std::lock_guard<gdut::mutex> lock(m_mutex);
        return m_available;
    }

private:
    gdut::mutex m_mutex;
    int m_available;
};

// 使用
resource_pool pool(5);

void worker_task(void *arg) {
    if (pool.acquire_resource()) {
        // 使用资源
        perform_work();
        
        // 释放资源
        pool.release_resource();
    }
}
```

## 有效性检查

```cpp
// 方法 1：使用 valid()
gdut::mutex mtx;
if (mtx.valid()) {
    mtx.lock();
    // 临界区
    mtx.unlock();
} else {
    // 互斥锁创建失败，处理错误
    handle_error();
}

// 方法 2：使用 operator bool()
if (mtx) {
    std::lock_guard<gdut::mutex> lock(mtx);
    // 临界区
}

// 方法 3：从已有 CMSIS-RTOS2 句柄构造
osMutexId_t existing_mutex = osMutexNew(nullptr);
gdut::mutex wrapped_mtx(existing_mutex);

// 方法 4：创建空互斥锁（稍后初始化）
gdut::mutex empty_mtx(gdut::empty_mutex);
if (!empty_mtx.valid()) {
    // 预期的无效状态
}
```

## 移动语义

```cpp
gdut::mutex create_mutex() {
    gdut::mutex mtx;
    return mtx;  // 返回时自动移动
}

void use_moved_mutex() {
    gdut::mutex mtx1;
    gdut::mutex mtx2 = std::move(mtx1);
    
    // mtx2 现在拥有互斥锁
    // mtx1 已失效
    
    if (mtx2.valid()) {
        mtx2.lock();
        // ...
        mtx2.unlock();
    }
}
```

## 与代码规范的对应
- 类与成员命名采用蛇形风格，私有成员使用 `m_` 前缀
- 通过 RAII 管理系统资源，避免显式资源管理
- 禁止拷贝，支持移动语义
- 强类型安全，编译期检查

## 注意事项/坑点
- ⚠️ **创建可能失败**：互斥锁创建依赖 FreeRTOS 堆，资源不足时会失败。务必使用 `valid()` 检查
- ⚠️ **无效锁会终止程序**：对无效互斥锁调用 `lock()`、`try_lock()` 或 `unlock()` 会触发 `std::terminate()`
- ⚠️ **禁止在 ISR 中使用**：互斥锁会阻塞线程，中断上下文中禁止使用
- ⚠️ **避免死锁**：
  - 总是按相同顺序获取多个锁
  - 使用 `std::lock()` 同时获取多个锁
  - 避免嵌套锁（除非是递归锁）
- ⚠️ **性能考虑**：长时间持锁会影响实时性，尽量缩短临界区
- ⚠️ **优先级反转**：虽然支持优先级继承，但仍需谨慎设计任务优先级
- ⚠️ **移动后失效**：移动后的源对象不再持有互斥锁，不能再使用

## 常见模式

### RAII 模式（推荐）
```cpp
void safe_function() {
    std::lock_guard<gdut::mutex> lock(g_mtx);
    // 临界区
    // 异常或提前返回都会自动释放锁
}
```

### 双重检查锁定
```cpp
bool initialized = false;
gdut::mutex init_mutex;

void initialize_once() {
    if (!initialized) {  // 第一次检查（无锁）
        std::lock_guard<gdut::mutex> lock(init_mutex);
        if (!initialized) {  // 第二次检查（已加锁）
            perform_initialization();
            initialized = true;
        }
    }
}
```

### 超时等待（配合 try_lock）
```cpp
bool acquire_with_timeout(uint32_t timeout_ms) {
    uint32_t start = osKernelGetTickCount();
    
    while ((osKernelGetTickCount() - start) < timeout_ms) {
        if (g_mtx.try_lock()) {
            return true;
        }
        osDelay(1);
    }
    
    return false;
}
```

相关源码：[Middlewares/GDUT_RC_Library/Async/mutex.hpp](../../Middlewares/GDUT_RC_Library/Async/mutex.hpp)
