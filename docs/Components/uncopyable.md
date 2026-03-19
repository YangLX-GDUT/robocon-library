# Components 不可拷贝基类模块（uncopyable.hpp）

## 原理
该模块提供一个禁止拷贝与赋值的基类，用于保护硬件资源类不被拷贝，避免重复释放或状态冲突。

## 实现思想
- 删除拷贝构造与拷贝赋值。
- 构造与析构保护为 `protected`，保证仅作为基类使用。

## 如何使用

```cpp
#include "uncopyable.hpp"

class my_device : private gdut::uncopyable {
public:
  void init();
  void deinit();
};

void demo() {
  my_device dev;
  dev.init();
  // my_device dev2 = dev; // 编译期禁止拷贝
}
```

## 与代码规范的对应
- 避免资源类被意外拷贝。
- 符合 RAII 与资源唯一性原则。

## 注意事项/坑点
- 该类仅用于继承，不提供任何资源管理功能。
- 继承方式建议使用 `private` 或 `protected`，避免暴露给外部接口。

相关源码：[Middlewares/GDUT_RC_Library/Components/uncopyable.hpp](../../Middlewares/GDUT_RC_Library/Components/uncopyable.hpp)
