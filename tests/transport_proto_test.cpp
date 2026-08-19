#include <gtest/gtest.h>
#include <common/bytebuffer.h>
#include <transport/proto.h>
#include <array>
#include <cstring>

using namespace kv;

// Test that TransportMeta has correct size (no padding)
TEST(TransportProtoTest, MetaSize) {
  EXPECT_EQ(sizeof(TransportMeta), 5U); // 1 byte type + 4 bytes len
}

// Test that we can create and initialize TransportMeta
TEST(TransportProtoTest, MetaInitialization) {
  TransportMeta meta;
  meta.type = TransportTypeStream;
  meta.len = 100;

  EXPECT_EQ(meta.type, 1);
  EXPECT_EQ(meta.len, 100U);
}

// Test network byte order conversion (htonl/ntohl)
TEST(TransportProtoTest, NetworkByteOrder) {
  TransportMeta meta;
  meta.type = TransportTypeStream;

  uint32_t host_len = 1000;
  meta.len = htonl(host_len);  // Convert to network byte order

  // On little-endian machine, bytes should be reversed
  uint32_t network_len = meta.len;
  uint32_t back_to_host = ntohl(network_len);

  EXPECT_EQ(back_to_host, host_len);
}

// Test that struct packing works (no padding between fields)
TEST(TransportProtoTest, StructPacking) {
  TransportMeta meta;

  // Set values
  meta.type = 0xAB;
  meta.len = 0x12345678;

  // Cast to bytes to verify memory layout
  uint8_t* bytes = reinterpret_cast<uint8_t*>(&meta);

  // First byte should be type
  EXPECT_EQ(bytes[0], 0xAB);

  // Next 4 bytes should be len (in whatever byte order we set)
  // Just verify we can read them back
  TransportMeta* meta_ptr = reinterpret_cast<TransportMeta*>(bytes);
  EXPECT_EQ(meta_ptr->type, 0xAB);
  EXPECT_EQ(meta_ptr->len, 0x12345678U);
}

TEST(TransportProtoTest, HeaderEncodingUsesNetworkByteOrder) {
  const auto bytes =
      transport::encode_header(TransportTypeStream, 0x01020304U);

  const std::array<std::uint8_t, 5> expected = {1, 1, 2, 3, 4};
  EXPECT_EQ(bytes, expected);

  const auto decoded = transport::decode_header(bytes.data(), bytes.size());
  EXPECT_EQ(decoded.type, TransportTypeStream);
  EXPECT_EQ(decoded.payload_size, 0x01020304U);
}

TEST(TransportProtoTest, RejectsEmptyMessageFrame) {
  const std::array<std::uint8_t, 5> empty_frame = {1, 0, 0, 0, 0};
  EXPECT_THROW(
      transport::decode_header(empty_frame.data(), empty_frame.size()),
      std::invalid_argument);
}

TEST(TransportProtoTest, RejectsUnsupportedAndOversizedHeaders) {
  const std::array<std::uint8_t, 5> unsupported = {2, 0, 0, 0, 1};
  EXPECT_THROW(transport::decode_header(unsupported.data(), unsupported.size()),
               std::invalid_argument);

  const auto oversized_length = transport::kMaxPayloadSize + 1U;
  const std::array<std::uint8_t, 5> oversized = {
      TransportTypeStream,
      static_cast<std::uint8_t>(oversized_length >> 24U),
      static_cast<std::uint8_t>(oversized_length >> 16U),
      static_cast<std::uint8_t>(oversized_length >> 8U),
      static_cast<std::uint8_t>(oversized_length)};
  EXPECT_THROW(transport::decode_header(oversized.data(), oversized.size()),
               std::length_error);
  EXPECT_THROW(
      transport::encode_header(TransportTypeStream, oversized_length),
      std::length_error);
}

TEST(ByteBufferTest, AppendsAndConsumesOnlyReadableBytes) {
  ByteBuffer buffer(8);
  const std::array<std::uint8_t, 3> first = {10, 20, 30};
  const std::array<std::uint8_t, 2> second = {40, 50};

  buffer.append(first.data(), first.size());
  buffer.append(second.data(), second.size());
  ASSERT_EQ(buffer.readable_bytes(), 5U);
  EXPECT_EQ(std::vector<std::uint8_t>(buffer.data(), buffer.data() + 5),
            (std::vector<std::uint8_t>{10, 20, 30, 40, 50}));

  buffer.consume(2);
  ASSERT_EQ(buffer.readable_bytes(), 3U);
  EXPECT_EQ(std::vector<std::uint8_t>(buffer.data(), buffer.data() + 3),
            (std::vector<std::uint8_t>{30, 40, 50}));
}

TEST(ByteBufferTest, EnforcesConfiguredReadableByteLimit) {
  ByteBuffer buffer(4);
  const std::array<std::uint8_t, 4> bytes = {1, 2, 3, 4};
  const std::uint8_t extra = 5;

  buffer.append(bytes.data(), bytes.size());
  EXPECT_THROW(buffer.append(&extra, 1), std::length_error);
  EXPECT_THROW(buffer.consume(5), std::out_of_range);

  buffer.consume(4);
  EXPECT_TRUE(buffer.empty());
}
