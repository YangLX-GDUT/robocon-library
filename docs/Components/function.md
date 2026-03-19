# Components 函数对象模块（function.hpp）

## 原理
该模块提供一个类型擦除的函数对象容器，类似标准库的 `std::function`，但支持自定义的存储大小和对齐要求，适合嵌入式系统的内存受限环境。避免堆分配，提供确定的内存占用，是实时系统和嵌入式开发的理想选择。

## 核心概念

### 什么是 `gdut::function`
`gdut::function` 是一个函数对象容器，可以存储任意**可调用对象**（Callable Object）：
- 函数指针
- Lambda 表达式
- 仿函数（带 `operator()` 的类）
- 成员函数指针

### 为什么使用 `gdut::function`
- **类型安全**：避免 C 风格的 `void*` 函数指针和 `void*` 参数
- **内存安全**：固定内联存储，无堆分配，内存占用可预测
- **灵活性**：一个变量可以存储不同实现的可调用对象
- **易用性**：支持 Lambda 捕获，更现代的 C++ 编程风格

## 快速开始

### 最简单的使用
```cpp
#include "function.hpp"

// 定义一个回调类型：无参数、返回 void
using callback_t = gdut::function<void()>;

// 存储函数指针
void on_button_pressed() {
    printf("Button pressed!\n");
}
callback_t btn_callback = on_button_pressed;
btn_callback();  // 输出：Button pressed!

// 存储 Lambda
callback_t btn_callback2 = []() {
    printf("Lambda called!\n");
};
btn_callback2();  // 输出：Lambda called!
```

### 带参数和返回值
```cpp
// 接受参数、返回值
using handler_t = gdut::function<int(int, int)>;

handler_t add = [](int a, int b) { return a + b; };
int result = add(5, 3);  // 8

handler_t multiply = [](int a, int b) { return a * b; };
result = multiply(5, 3);  // 15
```

### 与 Lambda 捕获
```cpp
int threshold = 100;
int max_value = 1000;

using validator_t = gdut::function<bool(int)>;

// Lambda 捕获多个变量
validator_t check = [threshold, max_value](int value) {
    return value > threshold && value < max_value;
};

if (check(500)) {
    printf("Value is valid\n");  // 会输出
}
```

## 常见使用场景

### 事件回调系统
```cpp
class button_manager {
public:
    using click_callback_t = gdut::function<void()>;
    using long_press_callback_t = gdut::function<void(int duration_ms)>;
    
    void set_on_click(click_callback_t cb) {
        m_on_click = cb;
    }
    
    void set_on_long_press(long_press_callback_t cb) {
        m_on_long_press = cb;
    }
    
    void simulate_click() {
        if (m_on_click) {
            m_on_click();
        }
    }
    
    void simulate_long_press(int duration) {
        if (m_on_long_press) {
            m_on_long_press(duration);
        }
    }

private:
    click_callback_t m_on_click;
    long_press_callback_t m_on_long_press;
};

// 使用
button_manager btn;

// 注册点击回调
btn.set_on_click([]() {
    printf("Button clicked\n");
});

// 注册长按回调
int press_count = 0;
btn.set_on_long_press([&press_count](int duration) {
    press_count++;
    printf("Long press %d times, duration: %dms\n", press_count, duration);
});

btn.simulate_click();
btn.simulate_long_press(1500);
```

### 中断处理注册
```cpp
class interrupt_handler {
public:
    using isr_t = gdut::function<void()>;
    
    void register_isr(uint32_t irq_num, isr_t handler) {
        if (irq_num < m_handlers.size()) {
            m_handlers[irq_num] = handler;
        }
    }
    
    void dispatch(uint32_t irq_num) {
        if (irq_num < m_handlers.size() && m_handlers[irq_num]) {
            m_handlers[irq_num]();
        }
    }

private:
    std::array<isr_t, 16> m_handlers;
};

// 使用
interrupt_handler irq_mgr;

int tick = 0;

// 注册 Timer 中断
irq_mgr.register_isr(28, [&tick]() {
    tick++;
    led_toggle();
});

// 模拟中断触发
irq_mgr.dispatch(28);
```

