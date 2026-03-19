# Async 线程模块（thread.hpp）

## 原理
该模块对 CMSIS-RTOS2 线程进行 C++ RAII 封装，提供类似 `std::thread` 的接口，支持自动资源管理、线程同步和优先级配置。

## 核心设计

### 模板类 `thread<StackSize, Priority>`
- `StackSize`：线程栈大小（字节），编译期指定
- `Priority`：线程优先级，默认为 `osPriorityNormal`
- RAII 自动管理线程生命周期
- 基于信号量实现 `join()` 同步
- 移动语义支持，禁止拷贝
- 内置内存池分配（CCM RAM），避免堆碎片

### 关键特性
- **类型安全**：使用 Lambda 和完美转发，避免 C 风格函数指针
- **异常安全**：资源自动清理，即使构造失败
- **可移动**：支持将线程对象传递和存储
- **join 语义**：类似 `std::thread`，可等待线程完成

## 如何使用

### 基础使用
```cpp
#include "thread.hpp"

void task_function() {
    // 任务逻辑
    for (int i = 0; i < 10; i++) {
        printf("Working: %d\n", i);
        osDelay(100);
    }
}

void create_thread() {
    // 创建线程，512 字节栈
    gdut::thread<512> worker(task_function);
    
    // 等待线程完成
    worker.join();
}
```

### 使用 Lambda
```cpp
// Lambda 无捕获
gdut::thread<512> t1([]() {
    printf("Hello from thread\n");
});

// Lambda 有捕获
int count = 10;
gdut::thread<512> t2([count]() {
    for (int i = 0; i < count; i++) {
        printf("%d ", i);
    }
});

t1.join();
t2.join();
```

### 带参数的线程函数
```cpp
void worker_with_args(int id, const char* name) {
    printf("Worker %d: %s\n", id, name);
}

void create_with_args() {
    // 传递参数给线程函数
    gdut::thread<512> worker(worker_with_args, 1, "TaskA");
    worker.join();
}
```

### 指定优先级
```cpp
// 低优先级线程（256 字节栈）
gdut::thread<256, osPriorityLow> low_priority_task([]() {
    background_work();
});

// 高优先级线程（1024 字节栈）
gdut::thread<1024, osPriorityHigh> high_priority_task([]() {
    critical_work();
});

low_priority_task.join();
high_priority_task.join();
```

### 检查线程状态
```cpp
gdut::thread<512> worker([]() {
    long_running_task();
});

if (worker.joinable()) {
    // 线程正在运行或未被 join
    printf("Thread is joinable\n");
    worker.join();
}

// join 后不再 joinable
if (!worker.joinable()) {
    printf("Thread has been joined\n");
}
```

### 提前终止线程
```cpp
gdut::thread<512> worker([]() {
    while (true) {
        do_work();
        osDelay(100);
    }
});

// 运行一段时间后强制终止
osDelay(1000);
worker.terminate();

// 注意：terminate() 不会等待线程结束，资源可能未清理
```

## 实际应用示例

### 多个工作线程
```cpp
void process_task(int task_id) {
    printf("Processing task %d\n", task_id);
    osDelay(500);
    printf("Task %d completed\n", task_id);
}

void create_workers() {
    std::vector<gdut::thread<512>> workers;
    
    // 创建 5 个工作线程
    for (int i = 0; i < 5; i++) {
        workers.emplace_back([i]() { process_task(i); });
    }
    
    // 等待所有线程完成
    for (auto &worker : workers) {
        worker.join();
    }
    
    printf("All workers completed\n");
}
```

### 传感器数据采集线程
```cpp
class sensor_reader {
public:
    sensor_reader() 
        : m_running(true),
          m_thread(512, osPriorityAboveNormal)([this]() { 
              this->read_loop(); 
          }) {}
    
    ~sensor_reader() {
        m_running = false;
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }

private:
    void read_loop() {
        while (m_running) {
            int value = read_sensor();
            process_value(value);
            osDelay(100);
        }
    }
    
    bool m_running;
    gdut::thread<512, osPriorityAboveNormal> m_thread;
};
```

### 异步任务执行器
```cpp
class async_executor {
public:
    template <typename Func>
    void execute_async(Func &&func) {
        auto worker = new gdut::thread<1024>([func = std::forward<Func>(func)]() {
            func();
        });
        
        // 分离线程（注意：需要手动管理生命周期）
        // 或者存储到容器中统一管理
        m_threads.push_back(std::unique_ptr<gdut::thread<1024>>(worker));
    }
    
    void wait_all() {
        for (auto &thread : m_threads) {
            if (thread->joinable()) {
                thread->join();
            }
        }
        m_threads.clear();
    }

private:
    std::vector<std::unique_ptr<gdut::thread<1024>>> m_threads;
};

// 使用
async_executor executor;

executor.execute_async([]() { task1(); });
executor.execute_async([]() { task2(); });
executor.execute_async([]() { task3(); });

executor.wait_all();
```

### 周期性任务
```cpp
class periodic_task {
public:
    periodic_task(uint32_t period_ms)
        : m_period(period_ms), m_running(true),
          m_thread([this]() { this->run(); }) {}
    
    ~periodic_task() {
        stop();
    }
    
    void stop() {
        m_running = false;
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }

private:
    void run() {
        while (m_running) {
            execute_task();
            osDelay(m_period);
        }
    }
    
    virtual void execute_task() = 0;
    
    uint32_t m_period;
    bool m_running;
    gdut::thread<512> m_thread;
};

class led_blinker : public periodic_task {
public:
    led_blinker() : periodic_task(500) {}

private:
    void execute_task() override {
        toggle_led();
    }
};

// 使用
led_blinker blinker;  // 自动启动
osDelay(5000);
blinker.stop();  // 停止闪烁
```

