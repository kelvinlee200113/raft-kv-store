#include <boost/asio.hpp>
#include <common/bytebuffer.h>
#include <iostream>
#include <msgpack.hpp>
#include <string>
#include <transport/peer.h>
#include <transport/proto.h>

namespace kv {

class PeerImpl;

// ClientSession manages ONE TCP connection to a remote peer
class ClientSession {
public:
  ClientSession(boost::asio::io_context &io_ctx, PeerImpl *peer,
                uint64_t peer_id, boost::asio::ip::tcp::endpoint endpoint)
      : socket_(io_ctx), peer_(peer), peer_id_(peer_id), endpoint_(endpoint),
        connected_(false) {}

  ~ClientSession() {}

  // Send data over the connection
  void send(uint8_t transport_type, const uint8_t *data, uint32_t len) {
    uint32_t remaining = buffer_.readable_bytes();

    // Create Transport header
    TransportMeta meta;
    meta.type = transport_type;
    meta.len = htonl(len); // Convert to network byte order
    assert(sizeof(TransportMeta) == 5);
    buffer_.put((const uint8_t *)&meta, sizeof(TransportMeta));
    buffer_.put(data, len);
    assert(remaining + sizeof(TransportMeta) + len == buffer_.readable_bytes());

    // If connected and buffer was empty, trigger async write
    if (connected_ && remaining == 0) {
      start_write();
    }
  }

  // Start async connection to remote peer
  void start_connect() {
    socket_.async_connect(endpoint_, [this](const boost::system::error_code &err) {
      if (err) {
        std::cerr << "Connect error: " << err.message() << std::endl;
        this->close_session();
        return;
      }
      this->connected_ = true;
      std::cout << "Connected to peer " << this->peer_id_ << std::endl;

      if (this->buffer_.readable()) {
        this->start_write();
      }
    });
  }

  // Close the session (defined later after PeerImpl is complete)
  void close_session();

private:
  // Async write buffered data to socket
  void start_write() {
    if (!buffer_.readable()) {
      return;
    }

    uint32_t remaining = buffer_.readable_bytes();
    auto buffer = boost::asio::buffer(buffer_.reader(), remaining);
    auto handler = [this](const boost::system::error_code &error,
                          std::size_t bytes) {
      if (error || bytes == 0) {
        std::cerr << "Send error: " << error.message() << std::endl;
        this->close_session();
        return;
      }
      this->buffer_.read_bytes(bytes);
      this->start_write(); // Recursively write remaining data
    };
    boost::asio::async_write(socket_, buffer, handler);
  }

  boost::asio::ip::tcp::socket socket_;
  boost::asio::ip::tcp::endpoint endpoint_;
  PeerImpl *peer_;
  uint64_t peer_id_;
  ByteBuffer buffer_;
  bool connected_;
};

// PeerImpl implements the Peer interface
class PeerImpl : public Peer {
public:
  PeerImpl(boost::asio::io_context &io_ctx, uint64_t peer_id,
           const std::string &peer_addr)
      : peer_id_(peer_id), io_ctx_(io_ctx) {

    // Parse "127.0.0.1:5001" into IP and port
    size_t colon_pos = peer_addr.find(':');
    if (colon_pos == std::string::npos) {
      std::cerr << "Invalid address format: " << peer_addr << std::endl;
      exit(1);
    }

    std::string ip = peer_addr.substr(0, colon_pos);
    std::string port_str = peer_addr.substr(colon_pos + 1);
    int port = std::stoi(port_str);

    // Create endpoint (IP + port)
    auto address = boost::asio::ip::make_address(ip);
    endpoint_ = boost::asio::ip::tcp::endpoint(address, port);
  }

  ~PeerImpl() override {}

  void start() override {
    // TODO: We'll implement this
  }

  void send(proto::MessagePtr msg) override {
    // TODO: Send serialized message to the peer we connected to
    msgpack::sbuffer sbuf;
    msgpack::pack(sbuf, *msg);
    do_send_data(TransportTypeStream, (const uint8_t *)sbuf.data(),
                 (uint32_t)sbuf.size());
  }

  void stop() override {
    // TODO: We'll implement this
  }

private:
  friend class ClientSession;  // Allow ClientSession to access session_
  void do_send_data(uint8_t type, const uint8_t *data, uint32_t len);

  uint64_t peer_id_;
  boost::asio::io_context &io_ctx_;
  boost::asio::ip::tcp::endpoint endpoint_;
  std::shared_ptr<ClientSession> session_;
};

void PeerImpl::do_send_data(uint8_t type, const uint8_t *data, uint32_t len) {
  if (!session_) {
    session_ =
        std::make_shared<ClientSession>(io_ctx_, this, peer_id_, endpoint_);
    session_->send(type, data, len);
    session_->start_connect();
  } else {
    session_->send(type, data, len);
  }
}

// ClientSession::close_session implementation (after PeerImpl is complete)
void ClientSession::close_session() {
  peer_->session_ = nullptr;
}

std::shared_ptr<Peer>
Peer::create(uint64_t peer_id, const std::string &peer_addr, void *io_service) {

  auto &io_ctx = *(boost::asio::io_context *)io_service;
  return std::make_shared<PeerImpl>(io_ctx, peer_id, peer_addr);
}

} // namespace kv
