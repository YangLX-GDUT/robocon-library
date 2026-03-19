# Components 校验算法模块（verification_algorithm.hpp）

## 原理
该模块为通信数据完整性校验提供统一抽象，采用 `CRTP`（Curiously Recurring Template Pattern）实现零虚函数开销的多态。

设计目标：
- **可扩展**：新增算法时不影响调用方接口。
- **高性能**：无虚表、可内联，适合 MCU 场景。
- **统一调用**：上层只依赖 `calculate()` / `verify()`。

## 核心设计

### 1) 抽象基类 `verify_algorithm<Derived>`
对外暴露统一接口：
- `calculate(begin, end, code_loc)`
- `verify(begin, end, code_loc)`

内部通过 `static_cast<Derived*>(this)` 调用派生类实现。

### 2) 校验字段定位 `code_loc`
`code_loc` 指向“校验码存放位置的起始字节”。

对于需要“跳过校验字段再计算”的算法（例如 CRC），该指针非常关键。

### 3) 已内置算法
- `checksum_algorithm`
  - 16-bit 一补和校验
  - 对数据按 16 位累加后回卷，最后按位取反
- `crc16_algorithm`
  - 表驱动 `CRC16`
  - 初值 `0xFFFF`
  - 计算时跳过校验字段本身

## 接口说明

### `calculate(begin, end, code_loc)`
- 输入：数据区间 `[begin, end)` 与校验字段位置 `code_loc`
- 输出：`uint16_t` 校验码

### `verify(begin, end, code_loc)`
- 输入：同上
- 输出：`bool`，表示校验是否通过

## 如何使用

### 使用 CRC16 生成并验证
```cpp
#include "verification_algorithm.hpp"

std::uint8_t frame[] = {
    0xAA,       // header
    0x00, 0x07, // size
    0x10, 0x01, // code
    0x00, 0x00  // crc placeholder
};

auto crc_loc = frame + 5;

gdut::crc16_algorithm crc;
std::uint16_t value = crc.calculate(std::begin(frame), std::end(frame), crc_loc);

crc_loc[0] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
crc_loc[1] = static_cast<std::uint8_t>(value & 0xFF);

bool ok = crc.verify(std::begin(frame), std::end(frame), crc_loc);
(void)ok;
```

### 使用 checksum 算法
```cpp
gdut::checksum_algorithm cs;
std::uint16_t sum = cs.calculate(std::begin(frame), std::end(frame), crc_loc);
(void)sum;
```

## 与 `transfer_protocol` 的关系
`transfer_protocol.hpp` 中的 `data_packet<VerifyAlgorithm>` 将本模块作为策略模板使用。

例如：
- `data_packet<gdut::crc16_algorithm>`
- `data_packet<gdut::checksum_algorithm>`

这样可在不改协议层接口的情况下切换校验算法。

## 扩展自定义算法
可按以下模式扩展：
1. 继承 `verify_algorithm<your_algo>`
2. 实现 `calculate_code_impl()` 与 `verify_code_impl()`
3. 在上层协议中作为模板参数使用

```cpp
class my_algorithm : public gdut::verify_algorithm<my_algorithm> {
protected:
    friend gdut::verify_algorithm<my_algorithm>;

    template <typename It>
    std::uint16_t calculate_code_impl(It begin, It end, It code_loc) noexcept {
        (void)begin; (void)end; (void)code_loc;
        return 0;
    }

    template <typename It>
    bool verify_code_impl(It begin, It end, It code_loc) noexcept {
        return calculate_code_impl(begin, end, code_loc) == 0;
    }
};
```

## 注意事项
- 当前实现使用了 `code_loc + 1` 等操作，建议使用随机访问迭代器或指针。
- `code_loc` 指向错误会导致“可重复失败”的校验问题。
- 算法变更后需要同步升级通信双方，避免协议不兼容。

## 与代码规范对应
- 策略解耦清晰，便于测试和复用。
- 不依赖异常机制，适配裸机/RTOS。
- 接口简单，降低误用概率。

相关源码：
- [Components/verification_algorithm.hpp](../../Components/verification_algorithm.hpp)
