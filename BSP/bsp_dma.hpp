#ifndef BSP_DMA_HPP
#define BSP_DMA_HPP

#include "stm32f4xx_hal.h"
#include "stm32f4xx_hal_dma.h"
#include "stm32f4xx_hal_i2c.h"
#include "stm32f4xx_hal_uart.h"

#include "bsp_function.hpp"
#include "bsp_type_traits.hpp"
#include "bsp_uncopyable.hpp"

#include <cstddef>
#include <cstdint>
#include <system_error>
#include <utility>

namespace gdut {

/**
 * @brief DMA 错误类型的 std::error_category 实现
 *
 * 将 STM32 HAL DMA 错误码（HAL_DMA_ERROR_*）映射为可读字符串，
 * 以便通过 std::error_code 统一向上层上报 DMA 传输错误。
 */
class dma_error_category : public std::error_category {
public:
  constexpr dma_error_category() noexcept = default;

  const char *name() const noexcept override { return "dma_error_code"; }

  std::string message(int ev) const override {
    // ErrorCode 是位掩码，多个错误标志可能同时置位，逐位检查并拼接描述
    if (ev == HAL_DMA_ERROR_NONE) {
      return "No error";
    }
    std::string msg;
    if (ev & HAL_DMA_ERROR_TE) {
      msg += "Transfer error; ";
    }
    if (ev & HAL_DMA_ERROR_FE) {
      msg += "FIFO error; ";
    }
    if (ev & HAL_DMA_ERROR_DME) {
      msg += "Direct mode error; ";
    }
    if (ev & HAL_DMA_ERROR_TIMEOUT) {
      msg += "Timeout error; ";
    }
    if (ev & HAL_DMA_ERROR_PARAM) {
      msg += "Parameter error; ";
    }
    if (ev & HAL_DMA_ERROR_NO_XFER) {
      msg += "Abort requested with no transfer ongoing; ";
    }
    if (ev & HAL_DMA_ERROR_NOT_SUPPORTED) {
      msg += "Not supported mode; ";
    }
    if (msg.empty()) {
      return "Unknown error";
    }
    // 移除末尾多余的 "; "
    msg.resize(msg.size() - 2);
    return msg;
  }

  static const dma_error_category &instance() {
    static dma_error_category instance;
    return instance;
  }
};

/**
 * @brief 类型安全的 DMA 错误码枚举，对应 HAL_DMA_ERROR_* 系列宏
 */
enum class dma_error_code : uint32_t {
  none = HAL_DMA_ERROR_NONE,
  transfer_error = HAL_DMA_ERROR_TE,
  fifo_error = HAL_DMA_ERROR_FE,
  direct_mode_error = HAL_DMA_ERROR_DME,
  timeout_error = HAL_DMA_ERROR_TIMEOUT,
  parameter_error = HAL_DMA_ERROR_PARAM,
  no_transfer = HAL_DMA_ERROR_NO_XFER,
  not_supported = HAL_DMA_ERROR_NOT_SUPPORTED
};

/**
 * @brief 将 dma_error_code 转换为 std::error_code。
 *
 * 提供此函数后，结合下方的 std::is_error_code_enum 特化，
 * dma_error_code 枚举值可隐式转换为 std::error_code，
 * 方便在错误处理代码中直接使用枚举值而无需手动构造 error_code。
 */
inline std::error_code make_error_code(dma_error_code e) {
  return {static_cast<int>(e), dma_error_category::instance()};
}

} // namespace gdut

namespace std {

/// 启用 gdut::dma_error_code 到 std::error_code 的隐式转换
template <> struct is_error_code_enum<gdut::dma_error_code> : true_type {};

} // namespace std

