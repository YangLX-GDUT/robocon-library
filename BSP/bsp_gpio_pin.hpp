#ifndef GPIO_PIN_HPP
#define GPIO_PIN_HPP

#include "bsp_type_traits.hpp"
#include "stm32f4xx_hal.h"

#include "bsp_uncopyable.hpp"

namespace gdut {

/**
 * @brief GPIO 引脚的编译期配置标签
 * @tparam Port GPIO 端口（A-I）
 * @tparam InitStruct HAL GPIO 初始化结构体
 */
template <gpio_port Port, GPIO_InitTypeDef InitStruct> struct gpio_pin_tag {
  static constexpr gpio_port port = Port;
  static constexpr GPIO_InitTypeDef init_struct = InitStruct;
};

/**
 * @brief HAL GPIO 引脚的 RAII 包装
 *
 * 该类提供编译期配置的 GPIO 引脚管理。
 * 构造时初始化，引脚析构时反初始化。
 *
 * 特性：
 * - 通过模板参数进行编译期配置
 * - RAII 资源管理
 * - 类型安全的端口和引脚选择
 * - 不可拷贝（硬件资源）
 *
 * 使用示例：
 *   gdut::gpio_pin<gdut::gpio_port::A,
 *                  GPIO_InitTypeDef{.Pin = GPIO_PIN_5,
 *                                   .Mode = GPIO_MODE_OUTPUT_PP}> led;
 *   led.write(true);  // 置高
 *
 * @tparam Port GPIO 端口（A-I）
 * @tparam InitStruct HAL GPIO 初始化结构体
 */
template <gpio_port Port, GPIO_InitTypeDef InitStruct>
class gpio_pin : private uncopyable {
public:
  using tag_type = gpio_pin_tag<Port, InitStruct>;

  gpio_pin() {
    GPIO_InitTypeDef init_struct = tag_type::init_struct;
    HAL_GPIO_Init(get_gpio_port_ptr(tag_type::port), &init_struct);
  }

  ~gpio_pin() {
    HAL_GPIO_DeInit(get_gpio_port_ptr(tag_type::port),
                    tag_type::init_struct.Pin);
  }

  void write(bool state) {
    HAL_GPIO_WritePin(get_gpio_port_ptr(tag_type::port),
                      tag_type::init_struct.Pin,
                      static_cast<GPIO_PinState>(state));
  }

  bool read() const noexcept {
    return HAL_GPIO_ReadPin(get_gpio_port_ptr(tag_type::port),
                            tag_type::init_struct.Pin);
  }

  void toggle() {
    HAL_GPIO_TogglePin(get_gpio_port_ptr(tag_type::port),
                       tag_type::init_struct.Pin);
  }
};

} // namespace gdut

#endif // GPIO_PIN_HPP
