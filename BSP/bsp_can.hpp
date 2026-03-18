#ifndef BSP_CAN_HPP
#define BSP_CAN_HPP

#include "stm32f407xx.h"
#include "stm32f4xx_hal.h"
#include "stm32f4xx_hal_can.h"

#include <cstddef>
#include <cstdint>
#include <utility>

namespace gdut {

enum class can_type : uint8_t { standard_type = 1, extended_type = 2 };

enum class can_mailbox : uint32_t { mailbox0 = 1, mailbox1 = 2, mailbox2 = 4 };

enum class can_fifo : uint8_t { fifo0 = 0, fifo1 = 1 };

inline constexpr can_mailbox operator|(can_mailbox lhs, can_mailbox rhs) {
  return static_cast<can_mailbox>(std::to_underlying(lhs) |
                                  std::to_underlying(rhs));
}

inline constexpr can_mailbox operator&(can_mailbox lhs, can_mailbox rhs) {
  return static_cast<can_mailbox>(std::to_underlying(lhs) &
                                  std::to_underlying(rhs));
}

inline constexpr can_mailbox all_mailboxes =
    can_mailbox::mailbox0 | can_mailbox::mailbox1 | can_mailbox::mailbox2;

inline constexpr bool mailbox_allowed(can_mailbox mask, can_mailbox value) {
  return std::to_underlying(mask & value) != 0U;
}

class base_can_proxy {
public:
  static constexpr size_t can_max_count =
      10;                                // 每个总线最多支持 10 个 CAN 代理实例
  static constexpr size_t bus_count = 2; // STM32F407 支持 CAN1/CAN2 两条总线

  base_can_proxy(CAN_HandleTypeDef &hcan, CAN_TxHeaderTypeDef tx_header,
                 can_mailbox mail_box);

  virtual ~base_can_proxy() noexcept;

  // 注销当前实例（从全局实例表中移除，避免悬空指针）
  // 为避免 bus_index 参数错误导致悬空注册，会搜索所有总线
  bool unregister_self();

  // 将当前实例注册到全局实例表，并按 CAN ID 排序以便二分查找
  bool register_self(size_t bus_index);

  // 全局分发函数：从接收中断回调中调用，二分查找对应的实例并调用其 receive
  static void dispatch(size_t bus_index, CAN_RxHeaderTypeDef *rxh,
                       uint8_t data[8]);

  bool start();

  bool stop();

  // 发送 CAN 数据帧（固定 8 字节）
  bool transmit(const uint8_t data[8]);

protected:
  // 获取当前实例的 CAN ID（子类必须实现）
  virtual uint32_t get_can_id() const = 0;
  // 接收回调（子类可选实现，用于处理接收到的 CAN 数据）
  virtual bool receive(CAN_RxHeaderTypeDef *rxh, uint8_t data[8]);

private:
  const CAN_TxHeaderTypeDef m_tx_header; // 发送帧头模板（由子类构造函数初始化）
  const can_mailbox m_mail_box; // 允许使用的邮箱掩码（由子类构造函数初始化）
  CAN_HandleTypeDef &m_hcan;    // CAN 外设句柄引用

private:
  // 全局实例表
  static base_can_proxy *instances[bus_count][can_max_count];
  // 各总线已注册实例数量
  static size_t instances_count[bus_count];
};

template <can_type Type, uint32_t CanId,
          can_mailbox MailboxMask = all_mailboxes>
class can_proxy : public base_can_proxy {
public:
  // 编译期检查 CAN ID 合法性（标准帧 11 位，扩展帧 29 位）
  static_assert((Type == can_type::standard_type && CanId <= 0x7FF) ||
                    (Type == can_type::extended_type && CanId <= 0x1FFFFFFF),
                "Invalid CAN ID for the specified CAN type.");
  // 发送帧头模板
  static constexpr CAN_TxHeaderTypeDef tx_header_v = {
      .StdId = (Type == can_type::standard_type) ? CanId : 0,
      .ExtId = (Type == can_type::extended_type) ? CanId : 0,
      .IDE = (Type == can_type::standard_type) ? CAN_ID_STD : CAN_ID_EXT,
      .RTR = CAN_RTR_DATA,
      .DLC = 8, // 固定发送 8 字节数据帧
      .TransmitGlobalTime = DISABLE};

  can_proxy(CAN_HandleTypeDef &hcan)
      : base_can_proxy(hcan, tx_header_v, MailboxMask) {}
  virtual ~can_proxy() noexcept override = default;

protected:
  // 返回模板参数指定的 CAN ID
  virtual uint32_t get_can_id() const override { return CanId; }
};

} // namespace gdut

#endif // BSP_CAN_HPP 结束