namespace gdut {

/**
 * @brief HAL DMA 句柄的 RAII 代理封装
 *
 * 对 STM32 HAL 的 DMA_HandleTypeDef 进行 C++ 面向对象封装，提供
 * 安全的初始化、配置和传输启动接口，以及基于 std::error_code 的统一错误回调。
 *
 * 特性：
 * - RAII 自动管理：析构时调用 deinit()，清理 HAL 句柄和回调指针
 * - 支持移动语义，禁止复制
 * - 统一错误回调（std::error_code）：传输完成/出错均通过同一回调通知
 * - 所有 set_* 配置方法均含 nullptr 有效性检查，空句柄时为空操作
 * - valid() / operator bool() 可快速检查句柄是否可用
 *
 * 线程安全：
 * - 该类**不**提供内部同步。回调函数在中断上下文执行；
 *   若需从任务上下文并发访问，调用方须在外部加互斥保护。
 * - bind_tx/bind_rx 和 init() 应在 DMA 传输开始前完成单次配置，
 *   不支持运行时并发修改。
 * - 在 DMA 传输进行中调用 set_callback_handler() 是不安全的。
 *
 * 重要约束：
 * - DMA 传输所用的数据缓冲区**不能**放在 CCMRAM（CCM RAM 不能被 DMA 访问）。
 *
 * 使用示例：
 * @code
 * // CubeMX 生成的全局 DMA 句柄
 * DMA_HandleTypeDef hdma_usart1_rx;
 *
 * gdut::dma_proxy dma_rx(&hdma_usart1_rx);
 * dma_rx.set_callback_handler([](std::error_code ec) {
 *     if (!ec) { // 传输完成（error_code 为空表示成功）
 *         process_dma_data();
 *     } else {   // 传输出错
 *         handle_dma_error(ec);
 *     }
 * });
 * dma_rx.init();
 * @endcode
 */
class dma_proxy : uncopyable {
public:
  using callback_t = function<void(std::error_code)>;

  explicit dma_proxy(DMA_HandleTypeDef *handle) : m_handle(handle) {}

  dma_proxy(dma_proxy &&other) noexcept
      : m_handle(std::exchange(other.m_handle, nullptr)),
        m_callback_handler(std::move(other.m_callback_handler)) {}

  dma_proxy &operator=(dma_proxy &&other) noexcept {
    if (this != std::addressof(other)) {
      deinit();
      m_handle = std::exchange(other.m_handle, nullptr);
      m_callback_handler = std::move(other.m_callback_handler);
    }
    return *this;
  }

  ~dma_proxy() noexcept { deinit(); }

  /**
   * @brief 初始化 DMA 句柄，调用 HAL_DMA_Init。
   *
   * 必须在开始任何传输之前调用。若 m_handle 为 nullptr，则为空操作。
   * 单独调用 start() 时，Parent 与回调由 start() 自动绑定；
   * 外设类（dma_uart / dma_spi / dma_i2c）在调用本函数后自行配置回调。
   */
  void init() {
    if (m_handle) {
      HAL_DMA_Init(m_handle);
    }
  }

  /**
   * @brief 反初始化 DMA，清理所有回调指针及句柄关联。
   *
   * 以尽力（best-effort）方式执行清理：即使 HAL_DMA_DeInit 返回非 HAL_OK，
   * 也会继续清空回调指针并将 m_handle 置为 nullptr，以防止悬空指针。
   */
  void deinit() {
    if (m_handle) {
      HAL_DMA_DeInit(m_handle); // 尽力反初始化，不检查返回值
      m_handle->XferCpltCallback = nullptr;
      m_handle->XferHalfCpltCallback = nullptr;
      m_handle->XferErrorCallback = nullptr;
      m_handle->XferAbortCallback = nullptr;
      m_handle->Parent = nullptr;
      m_handle = nullptr;
    }
  }

  [[nodiscard]] bool valid() const noexcept { return m_handle != nullptr; }

  explicit operator bool() const noexcept { return valid(); }

