#include "bsp_can.hpp"

#include <algorithm>

namespace gdut {

base_can_proxy *base_can_proxy::instances[base_can_proxy::bus_count]
                                         [base_can_proxy::can_max_count] = {};

size_t base_can_proxy::instances_count[base_can_proxy::bus_count] = {};

base_can_proxy::base_can_proxy(CAN_HandleTypeDef &hcan,
                               CAN_TxHeaderTypeDef tx_header,
                               can_mailbox mail_box)
    : m_tx_header(tx_header), m_mail_box(mail_box), m_hcan(hcan) {}

base_can_proxy::~base_can_proxy() noexcept { unregister_self(); }

bool base_can_proxy::unregister_self() {
  bool success = false;

  // 保护实例表的修改，避免与中断中的 dispatch 并发访问
  // 保存并恢复 PRIMASK 以支持嵌套临界区
  uint32_t primask = __get_PRIMASK();
  __disable_irq();

  // 在所有总线上查找并移除当前实例
  for (size_t bus = 0; bus < bus_count; ++bus) {
    for (size_t i = 0; i < instances_count[bus]; ++i) {
      if (instances[bus][i] == this) {
        // 将后续元素前移覆盖当前位置
        size_t j = i;
        for (; j < instances_count[bus] - 1; ++j) {
          instances[bus][j] = instances[bus][j + 1];
        }
        instances[bus][j] = nullptr;
        instances_count[bus]--;
        success = true;
        break;
      }
    }
    if (success) {
      break; // 找到并移除后退出
    }
  }

  __set_PRIMASK(primask);

  return success;
}

bool base_can_proxy::register_self(size_t bus_index) {
  uint32_t can_id = get_can_id();
  // 快速失败检查：不进入临界区
  if (bus_index >= bus_count || instances_count[bus_index] >= can_max_count) {
    return false; // 总线索引越界或实例表已满
  }

  bool success = false;

  // 保护实例表的修改，避免与中断中的 dispatch 并发访问
  // 保存并恢复 PRIMASK 以支持嵌套临界区
  uint32_t primask = __get_PRIMASK();
  __disable_irq();

  // 在临界区内再次检查容量和重复 ID，以防并发注册导致状态改变
  if (instances_count[bus_index] < can_max_count) {
    // 检查是否已注册相同 ID（遍历已有实例）
    bool id_conflict = false;
    for (size_t i = 0; i < instances_count[bus_index]; ++i) {
      if (instances[bus_index][i] != nullptr &&
          instances[bus_index][i]->get_can_id() == can_id) {
        id_conflict = true;
        break;
      }
    }

    if (!id_conflict) {
      // 追加到末尾
      instances[bus_index][instances_count[bus_index]] = this;
      instances_count[bus_index]++;
      // 按 CAN ID 升序排序（便于二分查找）
      std::sort(instances[bus_index],
                instances[bus_index] + instances_count[bus_index],
                [](const base_can_proxy *a, const base_can_proxy *b) {
                  return a->get_can_id() < b->get_can_id();
                });
      success = true;
    }
  }

  __set_PRIMASK(primask);

  return success;
}

void base_can_proxy::dispatch(size_t bus_index, CAN_RxHeaderTypeDef *rxh,
                              uint8_t data[8]) {
  if (bus_index >= bus_count) {
    return; // 总线索引非法
  }
  // 从帧头提取 CAN ID（区分标准帧/扩展帧）
  uint32_t can_id = (rxh->IDE == CAN_ID_STD) ? rxh->StdId : rxh->ExtId;
  using iter_t = base_can_proxy **;
  iter_t begin = instances[bus_index];
  iter_t end = instances[bus_index] + instances_count[bus_index];
  // 二分查找 CAN ID 对应的实例（实例数组已按 ID 升序排列）
  iter_t it = std::lower_bound(begin, end, can_id,
                               [](const base_can_proxy *proxy, uint32_t id) {
                                 return proxy->get_can_id() < id;
                               });
  if (it != end && (*it)->get_can_id() == can_id) {
    (*it)->receive(rxh, data); // 交给实例处理接收数据
  }
}

bool base_can_proxy::start() { return HAL_CAN_Start(&m_hcan) == HAL_OK; }

bool base_can_proxy::stop() { return HAL_CAN_Stop(&m_hcan) == HAL_OK; }

bool base_can_proxy::transmit(const uint8_t data[8]) {
  if (data == nullptr) {
    return false; // 参数非法
  }
  // 检查是否有空闲邮箱
  if (HAL_CAN_GetTxMailboxesFreeLevel(&m_hcan) == 0U) {
    return false; // 3 个邮箱都满，无法发送
  }
  uint32_t mailbox{0};
  // 使用可变的本地副本以保持 const-correctness
  CAN_TxHeaderTypeDef tx_header = m_tx_header;
  // 将帧加入发送队列，硬件会自动仲裁并发送
  auto status = HAL_CAN_AddTxMessage(&m_hcan, &tx_header, data, &mailbox);
  if (status != HAL_OK) {
    return false; // HAL 发送失败
  }
  // 检查是否使用了允许的邮箱（可选的邮箱掩码限制）
  if (!mailbox_allowed(m_mail_box, static_cast<can_mailbox>(mailbox))) {
    HAL_CAN_AbortTxRequest(&m_hcan, mailbox); // 终止不符合要求的邮箱
    return false;
  }
  return true;
}

bool base_can_proxy::receive(CAN_RxHeaderTypeDef *rxh, uint8_t data[8]) {
  static_cast<void>(rxh);
  static_cast<void>(data);
  return true; // 默认不处理接收数据
}

} // namespace gdut

