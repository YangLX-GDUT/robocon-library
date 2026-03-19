# Async 信号量模块（semaphore.hpp）

## 原理
该模块对 CMSIS-RTOS2 的计数信号量进行封装，提供 C++ 风格接口，并支持 `std::chrono` 超时参数。计数信号量可用于资源池管理、生产者-消费者同步、事件通知等场景。

## 核心设计

### 模板类 `counting_semaphore<LeastMaxValue>`
- `LeastMaxValue` 指定信号量的最大计数值
- RAII 自动管理 CMSIS-RTOS2 信号量生命周期
- 支持超时等待（基于 `std::chrono`）
- 移动语义支持，禁止拷贝
- 无效检查：`valid()` 和 `operator bool()`

### 类型别名
- `binary_semaphore`：二值信号量，等价于 `counting_semaphore<1>`

### 关键 API
- `release()`：释放信号量，计数 +1（V 操作）
- `acquire(timeout)`：获取信号量，计数 -1，支持超时（P 操作）
- `try_acquire()`：非阻塞尝试获取，立即返回
- `try_acquire_for(duration)`：带超时的尝试获取

## 如何使用

### 基础使用
```cpp
#include "semaphore.hpp"

// 创建计数信号量，最大值 4，初始值 0
gdut::counting_semaphore<4> sem(0);

void producer() {
    // 生产完成，释放一个信号
    sem.release();
}

void consumer() {
    // 等待信号，无超时限制
    sem.acquire();
    // 获取成功，执行消费逻辑
    process_item();
}
```

### 带超时的获取
```cpp
#include <chrono>

gdut::counting_semaphore<1> sem(0);

void timed_wait() {
    // 等待 500ms
    sem.acquire(std::chrono::milliseconds(500));
    // 如果超时，会阻塞直到获取或超时
    
    // 或使用 try_acquire_for 获取返回值
    if (sem.try_acquire_for(std::chrono::milliseconds(100))) {
        // 成功获取信号量
        perform_work();
    } else {
        // 超时未获取到
        handle_timeout();
    }
}
```

### try_acquire 非阻塞尝试
```cpp
gdut::counting_semaphore<1> sem(0);

void non_blocking_check() {
    if (sem.try_acquire()) {
        // 成功获取，不会阻塞
        do_work();
    } else {
        // 信号量不可用，执行其他操作
        do_alternative();
    }
}
```

### 二值信号量（Binary Semaphore）
```cpp
// 二值信号量，用于简单的互斥或通知
gdut::binary_semaphore sem(0);

void signaler() {
    perform_operation();
    sem.release();  // 通知完成
}

void waiter() {
    sem.acquire();  // 等待通知
    react_to_signal();
}
```

## 实际应用示例

### 生产者-消费者模式
```cpp
#include <queue>

gdut::counting_semaphore<10> items_available(0);  // 可用项数
gdut::counting_semaphore<10> spaces_available(10); // 可用空间数
gdut::mutex queue_mutex;
std::queue<int> buffer;

// 生产者任务
void producer_task(void *arg) {
    int counter = 0;
    
    while (true) {
        int item = produce_item(counter++);
        
        // 等待空间可用
        spaces_available.acquire();
        
        {
            std::lock_guard<gdut::mutex> lock(queue_mutex);
            buffer.push(item);
        }
        
        // 通知有新项
        items_available.release();
        
        osDelay(100);
    }
}

// 消费者任务
void consumer_task(void *arg) {
    while (true) {
        // 等待项可用
        items_available.acquire();
        
        int item;
        {
            std::lock_guard<gdut::mutex> lock(queue_mutex);
            item = buffer.front();
            buffer.pop();
        }
        
        // 释放空间
        spaces_available.release();
        
        consume_item(item);
    }
}
```

### 资源池管理
```cpp
class resource_pool {
public:
    resource_pool(int count) 
        : m_available(count) {
        // 初始时有 count 个资源可用
    }
    
    bool acquire_resource(std::chrono::milliseconds timeout) {
        return m_available.try_acquire_for(timeout);
    }
    
    void release_resource() {
        m_available.release();
    }

private:
    gdut::counting_semaphore<10> m_available;
};

// 使用
resource_pool pool(5);  // 5 个资源

void worker_task(void *arg) {
    // 尝试获取资源，最多等待 1 秒
    if (pool.acquire_resource(std::chrono::seconds(1))) {
        // 使用资源
        use_resource();
        
        // 释放资源
        pool.release_resource();
    } else {
        // 超时，资源不可用
        handle_no_resource();
    }
}
```

### 事件计数器
```cpp
class event_counter {
public:
    event_counter() : m_counter(0) {}
    
    void signal() {
        m_counter.release();
    }
    
    bool wait_for_events(int count, std::chrono::milliseconds timeout) {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        
        for (int i = 0; i < count; i++) {
            auto remaining = deadline - std::chrono::steady_clock::now();
            if (remaining <= std::chrono::milliseconds(0)) {
                return false;
            }
            
            if (!m_counter.try_acquire_for(
                std::chrono::duration_cast<std::chrono::milliseconds>(remaining))) {
                return false;
            }
        }
        
        return true;
    }

private:
    gdut::counting_semaphore<100> m_counter;
};

// 使用
event_counter counter;

void event_source() {
    // 产生 3 个事件
    counter.signal();
    counter.signal();
    counter.signal();
}

void event_handler() {
    // 等待至少 3 个事件，超时 5 秒
    if (counter.wait_for_events(3, std::chrono::seconds(5))) {
        // 收集到 3 个事件
        process_batch();
    }
}
```

