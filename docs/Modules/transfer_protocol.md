# Modules 数据传输协议模块（transfer_protocol.hpp）

## 原理
该模块实现了一个轻量、可校验、可流式解析的二进制协议栈，主要由两部分组成：

- `data_packet<VerifyAlgorithm>`：负责**单个包**的构建与解析。
- `packet_manager<VerifyAlgorithm>`：负责**字节流**中的粘包/半包处理与回调分发。

适用于串口、CAN 转发、无线链路等字节流通道。

## 协议格式

固定头结构：

$$
	ext{header}(1B) + \text{size}(2B) + \text{code}(2B) + \text{verify}(2B) + \text{payload}(N)
$$

说明：
- `header` 固定为 `0xAA`
- `size` 为整包长度（含头部和 payload）
- `code` 为业务功能码
- `verify` 为校验字段（由 `VerifyAlgorithm` 决定）
- `payload` 为业务数据

## 核心设计

### 1) 校验算法策略化
通过模板参数 `VerifyAlgorithm` 解耦校验实现：
- `crc16_algorithm`
- `checksum_algorithm`
- 或自定义算法（需继承 `verify_algorithm<Derived>`）

### 2) 标签构造防误用
使用标签类型区分构造意图：
- `build_packet`：从 `code + payload` 生成新包
- `from_whole_packet`：从完整字节区间解析包

### 3) PMR 内存资源支持
内部容器使用 `std::pmr::vector`，默认走 `portable_resource`，方便在 RTOS 下统一内存策略。

### 4) 流式解析器
`packet_manager::receive(begin, end)` 支持分段输入：
- 自动查找包头
- 自动等待半包补齐
- 完整包校验通过后回调
- 消费已处理字节，保留残留数据

## 接口总览

### `data_packet<VerifyAlgorithm>`
- 构包：`data_packet(code, begin, end, build_packet, mr)`
- 解包：`data_packet(begin, end, from_whole_packet, mr)`
- 访问：`size()` / `code()` / `crc()` / `data()`
- 包体：`body_size()` / `body_begin()` / `body_end()`
- 校验：`calculate_verification()` / `verify_verification()`
- 有效性：`operator bool()`

### `packet_manager<VerifyAlgorithm>`
- `set_send_function(...)`
- `set_receive_function(...)`
- `send(packet)`
- `receive(begin, end)`

## 如何使用

### 1) 基础收发
```cpp
#include "transfer_protocol.hpp"

using verify_t = gdut::crc16_algorithm;
using packet_t = gdut::data_packet<verify_t>;
using manager_t = gdut::packet_manager<verify_t>;

manager_t mgr;

mgr.set_send_function([](const std::uint8_t *b, const std::uint8_t *e) {
    uart_write_bytes(b, static_cast<std::size_t>(e - b));
});

mgr.set_receive_function([](packet_t packet) {
    switch (packet.code()) {
    case 0x1001:
        handle_telemetry(packet.body_begin(), packet.body_end());
        break;
    case 0x2001:
        handle_control(packet.body_begin(), packet.body_end());
        break;
    default:
        break;
    }
});

std::uint8_t payload[] = {0x01, 0x02, 0x03};
packet_t tx{0x1001, std::begin(payload), std::end(payload), gdut::build_packet};
if (tx) {
    mgr.send(tx);
}
```

### 2) 分段输入（半包）
```cpp
// 假设 rx_data1 / rx_data2 是两次中断收到的数据
mgr.receive(rx_data1.begin(), rx_data1.end());
mgr.receive(rx_data2.begin(), rx_data2.end());
// 第二次补齐后才会触发 receive_callback
```

### 3) 粘包输入
```cpp
// 一次输入包含多个包，内部 while 循环会逐个解析并回调
mgr.receive(stream.begin(), stream.end());
```

## 实际应用建议
- **中断上下文只喂数据**：在中断中调用 `receive()`，回调里尽量快出，复杂逻辑交给任务线程。
- **按 `code` 分发业务**：将协议层与业务层解耦。
- **统一大小端约定**：当前头部字段按高字节在前写入。

## 常见问题与排查
- **包构建后 `if(packet)` 为假**
  - 可能是负载过大超过上限导致构包失败。
- **收包无回调**
  - 检查头字节是否为 `0xAA`；检查输入是否到达完整包长度。
- **频繁校验失败**
  - 检查发送/接收双方校验算法、字段位置、大小端是否一致。
- **长时间运行内存增长**
  - 检查外部喂入是否含大量无效头，必要时加入输入过滤。

## 注意事项
- `VerifyAlgorithm` 必须满足 CRTP 约束。
- `receive()` 的迭代器应可稳定遍历输入区间。
- 回调中避免阻塞，防止拖慢后续解析。

## 与代码规范对应
- 协议结构清晰、接口职责单一。
- 通过模板策略降低耦合度。
- 使用 PMR 与现代容器，减少裸内存操作风险。

相关源码：
- [Modules/transfer_protocol.hpp](../../Modules/transfer_protocol.hpp)
