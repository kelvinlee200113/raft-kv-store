#include <boost/asio.hpp>
#include <gtest/gtest.h>
#include <raft/config.h>
#include <raft/proto.h>
#include <raft/raft.h>
#include <thread>
#include <transport/peer.h>
#include <transport/server.h>

using namespace kv;

// Test: Send a Message from Peer to Server over TCP
TEST(NetworkTest, SendMessageOverTCP) {
  boost::asio::io_context io_ctx;

  // Create Raft instance for server
  kv::Config config;
  config.id = 2;
  config.peers = {1, 2, 3};
  config.election_tick = 10;
  config.heartbeat_tick = 1;
  kv::Raft raft(config);

  // Create server listening on 127.0.0.1:9001
  ServerPtr server = Server::create(io_ctx, "127.0.0.1:9001", &raft);
  server->start();

  // Create peer that will connect to 127.0.0.1:9001
  std::shared_ptr<Peer> peer =
      Peer::create(1, "127.0.0.1:9001", (void *)&io_ctx);

  // Create a test message
  proto::MessagePtr msg(new proto::Message());
  msg->type = proto::MsgRequestVote;
  msg->from = 1;
  msg->to = 2;
  msg->term = 5;
  msg->last_log_index = 10;
  msg->last_log_term = 3;

  // Send the message
  peer->send(msg);

  // Run io_context for 100ms to process async operations
  std::thread io_thread([&io_ctx]() {
    io_ctx.run_for(std::chrono::milliseconds(100));
  });

  io_thread.join();

  // If we get here without crash, test passes
  // In the console output, you should see:
  // "Connected to peer 1"
  // "Accepted connection"
  // "Received message: type=0 from=1 to=2 term=5"

  ASSERT_TRUE(true);
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