  /**
   * @brief 以中断模式启动一次 DMA 传输。
   *
   * 自动将 Parent 与完成/错误回调绑定到本对象，以便 DMA 中断触发时转发给
   * 通过 set_callback_handler() 注册的用户回调。
   * 若启动失败（句柄无效或 HAL 出错），会通过回调向上层通知，
   * 以便仅依赖回调的调用方感知启动阶段的失败。
   *
   * @param src_address  源地址
   * @param dst_address  目标地址
   * @param data_length  数据长度（以 DMA 数据宽度单位为单位）
   */
  void start(void *src_address, void *dst_address, std::size_t data_length) {
    if (!m_handle) {
      if (m_callback_handler) {
        m_callback_handler(std::make_error_code(std::errc::invalid_argument));
      }
      return; // DMA 句柄无效
    }
    // 绑定 Parent 与回调，确保 DMA 中断能够回调到本对象
    m_handle->Parent = this;
    m_handle->XferCpltCallback = xfer_cplt_cb;
    m_handle->XferErrorCallback = xfer_error_cb;
    const auto status =
        HAL_DMA_Start_IT(m_handle, reinterpret_cast<uint32_t>(src_address),
                         reinterpret_cast<uint32_t>(dst_address), data_length);
    if (status != HAL_OK) {
      if (m_callback_handler) {
        // 启动失败，通过回调上报 HAL DMA 错误码（ErrorCode 含具体故障原因）
        m_callback_handler(
            make_error_code(static_cast<dma_error_code>(m_handle->ErrorCode)));
      }
    }
  }

  [[nodiscard]] DMA_HandleTypeDef *get_handle() const { return m_handle; }

  void set_callback_handler(const function<void(std::error_code)> &handler) {
    m_callback_handler = handler;
  }

  void set_callback_handler(function<void(std::error_code)> &&handler) {
    m_callback_handler = std::move(handler);
  }

  void set_instance(dma_stream_type instance) {
    if (!m_handle) {
      return;
    }
    m_handle->Instance = get_dma_stream(instance);
  }

  void set_channel(dma_channel channel) {
    if (!m_handle) {
      return;
    }
    m_handle->Init.Channel = std::to_underlying(channel);
  }

  void set_direction(dma_direction direction) {
    if (!m_handle) {
      return;
    }
    m_handle->Init.Direction = std::to_underlying(direction);
  }

  void set_periph_inc(bool enable) {
    if (!m_handle) {
      return;
    }
    m_handle->Init.PeriphInc = enable ? DMA_PINC_ENABLE : DMA_PINC_DISABLE;
  }

  void set_mem_inc(bool enable) {
    if (!m_handle) {
      return;
    }
    m_handle->Init.MemInc = enable ? DMA_MINC_ENABLE : DMA_MINC_DISABLE;
  }

  void set_periph_data_alignment(dma_peripheral_data_alignment alignment) {
    if (!m_handle) {
      return;
    }
    m_handle->Init.PeriphDataAlignment = std::to_underlying(alignment);
  }

  void set_mem_data_alignment(dma_memory_data_alignment alignment) {
    if (!m_handle) {
      return;
    }
    m_handle->Init.MemDataAlignment = std::to_underlying(alignment);
  }

  void set_mode(dma_mode mode) {
    if (!m_handle) {
      return;
    }
    m_handle->Init.Mode = std::to_underlying(mode);
  }

  void set_priority(dma_priority priority) {
    if (!m_handle) {
      return;
    }
    m_handle->Init.Priority = std::to_underlying(priority);
  }

  void set_fifo_mode(dma_fifo_mode mode) {
    if (!m_handle) {
      return;
    }
    m_handle->Init.FIFOMode = std::to_underlying(mode);
  }

  void set_fifo_threshold(dma_fifo_threshold threshold) {
    if (!m_handle) {
      return;
    }
    m_handle->Init.FIFOThreshold = std::to_underlying(threshold);
  }

  void set_memory_burst(dma_memory_burst burst) {
    if (!m_handle) {
      return;
    }
    m_handle->Init.MemBurst = std::to_underlying(burst);
  }

  void set_peripheral_burst(dma_peripheral_burst burst) {
    if (!m_handle) {
      return;
    }
    m_handle->Init.PeriphBurst = std::to_underlying(burst);
  }

  void call_dma_callback(std::error_code ec) {
    if (m_callback_handler) {
      m_callback_handler(ec);
    }
  }

private:
  static void xfer_cplt_cb(DMA_HandleTypeDef *hdma) {
    if (!hdma || !hdma->Parent)
      return;
    auto *self = static_cast<dma_proxy *>(hdma->Parent);
    if (self)
      self->call_dma_callback({});
  }

