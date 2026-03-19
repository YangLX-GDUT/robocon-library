# Async 事件标志模块（event.hpp）

## 原理
该模块对 CMSIS-RTOS2 的事件标志（Event Flags）功能进行 C++ 包装，提供类型安全且符合 RAII 的事件同步机制。

## 核心设计

### 事件标志概述
- 基于位掩码的事件通知机制
- 支持多个等待任务同时监听
- 可支持自动清空或手动清空
- 事件可以通过 `|`（并集）组合等待

### 类 `event_flags`
- RAII 自动管理 CMSIS-RTOS2 事件对象的生命周期
- 支持移动语义，不支持复制
- 可从已有的 CMSIS-RTOS2 事件 ID 构造（用于与 C 代码集成）
- 支持空对象构造（`empty_event_flags_t`）

## 如何使用

### 基础使用
```cpp
#include "event.hpp"

// 创建一个事件标志对象
gdut::event_flags my_event;

// 在任务中设置事件标志
my_event.set(0x01);          // 设置第 0 位
my_event.set(0x04);          // 设置第 2 位

// 等待单个事件标志
uint32_t flags = my_event.wait(0x01);  // 等待第 0 位
if (flags & 0x01) {
    // 事件发生
}

// 等待任意标志（并集）
flags = my_event.wait(0x01 | 0x02);  // 等待第 0 或第 1 位
```

### 带超时的等待
```cpp
#include <chrono>

gdut::event_flags event;

// 等待 1000ms 后超时
uint32_t flags = event.wait(0x01, std::chrono::milliseconds(1000));
if (flags == 0) {
    // 等待超时
}

// 立即检查（不等待）
flags = event.wait(0x01, std::chrono::milliseconds(0));
```

### 事件清空
```cpp
gdut::event_flags event;

event.set(0xFF);

// 手动清空特定标志位
event.clear(0x01);  // 清空第 0 位

// 清空所有标志
event.clear(0xFF);
```

### 等待方式选项
```cpp
// 使用选项控制等待行为
uint32_t flags = event.wait(0x03, std::chrono::milliseconds::max(), 
                            osFlagsWaitAll);  // 等待所有指定的位
// 或
uint32_t flags = event.wait(0x03, std::chrono::milliseconds::max(), 
                            osFlagsWaitAny);  // 等待任意指定的位
```

### 与已有 CMSIS-RTOS2 对象集成
```cpp
// 从既有的 CMSIS-RTOS2 事件 ID 构造
osEventFlagsId_t existing_event = osEventFlagsNew(nullptr);
gdut::event_flags wrapped_event(existing_event);

// 使用 C++ 包装进行操作
wrapped_event.set(0x10);
```

### 创建空对象
```cpp
// 创建一个无效的事件对象（例如用于延迟初始化）
gdut::event_flags empty(gdut::empty_event_flags);

if (!empty.valid()) {
    // 对象无效，此时调用 set/wait/clear 会返回错误或 0
    empty.set(0x01);  // 返回 osFlagsErrorParameter
}
```

## 多任务协调示例

```cpp
// 任务 1：生产者
void producer_task(void *arg) {
    auto *event = static_cast<gdut::event_flags *>(arg);
    
    while (true) {
        // 执行某些操作
        produce_data();
        
        // 信号数据已就绪
        event->set(0x01);
        
        osDelay(100);
    }
}

// 任务 2：消费者
void consumer_task(void *arg) {
    auto *event = static_cast<gdut::event_flags *>(arg);
    
    while (true) {
        // 等待数据就绪信号
        uint32_t flags = event->wait(0x01);
        
        if (flags & 0x01) {
            // 消费数据
            consume_data();
            
            // 清空标志
            event->clear(0x01);
        }
    }
}

// 创建并使用
gdut::event_flags sync_event;

osThreadNew(producer_task, &sync_event, nullptr);
osThreadNew(consumer_task, &sync_event, nullptr);
```

## 与代码规范的对应
- 自动 RAII 资源管理，避免手动析构
- 支持移动语义（move semantics）
- 蛇形命名约定
- 类型安全包装，避免直接操作 C API

## 错误处理
- `valid()` 方法检查对象是否有效
- 对无效对象的操作返回错误码或 0
- 支持 nullptr 输入，返回错误而非崩溃

## 注意事项/坑点
- 从空对象或 nullptr 构造的 `event_flags` 是无效的，所有操作都会返回错误
- `wait()` 调用会阻塞当前任务，不能在中断上下文中使用
- 超时参数为 `std::chrono::milliseconds::max()` 时表示无限等待
- 位掩码最多支持 32 位事件标志
- 移动后的源对象不再持有事件资源

相关源码：[Middlewares/GDUT_RC_Library/Async/event.hpp](../../Middlewares/GDUT_RC_Library/Async/event.hpp)
