#include <boost/asio.hpp>
#include <gtest/gtest.h>
#include <raft/proto.h>
#include <transport/peer.h>
#include <transport/proto.h>
#include <transport/server.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <msgpack.hpp>
#include <string>
#include <utility>
#include <vector>

using namespace kv;

namespace {

proto::MessagePtr vote_request(std::uint64_t term = 5) {
  auto message = std::make_shared<proto::Message>();
  message->type = proto::MsgRequestVote;
  message->from = 1;
  message->to = 2;
  message->term = term;
  message->last_log_index = 0;
  message->last_log_term = 0;
  return message;
}

proto::MessagePtr append_request(std::uint64_t leader_commit) {
  auto message = std::make_shared<proto::Message>();
  message->type = proto::MsgAppendEntries;
  message->from = 1;
  message->to = 2;
  message->term = 5;
  message->leader_commit = leader_commit;
  message->read_context = 9;
  proto::Entry entry;
  entry.index = 1;
  entry.term = 5;
  entry.data = {42};
  message->entries.push_back(std::move(entry));
  return message;
}

std::vector<std::uint8_t> encode_frame(const proto::Message &message) {
  msgpack::sbuffer payload;
  msgpack::pack(payload, message);
  const auto header = transport::encode_header(
      TransportTypeStream, static_cast<std::uint32_t>(payload.size()));

  std::vector<std::uint8_t> frame(header.begin(), header.end());
  const auto *payload_begin =
      reinterpret_cast<const std::uint8_t *>(payload.data());
  frame.insert(frame.end(), payload_begin, payload_begin + payload.size());
  return frame;
}

} // namespace

TEST(NetworkTest, DecodesAndDispatchesMessageOverTcp) {
  boost::asio::io_context io_context;
  std::vector<proto::Message> received;

  auto server = Server::create(
      io_context, "127.0.0.1:0",
      [&io_context, &received](proto::Message message) {
        received.push_back(std::move(message));
        io_context.stop();
      });
  server->start();

  const auto address =
      std::string("127.0.0.1:") + std::to_string(server->local_port());
  auto peer = Peer::create(1, address, io_context);
  peer->send(vote_request());

  io_context.run_for(std::chrono::milliseconds(500));

  ASSERT_EQ(received.size(), 1U);
  EXPECT_EQ(received.front().type, proto::MsgRequestVote);
  EXPECT_EQ(received.front().from, 1U);
  EXPECT_EQ(received.front().to, 2U);
  EXPECT_EQ(received.front().term, 5U);

  peer->stop();
  peer->stop();
  server->stop();
  server->stop();
  io_context.restart();
  io_context.run_for(std::chrono::milliseconds(20));
}

TEST(NetworkTest, ReassemblesAFrameSplitAcrossSocketReads) {
  boost::asio::io_context server_io;
  std::vector<proto::Message> received;
  auto server = Server::create(
      server_io, "127.0.0.1:0",
      [&server_io, &received](proto::Message message) {
        received.push_back(std::move(message));
        server_io.stop();
      });
  server->start();

  boost::asio::io_context client_io;
  boost::asio::ip::tcp::socket client(client_io);
  client.connect({boost::asio::ip::make_address("127.0.0.1"),
                  server->local_port()});

  const auto frame = encode_frame(*vote_request(7));
  boost::asio::write(client, boost::asio::buffer(frame.data(), 2U));
  server_io.run_for(std::chrono::milliseconds(20));
  EXPECT_TRUE(received.empty());

  server_io.restart();
  boost::asio::write(
      client,
      boost::asio::buffer(frame.data() + 2U, frame.size() - 2U));
  server_io.run_for(std::chrono::milliseconds(500));

  ASSERT_EQ(received.size(), 1U);
  EXPECT_EQ(received.front().term, 7U);
  server->stop();
  server_io.restart();
  server_io.run_for(std::chrono::milliseconds(20));
}