  static void xfer_error_cb(DMA_HandleTypeDef *hdma) {
    if (!hdma || !hdma->Parent)
      return;
    auto *self = static_cast<dma_proxy *>(hdma->Parent);
    if (self)
      self->call_dma_callback(
          std::error_code(hdma->ErrorCode, dma_error_category::instance()));
  }

  DMA_HandleTypeDef *m_handle{nullptr};
  callback_t
      m_callback_handler{}; // 显式初始化为空，防止未初始化的函数对象被调用
};

/**
 * @brief DMA 外设操作的 CRTP 基类
 *
 * 使用 CRTP（奇异递归模板模式）为具体外设（UART/I2C/SPI）提供统一的
 * DMA 操作接口（绑定 TX/RX 代理、发送、接收），避免虚函数调度开销。
 *
 * 派生类须实现以下私有方法（通过 friend class dma_transfer_base<Derived>
 * 开放访问）：
 * - do_bind_tx(dma_proxy*)
 * - do_bind_rx(dma_proxy*)
 * - do_transmit(const uint8_t*, std::size_t, uint16_t)
 * - do_receive(uint8_t*, std::size_t, uint16_t)
 *
 * @tparam Derived  派生类类型（如 dma_uart、dma_i2c、dma_spi）
 *
 * @note 不可复制，但允许派生类提供移动操作。
 * @note address 参数含义因外设而异：
 *       - UART/SPI：忽略 address 参数（内部通过 (void)address 消除警告）
 *       - I2C：7 位从机地址，有效范围 0x08~0x77，address=0 在 I2C 总线上无效。
 *
 * 线程安全：bind_tx/bind_rx/transmit/receive 均**不**提供内部同步，
 * 应在单一上下文中依次调用（配置阶段绑定，传输阶段发起）。
 */
template <typename Derived> class dma_transfer_base : uncopyable {
protected:
  dma_transfer_base() = default;
  ~dma_transfer_base() = default;

public:
  void bind_tx(dma_proxy *tx_dma) {
    static_cast<Derived *>(this)->do_bind_tx(tx_dma);
  }

  void bind_rx(dma_proxy *rx_dma) {
    static_cast<Derived *>(this)->do_bind_rx(rx_dma);
  }

  /**
   * @brief 发起 DMA 发送。
   * @param data     发送数据缓冲区（HAL 接口未做到 const-correct，
   *                 内部使用 const_cast 适配，HAL 不会修改数据内容）
   * @param size     数据长度（字节数）
   * @param address  仅 I2C 有效：7 位从机地址（UART/SPI 忽略）
   */
  bool transmit(const uint8_t *data, std::size_t size, uint16_t address = 0) {
    return static_cast<Derived *>(this)->do_transmit(data, size, address);
  }

  /**
   * @brief 发起 DMA 接收。
   * @param buffer   接收缓冲区
   * @param size     数据长度（字节数）
   * @param address  仅 I2C 有效：7 位从机地址（UART/SPI 忽略）
   */
  bool receive(uint8_t *buffer, std::size_t size, uint16_t address = 0) {
    return static_cast<Derived *>(this)->do_receive(buffer, size, address);
  }

  /// 同时绑定 TX 和 RX 两个 DMA 代理
  void bind(dma_proxy *tx_dma, dma_proxy *rx_dma) {
    bind_tx(tx_dma);
    bind_rx(rx_dma);
  }

  /// @deprecated 请使用 receive() 替代
  [[deprecated("请使用 receive() 替代 start_receive()")]]
  void start_receive(uint8_t *buffer, std::size_t size) {
    receive(buffer, size);
  }
};

/**
 * @brief 针对 I2C 外设的 DMA 操作封装（主机模式）
 *
 * 通过 CRTP 继承 dma_transfer_base，将 TX/RX dma_proxy 与 I2C HAL 句柄关联，
 * 并通过 HAL_I2C_Master_Transmit_DMA / HAL_I2C_Master_Receive_DMA 发起传输。
 *
 * @note address 为 7 位从机地址（有效范围 0x08~0x77），传 0 在 I2C 总线上无效。
 */
class dma_i2c : public dma_transfer_base<dma_i2c> {
public:
  explicit dma_i2c(I2C_HandleTypeDef *m_i2c) : m_i2c(m_i2c) {}

