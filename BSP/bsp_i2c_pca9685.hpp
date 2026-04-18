#ifndef BSP_I2C_PCA9685_HPP
#define BSP_I2C_PCA9685_HPP

#include "stm32f4xx_hal.h"
#include "bsp_uncopyable.hpp"
#include <cstdint>
#include <cmath>

namespace gdut {

// ======================= PCA9685 寄存器定义 =======================
#define PCA9685_MODE1       0x00
#define PCA9685_MODE2       0x01
#define PCA9685_SUBADR1     0x02
#define PCA9685_SUBADR2     0x03
#define PCA9685_SUBADR3     0x04
#define PCA9685_ALLCALLADR  0x05
#define PCA9685_LED0_ON_L   0x06
#define PCA9685_LED0_ON_H   0x07
#define PCA9685_LED0_OFF_L  0x08
#define PCA9685_LED0_OFF_H  0x09
#define PCA9685_ALL_LED_ON_L  0xFA
#define PCA9685_ALL_LED_ON_H  0xFB
#define PCA9685_ALL_LED_OFF_L 0xFC
#define PCA9685_ALL_LED_OFF_H 0xFD
#define PCA9685_PRESCALE    0xFE
#define PCA9685_TESTMODE    0xFF

// MODE1 寄存器位定义
#define MODE1_RESTART       0x80
#define MODE1_EXTCLK        0x40
#define MODE1_AI            0x20
#define MODE1_SLEEP         0x10
#define MODE1_SUB1          0x08
#define MODE1_SUB2          0x04
#define MODE1_SUB3          0x02
#define MODE1_ALLCALL       0x01

// MODE2 寄存器位定义
#define MODE2_OUTDRV        0x04
#define MODE2_OUTNE1        0x02
#define MODE2_OUTNE0        0x01

#define I2C_ANALOGFILTER_ENABLE 0x00000000U
/**
 * @brief PCA9685 驱动类（I2C 接口）
 * 
 * 通过 I2C 总线控制 PCA9685 产生 16 路 PWM，用于舵机或 LED。
 * 默认使用 I2C3（SCL: PA8, SDA: PC9），地址 0x40。
 */
class pca9685 : private uncopyable {
public:
    /**
     * @brief 构造函数
     * @param hi2c I2C 句柄指针（需已初始化）
     * @param dev_addr PCA9685 的 I2C 从机地址（默认 0x40）
     */
    pca9685(I2C_HandleTypeDef* hi2c, uint8_t dev_addr = 0x40, uint32_t timeout_ms = 100)
        : hi2c_(hi2c), dev_addr_(dev_addr << 1),prescale_(0), timeout_ms_(timeout_ms)  // 左移1位适配 HAL 库地址格式
    {
    }

    /**
     * @brief 初始化 PCA9685
     * @param freq PWM 频率（Hz），典型值 50Hz（舵机）
     * @return true 成功，false 失败
     */
    bool begin(float freq = 50.0f) {
        if (hi2c_ == nullptr) return false;

        // 复位芯片
        if (!write_register(PCA9685_MODE1, 0x00))
            return false;

        // 设置 PWM 频率
        if (!set_pwm_frequency(freq))
            return false;

        // 使能自动增量，重启 PWM
        uint8_t mode1 = read_register(PCA9685_MODE1);
        mode1 |= MODE1_AI | MODE1_RESTART;
        return write_register(PCA9685_MODE1, mode1);
    }

    /**
     * @brief 设置单个通道的 PWM 波形
     * @param channel 通道号 (0~15)
     * @param on      导通计数值 (0~4095)
     * @param off     关断计数值 (0~4095)
     * @return true 成功，false 失败
     */
    bool set_pwm(uint8_t channel, uint16_t on, uint16_t off) {
        if (channel > 15) return false;

        uint8_t reg = PCA9685_LED0_ON_L + 4 * channel;
        uint8_t data[4] = {
            static_cast<uint8_t>(on & 0xFF),
            static_cast<uint8_t>(on >> 8),
            static_cast<uint8_t>(off & 0xFF),
            static_cast<uint8_t>(off >> 8)
        };
        return write_registers(reg, data, 4);
    }

    /**
     * @brief 以脉冲宽度方式设置 PWM（简化接口）
     * @param channel 通道号 (0~15)
     * @param pulse   脉冲宽度计数值 (0~4095)
     * @param invert  是否反相（默认 false）
     * @return true 成功，false 失败
     * 
     * @note 内部调用 set_pwm(0, pulse) 或反相逻辑，与 Adafruit 库一致
     */
    bool set_pin(uint8_t channel, uint16_t pulse, bool invert = false) {
        if (pulse > 4095) pulse = 4095;

        if (invert) {
            if (pulse == 0)
                return set_pwm(channel, 4096, 0);
            else if (pulse == 4095)
                return set_pwm(channel, 0, 4096);
            else
                return set_pwm(channel, 0, 4095 - pulse);
        } else {
            if (pulse == 4095)
                return set_pwm(channel, 4096, 0);
            else if (pulse == 0)
                return set_pwm(channel, 0, 4096);
            else
                return set_pwm(channel, 0, pulse);
        }
    }

