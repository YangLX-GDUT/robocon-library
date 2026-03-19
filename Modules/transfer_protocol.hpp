#ifndef MODULES_TRANSFER_PROTOCOL_HPP
#define MODULES_TRANSFER_PROTOCOL_HPP

#include "memory_resource.hpp"
#include "verification_algorithm.hpp"
#include "function.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <memory_resource>
#include <type_traits>
#include <vector>

namespace gdut {

struct build_packet_t {};
inline constexpr build_packet_t build_packet;

struct from_whole_packet_t {};
inline constexpr from_whole_packet_t from_whole_packet;

template<typename VerifyAlgorithm>
class data_packet {
  static_assert(std::is_base_of_v<verify_algorithm<VerifyAlgorithm>, VerifyAlgorithm>, "VerifyAlgorithm must be derived class of verify_algorithm<VerifyAlgorithm>");
public:
  using verify_algorithm_t = VerifyAlgorithm;
  static constexpr std::uint8_t header = 0xAA;
  static constexpr std::size_t header_size = sizeof(uint8_t) + sizeof(uint16_t) * 3; // header + size + code + crc

  static std::pmr::memory_resource *default_memory_resource() noexcept {
    return gdut::pmr::portable_resource::get_instance();
  }

  data_packet(std::pmr::memory_resource* mr = default_memory_resource()) : m_data(mr) {}

  template<std::input_iterator It>
  data_packet(uint16_t code, It begin, It end, build_packet_t,
      std::pmr::memory_resource* mr = default_memory_resource()) : m_data(mr) {
    static_assert(sizeof(*It{}) == 1, "The data size of the iterator must be 1");
    if (std::distance(begin, end) >
      static_cast<decltype(std::distance(begin, end))>(
        std::numeric_limits<uint16_t>::max() - sizeof(uint16_t) * 3)) {
      return; // Body size is too large to fit in a packet
    }
    // size (2 bytes) + code (2 bytes) + crc (2 bytes) + body
    const uint16_t total_size = static_cast<uint16_t>(header_size) + static_cast<uint16_t>(std::distance(begin, end));
    m_data.resize(total_size);
    m_data[0] = header;
    m_data[1] = (total_size >> 8) & 0xFF;
    m_data[2] = total_size & 0xFF;
    m_data[3] = (code >> 8) & 0xFF;
    m_data[4] = code & 0xFF;
    std::copy(begin, end, m_data.begin() + header_size);
    uint16_t crc = calculate_verification();
    m_data[5] = (crc >> 8) & 0xFF;
    m_data[6] = crc & 0xFF;
  }

  template<std::input_iterator It>
  data_packet(It begin, It end, from_whole_packet_t,
      std::pmr::memory_resource* mr = default_memory_resource()) : m_data(mr) {
    if (begin == end || *begin != header) {
      return; // Invalid packet header
    }
    if (std::distance(begin, end) < header_size) {
      return;
    }
    uint16_t size = begin[1] << 8 | begin[2];
    if (std::distance(begin, end) < size) {
      return;
    }
    m_data.resize(std::distance(begin, end));
    std::copy(begin, end, m_data.begin());
    if (!verify_verification()) {
      m_data.clear();
      return;
    }
  }

  ~data_packet() = default;

  data_packet(const data_packet& packet, std::pmr::memory_resource* mr = default_memory_resource()) : m_data(packet.m_data, mr) {}
  data_packet(data_packet&& packet) noexcept : m_data(std::move(packet.m_data)) {}

  data_packet& operator=(const data_packet& packet) {
    if (this != std::addressof(packet)) {
      m_data = packet.m_data;
    }
    return *this;
  }

  data_packet& operator=(data_packet&& packet) noexcept {
    if (this != std::addressof(packet)) {
      m_data = std::move(packet.m_data);
    }
    return *this;
  }

  [[nodiscard]] uint16_t size() const noexcept {
    return m_data[1] << 8 | m_data[2];
  }

  [[nodiscard]] uint16_t code() const noexcept {
    return m_data[3] << 8 | m_data[4];
  }

