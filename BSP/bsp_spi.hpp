#ifndef BSP_SPI_HPP
#define BSP_SPI_HPP

#include "bsp_dma.hpp"
#include "bsp_type_traits.hpp"
#include "bsp_uncopyable.hpp"
#include "cmsis_os2.h"
#include "stm32f4xx_hal.h"
#include "stm32f4xx_hal_spi.h"
#include <chrono>
#include <cstdint>

namespace gdut {

/**
 * @brief 针对 SPI 外设的 DMA 操作封装
 *
 * 通过 CRTP 继承 dma_transfer_base，将 TX/RX dma_proxy 与 SPI HAL 句柄关联，
 * 并手动配置 DMA 发起传输。传输完成或出错时，由内部静态回调恢复 SPI HAL 状态
 * 后再通过 dma_proxy::call_dma_callback() 转发用户回调。
 *
 * @note address 参数对 SPI 无意义，所有传输均忽略该参数。
 * @note 不直接调用 HAL_SPI_Transmit_DMA / HAL_SPI_TransmitReceive_DMA，
 *       以防止 HAL 内部覆盖 dma_proxy 已注册的回调函数。
 */
class dma_spi : public dma_transfer_base<dma_spi> {
public:
  explicit dma_spi(SPI_HandleTypeDef *m_spi) : m_spi(m_spi) {}

  ~dma_spi() = default;

  dma_spi(dma_spi &&other) noexcept
      : m_spi(std::exchange(other.m_spi, nullptr)),
        m_tx_dma(std::exchange(other.m_tx_dma, nullptr)),
        m_rx_dma(std::exchange(other.m_rx_dma, nullptr)) {}

  dma_spi &operator=(dma_spi &&other) noexcept {
    if (this != std::addressof(other)) {
      m_spi = std::exchange(other.m_spi, nullptr);
      m_tx_dma = std::exchange(other.m_tx_dma, nullptr);
      m_rx_dma = std::exchange(other.m_rx_dma, nullptr);
    }
    return *this;
  }

private:
  friend class dma_transfer_base<dma_spi>;

  void do_bind_tx(dma_proxy *tx_dma) {
    if (tx_dma && m_spi) {
      m_tx_dma = tx_dma;
      __HAL_LINKDMA(m_spi, hdmatx, *tx_dma->get_handle());
    }
  }

  void do_bind_rx(dma_proxy *rx_dma) {
    if (rx_dma && m_spi) {
      m_rx_dma = rx_dma;
      __HAL_LINKDMA(m_spi, hdmarx, *rx_dma->get_handle());
    }
  }