  ~dma_i2c() = default;

  dma_i2c(dma_i2c &&other) noexcept
      : m_i2c(std::exchange(other.m_i2c, nullptr)),
        m_tx_dma(std::exchange(other.m_tx_dma, nullptr)),
        m_rx_dma(std::exchange(other.m_rx_dma, nullptr)) {}

  dma_i2c &operator=(dma_i2c &&other) noexcept {
    if (this != std::addressof(other)) {
      m_i2c = std::exchange(other.m_i2c, nullptr);
      m_tx_dma = std::exchange(other.m_tx_dma, nullptr);
      m_rx_dma = std::exchange(other.m_rx_dma, nullptr);
    }
    return *this;
  }

private:
  friend class dma_transfer_base<dma_i2c>;

  void do_bind_tx(dma_proxy *tx_dma) {
    if (tx_dma && m_i2c) {
      DMA_HandleTypeDef *tx_handle = tx_dma->get_handle();
      if (tx_handle != nullptr) {
        m_tx_dma = tx_dma;
        __HAL_LINKDMA(m_i2c, hdmatx, *tx_handle);
      }
    }
  }

  void do_bind_rx(dma_proxy *rx_dma) {
    if (rx_dma && m_i2c) {
      DMA_HandleTypeDef *rx_handle = rx_dma->get_handle();
      if (rx_handle != nullptr) {
        m_rx_dma = rx_dma;
        __HAL_LINKDMA(m_i2c, hdmarx, *rx_handle);
      }
    }
  }

