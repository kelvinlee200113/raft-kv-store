#include <gtest/gtest.h>

#include <server/resp_codec.h>
#include <server/resp_server.h>

#include <boost/asio.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using boost::asio::ip::tcp;

class RunningServer {
 public:
  explicit RunningServer(kv::resp::RespServer::CommandHandler handler) {
    server_ = kv::resp::RespServer::create(io_context_, "127.0.0.1:0",
                                           std::move(handler));
    endpoint_ = server_->local_endpoint();
    server_->start();
    // The runtime gives each io_context one owner thread. A single thread here
    // also ensures a waiting client cannot be masked by a spare executor.
    io_thread_ = std::thread([this] { io_context_.run(); });
  }

  ~RunningServer() {
    server_->stop();
    io_thread_.join();
  }

  RunningServer(const RunningServer&) = delete;
  RunningServer& operator=(const RunningServer&) = delete;

  tcp::endpoint endpoint() const { return endpoint_; }
  void stop() { server_->stop(); }

 private:
  boost::asio::io_context io_context_;
  std::shared_ptr<kv::resp::RespServer> server_;
  tcp::endpoint endpoint_;
  std::thread io_thread_;
};

std::string read_exactly(tcp::socket& socket, std::size_t size) {
  std::string reply(size, '\0');
  boost::asio::read(socket, boost::asio::buffer(reply));
  return reply;
}

TEST(RespServerTest, ReassemblesAFragmentedCommandAndReturnsItsReply) {
  RunningServer server(
      [](kv::resp::RespCommand command,
         kv::resp::RespServer::ReplyCallback reply) {
        ASSERT_EQ(command.arguments,
                  (std::vector<std::string>{"PING"}));
        reply(kv::resp::encode_simple_string("PONG"));
      });

  boost::asio::io_context client_io;
  tcp::socket client(client_io);
  client.connect(server.endpoint());

  boost::asio::write(client, boost::asio::buffer("*1\r\n$4\r\nPI", 10));
  boost::asio::write(client, boost::asio::buffer("NG\r\n", 4));

  EXPECT_EQ(read_exactly(client, 7U), "+PONG\r\n");
}

TEST(RespServerTest, DispatchesOnePipelinedCommandAtATimeInReplyOrder) {
  std::mutex mutex;
  std::condition_variable handled;
  std::vector<std::string> commands;
  kv::resp::RespServer::ReplyCallback first_reply;

  RunningServer server(
      [&](kv::resp::RespCommand command,
          kv::resp::RespServer::ReplyCallback reply) {
        const std::string name = command.arguments.at(0);
        {
          std::lock_guard<std::mutex> lock(mutex);
          commands.push_back(name);
          if (name == "ONE") {
            first_reply = std::move(reply);
          }
        }
        handled.notify_all();

        if (name == "TWO") {
          reply(kv::resp::encode_simple_string("TWO"));
        }
      });

  boost::asio::io_context client_io;
  tcp::socket client(client_io);
  client.connect(server.endpoint());
  const std::string pipeline =
      "*1\r\n$3\r\nONE\r\n*1\r\n$3\r\nTWO\r\n";
  boost::asio::write(client, boost::asio::buffer(pipeline));

  {
    std::unique_lock<std::mutex> lock(mutex);
    ASSERT_TRUE(handled.wait_for(lock, 1s,
                                 [&] { return !commands.empty(); }));
    EXPECT_FALSE(handled.wait_for(lock, 50ms,
                                  [&] { return commands.size() == 2U; }));
    ASSERT_EQ(commands, (std::vector<std::string>{"ONE"}));
  }

  first_reply(kv::resp::encode_simple_string("ONE"));

  EXPECT_EQ(read_exactly(client, 12U), "+ONE\r\n+TWO\r\n");
  {
    std::lock_guard<std::mutex> lock(mutex);
    EXPECT_EQ(commands, (std::vector<std::string>{"ONE", "TWO"}));
  }
}