TEST(NetworkTest, DispatchesCoalescedFramesIndependentlyInOrder) {
  boost::asio::io_context server_io;
  std::vector<proto::Message> received;
  auto server = Server::create(
      server_io, "127.0.0.1:0",
      [&server_io, &received](proto::Message message) {
        received.push_back(std::move(message));
        if (received.size() == 2U) {
          server_io.stop();
        }
      });
  server->start();

  boost::asio::io_context client_io;
  boost::asio::ip::tcp::socket client(client_io);
  client.connect({boost::asio::ip::make_address("127.0.0.1"),
                  server->local_port()});

  auto bytes = encode_frame(*vote_request(8));
  const auto second = encode_frame(*vote_request(9));
  bytes.insert(bytes.end(), second.begin(), second.end());
  boost::asio::write(client, boost::asio::buffer(bytes));
  server_io.run_for(std::chrono::milliseconds(500));

  ASSERT_EQ(received.size(), 2U);
  EXPECT_EQ(received[0].term, 8U);
  EXPECT_EQ(received[1].term, 9U);
  server->stop();
  server_io.restart();
  server_io.run_for(std::chrono::milliseconds(20));
}

TEST(NetworkTest, PreservesIndependentMessagesQueuedWhileDisconnected) {
  boost::asio::io_context io_context;
  boost::asio::ip::tcp::acceptor port_reservation(
      io_context,
      boost::asio::ip::tcp::endpoint(boost::asio::ip::tcp::v4(), 0));
  const auto port = port_reservation.local_endpoint().port();
  port_reservation.close();

  const auto address = std::string("127.0.0.1:") + std::to_string(port);
  auto peer = Peer::create(1, address, io_context);
  peer->send(vote_request(3));
  peer->send(vote_request(4));
  peer->send(vote_request(5));

  io_context.run_for(std::chrono::milliseconds(80));
  io_context.restart();

  std::vector<proto::Message> received;
  auto server = Server::create(
      io_context, address, [&io_context, &received](proto::Message message) {
        received.push_back(std::move(message));
        if (received.size() == 3) {
          io_context.stop();
        }
      });
  server->start();
  io_context.run_for(std::chrono::milliseconds(700));

  ASSERT_EQ(received.size(), 3U);
  EXPECT_EQ(received[0].type, proto::MsgRequestVote);
  EXPECT_EQ(received[0].term, 3U);
  EXPECT_EQ(received[1].term, 4U);
  EXPECT_EQ(received[2].term, 5U);

  peer->stop();
  peer->stop();
  server->stop();
  server->stop();
  io_context.restart();
  io_context.run_for(std::chrono::milliseconds(20));
}

TEST(NetworkTest, CoalescesOnlyConsecutiveAppendRetries) {
  boost::asio::io_context io_context;
  boost::asio::ip::tcp::acceptor port_reservation(
      io_context,
      boost::asio::ip::tcp::endpoint(boost::asio::ip::tcp::v4(), 0));
  const auto port = port_reservation.local_endpoint().port();
  port_reservation.close();

  const auto address = std::string("127.0.0.1:") + std::to_string(port);
  auto peer = Peer::create(1, address, io_context);
  for (std::uint64_t commit = 1; commit <= 100; ++commit) {
    ASSERT_TRUE(peer->send(append_request(commit)));
  }

  io_context.run_for(std::chrono::milliseconds(80));
  io_context.restart();

  std::vector<proto::Message> received;
  auto server = Server::create(
      io_context, address, [&received](proto::Message message) {
        received.push_back(std::move(message));
      });
  server->start();
  io_context.run_for(std::chrono::milliseconds(700));

  ASSERT_EQ(received.size(), 1U);
  EXPECT_EQ(received.front().type, proto::MsgAppendEntries);
  ASSERT_EQ(received.front().entries.size(), 1U);
  EXPECT_EQ(received.front().entries.front().data,
            (std::vector<std::uint8_t>{42}));
  EXPECT_EQ(received.front().read_context, 9U);
  EXPECT_EQ(received.front().leader_commit, 100U);

  peer->stop();
  server->stop();
  io_context.restart();
  io_context.run_for(std::chrono::milliseconds(20));
}

TEST(NetworkTest, RejectsNewMessagesWhenDisconnectedQueueIsFull) {
  boost::asio::io_context io_context;
  boost::asio::ip::tcp::acceptor port_reservation(
      io_context,
      boost::asio::ip::tcp::endpoint(boost::asio::ip::tcp::v4(), 0));
  const auto port = port_reservation.local_endpoint().port();
  port_reservation.close();

  const auto address = std::string("127.0.0.1:") + std::to_string(port);
  auto peer = Peer::create(1, address, io_context);

  std::size_t accepted = 0;
  while (accepted < 1024 && peer->send(vote_request(accepted + 1))) {
    ++accepted;
  }

  EXPECT_GT(accepted, 0U);
  EXPECT_LT(accepted, 1024U);

  peer->stop();
  io_context.run_for(std::chrono::milliseconds(20));
}