  bool do_transmit(const uint8_t *data, std::size_t size, uint16_t address) {
    (void)address; // SPI 不使用地址参数，忽略以避免编译器警告

    // 传输大小超过 HAL uint16_t 范围则拒绝，避免截断导致错误传输
    if (m_spi == nullptr || m_tx_dma == nullptr || data == nullptr ||
        size == 0U || size > 65535U) {
      return false;
    }

    if (!IS_SPI_DMA_HANDLE(m_spi->hdmatx) ||
        !IS_SPI_DIRECTION_2LINES_OR_1LINE(m_spi->Init.Direction)) {
      std::
          terminate(); // 这属于严重的配置错误，无法通过返回值优雅处理，直接终止程序以引起注意
    }

    if (m_spi->State != HAL_SPI_STATE_READY) {
      return false;
    }

    // 初始化 DMA 代理（调用 HAL_DMA_Init 初始化 DMA 句柄）
    m_tx_dma->init();

    // 覆盖 DMA 句柄的 Parent 与完成/错误回调，以便在 DMA 中断里恢复 SPI 状态
    // 并通过 dma_proxy::call_dma_callback() 转发到用户回调。
    // 注意：不能直接调用 HAL_SPI_Transmit_DMA，否则 HAL 会重新覆盖这些回调。
    DMA_HandleTypeDef *hdma_tx = m_tx_dma->get_handle();
    if (hdma_tx == nullptr) {
      return false;
    }
    hdma_tx->Parent = this;
    hdma_tx->XferCpltCallback = tx_dma_cplt_cb;
    hdma_tx->XferErrorCallback = tx_dma_error_cb;

    /* Process Locked */
    __HAL_LOCK(m_spi);

    // 参考 HAL_SPI_Transmit_DMA 内部实现，手动配置 SPI 状态与 DMA
    m_spi->State = HAL_SPI_STATE_BUSY_TX;
    m_spi->ErrorCode = HAL_SPI_ERROR_NONE;
    m_spi->pTxBuffPtr = const_cast<uint8_t *>(data);
    m_spi->TxXferSize = static_cast<uint16_t>(size);
    m_spi->TxXferCount = static_cast<uint16_t>(size);
    m_spi->pRxBuffPtr = nullptr;
    m_spi->TxISR = nullptr;
    m_spi->RxISR = nullptr;
    m_spi->RxXferSize = 0U;
    m_spi->RxXferCount = 0U;

    if (m_spi->Init.Direction == SPI_DIRECTION_1LINE) {
      __HAL_SPI_DISABLE(m_spi);
      SPI_1LINE_TX(m_spi);
    }

#if (USE_SPI_CRC != 0U)
    if (m_spi->Init.CRCCalculation == SPI_CRCCALCULATION_ENABLE) {
      SPI_RESET_CRC(m_spi);
    }
#endif /* USE_SPI_CRC */

    const auto src_addr =
        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(m_spi->pTxBuffPtr));
    const auto dst_addr = static_cast<uint32_t>(
        reinterpret_cast<uintptr_t>(&m_spi->Instance->DR));
    if (HAL_OK !=
        HAL_DMA_Start_IT(hdma_tx, src_addr, dst_addr, m_spi->TxXferCount)) {
      SET_BIT(m_spi->ErrorCode, HAL_SPI_ERROR_DMA);
      m_spi->State = HAL_SPI_STATE_READY;
      m_spi->TxXferCount = 0U;
      m_spi->TxXferSize = 0U;
      m_spi->pTxBuffPtr = nullptr;
      __HAL_UNLOCK(m_spi);
      return false;
    }

    if ((m_spi->Instance->CR1 & SPI_CR1_SPE) != SPI_CR1_SPE) {
      __HAL_SPI_ENABLE(m_spi);
    }

    __HAL_UNLOCK(m_spi);
    __HAL_SPI_ENABLE_IT(m_spi, (SPI_IT_ERR));
    SET_BIT(m_spi->Instance->CR2, SPI_CR2_TXDMAEN);
    return true;
  }

