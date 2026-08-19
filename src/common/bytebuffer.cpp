#include <common/bytebuffer.h>

#include <stdexcept>

namespace kv {

ByteBuffer::ByteBuffer(std::size_t max_size) : max_size_(max_size) {
  if (max_size_ == 0) {
    throw std::invalid_argument("byte buffer limit must be positive");
  }
}

void ByteBuffer::append(const std::uint8_t *bytes, std::size_t size) {
  if (size == 0) {
    return;
  }
  if (bytes == nullptr) {
    throw std::invalid_argument("cannot append a null byte range");
  }
  if (size > max_size_ - readable_bytes()) {
    throw std::length_error("byte buffer limit exceeded");
  }

  compact();
  bytes_.insert(bytes_.end(), bytes, bytes + size);
}

void ByteBuffer::append(const void *bytes, std::size_t size) {
  append(static_cast<const std::uint8_t *>(bytes), size);
}

void ByteBuffer::consume(std::size_t size) {
  if (size > readable_bytes()) {
    throw std::out_of_range("cannot consume beyond readable bytes");
  }

  read_offset_ += size;
  if (read_offset_ == bytes_.size()) {
    clear();
  }
}

void ByteBuffer::clear() noexcept {
  bytes_.clear();
  read_offset_ = 0;
}

const std::uint8_t *ByteBuffer::data() const noexcept {
  return empty() ? nullptr : bytes_.data() + read_offset_;
}

std::uint8_t *ByteBuffer::data() noexcept {
  return empty() ? nullptr : bytes_.data() + read_offset_;
}

std::size_t ByteBuffer::readable_bytes() const noexcept {
  return bytes_.size() - read_offset_;
}

void ByteBuffer::compact() {
  if (read_offset_ == 0) {
    return;
  }

  bytes_.erase(bytes_.begin(), bytes_.begin() +
                                   static_cast<std::ptrdiff_t>(read_offset_));
  read_offset_ = 0;
}

} // namespace kv