  bool do_transmit(const uint8_t *data, std::size_t size, uint16_t address) {
    // STM32 HAL 的 HAL_I2C_Master_Transmit_DMA 在语义上将 data 视为只读缓冲区，
    // 但其 C 接口未做到 const-correct，参数类型为 uint8_t* 而非 const
    // uint8_t*。 此处使用 const_cast 仅用于适配该 HAL 接口；HAL 不会修改 data
    // 指向的数据。

    // 传输大小超过 HAL uint16_t 范围则拒绝，避免截断导致错误传输
    if (m_tx_dma == nullptr || m_i2c == nullptr || data == nullptr ||
        size == 0U || size > 65535U) {
      return false;
    }

    m_tx_dma->init(); // 确保 DMA 代理已初始化并关联到 HAL 句柄

    // 覆盖 DMA 句柄的 Parent 与完成/错误回调，以便在 DMA 中断里转发用户回调
    DMA_HandleTypeDef *hdma_tx = m_tx_dma->get_handle();
    if (!hdma_tx) {
      return false;
    }
    hdma_tx->Parent = this;
    hdma_tx->XferCpltCallback = tx_dma_cplt_cb;
    hdma_tx->XferErrorCallback = tx_dma_error_cb;

    // HAL_I2C_Master_Transmit_DMA(m_i2c, address, const_cast<uint8_t *>(data),
    //                             static_cast<uint16_t>(size));
    if (m_i2c->State != HAL_I2C_STATE_READY) {
      return false;
    }

    __IO uint32_t count = 0U;
    HAL_StatusTypeDef dmaxferstatus;
    /* Wait until BUSY flag is reset */
    count = 25U * (SystemCoreClock / 25U / 1000U);
    do {
      count--;
      if (count == 0U) {
        m_i2c->PreviousState = HAL_I2C_MODE_NONE;
        m_i2c->State = HAL_I2C_STATE_READY;
        m_i2c->Mode = HAL_I2C_MODE_NONE;
        m_i2c->ErrorCode |= HAL_I2C_ERROR_TIMEOUT;

        return false;
      }
    } while (__HAL_I2C_GET_FLAG(m_i2c, I2C_FLAG_BUSY) != RESET);

    /* Process Locked */
    __HAL_LOCK(m_i2c);

    /* Check if the I2C is already enabled */
    if ((m_i2c->Instance->CR1 & I2C_CR1_PE) != I2C_CR1_PE) {
      /* Enable I2C peripheral */
      __HAL_I2C_ENABLE(m_i2c);
    }

    /* Disable Pos */
    CLEAR_BIT(m_i2c->Instance->CR1, I2C_CR1_POS);

    m_i2c->State = HAL_I2C_STATE_BUSY_TX;
    m_i2c->Mode = HAL_I2C_MODE_MASTER;
    m_i2c->ErrorCode = HAL_I2C_ERROR_NONE;

    /* Prepare transfer parameters */
    m_i2c->pBuffPtr = const_cast<uint8_t *>(data);
    m_i2c->XferCount = static_cast<uint16_t>(size);
    m_i2c->XferSize = m_i2c->XferCount;
    m_i2c->XferOptions = 0xFFFF0000U; /* Don't start the transfer yet */
    m_i2c->Devaddress = address;
    if (m_i2c->XferSize > 0U) {
      if (m_i2c->hdmatx != nullptr) {
        /* Enable the DMA stream */
        dmaxferstatus = HAL_DMA_Start_IT(
            m_i2c->hdmatx, reinterpret_cast<uint32_t>(m_i2c->pBuffPtr),
            reinterpret_cast<uint32_t>(&m_i2c->Instance->DR), m_i2c->XferSize);
      } else {
        /* Update I2C state */
        m_i2c->State = HAL_I2C_STATE_READY;
        m_i2c->Mode = HAL_I2C_MODE_NONE;

        /* Update I2C error code */
        m_i2c->ErrorCode |= HAL_I2C_ERROR_DMA_PARAM;

        /* Process Unlocked */
        __HAL_UNLOCK(m_i2c);

        return false;
      }

      if (dmaxferstatus == HAL_OK) {
        /* Process Unlocked */
        __HAL_UNLOCK(m_i2c);

        /* Note : The I2C interrupts must be enabled after unlocking current
        process to avoid the risk of I2C interrupt handle execution before
        current process unlock */

        /* Enable EVT and ERR interrupt */
        __HAL_I2C_ENABLE_IT(m_i2c, I2C_IT_EVT | I2C_IT_ERR);

        /* Enable DMA Request */
        SET_BIT(m_i2c->Instance->CR2, I2C_CR2_DMAEN);

        /* Enable Acknowledge */
        SET_BIT(m_i2c->Instance->CR1, I2C_CR1_ACK);

        /* Generate Start */
        SET_BIT(m_i2c->Instance->CR1, I2C_CR1_START);
      } else {
        /* Update I2C state */
        m_i2c->State = HAL_I2C_STATE_READY;
        m_i2c->Mode = HAL_I2C_MODE_NONE;

        /* Update I2C error code */
        m_i2c->ErrorCode |= HAL_I2C_ERROR_DMA;

        /* Process Unlocked */
        __HAL_UNLOCK(m_i2c);

        return false;
      }
    } else {
      /* Enable Acknowledge */
      SET_BIT(m_i2c->Instance->CR1, I2C_CR1_ACK);

      /* Generate Start */
      SET_BIT(m_i2c->Instance->CR1, I2C_CR1_START);

      /* Process Unlocked */
      __HAL_UNLOCK(m_i2c);

      /* Note : The I2C interrupts must be enabled after unlocking current
      process to avoid the risk of I2C interrupt handle execution before current
      process unlock */

      /* Enable EVT, BUF and ERR interrupt */
      __HAL_I2C_ENABLE_IT(m_i2c, I2C_IT_EVT | I2C_IT_BUF | I2C_IT_ERR);
    }

    return true;
  }