### 任务调度
```cpp
struct scheduled_task {
    gdut::function<void()> action;
    uint32_t interval_ms;
    uint32_t last_run_ms = 0;
};

class task_scheduler {
public:
    void add_task(const scheduled_task &task) {
        m_tasks.push_back(task);
    }
    
    void update(uint32_t current_time_ms) {
        for (auto &task : m_tasks) {
            if (current_time_ms - task.last_run_ms >= task.interval_ms) {
                if (task.action) {
                    task.action();
                }
                task.last_run_ms = current_time_ms;
            }
        }
    }

private:
    std::vector<scheduled_task> m_tasks;
};

// 使用
task_scheduler scheduler;

// 每 100ms 读取一次传感器
scheduler.add_task({
    .action = []() { read_sensor_data(); },
    .interval_ms = 100
});

// 每 500ms 发送一次数据
scheduler.add_task({
    .action = []() { send_telemetry(); },
    .interval_ms = 500
});

// 在主循环中
while (true) {
    scheduler.update(get_current_time_ms());
    osDelay(10);
}
```

### 状态机
```cpp
enum class state { idle, running, error };

class simple_state_machine {
public:
    using state_handler_t = gdut::function<void()>;
    
    void set_state_handler(state s, state_handler_t handler) {
        m_handlers[static_cast<int>(s)] = handler;
    }
    
    void transition(state new_state) {
        auto handler = m_handlers[static_cast<int>(new_state)];
        if (handler) {
            handler();
        }
    }

private:
    std::array<state_handler_t, 3> m_handlers;
};

// 使用
simple_state_machine fsm;

fsm.set_state_handler(state::idle, []() {
    printf("Entering IDLE state\n");
    power_down();
});

fsm.set_state_handler(state::running, []() {
    printf("Entering RUNNING state\n");
    enable_motor();
});

fsm.set_state_handler(state::error, []() {
    printf("Entering ERROR state\n");
    stop_all();
});

fsm.transition(state::running);
```

### 传感器数据处理
```cpp
class sensor_reader {
public:
    using data_handler_t = gdut::function<void(int32_t value)>;
    
    void set_on_data_ready(data_handler_t handler) {
        m_on_data_ready = handler;
    }
    
    void read_and_process() {
        int32_t raw_value = adc_read();
        
        if (m_on_data_ready) {
            m_on_data_ready(raw_value);
        }
    }

private:
    data_handler_t m_on_data_ready;
};

// 使用：实时计算移动平均
sensor_reader sensor;

const int window_size = 10;
std::array<int32_t, window_size> samples;
int idx = 0;

sensor.set_on_data_ready([&](int32_t value) {
    samples[idx] = value;
    idx = (idx + 1) % window_size;
    
    int32_t sum = 0;
    for (int s : samples) {
        sum += s;
    }
    int32_t average = sum / window_size;
    
    printf("Moving average: %d\n", average);
});

sensor.read_and_process();
```

## 有效性检查
```cpp
using callback_t = gdut::function<void()>;

callback_t cb;  // 默认无效

// 方法 1：valid() 检查
if (cb.valid()) {
    cb();
}

// 方法 2：operator bool
if (cb) {
    cb();
}

// 赋值后变为有效
cb = []() { printf("Now valid\n"); };
if (cb) {
    cb();  // 输出：Now valid
}

// 清空为无效
cb = nullptr;
if (!cb) {
    printf("Now invalid\n");  // 输出
}
```

## 移动语义
```cpp
using my_callback = gdut::function<void()>;

my_callback cb1 = []() { printf("Original\n"); };
my_callback cb2;

// 移动（cb1 失效，cb2 获得所有权）
cb2 = std::move(cb1);

cb2();  // OK，输出：Original
// cb1();  // 错误：cb1 已失效
```

