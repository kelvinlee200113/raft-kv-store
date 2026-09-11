#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>

#if defined(_WIN32)
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

namespace kv {

using TransportType = std::uint8_t;
inline constexpr TransportType TransportTypeStream = 1;

struct PackedUint32 {
  PackedUint32() = default;
  PackedUint32(std::uint32_t value) { *this = value; }

  PackedUint32 &operator=(std::uint32_t value) noexcept {
    std::memcpy(bytes.data(), &value, sizeof(value));
    return *this;
  }

  operator std::uint32_t() const noexcept {
    std::uint32_t value = 0;
    std::memcpy(&value, bytes.data(), sizeof(value));
    return value;
  }

  std::array<std::uint8_t, 4> bytes{};
};

static_assert(sizeof(PackedUint32) == 4 && alignof(PackedUint32) == 1,
              "packed integer must have byte alignment");

// Retained as a safe five-byte wire-layout description for compatibility.
// The network path uses the explicit codec below instead of struct casts.
struct TransportMeta {
  TransportType type = TransportTypeStream;
  PackedUint32 len;
};

static_assert(sizeof(TransportMeta) == 5,
              "transport header must be exactly five bytes");

namespace transport {

inline constexpr std::size_t kHeaderSize = 5;
inline constexpr std::uint32_t kMaxPayloadSize = 64U * 1024U * 1024U;

struct Header {
  TransportType type;
  std::uint32_t payload_size;
};

inline std::array<std::uint8_t, kHeaderSize>
encode_header(TransportType type, std::uint32_t payload_size) {
  if (type != TransportTypeStream) {
    throw std::invalid_argument("unsupported transport frame type");
  }
  if (payload_size == 0) {
    throw std::invalid_argument("transport message payload cannot be empty");
  }
  if (payload_size > kMaxPayloadSize) {
    throw std::length_error("transport frame exceeds payload limit");
  }

  return {type, static_cast<std::uint8_t>(payload_size >> 24U),
          static_cast<std::uint8_t>(payload_size >> 16U),
          static_cast<std::uint8_t>(payload_size >> 8U),
          static_cast<std::uint8_t>(payload_size)};
}

inline Header decode_header(const std::uint8_t *bytes, std::size_t size) {
  if (bytes == nullptr || size < kHeaderSize) {
    throw std::invalid_argument("incomplete transport header");
  }
  if (bytes[0] != TransportTypeStream) {
    throw std::invalid_argument("unsupported transport frame type");
  }

  const auto payload_size =
      (static_cast<std::uint32_t>(bytes[1]) << 24U) |
      (static_cast<std::uint32_t>(bytes[2]) << 16U) |
      (static_cast<std::uint32_t>(bytes[3]) << 8U) |
      static_cast<std::uint32_t>(bytes[4]);
  if (payload_size == 0) {
    throw std::invalid_argument("transport message payload cannot be empty");
  }
  if (payload_size > kMaxPayloadSize) {
    throw std::length_error("transport frame exceeds payload limit");
  }

  return Header{bytes[0], payload_size};
}

} // namespace transport
} // namespace kv