  [[nodiscard]] uint16_t crc() const noexcept {
    return m_data[5] << 8 | m_data[6];
  }

  [[nodiscard]] const uint8_t* data() const noexcept {
    return m_data.data();
  }

  [[nodiscard]] const uint8_t* begin() const noexcept {
    return m_data.data();
  }

  [[nodiscard]] const uint8_t* end() const noexcept {
    return m_data.data() + m_data.size();
  }

  [[nodiscard]] uint16_t body_size() const noexcept {
    return size() - header_size;
  }

  [[nodiscard]] const uint8_t* body_data() const noexcept {
    return m_data.data() + header_size;
  }

  [[nodiscard]] const uint8_t* body_begin() const noexcept {
    return m_data.data() + header_size;
  }

  [[nodiscard]] const uint8_t* body_end() const noexcept {
    return m_data.data() + size();
  }

  [[nodiscard]] uint16_t calculate_verification() const noexcept {
    verify_algorithm_t va;
    return va.calculate(m_data.begin(), m_data.end(), m_data.begin() + sizeof(uint8_t) + sizeof(uint16_t) * 2);
  }

  [[nodiscard]] bool verify_verification() const noexcept {
    verify_algorithm_t va;
    return va.verify(m_data.begin(), m_data.end(), m_data.begin() + sizeof(uint8_t) + sizeof(uint16_t) * 2);
  }

  explicit operator bool() noexcept {
    return !m_data.empty();
  }

private:
  /*
  struct {
    uint8_t header;
    uint16_t size;
    uint16_t code;
    uint16_t verify;
    uint8_t  payload[];
  } header;
  */
  std::pmr::vector<std::uint8_t> m_data;
};

template<typename VerifyAlgorithm>
class packet_manager {
public:
  using packet_t = data_packet<VerifyAlgorithm>;

  packet_manager()
      : m_receive_buffer(gdut::pmr::portable_resource::get_instance()) {}
  ~packet_manager() = default;

  void set_send_function(gdut::function<void(const std::uint8_t*, const std::uint8_t*)> func) {
    m_send_function = std::move(func);
  }

  void set_receive_function(gdut::function<void(packet_t)> func) {
    m_receive_function = std::move(func);
  }

  void send(const packet_t& packet) {
    if (m_send_function) {
      m_send_function(packet.data(), packet.data() + packet.size());
    }
  }

  template<std::input_iterator It>
  void receive(It begin, It end) {
    m_receive_buffer.append_range(receive_range<It>{begin, end});
    while (true) {
      auto packet_start = std::find(m_receive_buffer.begin(), m_receive_buffer.end(), packet_t::header);
      if (packet_start == m_receive_buffer.end()) {
        break; // No packet header found, wait for more data
      }
      if (std::distance(packet_start, m_receive_buffer.end()) < static_cast<std::ptrdiff_t>(packet_t::header_size)) {
        break; // Not enough data for header, wait for more data
      }
      uint16_t size = *(packet_start + 1) << 8 | *(packet_start + 2);
      if (std::distance(packet_start, m_receive_buffer.end()) < static_cast<std::ptrdiff_t>(size)) {
        break; // Not enough data for whole packet, wait for more data
      }
      packet_t packet{packet_start, packet_start + size, from_whole_packet};
      if (packet) {
        if (m_receive_function) {
          m_receive_function(std::move(packet));
        }
      }
      m_receive_buffer.erase(m_receive_buffer.begin(), packet_start + size); // Remove processed packet from buffer
    }
  }

protected:
  template<std::input_iterator It>
  struct receive_range {
    It m_begin;
    It m_end;
    It begin() { return m_begin; }
    It end() { return m_end; }
  };

private:
  gdut::function<void(const std::uint8_t*, const std::uint8_t*)> m_send_function;
  gdut::function<void(packet_t)> m_receive_function;
  std::pmr::vector<std::uint8_t> m_receive_buffer;
};

} // namespace gdut

#endif // MODULES_TRANSFER_PROTOCOL_HPP