  bool do_receive(uint8_t *buffer, std::size_t size, uint16_t address) {
    (void)address; // SPI 不使用地址参数，忽略以避免编译器警告

    // 传输大小超过 HAL uint16_t 范围则拒绝，避免截断导致错误传输
    if (m_spi == nullptr || m_rx_dma == nullptr || buffer == nullptr ||
        size == 0U || size > 65535U) {
      return false;
    }

    /* 全双工模式需要同时配置 TX/RX DMA 句柄 */
    if (!IS_SPI_DMA_HANDLE(m_spi->hdmarx) ||
        !IS_SPI_DMA_HANDLE(m_spi->hdmatx) ||
        !IS_SPI_DIRECTION_2LINES(m_spi->Init.Direction)) {
      std::
          terminate(); // 这属于严重的配置错误，无法通过返回值优雅处理，直接终止程序以引起注意
    }

    HAL_SPI_StateTypeDef tmp_state = m_spi->State;
    uint32_t tmp_mode = m_spi->Init.Mode;
    if (!((tmp_state == HAL_SPI_STATE_READY) ||
          ((tmp_mode == SPI_MODE_MASTER) &&
           (m_spi->Init.Direction == SPI_DIRECTION_2LINES) &&
           (tmp_state == HAL_SPI_STATE_BUSY_RX)))) {
      return false;
    }

    // 初始化 RX DMA 代理（调用 HAL_DMA_Init 初始化 DMA 句柄）
    m_rx_dma->init();

    // 覆盖 RX DMA 句柄的 Parent 与完成/错误回调，以便在 DMA 中断里恢复 SPI 状态
    // 并通过 dma_proxy::call_dma_callback() 转发到用户回调。
    // 注意：不能直接调用 HAL_SPI_TransmitReceive_DMA，否则 HAL
    // 会重新覆盖这些回调。
    DMA_HandleTypeDef *hdma_rx = m_rx_dma->get_handle();
    if (hdma_rx == nullptr) {
      return false;
    }
    hdma_rx->Parent = this;
    hdma_rx->XferCpltCallback = rx_dma_cplt_cb;
    hdma_rx->XferErrorCallback = rx_dma_error_cb;

    /* Process locked */
    __HAL_LOCK(m_spi);

    if (m_spi->State != HAL_SPI_STATE_BUSY_RX) {
      m_spi->State = HAL_SPI_STATE_BUSY_TX_RX;
    }

    // 参考 HAL_SPI_TransmitReceive_DMA 内部实现，手动配置 SPI 状态与 DMA
    m_spi->ErrorCode = HAL_SPI_ERROR_NONE;
    m_spi->pTxBuffPtr = buffer;
    m_spi->TxXferSize = static_cast<uint16_t>(size);
    m_spi->TxXferCount = static_cast<uint16_t>(size);
    m_spi->pRxBuffPtr = buffer;
    m_spi->RxXferSize = static_cast<uint16_t>(size);
    m_spi->RxXferCount = static_cast<uint16_t>(size);
    m_spi->RxISR = nullptr;
    m_spi->TxISR = nullptr;

#if (USE_SPI_CRC != 0U)
    if (m_spi->Init.CRCCalculation == SPI_CRCCALCULATION_ENABLE) {
      SPI_RESET_CRC(m_spi);
    }
#endif /* USE_SPI_CRC */

    const auto rx_src_addr = static_cast<uint32_t>(
        reinterpret_cast<uintptr_t>(&m_spi->Instance->DR));
    const auto rx_dst_addr =
        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(m_spi->pRxBuffPtr));
    if (HAL_OK != HAL_DMA_Start_IT(hdma_rx, rx_src_addr, rx_dst_addr,
                                   m_spi->RxXferCount)) {
      SET_BIT(m_spi->ErrorCode, HAL_SPI_ERROR_DMA);
      m_spi->State = HAL_SPI_STATE_READY;
      __HAL_UNLOCK(m_spi);
      return false;
    }
    SET_BIT(m_spi->Instance->CR2, SPI_CR2_RXDMAEN);

    // TX DMA 完成不单独处理，全双工时由 RX DMA 完成回调统一执行状态清理；
    // TX DMA 完成/半完成回调置 nullptr，避免意外触发旧回调；
    // 但保留 TX DMA 的错误回调（与 RX 相同），确保 TX
    // 错误时能清理状态并通知用户
    m_spi->hdmatx->Parent = this;
    m_spi->hdmatx->XferHalfCpltCallback = nullptr;
    m_spi->hdmatx->XferCpltCallback = nullptr;
    m_spi->hdmatx->XferErrorCallback = rx_dma_error_cb;
    m_spi->hdmatx->XferAbortCallback = nullptr;