  bool do_receive(uint8_t *buffer, std::size_t size, uint16_t address) {
    // 传输大小超过 HAL uint16_t 范围则拒绝，避免截断导致错误传输
    if (m_rx_dma == nullptr || m_i2c == nullptr || buffer == nullptr ||
        size == 0U || size > 65535U) {
      return false;
    }

    m_rx_dma->init();

    // 覆盖 DMA 句柄的 Parent 与完成/错误回调，以便在 DMA 中断里转发用户回调
    DMA_HandleTypeDef *hdma_rx = m_rx_dma->get_handle();
    if (!hdma_rx) {
      return false;
    }
    hdma_rx->Parent = this;
    hdma_rx->XferCpltCallback = rx_dma_cplt_cb;
    hdma_rx->XferErrorCallback = rx_dma_error_cb;

    // HAL_I2C_Master_Receive_DMA(m_i2c, address, buffer,
    //                            static_cast<uint16_t>(size));
    if (m_i2c->State != HAL_I2C_STATE_READY) {
      return false;
    }

    __IO uint32_t count = 0U;
    HAL_StatusTypeDef dmaxferstatus;
    /* Wait until BUSY flag is reset */
    count = 25U * (SystemCoreClock / 25U / 1000U);
    do {
      count--;
      if (count == 0U) {
        m_i2c->PreviousState = HAL_I2C_MODE_NONE;
        m_i2c->State = HAL_I2C_STATE_READY;
        m_i2c->Mode = HAL_I2C_MODE_NONE;
        m_i2c->ErrorCode |= HAL_I2C_ERROR_TIMEOUT;

        return false;
      }
    } while (__HAL_I2C_GET_FLAG(m_i2c, I2C_FLAG_BUSY) != RESET);

    /* Process Locked */
    __HAL_LOCK(m_i2c);

    /* Check if the I2C is already enabled */
    if ((m_i2c->Instance->CR1 & I2C_CR1_PE) != I2C_CR1_PE) {
      /* Enable I2C peripheral */
      __HAL_I2C_ENABLE(m_i2c);
    }

    /* Disable Pos */
    CLEAR_BIT(m_i2c->Instance->CR1, I2C_CR1_POS);

    m_i2c->State = HAL_I2C_STATE_BUSY_RX;
    m_i2c->Mode = HAL_I2C_MODE_MASTER;
    m_i2c->ErrorCode = HAL_I2C_ERROR_NONE;

    /* Prepare transfer parameters */
    m_i2c->pBuffPtr = buffer;
    m_i2c->XferCount = static_cast<uint16_t>(size);
    m_i2c->XferSize = m_i2c->XferCount;
    m_i2c->XferOptions = 0xFFFF0000U; /* Don't start the transfer yet */
    m_i2c->Devaddress = address;
    if (m_i2c->XferSize > 0U) {
      if (m_i2c->hdmarx != nullptr) {
        /* Enable the DMA stream */
        dmaxferstatus = HAL_DMA_Start_IT(
            m_i2c->hdmarx, reinterpret_cast<uint32_t>(&m_i2c->Instance->DR),
            reinterpret_cast<uint32_t>(m_i2c->pBuffPtr), m_i2c->XferSize);
      } else {
        /* Update I2C state */
        m_i2c->State = HAL_I2C_STATE_READY;
        m_i2c->Mode = HAL_I2C_MODE_NONE;

        /* Update I2C error code */
        m_i2c->ErrorCode |= HAL_I2C_ERROR_DMA_PARAM;

        /* Process Unlocked */
        __HAL_UNLOCK(m_i2c);

        return false;
      }

      if (dmaxferstatus == HAL_OK) {
        /* Enable Acknowledge */
        SET_BIT(m_i2c->Instance->CR1, I2C_CR1_ACK);

        /* Generate Start */
        SET_BIT(m_i2c->Instance->CR1, I2C_CR1_START);

        /* Process Unlocked */
        __HAL_UNLOCK(m_i2c);

        /* Note : The I2C interrupts must be enabled after unlocking current
        process to avoid the risk of I2C interrupt handle execution before
        current process unlock */

        /* Enable EVT and ERR interrupt */
        __HAL_I2C_ENABLE_IT(m_i2c, I2C_IT_EVT | I2C_IT_ERR);

        /* Enable DMA Request */
        SET_BIT(m_i2c->Instance->CR2, I2C_CR2_DMAEN);
      } else {
        /* Update I2C state */
        m_i2c->State = HAL_I2C_STATE_READY;
        m_i2c->Mode = HAL_I2C_MODE_NONE;

        /* Update I2C error code */
        m_i2c->ErrorCode |= HAL_I2C_ERROR_DMA;

        /* Process Unlocked */
        __HAL_UNLOCK(m_i2c);

        return false;
      }
    } else {
      /* Process Unlocked */
      __HAL_UNLOCK(m_i2c);

      /* Note : The I2C interrupts must be enabled after unlocking current
      process to avoid the risk of I2C interrupt handle execution before current
      process unlock */

      /* Enable EVT, BUF and ERR interrupt */
      __HAL_I2C_ENABLE_IT(m_i2c, I2C_IT_EVT | I2C_IT_BUF | I2C_IT_ERR);

      /* Enable Acknowledge */
      SET_BIT(m_i2c->Instance->CR1, I2C_CR1_ACK);

      /* Generate Start */
      SET_BIT(m_i2c->Instance->CR1, I2C_CR1_START);
    }

    return true;
  }