    /**
     * @brief 设置舵机角度（简化接口）
     * @param channel 通道号 (0~15)
     * @param angle   角度 (0~180)
     * @param min_pulse_us 0° 对应脉宽（微秒），默认 1000
     * @param max_pulse_us 180° 对应脉宽（微秒），默认 2000
     * @return true 成功，false 失败
     * 
     * @note 脉宽映射与 main.cpp 中 set_servo_angle() 一致
     */
    bool set_servo_angle(uint8_t channel, uint8_t angle,
                         uint16_t min_pulse_us = 1000,
                         uint16_t max_pulse_us = 2000) {
        if (angle > 180) angle = 180;

        // 根据当前 PWM 频率计算每计数对应的时间（微秒）
        // 频率 = 内部时钟 / (4096 * (prescale + 1))
        // 每计数时间 = 1 / (频率 * 4096) = (prescale + 1) / 25e6  秒
        // 微秒/计数 = (prescale + 1) / 25
        float us_per_count = static_cast<float>(prescale_ + 1) / 25.0f;

        // 将微秒转换为计数值
        uint16_t pulse_counts = static_cast<uint16_t>(
            (min_pulse_us + (static_cast<uint32_t>(angle) * (max_pulse_us - min_pulse_us)) / 180) / us_per_count
        );

        // 限制最大计数值为 4095
        if (pulse_counts > 4095) pulse_counts = 4095;

        return set_pin(channel, pulse_counts);
    }

    /**
     * @brief 获取 I2C 句柄（用于外部操作）
     */
    I2C_HandleTypeDef* get_i2c_handle() const { return hi2c_; }

    // 新增：检查设备是否在线（发送地址并等待 ACK）
    bool is_connected() {
        return HAL_I2C_IsDeviceReady(hi2c_, dev_addr_, 2, timeout_ms_) == HAL_OK;
    }
        
private:
    I2C_HandleTypeDef* hi2c_;
    uint16_t dev_addr_;          // 左移1位后的地址（HAL 格式）
    uint8_t prescale_;           // 当前预分频值
    uint32_t timeout_ms_;
    
    // 写单个寄存器
    bool write_register(uint8_t reg, uint8_t value) {
        return HAL_I2C_Mem_Write(hi2c_, dev_addr_, reg, I2C_MEMADD_SIZE_8BIT,
                                 &value, 1, timeout_ms_) == HAL_OK;
    }

    // 读单个寄存器
    uint8_t read_register(uint8_t reg) {
        uint8_t value = 0;
        HAL_I2C_Mem_Read(hi2c_, dev_addr_, reg, I2C_MEMADD_SIZE_8BIT,
                         &value, 1, timeout_ms_);
        return value;
    }

    // 连续写多个寄存器
    bool write_registers(uint8_t reg, const uint8_t* data, size_t len) {
        return HAL_I2C_Mem_Write(hi2c_, dev_addr_, reg, I2C_MEMADD_SIZE_8BIT,
                                 const_cast<uint8_t*>(data), len, timeout_ms_) == HAL_OK;
    }



    /**
     * @brief 设置 PCA9685 的 PWM 频率
     * @param freq 目标频率（Hz）
     * @return true 成功，false 失败
     * 
     * @note 参考 Adafruit 库，频率需乘以 0.9 修正
     */
    bool set_pwm_frequency(float freq) {
        // 频率修正（与 Adafruit 库一致）
        freq *= 0.9f;
        // 计算预分频值：prescale = round(25e6 / (4096 * freq)) - 1
        float prescale_val = 25000000.0f;
        prescale_val /= 4096.0f;
        prescale_val /= freq;
        prescale_val -= 1.0f;
        prescale_ = static_cast<uint8_t>(prescale_val + 0.5f);

        // 读取当前 MODE1
        uint8_t old_mode = read_register(PCA9685_MODE1);
        // 进入睡眠模式（SLEEP 位置 1）
        uint8_t new_mode = (old_mode & 0x7F) | MODE1_SLEEP;
        if (!write_register(PCA9685_MODE1, new_mode))
            return false;

        // 写入预分频值
        if (!write_register(PCA9685_PRESCALE, prescale_))
            return false;

        // 恢复原模式（退出睡眠）
        if (!write_register(PCA9685_MODE1, old_mode))
            return false;

        // 等待振荡器稳定（至少 500us）
        HAL_Delay(1);

        return true;
    }
};

/**
 * @brief I2C 硬件初始化辅助函数
 * 
 * 初始化 I2C3（SCL: PA8, SDA: PC9）并返回句柄指针。
 * 调用示例：
 *   I2C_HandleTypeDef hi2c3;
 *   i2c3_init(&hi2c3);
 *   pca9685 pwm_driver(&hi2c3);
 */
inline bool i2c3_init(I2C_HandleTypeDef* hi2c) {
    if (hi2c == nullptr) return false;

    // 使能时钟
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_I2C3_CLK_ENABLE();

    // 配置 GPIO：PA8 (SCL), PC9 (SDA)
    GPIO_InitTypeDef gpio = {0};
    gpio.Mode = GPIO_MODE_AF_OD;
    gpio.Pull = GPIO_PULLUP;
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = GPIO_AF4_I2C3;

    gpio.Pin = GPIO_PIN_8;
    HAL_GPIO_Init(GPIOA, &gpio);

    gpio.Pin = GPIO_PIN_9;
    HAL_GPIO_Init(GPIOC, &gpio);

    // I2C 参数配置（标准模式 100kHz，快速模式 400kHz 可选）
    hi2c->Instance = I2C3;
    hi2c->Init.ClockSpeed = 100000;          // 100kHz
    hi2c->Init.DutyCycle = I2C_DUTYCYCLE_2;
    hi2c->Init.OwnAddress1 = 0;
    hi2c->Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
    hi2c->Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
    hi2c->Init.OwnAddress2 = 0;
    hi2c->Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
    hi2c->Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;

    if (HAL_I2C_Init(hi2c) != HAL_OK) {
        return false;
    }

    

    return true;
}

} // namespace gdut

#endif // BSP_I2C_PCA9685_HPP