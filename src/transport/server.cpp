#include <boost/asio.hpp>
#include <iostream>
#include <msgpack.hpp>
#include <raft/raft.h>
#include <transport/proto.h>
#include <transport/server.h>

namespace kv {

// Forward declaration
class ServerImpl;

// ServerSession manages ONE incoming TCP connection from a remote peer
// Uses std::enable_shared_from_this to safely capture 'this' in async
// callbacks
class ServerSession : public std::enable_shared_from_this<ServerSession> {
public:
  ServerSession(boost::asio::io_context &io_ctx, Raft* raft)
      : socket_(io_ctx), raft_(raft) {}

  ~ServerSession() {}

  // Start reading TransportMeta header (5 bytes: type + length)
  void start_read_meta() {
    auto self = shared_from_this(); // Keep session alive during async operation
    auto buffer = boost::asio::buffer(&meta_, sizeof(meta_));
    auto handler = [self](const boost::system::error_code &error,
                          std::size_t bytes) {
      if (error || bytes == 0) {
        std::cerr << "Read meta error: " << error.message() << std::endl;
        return;
      }

      if (bytes != sizeof(TransportMeta)) {
        std::cerr << "Invalid meta size: " << bytes << std::endl;
        return;
      }

      self->start_read_message();
    };

    // Read exactly 5 bytes (TransportMeta)
    boost::asio::async_read(socket_, buffer,
                            boost::asio::transfer_exactly(sizeof(meta_)),
                            handler);
  }

  // Read message payload based on length from meta_
  void start_read_message() {
    uint32_t len = ntohl(meta_.len); // Convert from network byte order

    // Resize buffer if needed
    if (buffer_.capacity() < len) {
      buffer_.resize(len);
    }

    auto self = shared_from_this();
    auto buffer = boost::asio::buffer(buffer_.data(), len);
    auto handler = [self, len](const boost::system::error_code &error,
                               std::size_t bytes) {
      if (error || bytes == 0) {
        std::cerr << "Read message error: " << error.message() << std::endl;
        return;
      }

      if (bytes != len) {
        std::cerr << "Invalid message size: " << bytes << " expected: " << len
                  << std::endl;
        return;
      }

      self->decode_message(len);
    };

    // Read exactly 'len' bytes
    boost::asio::async_read(socket_, buffer,
                            boost::asio::transfer_exactly(len), handler);
  }

  // Decode received message and continue reading
  void decode_message(uint32_t len) {
    switch (meta_.type) {
    case TransportTypeStream: {
      // Deserialize msgpack binary to Message struct
      proto::MessagePtr msg(new proto::Message());
      try {
        msgpack::object_handle oh =
            msgpack::unpack((const char *)buffer_.data(), len);
        oh.get().convert(*msg);
      } catch (std::exception &e) {
        std::cerr << "Msgpack decode error: " << e.what() << std::endl;
        return;
      }

      // Route message to Raft based on type
      switch (msg->type) {
      case proto::MsgRequestVote:
        std::cout << "Received RequestVote from " << msg->from
                  << " term=" << msg->term << std::endl;
        {
          proto::Message response = raft_->handle_request_vote(*msg);
          raft_->send(response); // Queue response to be sent
        }
        break;

      case proto::MsgAppendEntries:
        std::cout << "Received AppendEntries from " << msg->from
                  << " term=" << msg->term << " entries=" << msg->entries.size()
                  << std::endl;
        {
          proto::Message response = raft_->handle_append_entries(*msg);
          raft_->send(response); // Queue response to be sent
        }
        break;

      case proto::MsgPreVote:
        std::cout << "Received PreVote from " << msg->from
                  << " term=" << msg->term << std::endl;
        {
          proto::Message response = raft_->handle_pre_vote(*msg);
          raft_->send(response); // Queue response to be sent
        }
        break;

      case proto::MsgPreVoteResponse:
        std::cout << "Received PreVoteResponse from " << msg->from
                  << " granted=" << msg->vote_granted << std::endl;
        raft_->handle_pre_vote_response(*msg);
        break;

      case proto::MsgRequestVoteResponse:
        std::cout << "Received RequestVoteResponse from " << msg->from
                  << " granted=" << msg->vote_granted << std::endl;
        raft_->handle_request_vote_response(*msg);
        break;

      case proto::MsgAppendEntriesResponse:
        std::cout << "Received AppendEntriesResponse from " << msg->from
                  << " success=" << msg->success << std::endl;
        raft_->handle_append_entries_response(*msg);
        break;

      default:
        std::cerr << "Unknown message type: " << (int)msg->type << std::endl;
        break;
      }
      break;
    }
    default:
      std::cerr << "Unknown transport type: " << (int)meta_.type << std::endl;
      return;
    }

    // Continue reading next message
    start_read_meta();
  }

  boost::asio::ip::tcp::socket socket_; // Public for acceptor

private:
  Raft* raft_;
  TransportMeta meta_;
  std::vector<uint8_t> buffer_;
};

typedef std::shared_ptr<ServerSession> ServerSessionPtr;

// ServerImpl implements the Server interface
class ServerImpl : public Server {
public:
  ServerImpl(boost::asio::io_context &io_ctx, const std::string &host, Raft* raft)
      : io_ctx_(io_ctx), acceptor_(io_ctx), raft_(raft) {

    // Parse "127.0.0.1:5001" into IP and port
    size_t colon_pos = host.find(':');
    if (colon_pos == std::string::npos) {
      std::cerr << "Invalid host format: " << host << std::endl;
      exit(1);
    }

    std::string ip = host.substr(0, colon_pos);
    std::string port_str = host.substr(colon_pos + 1);
    int port = std::stoi(port_str);

    // Create endpoint
    auto address = boost::asio::ip::make_address(ip);
    auto endpoint = boost::asio::ip::tcp::endpoint(address, port);

    // Setup acceptor
    acceptor_.open(endpoint.protocol());
    acceptor_.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));
    acceptor_.bind(endpoint);
    acceptor_.listen();

    std::cout << "Server listening on " << ip << ":" << port << std::endl;
  }

  ~ServerImpl() override {}

  void start() override {
    // Create new session for incoming connection
    ServerSessionPtr session = std::make_shared<ServerSession>(io_ctx_, raft_);

    // Async accept - callback fires when client connects
    acceptor_.async_accept(
        session->socket_, [this, session](const boost::system::error_code &error) {
          if (error) {
            std::cerr << "Accept error: " << error.message() << std::endl;
            return;
          }

          std::cout << "Accepted connection" << std::endl;

          // Start reading from this session
          session->start_read_meta();

          // Accept next connection (recursive)
          this->start();
        });
  }

  void stop() override {
    // TODO: Implement
  }

private:
  boost::asio::io_context &io_ctx_;
  boost::asio::ip::tcp::acceptor acceptor_;
  Raft* raft_;
};

std::shared_ptr<Server> Server::create(boost::asio::io_context &io_ctx,
                                       const std::string &host,
                                       Raft* raft) {
  return std::make_shared<ServerImpl>(io_ctx, host, raft);
}

} // namespace kv