    const auto tx_src_addr =
        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(m_spi->pTxBuffPtr));
    const auto tx_dst_addr = static_cast<uint32_t>(
        reinterpret_cast<uintptr_t>(&m_spi->Instance->DR));
    if (HAL_OK != HAL_DMA_Start_IT(m_spi->hdmatx, tx_src_addr, tx_dst_addr,
                                   m_spi->TxXferCount)) {
      // TX DMA 启动失败，回滚已启动的 RX DMA 以避免外设处于不一致状态
      (void)HAL_DMA_Abort(hdma_rx);
      CLEAR_BIT(m_spi->Instance->CR2, SPI_CR2_RXDMAEN);
      SET_BIT(m_spi->ErrorCode, HAL_SPI_ERROR_DMA);
      m_spi->State = HAL_SPI_STATE_READY;
      __HAL_UNLOCK(m_spi);
      return false;
    }

    if ((m_spi->Instance->CR1 & SPI_CR1_SPE) != SPI_CR1_SPE) {
      __HAL_SPI_ENABLE(m_spi);
    }
    __HAL_UNLOCK(m_spi);
    __HAL_SPI_ENABLE_IT(m_spi, (SPI_IT_ERR));
    SET_BIT(m_spi->Instance->CR2, SPI_CR2_TXDMAEN);
    return true;
  }

  // TX DMA 传输完成回调：清除 TXDMAEN 位、恢复 SPI State，然后转发用户回调
  static void tx_dma_cplt_cb(DMA_HandleTypeDef *hdma) {
    if (!hdma)
      return;
    auto *self = static_cast<dma_spi *>(hdma->Parent);
    if (!self || !self->m_spi)
      return;
    ATOMIC_CLEAR_BIT(self->m_spi->Instance->CR2, SPI_CR2_TXDMAEN);
    self->m_spi->State = HAL_SPI_STATE_READY;
    if (self->m_tx_dma)
      self->m_tx_dma->call_dma_callback({});
  }

  // TX DMA 错误回调：清除 TXDMAEN 位、恢复 SPI State，然后上报错误
  static void tx_dma_error_cb(DMA_HandleTypeDef *hdma) {
    if (!hdma)
      return;
    auto *self = static_cast<dma_spi *>(hdma->Parent);
    if (!self || !self->m_spi)
      return;
    ATOMIC_CLEAR_BIT(self->m_spi->Instance->CR2, SPI_CR2_TXDMAEN);
    SET_BIT(self->m_spi->ErrorCode, HAL_SPI_ERROR_DMA);
    self->m_spi->State = HAL_SPI_STATE_READY;
    if (self->m_tx_dma)
      self->m_tx_dma->call_dma_callback(
          std::error_code(hdma->ErrorCode, dma_error_category::instance()));
  }

  // RX DMA 传输完成回调（全双工）：清除 TXDMAEN/RXDMAEN 位、恢复 SPI
  // State，然后转发用户回调
  static void rx_dma_cplt_cb(DMA_HandleTypeDef *hdma) {
    if (!hdma)
      return;
    auto *self = static_cast<dma_spi *>(hdma->Parent);
    if (!self || !self->m_spi)
      return;
    ATOMIC_CLEAR_BIT(self->m_spi->Instance->CR2,
                     SPI_CR2_TXDMAEN | SPI_CR2_RXDMAEN);
    self->m_spi->State = HAL_SPI_STATE_READY;
    if (self->m_rx_dma)
      self->m_rx_dma->call_dma_callback({});
  }

  // RX DMA 错误回调（全双工）：清除 TXDMAEN/RXDMAEN 位、恢复 SPI
  // State，然后上报错误
  static void rx_dma_error_cb(DMA_HandleTypeDef *hdma) {
    if (!hdma)
      return;
    auto *self = static_cast<dma_spi *>(hdma->Parent);
    if (!self || !self->m_spi)
      return;
    ATOMIC_CLEAR_BIT(self->m_spi->Instance->CR2,
                     SPI_CR2_TXDMAEN | SPI_CR2_RXDMAEN);
    SET_BIT(self->m_spi->ErrorCode, HAL_SPI_ERROR_DMA);
    self->m_spi->State = HAL_SPI_STATE_READY;
    if (self->m_rx_dma)
      self->m_rx_dma->call_dma_callback(
          std::error_code(hdma->ErrorCode, dma_error_category::instance()));
  }

  SPI_HandleTypeDef *m_spi{nullptr};
  dma_proxy *m_tx_dma{nullptr};
  dma_proxy *m_rx_dma{nullptr};
};

// SPI 代理类：封装 HAL SPI 阻塞式传输/接收操作，支持 C++20 chrono 超时
class spi_proxy : private uncopyable {
public:
  spi_proxy(SPI_HandleTypeDef *hspi) : m_hspi(hspi) {}

  ~spi_proxy() = default;

  void init() {
    if (!m_hspi) {
      return;
    }
    HAL_SPI_Init(m_hspi);
  }

  void deinit() {
    if (!m_hspi) {
      return;
    }
    HAL_SPI_DeInit(m_hspi);
  }

