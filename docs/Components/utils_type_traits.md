# Components 类型萃取工具模块（utils_type_traits.hpp）

## 原理
该模块提供少量高频使用的模板元编程工具，目标是：
- 简化编译期条件判断
- 提升模板错误信息可读性
- 减少重复手写 trait 代码

模块中的工具均为 `constexpr` / 类型萃取风格，零运行时开销。

## 核心设计

### 1) `is_power_of_two<Value>`
用于判断编译期常量 `Value` 是否为 2 的幂：

$$
Value > 0 \land (Value \& (Value - 1)) = 0
$$

提供两种形式：
- 类型形式：`is_power_of_two<Value>::value`
- 变量模板：`is_power_of_two_v<Value>`

### 2) `always_false<T>`
提供一个“依赖模板参数”的恒 `false`，常用于 `if constexpr` 的 `else` 分支触发编译期错误。

提供两种形式：
- 类型形式：`always_false<T>::value`
- 变量模板：`always_false_v<T>`

## 如何使用

### 编译期配置检查
```cpp
#include "utils_type_traits.hpp"

template <std::size_t StackSize>
struct worker_config {
  static_assert(gdut::is_power_of_two_v<StackSize>,
          "StackSize must be power of two");
};
```

### 模板分派中的兜底报错
```cpp
template <typename T>
void serialize(const T &obj) {
  if constexpr (std::is_integral_v<T>) {
    // integral serialize
  } else if constexpr (std::is_floating_point_v<T>) {
    // floating serialize
  } else {
    static_assert(gdut::always_false_v<T>,
            "serialize(): unsupported type");
  }
  (void)obj;
}
```

### 与标准库 trait 组合
```cpp
template <typename T, std::size_t N>
struct dma_buffer {
  static_assert(std::is_trivially_copyable_v<T>,
          "DMA buffer element must be trivially copyable");
  static_assert(gdut::is_power_of_two_v<N>,
          "DMA buffer size must be power of two");
};
```

## 常见场景
- **线程栈大小检查**：保证内存对齐和配置规范。
- **环形缓冲区容量检查**：很多位运算优化依赖 2 的幂。
- **模板接口约束**：为不支持类型提供明确报错。

## 注意事项
- `is_power_of_two_v<0>` 为 `false`，这是有意设计。
- `always_false_v<T>` 应主要用于模板上下文；在普通代码中直接使用会恒触发错误。
- 该模块不替代 `<type_traits>`，而是作为工程内常用补充。

## 与代码规范对应
- 编译期失败优先于运行时失败。
- 错误信息集中在模板定义处，便于定位。
- 无额外运行时成本，适合实时系统。

相关源码：
- [Components/utils_type_traits.hpp](../../Components/utils_type_traits.hpp)
