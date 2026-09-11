#include <server/resp_codec.h>

#include <charconv>
#include <optional>
#include <utility>

namespace kv::resp {
namespace {

struct ParsedCommand {
  RespCommand command;
  std::size_t consumed = 0;
};

std::optional<std::string_view> read_line(const std::string& input,
                                          std::size_t& cursor) {
  const auto end = input.find("\r\n", cursor);
  if (end == std::string::npos) {
    return std::nullopt;
  }

  const std::string_view line(input.data() + cursor, end - cursor);
  cursor = end + 2U;
  return line;
}

std::size_t parse_size(std::string_view value, const char* field) {
  if (value.empty()) {
    throw RespProtocolError(std::string("empty ") + field);
  }

  std::size_t result = 0;
  const auto parsed =
      std::from_chars(value.data(), value.data() + value.size(), result);
  if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
    throw RespProtocolError(std::string("invalid ") + field);
  }
  return result;
}

std::optional<ParsedCommand> parse_command(const std::string& input,
                                           const RespLimits& limits) {
  std::size_t cursor = 0;
  if (input.empty()) {
    return std::nullopt;
  }
  if (input[cursor++] != '*') {
    throw RespProtocolError("expected RESP array");
  }

  const auto count_line = read_line(input, cursor);
  if (!count_line) {
    return std::nullopt;
  }
  const auto count = parse_size(*count_line, "array length");
  if (count > limits.max_array_elements) {
    throw RespProtocolError("array length exceeds limit");
  }

  RespCommand command;
  command.arguments.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    if (cursor == input.size()) {
      return std::nullopt;
    }
    if (input[cursor++] != '$') {
      throw RespProtocolError("expected bulk string");
    }

    const auto length_line = read_line(input, cursor);
    if (!length_line) {
      return std::nullopt;
    }
    const auto length = parse_size(*length_line, "bulk string length");
    if (length > limits.max_bulk_string_bytes) {
      throw RespProtocolError("bulk string length exceeds limit");
    }
    if (length > input.size() - cursor) {
      return std::nullopt;
    }
    if (input.size() - cursor - length < 2U) {
      return std::nullopt;
    }

    command.arguments.emplace_back(input.data() + cursor, length);
    cursor += length;
    if (input[cursor] != '\r' || input[cursor + 1U] != '\n') {
      throw RespProtocolError("bulk string missing CRLF");
    }
    cursor += 2U;
  }

  return ParsedCommand{std::move(command), cursor};
}

}  // namespace

RespDecoder::RespDecoder(RespLimits limits) : limits_(limits) {}

std::vector<RespCommand> RespDecoder::feed(std::string_view bytes) {
  if (bytes.size() > limits_.max_buffer_bytes - buffer_.size()) {
    throw RespProtocolError("input buffer exceeds limit");
  }
  if (!bytes.empty()) {
    buffer_.append(bytes.data(), bytes.size());
  }

  std::vector<RespCommand> commands;
  while (true) {
    auto parsed = parse_command(buffer_, limits_);
    if (!parsed) {
      break;
    }

    commands.push_back(std::move(parsed->command));
    buffer_.erase(0, parsed->consumed);
  }
  return commands;
}

std::size_t RespDecoder::buffered_bytes() const noexcept {
  return buffer_.size();
}

std::string encode_simple_string(std::string_view value) {
  if (value.find_first_of("\r\n") != std::string_view::npos) {
    throw RespProtocolError("simple string contains CR or LF");
  }

  std::string reply;
  reply.reserve(value.size() + 3U);
  reply.push_back('+');
  if (!value.empty()) {
    reply.append(value.data(), value.size());
  }
  reply.append("\r\n");
  return reply;
}

std::string encode_bulk_string(std::string_view value) {
  const auto length = std::to_string(value.size());
  std::string reply;
  reply.reserve(1U + length.size() + 2U + value.size() + 2U);
  reply.push_back('$');
  reply.append(length);
  reply.append("\r\n");
  if (!value.empty()) {
    reply.append(value.data(), value.size());
  }
  reply.append("\r\n");
  return reply;
}

std::string encode_nil_bulk_string() { return "$-1\r\n"; }

std::string encode_integer(std::int64_t value) {
  return ":" + std::to_string(value) + "\r\n";
}

std::string encode_error(std::string_view value) {
  if (value.find_first_of("\r\n") != std::string_view::npos) {
    throw RespProtocolError("error contains CR or LF");
  }

  std::string reply;
  reply.reserve(value.size() + 3U);
  reply.push_back('-');
  if (!value.empty()) {
    reply.append(value.data(), value.size());
  }
  reply.append("\r\n");
  return reply;
}

}  // namespace kv::resp