// CAN FIFO0 接收中断回调（HAL 库会在 FIFO0 有消息挂起时自动调用）
// 强符号定义以可靠覆盖 HAL 的 __weak 默认实现
extern "C" void HAL_CAN_RxFifo0MsgPendingCallback(CAN_HandleTypeDef *hcan) {
  CAN_RxHeaderTypeDef rxh;
  uint8_t data[8];
  // 从 FIFO0 读取一帧数据
  if (HAL_CAN_GetRxMessage(hcan, CAN_RX_FIFO0, &rxh, data) != HAL_OK) {
    return; // 读取失败，跳过
  }

  // 根据 CAN 外设实例确定总线索引（CAN1=0, CAN2=1）
  size_t bus_index = (hcan->Instance == CAN1)   ? 0
                     : (hcan->Instance == CAN2) ? 1
                                                : 2;
  if (bus_index >= gdut::base_can_proxy::bus_count) {
    return; // 未知总线，跳过
  }

  // 交给全局分发函数，二分查找并调用对应实例的 receive
  gdut::base_can_proxy::dispatch(bus_index, &rxh, data);
}

// CAN FIFO1 接收中断回调（HAL 库会在 FIFO1 有消息挂起时自动调用）
// 强符号定义以可靠覆盖 HAL 的 __weak 默认实现
extern "C" void HAL_CAN_RxFifo1MsgPendingCallback(CAN_HandleTypeDef *hcan) {
  CAN_RxHeaderTypeDef rxh;
  uint8_t data[8];
  // 从 FIFO1 读取一帧数据
  if (HAL_CAN_GetRxMessage(hcan, CAN_RX_FIFO1, &rxh, data) != HAL_OK) {
    return; // 读取失败，跳过
  }

  // 根据 CAN 外设实例确定总线索引（CAN1=0, CAN2=1）
  size_t bus_index = (hcan->Instance == CAN1)   ? 0
                     : (hcan->Instance == CAN2) ? 1
                                                : 2;
  if (bus_index >= gdut::base_can_proxy::bus_count) {
    return; // 未知总线，跳过
  }

  // 交给全局分发函数，二分查找并调用对应实例的 receive
  gdut::base_can_proxy::dispatch(bus_index, &rxh, data);
}
