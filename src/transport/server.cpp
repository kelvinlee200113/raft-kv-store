#include <boost/asio.hpp>
#include <iostream>
#include <msgpack.hpp>
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
  ServerSession(boost::asio::io_context &io_ctx) : socket_(io_ctx) {}

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

      // TODO: Pass message to Raft for processing
      std::cout << "Received message: type=" << (int)msg->type
                << " from=" << msg->from << " to=" << msg->to
                << " term=" << msg->term << std::endl;
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
  TransportMeta meta_;
  std::vector<uint8_t> buffer_;
};

typedef std::shared_ptr<ServerSession> ServerSessionPtr;

// ServerImpl implements the Server interface
class ServerImpl : public Server {
public:
  ServerImpl(boost::asio::io_context &io_ctx, const std::string &host)
      : io_ctx_(io_ctx), acceptor_(io_ctx) {

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
    ServerSessionPtr session = std::make_shared<ServerSession>(io_ctx_);

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
};

std::shared_ptr<Server> Server::create(boost::asio::io_context &io_ctx,
                                       const std::string &host) {
  return std::make_shared<ServerImpl>(io_ctx, host);
}

} // namespace kv