TEST(RespServerTest, AWaitingClientDoesNotBlockAnotherClient) {
  std::mutex mutex;
  std::condition_variable handled;
  kv::resp::RespServer::ReplyCallback held_reply;

  RunningServer server(
      [&](kv::resp::RespCommand command,
          kv::resp::RespServer::ReplyCallback reply) {
        const std::string name = command.arguments.at(0);
        if (name == "HOLD") {
          {
            std::lock_guard<std::mutex> lock(mutex);
            held_reply = std::move(reply);
          }
          handled.notify_all();
          return;
        }
        reply(kv::resp::encode_simple_string("PONG"));
      });

  boost::asio::io_context client_io;
  tcp::socket first(client_io);
  tcp::socket second(client_io);
  first.connect(server.endpoint());
  second.connect(server.endpoint());
  boost::asio::write(first,
                     boost::asio::buffer("*1\r\n$4\r\nHOLD\r\n", 14));

  {
    std::unique_lock<std::mutex> lock(mutex);
    ASSERT_TRUE(handled.wait_for(lock, 1s,
                                 [&] { return static_cast<bool>(held_reply); }));
  }

  boost::asio::write(second,
                     boost::asio::buffer("*1\r\n$4\r\nPING\r\n", 14));
  EXPECT_EQ(read_exactly(second, 7U), "+PONG\r\n");

  held_reply(kv::resp::encode_simple_string("DONE"));
  EXPECT_EQ(read_exactly(first, 7U), "+DONE\r\n");
}

TEST(RespServerTest, StopClosesExistingConnections) {
  RunningServer server(
      [](kv::resp::RespCommand,
         kv::resp::RespServer::ReplyCallback) {});

  boost::asio::io_context client_io;
  tcp::socket client(client_io);
  client.connect(server.endpoint());

  server.stop();

  char byte = 0;
  boost::system::error_code error;
  client.read_some(boost::asio::buffer(&byte, 1U), error);
  EXPECT_TRUE(error == boost::asio::error::eof ||
              error == boost::asio::error::connection_reset);
}

TEST(RespServerTest, IncompleteRequestAtEofClosesWithoutDispatch) {
  std::atomic<bool> dispatched{false};
  RunningServer server(
      [&dispatched](kv::resp::RespCommand,
                    kv::resp::RespServer::ReplyCallback) {
        dispatched.store(true, std::memory_order_release);
      });

  boost::asio::io_context client_io;
  tcp::socket client(client_io);
  client.connect(server.endpoint());
  const std::string incomplete = "*2\r\n$3\r\nGET\r\n$3\r\nke";
  boost::asio::write(client, boost::asio::buffer(incomplete));
  client.shutdown(tcp::socket::shutdown_send);

  char byte = 0;
  boost::system::error_code error;
  client.read_some(boost::asio::buffer(&byte, 1U), error);
  EXPECT_FALSE(dispatched.load(std::memory_order_acquire));
  EXPECT_TRUE(error == boost::asio::error::eof ||
              error == boost::asio::error::connection_reset);
}

TEST(RespServerTest, DuplicateReplyCallbackCannotWriteTwice) {
  RunningServer server(
      [](kv::resp::RespCommand,
         kv::resp::RespServer::ReplyCallback reply) {
        reply(kv::resp::encode_simple_string("FIRST"));
        reply(kv::resp::encode_simple_string("SECOND"));
      });

  boost::asio::io_context client_io;
  tcp::socket client(client_io);
  client.connect(server.endpoint());
  boost::asio::write(client,
                     boost::asio::buffer("*1\r\n$4\r\nPING\r\n", 14));
  EXPECT_EQ(read_exactly(client, 8U), "+FIRST\r\n");

  client.non_blocking(true);
  char byte = 0;
  boost::system::error_code error;
  client.read_some(boost::asio::buffer(&byte, 1U), error);
  EXPECT_TRUE(error == boost::asio::error::would_block ||
              error == boost::asio::error::try_again);
}

}  // namespace