  static void tx_dma_cplt_cb(DMA_HandleTypeDef *hdma) {
    if (!hdma)
      return;
    auto *self = static_cast<dma_i2c *>(hdma->Parent);
    if (!self)
      return;
    if (self->m_i2c) {
      __HAL_I2C_DISABLE_IT(self->m_i2c, I2C_IT_EVT | I2C_IT_ERR | I2C_IT_BUF);
      CLEAR_BIT(self->m_i2c->Instance->CR2, I2C_CR2_DMAEN);
      self->m_i2c->State = HAL_I2C_STATE_READY;
      self->m_i2c->Mode = HAL_I2C_MODE_NONE;
    }
    if (self->m_tx_dma)
      self->m_tx_dma->call_dma_callback({});
  }

  static void tx_dma_error_cb(DMA_HandleTypeDef *hdma) {
    if (!hdma)
      return;
    auto *self = static_cast<dma_i2c *>(hdma->Parent);
    if (!self)
      return;
    if (self->m_i2c) {
      __HAL_I2C_DISABLE_IT(self->m_i2c, I2C_IT_EVT | I2C_IT_ERR | I2C_IT_BUF);
      CLEAR_BIT(self->m_i2c->Instance->CR2, I2C_CR2_DMAEN);
      self->m_i2c->ErrorCode |= HAL_I2C_ERROR_DMA;
      self->m_i2c->State = HAL_I2C_STATE_READY;
      self->m_i2c->Mode = HAL_I2C_MODE_NONE;
    }
    if (self->m_tx_dma)
      self->m_tx_dma->call_dma_callback(
          std::error_code(hdma->ErrorCode, dma_error_category::instance()));
  }

  static void rx_dma_cplt_cb(DMA_HandleTypeDef *hdma) {
    if (!hdma)
      return;
    auto *self = static_cast<dma_i2c *>(hdma->Parent);
    if (!self)
      return;
    if (self->m_i2c) {
      __HAL_I2C_DISABLE_IT(self->m_i2c, I2C_IT_EVT | I2C_IT_ERR | I2C_IT_BUF);
      CLEAR_BIT(self->m_i2c->Instance->CR2, I2C_CR2_DMAEN);
      self->m_i2c->State = HAL_I2C_STATE_READY;
      self->m_i2c->Mode = HAL_I2C_MODE_NONE;
    }
    if (self->m_rx_dma)
      self->m_rx_dma->call_dma_callback({});
  }

  static void rx_dma_error_cb(DMA_HandleTypeDef *hdma) {
    if (!hdma)
      return;
    auto *self = static_cast<dma_i2c *>(hdma->Parent);
    if (!self)
      return;
    if (self->m_i2c) {
      __HAL_I2C_DISABLE_IT(self->m_i2c, I2C_IT_EVT | I2C_IT_ERR | I2C_IT_BUF);
      CLEAR_BIT(self->m_i2c->Instance->CR2, I2C_CR2_DMAEN);
      self->m_i2c->ErrorCode |= HAL_I2C_ERROR_DMA;
      self->m_i2c->State = HAL_I2C_STATE_READY;
      self->m_i2c->Mode = HAL_I2C_MODE_NONE;
    }
    if (self->m_rx_dma)
      self->m_rx_dma->call_dma_callback(
          std::error_code(hdma->ErrorCode, dma_error_category::instance()));
  }

  I2C_HandleTypeDef *m_i2c{nullptr};
  dma_proxy *m_tx_dma{nullptr};
  dma_proxy *m_rx_dma{nullptr};
};

} // namespace gdut

#endif // BSP_DMA_HPP