### 生产者-消费者模式
```cpp
#include <queue>

gdut::mutex g_queue_mutex;
std::queue<int> g_queue;
gdut::counting_semaphore<10> g_items(0);

void producer_func() {
    for (int i = 0; i < 20; i++) {
        {
            std::lock_guard<gdut::mutex> lock(g_queue_mutex);
            g_queue.push(i);
        }
        g_items.release();
        osDelay(50);
    }
}

void consumer_func() {
    for (int i = 0; i < 20; i++) {
        g_items.acquire();
        
        int item;
        {
            std::lock_guard<gdut::mutex> lock(g_queue_mutex);
            item = g_queue.front();
            g_queue.pop();
        }
        
        printf("Consumed: %d\n", item);
        osDelay(100);
    }
}

void run_producer_consumer() {
    gdut::thread<512> producer(producer_func);
    gdut::thread<512> consumer(consumer_func);
    
    producer.join();
    consumer.join();
}
```

## 移动语义

```cpp
gdut::thread<512> create_worker() {
    return gdut::thread<512>([]() {
        do_background_work();
    });
}

void use_moved_thread() {
    // 从函数返回（自动移动）
    gdut::thread<512> worker1 = create_worker();
    
    // 显式移动
    gdut::thread<512> worker2;
    worker2 = std::move(worker1);
    
    // worker2 现在拥有线程
    // worker1 已失效
    
    if (worker2.joinable()) {
        worker2.join();
    }
}

// 存储到容器
void store_threads() {
    std::vector<gdut::thread<512>> threads;
    
    for (int i = 0; i < 5; i++) {
        threads.emplace_back([i]() {
            printf("Thread %d\n", i);
        });
    }
    
    // 等待所有线程
    for (auto &t : threads) {
        t.join();
    }
}
```

## 优先级配置

CMSIS-RTOS2 支持的优先级（从低到高）：
```cpp
osPriorityIdle          // 空闲优先级
osPriorityLow           // 低优先级
osPriorityBelowNormal   // 低于正常
osPriorityNormal        // 正常（默认）
osPriorityAboveNormal   // 高于正常
osPriorityHigh          // 高优先级
osPriorityRealtime      // 实时优先级
```

示例：
```cpp
// 后台任务
gdut::thread<256, osPriorityLow> bg_task([]() {
    background_processing();
});

// 常规任务
gdut::thread<512, osPriorityNormal> normal_task([]() {
    normal_processing();
});

// 关键任务
gdut::thread<1024, osPriorityHigh> critical_task([]() {
    critical_processing();
});
```

## 栈大小指南

| 栈大小 | 适用场景 |
|--------|---------|
| 128-256 字节 | 极简任务，无函数调用 |
| 512 字节 | 一般任务，少量局部变量 |
| 1024 字节 | 中等任务，有函数调用和局部数组 |
| 2048+ 字节 | 复杂任务，深度递归或大量局部变量 |

**注意**：栈溢出会导致系统崩溃，建议预留余量。

## 与代码规范的对应
- RAII 自动资源管理
- 禁止拷贝，支持移动语义
- 类型安全的参数传递
- 蛇形命名约定

## 注意事项/坑点
- ⚠️ **join() 只能调用一次**：重复调用无效，`joinable()` 返回 false
- ⚠️ **terminate() 不安全**：强制终止线程，可能导致资源泄漏和未完成的操作
- ⚠️ **栈溢出风险**：栈大小必须足够，否则导致硬错误（HardFault）
- ⚠️ **Lambda 捕获生命周期**：捕获的引用必须在线程运行期间有效
- ⚠️ **分离线程管理**：没有 `detach()` 方法，线程对象析构会尝试终止线程
- ⚠️ **优先级反转**：使用互斥锁时注意优先级设置
- ⚠️ **内存池大小**：默认 4KB 内存池，多个大线程可能耗尽
- ⚠️ **移动后失效**：移动后的源对象不再拥有线程

## 与 std::thread 的对比

| 特性 | gdut::thread | std::thread |
|------|--------------|-------------|
| RAII 支持 | ✅ 是 | ✅ 是 |
| join() | ✅ 支持 | ✅ 支持 |
| detach() | ❌ 不支持 | ✅ 支持 |
| 优先级控制 | ✅ 编译期模板参数 | ❌ 无 |
| 栈大小控制 | ✅ 编译期模板参数 | ❌ 依赖系统 |
| 嵌入式友好 | ✅ 优秀 | ❌ 不适用 |

## 调试技巧

### 检测栈溢出
```cpp
// 在 FreeRTOSConfig.h 中启用
#define configCHECK_FOR_STACK_OVERFLOW 2

// 实现溢出钩子
extern "C" void vApplicationStackOverflowHook(TaskHandle_t xTask, 
                                               char *pcTaskName) {
    printf("Stack overflow in task: %s\n", pcTaskName);
    while(1);  // 停止调试
}
```

### 线程状态监控
```cpp
void monitor_threads() {
    // 使用 FreeRTOS 原生 API 获取任务信息
    TaskStatus_t *tasks = new TaskStatus_t[10];
    UBaseType_t count = uxTaskGetSystemState(tasks, 10, nullptr);
    
    for (UBaseType_t i = 0; i < count; i++) {
        printf("Task: %s, Priority: %d, Stack remaining: %u\n",
               tasks[i].pcTaskName,
               tasks[i].uxCurrentPriority,
               tasks[i].usStackHighWaterMark);
    }
    
    delete[] tasks;
}
```

相关源码：[Middlewares/GDUT_RC_Library/Async/thread.hpp](../../Middlewares/GDUT_RC_Library/Async/thread.hpp)