## 高级用法

### 动态切换回调
```cpp
// 根据运行时条件切换回调实现

using processor_t = gdut::function<void(uint8_t *data, int len)>;

processor_t processor;

void set_processing_mode(int mode) {
    if (mode == 0) {
        // 模式 0：直接转发
        processor = [](uint8_t *data, int len) {
            send_data(data, len);
        };
    } else if (mode == 1) {
        // 模式 1：加密后转发
        processor = [](uint8_t *data, int len) {
            encrypt_data(data, len);
            send_data(data, len);
        };
    } else {
        // 模式 2：压缩后转发
        processor = [](uint8_t *data, int len) {
            int compressed_len = compress_data(data, len);
            send_data(data, compressed_len);
        };
    }
}

void process_incoming_data(uint8_t *data, int len) {
    if (processor) {
        processor(data, len);
    }
}
```

### 条件执行
```cpp
// 有条件地执行回调

using guard_callback_t = gdut::function<void()>;

class conditional_executor {
public:
    void set_callback(guard_callback_t cb) {
        m_callback = cb;
    }
    
    void execute_if(bool condition) {
        if (condition && m_callback) {
            m_callback();
        }
    }

private:
    guard_callback_t m_callback;
};

// 使用
conditional_executor executor;

int fault_count = 0;
executor.set_callback([&fault_count]() {
    fault_count++;
    if (fault_count > 3) {
        trigger_shutdown();
    }
});

// 只有当条件满足时才执行
executor.execute_if(system_error_detected);
```

### 链式调用
```cpp
// 顺序执行多个回调

class callback_chain {
public:
    using callback_t = gdut::function<void()>;
    
    void add(callback_t cb) {
        m_callbacks.push_back(cb);
    }
    
    void execute_all() {
        for (auto &cb : m_callbacks) {
            if (cb) {
                cb();
            }
        }
    }

private:
    std::vector<callback_t> m_callbacks;
};

// 使用
callback_chain init_chain;

init_chain.add([]() { init_uart(); });
init_chain.add([]() { init_spi(); });
init_chain.add([]() { init_can(); });
init_chain.add([]() { init_display(); });

init_chain.execute_all();  // 按顺序初始化所有外设
```

## 与代码规范的对应
- 类型安全，避免 `void*` 函数指针
- 支持现代 C++ 特性（Lambda、移动语义）
- 内存安全，无堆分配
- 蛇形命名约定
- RAII 风格自动管理资源

## 使用建议

### 何时使用 `gdut::function`
✅ 事件驱动系统中的回调  
✅ 中断处理函数注册  
✅ 任务调度系统  
✅ 状态机实现  
✅ 观察者模式  
✅ 异步操作的完成回调

### 何时不需要使用
❌ 简单的一次性函数调用  
❌ 性能关键路径且无法内联的情况  
❌ 不需要多态行为的地方  

## 内存占用

默认 `gdut::function<Func>` 使用固定 16 字节存储，足以容纳：
- 函数指针
- 简单 Lambda（无或少量捕获）
- 小型仿函数

对于复杂的 Lambda（多个捕获变量），可能需要自定义 `basic_function`：

```cpp
// 如果默认 16 字节不够，自定义更大的存储
using large_callback = gdut::basic_function<void(int), 256, 8>;

// 捕获多个变量的 Lambda
int a, b, c, d;
large_callback cb = [a, b, c, d](int x) {
    process(a + b + c + d + x);
};
```

## 常见问题

### Q：编译错误 "too large and exceeds the storage space"
A：Lambda 捕获变量太多，超过了默认 16 字节。有两种解决方案：
1. 减少捕获变量数量或使用指针
2. 使用自定义 `basic_function<Sig, LargerSize, Alignment>`