### 限流器（Rate Limiter）
```cpp
class rate_limiter {
public:
    rate_limiter(int max_rate) : m_tokens(max_rate), m_max(max_rate) {}
    
    bool allow_request() {
        return m_tokens.try_acquire();
    }
    
    void refill() {
        // 定期调用以补充令牌
        for (int i = 0; i < m_max; i++) {
            m_tokens.release();
        }
    }

private:
    gdut::counting_semaphore<100> m_tokens;
    int m_max;
};

// 使用
rate_limiter limiter(10);  // 每周期最多 10 个请求

void request_handler() {
    if (limiter.allow_request()) {
        // 处理请求
        handle_request();
    } else {
        // 请求被限流
        reject_request();
    }
}

void refill_task(void *arg) {
    while (true) {
        osDelay(1000);  // 每秒补充一次
        limiter.refill();
    }
}
```

### 任务完成通知
```cpp
gdut::binary_semaphore task_done(0);

void background_task(void *arg) {
    // 执行长时间运行的任务
    perform_heavy_computation();
    
    // 通知完成
    task_done.release();
}

void main_task(void *arg) {
    // 启动后台任务
    osThreadNew(background_task, nullptr, nullptr);
    
    // 等待完成，最多 10 秒
    if (task_done.try_acquire_for(std::chrono::seconds(10))) {
        printf("Task completed\n");
    } else {
        printf("Task timeout\n");
    }
}
```

## 有效性检查

```cpp
// 方法 1：使用 valid()
gdut::counting_semaphore<4> sem(2);
if (sem.valid()) {
    sem.acquire();
    // 使用信号量
} else {
    // 信号量创建失败
    handle_error();
}

// 方法 2：使用 operator bool()
if (sem) {
    sem.release();
}

// 方法 3：创建空信号量（延迟初始化）
gdut::counting_semaphore<4> empty_sem(gdut::empty_semaphore);
if (!empty_sem.valid()) {
    // 预期的无效状态
}

// 方法 4：从已有 CMSIS-RTOS2 句柄构造
osSemaphoreId_t existing = osSemaphoreNew(4, 0, nullptr);
gdut::counting_semaphore<4> wrapped(existing);
```

## 移动语义

```cpp
gdut::counting_semaphore<4> create_semaphore() {
    gdut::counting_semaphore<4> sem(2);
    return sem;  // 自动移动
}

void use_moved_semaphore() {
    gdut::counting_semaphore<4> sem1(2);
    gdut::counting_semaphore<4> sem2 = std::move(sem1);
    
    // sem2 现在拥有信号量
    // sem1 已失效
    
    if (sem2.valid()) {
        sem2.release();
    }
}
```

## 与代码规范的对应
- 明确的资源生命周期管理与 RAII 习惯
- 命名使用蛇形风格
- 支持 C++11 `std::chrono` 时间类型
- 禁止拷贝，支持移动语义

## 注意事项/坑点
- ⚠️ **创建可能失败**：信号量创建依赖 FreeRTOS 堆，资源不足时会失败。务必使用 `valid()` 检查
- ⚠️ **无效信号量会终止程序**：对无效信号量调用 `acquire()`、`release()` 或 `try_acquire()` 会触发 `std::terminate()`
- ⚠️ **计数上限**：由模板参数 `LeastMaxValue` 限定，`release()` 超出上限会导致未定义行为
- ⚠️ **超时单位**：超时参数为毫秒，子毫秒精度会被截断
- ⚠️ **acquire() 无返回值**：`acquire()` 是阻塞的，无返回值。如需检查超时，使用 `try_acquire_for()`
- ⚠️ **优先级反转**：信号量不支持优先级继承，可能导致优先级反转问题
- ⚠️ **中断上下文**：`acquire()` 会阻塞，禁止在中断中使用。可使用 `try_acquire()` 但需谨慎
- ⚠️ **移动后失效**：移动后的源对象不再持有信号量，不能再使用

## 与 mutex 的对比

| 特性 | mutex | counting_semaphore |
|------|-------|-------------------|
| 用途 | 互斥访问 | 资源计数/同步 |
| 所有权 | 有（加锁者必须解锁） | 无 |
| 递归 | 支持 | 不适用 |
| 优先级继承 | 支持 | 不支持 |
| 计数功能 | 无 | 有（0 到 LeastMaxValue） |
| 典型场景 | 保护临界区 | 生产者-消费者、资源池 |

## 常见模式

### 信号通知
```cpp
gdut::binary_semaphore ready(0);

void setup() {
    initialize();
    ready.release();  // 通知就绪
}

void worker() {
    ready.acquire();  // 等待就绪
    start_work();
}
```

### 限制并发数
```cpp
gdut::counting_semaphore<3> max_workers(3);  // 最多 3 个并发

void worker_task(void *arg) {
    max_workers.acquire();  // 获取许可
    
    perform_work();
    
    max_workers.release();  // 释放许可
}
```

### 批量通知
```cpp
gdut::counting_semaphore<10> batch_ready(0);

void prepare_batch() {
    for (int i = 0; i < 5; i++) {
        prepare_item(i);
        batch_ready.release();  // 每完成一个就通知
    }
}

void process_batch() {
    for (int i = 0; i < 5; i++) {
        batch_ready.acquire();  // 等待每一个
        process_item(i);
    }
}
```

相关源码：[Middlewares/GDUT_RC_Library/Async/semaphore.hpp](../../Middlewares/GDUT_RC_Library/Async/semaphore.hpp)
