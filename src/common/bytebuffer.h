#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace kv {

// A bounded byte accumulator. Consumed bytes are never exposed again, and
// appends are rejected before the configured readable-byte limit is exceeded.
class ByteBuffer {
public:
  static constexpr std::size_t kDefaultLimit =
      64U * 1024U * 1024U + 5U;

  explicit ByteBuffer(std::size_t max_size = kDefaultLimit);

  void append(const std::uint8_t *bytes, std::size_t size);
  void append(const void *bytes, std::size_t size);
  void consume(std::size_t size);
  void clear() noexcept;

  const std::uint8_t *data() const noexcept;
  std::uint8_t *data() noexcept;
  std::size_t readable_bytes() const noexcept;
  std::size_t size() const noexcept { return readable_bytes(); }
  bool empty() const noexcept { return readable_bytes() == 0; }
  std::size_t max_size() const noexcept { return max_size_; }

  // Compatibility names used by the original public transport boundary.
  void put(const std::uint8_t *bytes, std::size_t size) { append(bytes, size); }
  std::size_t readable() const noexcept { return readable_bytes(); }
  const std::uint8_t *reader() const noexcept { return data(); }
  std::uint8_t *reader() noexcept { return data(); }
  void read_bytes(std::size_t size) { consume(size); }

private:
  void compact();

  std::vector<std::uint8_t> bytes_;
  std::size_t read_offset_ = 0;
  std::size_t max_size_;
};

} // namespace kv
