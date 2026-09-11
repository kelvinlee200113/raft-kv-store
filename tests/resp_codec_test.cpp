#include <gtest/gtest.h>

#include <server/resp_codec.h>

TEST(RespDecoderTest, DecodesPingRequest) {
  kv::resp::RespDecoder decoder;

  const auto commands = decoder.feed("*1\r\n$4\r\nPING\r\n");

  ASSERT_EQ(commands.size(), 1U);
  EXPECT_EQ(commands[0].arguments,
            (std::vector<std::string>{"PING"}));
  EXPECT_EQ(decoder.buffered_bytes(), 0U);
}

TEST(RespDecoderTest, DecodesEveryCompleteRequestInOneRead) {
  kv::resp::RespDecoder decoder;

  const auto commands = decoder.feed(
      "*3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$5\r\nvalue\r\n"
      "*2\r\n$3\r\nGET\r\n$3\r\nkey\r\n");

  ASSERT_EQ(commands.size(), 2U);
  EXPECT_EQ(commands[0].arguments,
            (std::vector<std::string>{"SET", "key", "value"}));
  EXPECT_EQ(commands[1].arguments,
            (std::vector<std::string>{"GET", "key"}));
  EXPECT_EQ(decoder.buffered_bytes(), 0U);
}

TEST(RespDecoderTest, DecodesBinarySafeRequestArguments) {
  kv::resp::RespDecoder decoder;
  const std::string frame(
      "*3\r\n$3\r\nSET\r\n$3\r\nk\0y\r\n$3\r\nv\0x\r\n", 31);

  const auto commands = decoder.feed(frame);

  ASSERT_EQ(commands.size(), 1U);
  ASSERT_EQ(commands.front().arguments.size(), 3U);
  EXPECT_EQ(commands.front().arguments[0], "SET");
  EXPECT_EQ(commands.front().arguments[1], std::string("k\0y", 3));
  EXPECT_EQ(commands.front().arguments[2], std::string("v\0x", 3));
}

TEST(RespDecoderTest, RetainsAnIncompleteTailUntilTheNextRead) {
  kv::resp::RespDecoder decoder;

  const auto first = decoder.feed(
      "*1\r\n$4\r\nPING\r\n"
      "*2\r\n$3\r\nDEL\r\n$3\r\nke");

  ASSERT_EQ(first.size(), 1U);
  EXPECT_EQ(first[0].arguments,
            (std::vector<std::string>{"PING"}));
  EXPECT_GT(decoder.buffered_bytes(), 0U);

  const auto second = decoder.feed("y\r\n");

  ASSERT_EQ(second.size(), 1U);
  EXPECT_EQ(second[0].arguments,
            (std::vector<std::string>{"DEL", "key"}));
  EXPECT_EQ(decoder.buffered_bytes(), 0U);
}

TEST(RespDecoderTest, DecodesARequestSplitAtEveryByteBoundary) {
  const std::string frame =
      "*3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$5\r\nvalue\r\n";

  for (std::size_t split = 0; split <= frame.size(); ++split) {
    kv::resp::RespDecoder decoder;
    const auto first = decoder.feed(
        std::string_view(frame.data(), split));
    const auto second = decoder.feed(std::string_view(
        frame.data() + split, frame.size() - split));

    ASSERT_EQ(first.size() + second.size(), 1U) << "split=" << split;
    const auto& command = first.empty() ? second[0] : first[0];
    EXPECT_EQ(command.arguments,
              (std::vector<std::string>{"SET", "key", "value"}))
        << "split=" << split;
  }
}

TEST(RespDecoderTest, RejectsInlineCommands) {
  kv::resp::RespDecoder decoder;

  EXPECT_THROW(decoder.feed("PING\r\n"), kv::resp::RespProtocolError);
}

TEST(RespDecoderTest, RejectsRequestsAboveTheArgumentLimit) {
  kv::resp::RespLimits limits;
  limits.max_array_elements = 2U;
  kv::resp::RespDecoder decoder(limits);

  EXPECT_THROW(decoder.feed("*3\r\n"), kv::resp::RespProtocolError);
}

TEST(RespDecoderTest, RejectsBulkStringsAboveThePayloadLimit) {
  kv::resp::RespLimits limits;
  limits.max_bulk_string_bytes = 4U;
  kv::resp::RespDecoder decoder(limits);

  EXPECT_THROW(decoder.feed("*1\r\n$5\r\n"),
               kv::resp::RespProtocolError);
}

TEST(RespDecoderTest, RejectsAnIncompleteFrameThatFillsTheBuffer) {
  kv::resp::RespLimits limits;
  limits.max_buffer_bytes = 4U;
  kv::resp::RespDecoder decoder(limits);

  EXPECT_TRUE(decoder.feed("*1\r\n").empty());
  EXPECT_THROW(decoder.feed("$"), kv::resp::RespProtocolError);
}

TEST(RespEncoderTest, EncodesSimpleStringReply) {
  EXPECT_EQ(kv::resp::encode_simple_string("PONG"), "+PONG\r\n");
}

TEST(RespEncoderTest, EncodesBinarySafeBulkStringReply) {
  EXPECT_EQ(kv::resp::encode_bulk_string(std::string_view("a\0b", 3)),
            std::string("$3\r\na\0b\r\n", 9));
}

TEST(RespEncoderTest, EncodesNilBulkStringReply) {
  EXPECT_EQ(kv::resp::encode_nil_bulk_string(), "$-1\r\n");
}

TEST(RespEncoderTest, EncodesIntegerReply) {
  EXPECT_EQ(kv::resp::encode_integer(2), ":2\r\n");
}

TEST(RespEncoderTest, EncodesErrorReply) {
  EXPECT_EQ(kv::resp::encode_error("ERR unsupported command"),
            "-ERR unsupported command\r\n");
}