  void set_mode(spi_mode mode) {
    if (!m_hspi) {
      return;
    }
    m_hspi->Init.Mode = std::to_underlying(mode);
  }
  void set_direction(spi_direction direction) {
    if (!m_hspi) {
      return;
    }
    m_hspi->Init.Direction = std::to_underlying(direction);
  }
  void set_data_size(spi_data_size data_size) {
    if (!m_hspi) {
      return;
    }
    m_hspi->Init.DataSize = std::to_underlying(data_size);
  }
  void set_clock_polarity(spi_clock_polarity polarity) {
    if (!m_hspi) {
      return;
    }
    m_hspi->Init.CLKPolarity = std::to_underlying(polarity);
  }
  void set_clock_phase(spi_clock_phase phase) {
    if (!m_hspi) {
      return;
    }
    m_hspi->Init.CLKPhase = std::to_underlying(phase);
  }
  void set_nss(spi_nss nss) {
    if (!m_hspi) {
      return;
    }
    m_hspi->Init.NSS = std::to_underlying(nss);
  }
  void set_baud_rate_prescaler(spi_baud_rate_prescaler prescaler) {
    if (!m_hspi) {
      return;
    }
    m_hspi->Init.BaudRatePrescaler = std::to_underlying(prescaler);
  }
  void set_first_bit(spi_first_bit first_bit) {
    if (!m_hspi) {
      return;
    }
    m_hspi->Init.FirstBit = std::to_underlying(first_bit);
  }
  void set_timode(spi_ti_mode ti_mode) {
    if (!m_hspi) {
      return;
    }
    m_hspi->Init.TIMode = std::to_underlying(ti_mode);
  }
  void set_crc_calculation(spi_crc_calculation crc_calculation) {
    if (!m_hspi) {
      return;
    }
    m_hspi->Init.CRCCalculation = std::to_underlying(crc_calculation);
  }
  void set_crc_polynomial(uint32_t crc_polynomial) {
    if (!m_hspi) {
      return;
    }
    m_hspi->Init.CRCPolynomial = crc_polynomial;
  }

  // SPI 发送数据（阻塞模式）
  // 参数：begin - 发送数据指针，size - 字节数，timeout - 超时时间（默认最大值）
  bool transmit(
      const uint8_t *begin, uint16_t size,
      std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) {
    if (m_hspi == nullptr || begin == nullptr || size == 0) {
      return false; // 参数非法
    }
    auto timeout_ms = timeout.count();
    // HAL_SPI_Transmit 要求 uint8_t* 而非 const uint8_t*，需要 const_cast
    return HAL_SPI_Transmit(m_hspi, const_cast<uint8_t *>(begin), size,
                            timeout_ms >= osWaitForever
                                ? osWaitForever
                                : static_cast<uint32_t>(timeout_ms)) == HAL_OK;
  }

  // SPI 接收数据（阻塞模式）
  // 参数：data - 接收缓冲区，size - 字节数，timeout - 超时时间（默认最大值）
  bool receive(
      uint8_t *data, uint16_t size,
      std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) {
    if (m_hspi == nullptr || data == nullptr || size == 0) {
      return false; // 参数非法
    }
    auto timeout_ms = timeout.count();
    return HAL_SPI_Receive(m_hspi, data, size,
                           timeout_ms >= osWaitForever
                               ? osWaitForever
                               : static_cast<uint32_t>(timeout_ms)) == HAL_OK;
  }

  // SPI 全双工传输（同时发送和接收，阻塞模式）
  // 参数：tx_data - 发送数据，rx_data - 接收缓冲区，size - 字节数，timeout -
  // 超时
  bool transmit_receive(
      const uint8_t *tx_data, uint8_t *rx_data, uint16_t size,
      std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) {
    if (m_hspi == nullptr || tx_data == nullptr || rx_data == nullptr ||
        size == 0) {
      return false; // 参数非法
    }
    auto timeout_ms = timeout.count();
    // HAL_SPI_TransmitReceive 要求 uint8_t* 而非 const uint8_t*，需要
    // const_cast
    return HAL_SPI_TransmitReceive(
               m_hspi, const_cast<uint8_t *>(tx_data), rx_data, size,
               timeout_ms >= osWaitForever
                   ? osWaitForever
                   : static_cast<uint32_t>(timeout_ms)) == HAL_OK;
  }

  SPI_HandleTypeDef *get_handle() const { return m_hspi; }

private:
  SPI_HandleTypeDef *m_hspi{nullptr}; // SPI 外设句柄引用
};

} // namespace gdut
#endif // BSP_SPI_HPP
