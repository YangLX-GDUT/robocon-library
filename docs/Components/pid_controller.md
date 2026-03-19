# Components PID 控制器模块（pid_controller.hpp）

## 原理
该模块提供了一个面向嵌入式控制场景的 `PID` 控制器实现：`gdut::pid_controller<T>`。

核心目标：
- **类型安全**：模板参数 `T` 限制为浮点类型。
- **工程可用**：提供死区、输出限幅、积分限幅、微分滤波等常用能力。
- **低侵入**：接口直接使用误差 `error` 与采样周期 `dt`，方便接入现有控制环。

控制输出可表示为：

$$
u = \mathrm{clamp}\left(K_p e + K_i \int e\,dt + K_d \dot e_f,\ u_{min},\ u_{max}\right)
$$

其中 $\dot e_f$ 为滤波后的微分项。

## 核心设计

### 1) 参数合法性检查
所有 `set_*` 接口会做范围检查，非法参数返回 `false`：
- `Kp/Ki/Kd`：必须非负
- `DeadZone`：必须非负
- `IntegralWindupLimit`：必须非负
- `MinOutput < MaxOutput`
- `Alpha ∈ [0, 1]`

### 2) 积分抗饱和
当设置了 `IntegralWindupLimit` 后，积分项会被限制在：

$$
I \in [-I_{max}, I_{max}]
$$

用于抑制执行器饱和时的积分累积。

### 3) 微分滤波
导数使用指数移动平均（EMA）滤波：

$$
\dot e_f(k) = \alpha \cdot \dot e(k) + (1-\alpha)\cdot \dot e_f(k-1)
$$

减少高频噪声放大，增强输出稳定性。

### 4) 死区
当 `|error| < DeadZone` 时，控制器维持当前输出并更新前次误差，降低小误差抖动。

## 接口总览

### 构造与配置
- `pid_controller()`：默认构造。
- `pid_controller(Kp, Ki, Kd, DeadZone, IntegralWindupLimit, MinOutput, MaxOutput, Alpha)`：一次性初始化。
- `set_parameters(...)`：批量更新参数。

### 单项参数设置
- `set_Kp()` / `set_Ki()` / `set_Kd()`
- `set_dead_zone()`
- `set_integral_windup_limit()`
- `set_output_limits()`
- `set_alpha()`

### 控制更新
- `update(error, dt)`：输入误差与采样周期，返回控制输出。

### 编译期工厂
- `make_pid_controller<T, ...>()`
  - 参数在编译期静态检查，适合固定控制参数的场景。

## 如何使用

### 基础闭环示例
```cpp
#include "pid_controller.hpp"

gdut::pid_controller<float> speed_pid(
    1.2f, 0.15f, 0.03f,
    0.0f,     // DeadZone
    200.0f,   // IntegralWindupLimit
    -1000.0f, // MinOutput
    1000.0f,  // MaxOutput
    0.1f      // Alpha
);

float target_rpm = 3000.0f;
float current_rpm = 2800.0f;
float dt = 0.001f; // 1kHz

float error = target_rpm - current_rpm;
float duty = speed_pid.update(error, dt);
set_motor_pwm(duty);
```

### 运行中调参示例
```cpp
void tune_pid(gdut::pid_controller<float> &pid) {
    (void)pid.set_Kp(1.0f);
    (void)pid.set_Ki(0.2f);
    (void)pid.set_Kd(0.05f);
    (void)pid.set_dead_zone(0.5f);
    (void)pid.set_integral_windup_limit(100.0f);
    (void)pid.set_output_limits(-500.0f, 500.0f);
    (void)pid.set_alpha(0.15f);
}
```

### 编译期固定参数示例
```cpp
auto angle_pid = gdut::make_pid_controller<float,
    8.0f,   // Kp
    0.0f,   // Ki
    0.2f,   // Kd
    0.1f,   // DeadZone
    30.0f,  // IntegralWindupLimit
    -300.0f,
    300.0f,
    0.2f>();
```

## 实际应用建议
- **电机速度环（快环）**：`dt` 固定，`Kd` 小，`Alpha` 适中（0.05~0.2）。
- **位置环（慢环）**：可降低 `Ki`，适当增加死区避免抖动。
- **串级控制**：外环位置 -> 内环速度，分别独立 PID 实例。

## 调参与排障

### 典型症状与处理
- **震荡明显**：降低 `Kp`/`Kd`，或增大 `Alpha`。
- **响应慢**：增大 `Kp`，再少量引入 `Ki`。
- **长期静差**：增大 `Ki`，并设置合理积分限幅。
- **小范围抖动**：增加 `DeadZone`。

### 常见坑点
- `dt == 0` 会导致微分项异常（除零风险）。
- 误差定义必须一致：`error = target - current`。
- 输出限幅过小会导致“看起来调不动”。

## 与代码规范对应
- 参数校验明确，避免隐式错误。
- 接口不依赖裸指针，易于单元测试与复用。
- 具备可预期行为，适配实时控制场景。

相关源码：
- [Components/pid_controller.hpp](../../Components/pid_controller.hpp)