### Q：对无效的 `function` 调用会怎样
A：会导致程序终止（`std::terminate`）。使用前总是检查有效性。

### Q：性能如何
A：虚函数调用有轻微开销，通常被 CPU 分支预测优化。对于中断驱动的系统已足够快。

### Q：支持成员函数吗
A：支持，使用 `std::bind` 或 Lambda 包装：
```cpp
class MyClass {
    void my_method() { /* ... */ }
};

MyClass obj;
gdut::function<void()> cb = [&obj]() { obj.my_method(); };
```

## 注意事项/坑点
- 对无效的 `function` 调用会导致运行时终止，务必检查有效性
- Lambda 捕获变量超过 16 字节会导致编译错误
- 捕获的引用必须保证生命周期长于 `function` 对象
- 移动后的源对象失效，不能再使用
- 不能直接存储成员函数指针，需要用 Lambda 或 `std::bind`
- 在中断上下文中创建 `function` 需要谨慎（涉及类型初始化）

---

## 高级主题：自定义 `basic_function`

### 什么是 `basic_function`
`gdut::function` 实际上是 `basic_function` 的类型别名，预设了合理的默认参数。当需要更大的存储空间或特殊对齐要求时，可以直接使用 `basic_function`。

```cpp
// basic_function 的完整签名
template <typename R, typename... Args, std::size_t StorageSize, 
          std::size_t Alignment>
class basic_function<R(Args...), StorageSize, Alignment> { /* ... */ };

// gdut::function 是这样定义的别名
template <typename Func, std::size_t StorageSize = 16,
          std::size_t Alignment = alignof(std::max_align_t)>
using function = basic_function<Func, StorageSize, Alignment>;
```

### 自定义存储大小和对齐

当默认的 16 字节不足时：

```cpp
// 自定义：256 字节存储，8 字节对齐
using large_callback = gdut::basic_function<void(), 256, 8>;

// 复杂 Lambda 可以存储
std::string msg = "Complex data";
std::vector<int> numbers = {1, 2, 3, 4, 5};

large_callback cb = [msg, numbers]() {
    printf("%s: ", msg.c_str());
    for (int n : numbers) {
        printf("%d ", n);
    }
    printf("\n");
};

cb();  // 输出：Complex data: 1 2 3 4 5
```

### 存储大小选择指南

| StorageSize | 典型使用场景 |
|-------------|-----------|
| 16 字节    | 函数指针、简单 Lambda（无或 1-2 个捕获） |
| 32 字节    | Lambda 捕获 2-3 个简单类型 |
| 64 字节    | Lambda 捕获 4-8 个简单类型或 1 个对象 |
| 256 字节   | 复杂对象、多个对象、状态机 |
| 512+ 字节  | 包含大对象或容器的情况 |

### 对齐要求

对齐参数必须是 2 的幂。常见值：
- `8` - 大多数情况下足够
- `16` - SSE/AVX 向量运算
- `32` - 高级 SIMD 操作

```cpp
// 标准对齐
using std_callback = gdut::function<void()>;  // 默认对齐

// 宽松对齐
using aligned_callback = gdut::basic_function<void(), 256, 32>;
```

## 与 `std::function` 对比

| 特性 | gdut::function | std::function |
|------|---|---|
| 堆分配 | ❌ 否 | ⚠️ 可能有 |
| 内存占用 | ✅ 固定 16 字节 | ❌ 不确定 |
| 内存安全 | ✅ 栈分配 | ⚠️ 可能堆分配 |
| 嵌入式友好 | ✅ 优秀 | ❌ 不推荐 |
| 标准库兼容 | ❌ 否 | ✅ 是 |
| 实时性保证 | ✅ 有 | ❌ 无 |

相关源码：[Middlewares/GDUT_RC_Library/Components/function.hpp](../../Middlewares/GDUT_RC_Library/Components/function.hpp)
