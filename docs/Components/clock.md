# Components 时钟模块（clock.hpp）

## 原理
该模块将 CMSIS-RTOS2 的系统节拍与系统计时器封装为 `std::chrono` 风格的时钟接口，便于在应用层使用统一的时间类型。

## 实现思想
- `basic_kernel_clock` 负责原始 tick 与 timer 的读取。
- `system_clock` 使用 tick 计算毫秒时间戳，适合普通时间点。
- `steady_clock` 使用系统定时器计数计算微秒时间戳，保证单调性。

## 如何使用

```cpp
#include "clock.hpp"

void timing_demo() {
	auto t0 = gdut::steady_clock::now();

	// ... 执行任务

	auto t1 = gdut::steady_clock::now();
	auto dt = t1 - t0; // std::chrono::duration

	// 转为微秒
	auto us = std::chrono::duration_cast<std::chrono::microseconds>(dt).count();
	(void)us;
}
```

## 与代码规范的对应
- 使用标准时间类型与强类型，减少单位混用。
- 无裸指针与 C 风格强转。

## 注意事项/坑点
- `system_clock` 可能被系统调整，不保证单调；测时请用 `steady_clock`。
- tick 频率变化会影响时间换算，确保系统配置一致。

相关源码：[Middlewares/GDUT_RC_Library/Components/clock.hpp](../../Middlewares/GDUT_RC_Library/Components/clock.hpp)
