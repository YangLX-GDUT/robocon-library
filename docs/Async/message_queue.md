# Async 消息队列模块（message_queue.hpp）

## 原理
该模块对 CMSIS-RTOS2 的消息队列（Message Queue）功能进行 C++ 模板包装，提供类型安全的任务间通讯机制。

## 核心设计

### 消息队列概述
- 模板化的类型安全队列，支持任意平凡可拷贝类型
- 自动 RAII 管理队列生命周期
- 支持带优先级的消息发送
- 支持同步发送（阻塞）和异步发送（ISR）
- 支持带超时的消息接收

### 模板类 `message_queue<Ty>`
- 模板参数 `Ty` 必须满足 `std::is_trivially_copyable_v` 要求
- 泛型消息队列，可存储任何平凡可拷贝类型
- 移动语义支持，禁止复制

## 如何使用

### 基础使用
```cpp
#include "message_queue.hpp"

// 定义消息类型
struct my_message {
    uint32_t id;
    uint8_t data[4];
};

// 创建消息队列，容量为 10 条消息
gdut::message_queue<my_message> msg_queue(10);

// 发送消息
my_message msg{.id = 1, .data{0x01, 0x02, 0x03, 0x04}};
if (msg_queue.send(msg)) {
    // 发送成功
}

// 接收消息
my_message received;
if (msg_queue.receive(received)) {
    // 接收到消息：received.id == 1
}
```

### 带超时的发送和接收
```cpp
#include <chrono>

gdut::message_queue<my_message> queue(10);

my_message msg{.id = 42};

// 发送消息，超时时间为 1000ms
if (queue.send(msg, std::chrono::milliseconds(1000))) {
    // 发送成功
}

// 接收消息，超时时间为 500ms
my_message received;
if (queue.receive(received, std::chrono::milliseconds(500))) {
    // 接收成功
}

// 立即返回（不等待）
if (queue.receive(received, std::chrono::milliseconds(0))) {
    // 队列不为空
} else {
    // 队列为空，立即返回
}
```

### 优先级发送
```cpp
// CMSIS-RTOS2 支持 0-255 的优先级，数字越小优先级越低
gdut::message_queue<my_message> queue(10);
my_message msg{.id = 100};

// 发送高优先级消息（优先级为 200）
queue.send(msg, std::chrono::milliseconds::max(), 200);

// 发送低优先级消息（优先级为 10，默认）
queue.send(msg);
```

### 在中断上下文中使用
```cpp
// ISR 中发送消息（非阻塞）
void my_isr_handler(void) {
    my_message msg{.id = 999};
    
    // 必须使用 send_from_isr，不能使用带超时的 send
    if (queue.send_from_isr(msg)) {
        // 消息已添加到队列
    }
    
    // 接收同样有 ISR 版本
    my_message received;
    if (queue.receive_from_isr(received)) {
        // 接收成功
    }
}
```

### 获取队列状态
```cpp
gdut::message_queue<my_message> queue(10);

// 检查队列是否有效
if (queue.valid()) {
    // 队列有效
}

// 获取队列中消息数
uint32_t count = queue.message_count();
if (count == 0) {
    // 队列为空
}

// 获取队列容量
uint32_t capacity = queue.message_capacity();
```

## 多任务通讯示例

### 生产者-消费者模式
```cpp
struct task_msg {
    uint32_t value;
    uint8_t type;
};

gdut::message_queue<task_msg> g_queue(20);

// 生产者任务
void producer_task() {
    int counter = 0;
    
    while (true) {
        task_msg msg{.value = counter++, .type = 0};
        
        // 发送消息，等待最多 100ms
        if (g_queue.send(msg, std::chrono::milliseconds(100))) {
            // 消息已发送
        }
        
        osDelay(50);
    }
}

// 消费者任务
void consumer_task() {
    task_msg msg;
    
    while (true) {
        // 等待消息，无超时限制
        if (g_queue.receive(msg, std::chrono::milliseconds::max())) {
            // 处理消息
            process_message(msg);
        }
    }
}

// 中断处理函数
void external_interrupt_handler(void) {
    task_msg urgent{.value = 0xFF, .type = 1};
    
    // 从 ISR 发送紧急消息，设置高优先级
    g_queue.send_from_isr(urgent, 200);
}

// 创建任务（使用 gdut::thread）
gdut::thread<512> producer(producer_task);
gdut::thread<512> consumer(consumer_task);

// 或者使用 Lambda
// gdut::thread<512> producer([]() {
//     int counter = 0;
//     while (true) {
//         task_msg msg{.value = counter++, .type = 0};
//         g_queue.send(msg, std::chrono::milliseconds(100));
//         osDelay(50);
//     }
// });
// 
// gdut::thread<512> consumer([]() {
//     task_msg msg;
//     while (true) {
//         if (g_queue.receive(msg, std::chrono::milliseconds::max())) {
//             process_message(msg);
//         }
//     }
// });
```

### 多消费者场景
```cpp
// 多个消费者等待同一个队列
gdut::message_queue<int> work_queue(50);

void worker_task() {
    int work_item;
    
    while (true) {
        // 只要有任务就处理
        if (work_queue.receive(work_item, std::chrono::milliseconds::max())) {
            do_work(work_item);
        }
    }
}

// 创建 4 个工作线程（使用 gdut::thread）
std::vector<gdut::thread<512>> workers;
for (int i = 0; i < 4; i++) {
    workers.emplace_back(worker_task);
}

// 或使用 Lambda 传递参数
// std::vector<gdut::thread<512>> workers;
// for (int i = 0; i < 4; i++) {
//     workers.emplace_back([i]() {
//         int work_item;
//         while (true) {
//             if (work_queue.receive(work_item, std::chrono::milliseconds::max())) {
//                 printf("Worker %d: processing %d\n", i, work_item);
//                 do_work(work_item);
//             }
//         }
//     });
// }
```

## 与代码规范的对应
- 模板化类型安全，避免类型错误和 void* 转换
- 自动 RAII 资源管理，不需要手动清理
- 支持移动语义（move semantics）
- 蛇形命名约定
- 编译期类型检查（`std::is_trivially_copyable_v`）

## 类型要求
消息类型必须满足以下条件：
```cpp
static_assert(std::is_trivially_copyable_v<MessageType>, 
              "Message type must be trivially copyable");
```

这意味着：
- 没有虚函数
- 没有虚基类
- 可以安全地 `memcpy`
- 所有成员都必须满足这些条件

## 注意事项/坑点
- 消息类型必须满足 `std::is_trivially_copyable` 要求，否则编译失败
- 队列容量必须 > 0，且在创建时指定（不可动态调整）
- 超时参数为 `std::chrono::milliseconds::max()` 表示无限等待
- 在 ISR 中只能使用 `send_from_isr()` 和 `receive_from_isr()`，不能使用带超时的版本
- 移动后的源对象不再持有队列资源
- 接收操作会阻塞任务，若想异步处理应结合 `receive_from_isr()`

相关源码：[Middlewares/GDUT_RC_Library/Async/message_queue.hpp](../../Middlewares/GDUT_RC_Library/Async/message_queue.hpp)
