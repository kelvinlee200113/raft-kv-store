#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace kv::resp {

struct RespCommand {
  std::vector<std::string> arguments;
};

struct RespLimits {
  std::size_t max_buffer_bytes = 2U * 1024U * 1024U;
  std::size_t max_array_elements = 1024U;
  std::size_t max_bulk_string_bytes = 1024U * 1024U;
};

class RespProtocolError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

class RespDecoder {
 public:
  explicit RespDecoder(RespLimits limits = {});

  std::vector<RespCommand> feed(std::string_view bytes);
  std::size_t buffered_bytes() const noexcept;

 private:
  RespLimits limits_;
  std::string buffer_;
};

std::string encode_simple_string(std::string_view value);
std::string encode_bulk_string(std::string_view value);
std::string encode_nil_bulk_string();
std::string encode_integer(std::int64_t value);
std::string encode_error(std::string_view value);

}  // namespace kv::resp
