#include <transport/server.h>

#include <raft/proto.h>
#include <transport/proto.h>

#include <boost/asio.hpp>
#include <msgpack.hpp>

#include <array>
#include <atomic>
#include <stdexcept>
#include <unordered_set>
#include <utility>
#include <vector>

namespace kv {
namespace {

struct AddressParts {
  std::string host;
  std::string service;
};

AddressParts split_address(const std::string &address) {
  if (address.empty()) {
    throw std::invalid_argument("server address is empty");
  }

  if (address.front() == '[') {
    const auto bracket = address.find(']');
    if (bracket == std::string::npos || bracket + 1 >= address.size() ||
        address[bracket + 1] != ':') {
      throw std::invalid_argument("invalid bracketed server address");
    }
    const auto host = address.substr(1, bracket - 1);
    const auto service = address.substr(bracket + 2);
    if (host.empty() || service.empty()) {
      throw std::invalid_argument("server address requires host and port");
    }
    return {host, service};
  }

  const auto separator = address.rfind(':');
  if (separator == std::string::npos || separator == 0 ||
      separator + 1 == address.size()) {
    throw std::invalid_argument("server address must be host:port");
  }
  return {address.substr(0, separator), address.substr(separator + 1)};
}

} // namespace

struct Server::Impl : std::enable_shared_from_this<Server::Impl> {
  using Tcp = boost::asio::ip::tcp;
  using Strand = boost::asio::strand<boost::asio::io_context::executor_type>;

  struct Connection : std::enable_shared_from_this<Connection> {
    Connection(Tcp::socket accepted_socket, std::weak_ptr<Impl> owner,
               Strand strand)
        : socket(std::move(accepted_socket)), owner(std::move(owner)),
          strand(std::move(strand)) {}

    void start() { read_header(); }

    void stop() {
      if (stopped) {
        return;
      }
      stopped = true;
      boost::system::error_code ignored;
      socket.cancel(ignored);
      socket.shutdown(Tcp::socket::shutdown_both, ignored);
      socket.close(ignored);
    }

    void read_header() {
      if (stopped) {
        return;
      }

      auto self = shared_from_this();
      boost::asio::async_read(
          socket, boost::asio::buffer(header_bytes),
          boost::asio::bind_executor(
              strand, [self](const boost::system::error_code &error,
                             std::size_t) {
                if (error) {
                  self->finish();
                  return;
                }

                try {
                  const auto header = transport::decode_header(
                      self->header_bytes.data(), self->header_bytes.size());
                  self->payload.resize(header.payload_size);
                } catch (const std::exception &) {
                  self->finish();
                  return;
                }
                self->read_payload();
              }));
    }

    void read_payload() {
      if (payload.empty()) {
        decode_and_dispatch();
        return;
      }

      auto self = shared_from_this();
      boost::asio::async_read(
          socket, boost::asio::buffer(payload),
          boost::asio::bind_executor(
              strand, [self](const boost::system::error_code &error,
                             std::size_t) {
                if (error) {
                  self->finish();
                  return;
                }
                self->decode_and_dispatch();
              }));
    }

    void decode_and_dispatch() {
      try {
        const auto object = msgpack::unpack(
            reinterpret_cast<const char *>(payload.data()), payload.size());
        proto::Message message;
        object.get().convert(message);

        const auto server = owner.lock();
        if (!server || !server->dispatch(std::move(message))) {
          finish();
          return;
        }
      } catch (const std::exception &) {
        finish();
        return;
      }

      read_header();
    }

    void finish() {
      if (stopped) {
        return;
      }
      stop();
      if (const auto server = owner.lock()) {
        server->remove_connection(shared_from_this());
      }
    }

    Tcp::socket socket;
    std::weak_ptr<Impl> owner;
    Strand strand;
    std::array<std::uint8_t, transport::kHeaderSize> header_bytes{};
    std::vector<std::uint8_t> payload;
    bool stopped = false;
  };

  Impl(boost::asio::io_context &io_context, const std::string &address,
       Server::MessageHandler message_handler)
      : strand(boost::asio::make_strand(io_context)), acceptor(strand),
        handler(std::move(message_handler)) {
    if (!handler) {
      throw std::invalid_argument("transport server requires a message handler");
    }

    const auto parts = split_address(address);
    Tcp::resolver resolver(io_context);
    const auto endpoints = resolver.resolve(
        parts.host, parts.service, Tcp::resolver::passive);
    if (endpoints.empty()) {
      throw std::invalid_argument("server address did not resolve");
    }

    const auto endpoint = endpoints.begin()->endpoint();
    acceptor.open(endpoint.protocol());
    acceptor.set_option(Tcp::acceptor::reuse_address(true));
    acceptor.bind(endpoint);
    acceptor.listen();
  }

  void request_start() {
    auto self = shared_from_this();
    boost::asio::dispatch(strand, [self]() {
      if (self->started || self->stop_requested.load(std::memory_order_acquire)) {
        return;
      }
      self->started = true;
      self->accept_next();
    });
  }

  void request_stop() {
    bool expected = false;
    if (!stop_requested.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
      return;
    }

    auto self = shared_from_this();
    boost::asio::post(strand, [self]() { self->stop_now(); });
  }

  std::uint16_t local_port() const {
    boost::system::error_code error;
    const auto endpoint = acceptor.local_endpoint(error);
    if (error) {
      throw boost::system::system_error(error);
    }
    return endpoint.port();
  }

  void accept_next() {
    if (stop_requested.load(std::memory_order_acquire)) {
      return;
    }

    auto self = shared_from_this();
    acceptor.async_accept(
        boost::asio::bind_executor(
            strand,
            [self](const boost::system::error_code &error, Tcp::socket socket) {
              if (!error &&
                  !self->stop_requested.load(std::memory_order_acquire)) {
                auto connection = std::make_shared<Connection>(
                    std::move(socket), self, self->strand);
                self->connections.insert(connection);
                connection->start();
              }

              if (!self->stop_requested.load(std::memory_order_acquire)) {
                self->accept_next();
              }
            }));
  }

  bool dispatch(proto::Message message) {
    switch (message.type) {
    case proto::MsgRequestVote:
    case proto::MsgRequestVoteResponse:
    case proto::MsgAppendEntries:
    case proto::MsgAppendEntriesResponse:
    case proto::MsgPreVote:
    case proto::MsgPreVoteResponse:
    case proto::MsgInstallSnapshot:
    case proto::MsgInstallSnapshotResponse:
      handler(std::move(message));
      return true;
    default:
      return false;
    }
  }

  void remove_connection(const std::shared_ptr<Connection> &connection) {
    connections.erase(connection);
  }

  void stop_now() {
    boost::system::error_code ignored;
    acceptor.cancel(ignored);
    acceptor.close(ignored);
    for (const auto &connection : connections) {
      connection->stop();
    }
    connections.clear();
  }

  Strand strand;
  Tcp::acceptor acceptor;
  Server::MessageHandler handler;
  std::unordered_set<std::shared_ptr<Connection>> connections;
  std::atomic<bool> stop_requested{false};
  bool started = false;
};

Server::Server(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}

Server::~Server() { stop(); }

std::shared_ptr<Server> Server::create(boost::asio::io_context &io_context,
                                       const std::string &address,
                                       MessageHandler handler) {
  return std::shared_ptr<Server>(
      new Server(std::make_shared<Impl>(io_context, address,
                                        std::move(handler))));
}

void Server::start() { impl_->request_start(); }

void Server::stop() {
  if (impl_) {
    impl_->request_stop();
  }
}

std::uint16_t Server::local_port() const { return impl_->local_port(); }

} // namespace kv
