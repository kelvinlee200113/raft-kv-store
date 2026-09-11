#include <server/resp_server.h>

#include <boost/asio.hpp>

#include <array>
#include <charconv>
#include <cstdint>
#include <deque>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kv::resp {
namespace {

using boost::asio::ip::tcp;

constexpr std::size_t kReadBufferBytes = 8U * 1024U;

tcp::endpoint parse_listen_address(std::string_view listen_address) {
  const auto separator = listen_address.rfind(':');
  if (separator == std::string_view::npos || separator == 0U ||
      separator + 1U == listen_address.size()) {
    throw std::invalid_argument(
        "listen address must have the form IP:port");
  }

  std::string_view address = listen_address.substr(0U, separator);
  if (address.size() >= 2U && address.front() == '[' &&
      address.back() == ']') {
    address.remove_prefix(1U);
    address.remove_suffix(1U);
  }

  std::uint32_t port = 0U;
  const std::string_view port_text = listen_address.substr(separator + 1U);
  const auto parsed = std::from_chars(port_text.data(),
                                      port_text.data() + port_text.size(), port);
  if (parsed.ec != std::errc{} ||
      parsed.ptr != port_text.data() + port_text.size() || port > 65535U) {
    throw std::invalid_argument("listen address contains an invalid port");
  }

  return {boost::asio::ip::make_address(address),
          static_cast<std::uint16_t>(port)};
}

class RespSession : public std::enable_shared_from_this<RespSession> {
 public:
  using CloseHandler = std::function<void(RespSession*)>;

  RespSession(tcp::socket socket, CommandHandler command_handler,
              RespLimits limits, CloseHandler close_handler)
      : socket_(std::move(socket)),
        command_handler_(std::move(command_handler)),
        decoder_(limits),
        close_handler_(std::move(close_handler)) {}

  void start() {
    auto self = shared_from_this();
    boost::asio::dispatch(socket_.get_executor(),
                          [self] { self->read_next(); });
  }

  void stop() {
    auto self = shared_from_this();
    boost::asio::dispatch(socket_.get_executor(),
                          [self] { self->close(); });
  }

 private:
  void read_next() {
    if (closed_ || awaiting_reply_ || write_in_progress_ ||
        !pending_commands_.empty()) {
      return;
    }

    auto self = shared_from_this();
    socket_.async_read_some(
        boost::asio::buffer(read_buffer_),
        [self](const boost::system::error_code& error,
               std::size_t bytes_transferred) {
          self->on_read(error, bytes_transferred);
        });
  }

  void on_read(const boost::system::error_code& error,
               std::size_t bytes_transferred) {
    if (error || bytes_transferred == 0U) {
      close();
      return;
    }

    try {
      auto commands = decoder_.feed(
          std::string_view(read_buffer_.data(), bytes_transferred));
      for (auto& command : commands) {
        pending_commands_.push_back(std::move(command));
      }
    } catch (const RespProtocolError&) {
      close();
      return;
    }

    if (pending_commands_.empty()) {
      read_next();
      return;
    }
    dispatch_next();
  }

  void dispatch_next() {
    if (closed_ || awaiting_reply_ || write_in_progress_) {
      return;
    }
    if (pending_commands_.empty()) {
      read_next();
      return;
    }

    RespCommand command = std::move(pending_commands_.front());
    pending_commands_.pop_front();
    awaiting_reply_ = true;
    const std::uint64_t request_id = ++active_request_id_;
    std::weak_ptr<RespSession> weak_self = shared_from_this();

    ReplyCallback reply =
        [weak_self, request_id](std::string encoded_reply) mutable {
          auto self = weak_self.lock();
          if (!self) {
            return;
          }
          boost::asio::dispatch(
              self->socket_.get_executor(),
              [self, request_id,
               encoded_reply = std::move(encoded_reply)]() mutable {
                self->on_reply(request_id, std::move(encoded_reply));
              });
        };

    try {
      command_handler_(std::move(command), std::move(reply));
    } catch (...) {
      close();
    }
  }

  void on_reply(std::uint64_t request_id, std::string encoded_reply) {
    if (closed_ || !awaiting_reply_ || request_id != active_request_id_) {
      return;
    }

    awaiting_reply_ = false;
    if (encoded_reply.empty()) {
      dispatch_next();
      return;
    }

    write_buffer_ = std::move(encoded_reply);
    write_in_progress_ = true;
    auto self = shared_from_this();
    boost::asio::async_write(
        socket_, boost::asio::buffer(write_buffer_),
        [self](const boost::system::error_code& error,
               std::size_t /*bytes_transferred*/) {
          self->write_in_progress_ = false;
          self->write_buffer_.clear();
          if (error) {
            self->close();
            return;
          }
          self->dispatch_next();
        });
  }

  void close() {
    if (closed_) {
      return;
    }
    closed_ = true;

    boost::system::error_code ignored;
    socket_.shutdown(tcp::socket::shutdown_both, ignored);
    socket_.close(ignored);

    if (close_handler_) {
      auto close_handler = std::move(close_handler_);
      close_handler(this);
    }
  }

  tcp::socket socket_;
  CommandHandler command_handler_;
  RespDecoder decoder_;
  CloseHandler close_handler_;
  std::array<char, kReadBufferBytes> read_buffer_{};
  std::deque<RespCommand> pending_commands_;
  std::string write_buffer_;
  std::uint64_t active_request_id_ = 0U;
  bool awaiting_reply_ = false;
  bool write_in_progress_ = false;
  bool closed_ = false;
};

}  // namespace

class RespServer::Impl : public std::enable_shared_from_this<Impl> {
 public:
  Impl(boost::asio::io_context& io_context, std::string listen_address,
       CommandHandler command_handler, RespLimits limits)
      : io_context_(io_context),
        strand_(boost::asio::make_strand(io_context)),
        acceptor_(strand_),
        command_handler_(std::move(command_handler)),
        limits_(limits) {
    if (!command_handler_) {
      throw std::invalid_argument("command handler must be set");
    }

    const auto endpoint = parse_listen_address(listen_address);
    acceptor_.open(endpoint.protocol());
    acceptor_.set_option(tcp::acceptor::reuse_address(true));
    acceptor_.bind(endpoint);
    acceptor_.listen();
  }

  void start() {
    auto self = shared_from_this();
    boost::asio::dispatch(strand_, [self] {
      if (self->started_ || self->stopped_) {
        return;
      }
      self->started_ = true;
      self->accept_next();
    });
  }

  void stop() {
    auto self = shared_from_this();
    boost::asio::dispatch(strand_, [self] { self->stop_on_strand(); });
  }

  tcp::endpoint local_endpoint() const { return acceptor_.local_endpoint(); }

 private:
  void accept_next() {
    if (stopped_) {
      return;
    }

    auto socket =
        std::make_shared<tcp::socket>(boost::asio::make_strand(io_context_));
    auto self = shared_from_this();
    acceptor_.async_accept(
        *socket, boost::asio::bind_executor(
                     strand_, [self, socket](
                                  const boost::system::error_code& error) {
                       if (!error && !self->stopped_) {
                         self->add_session(std::move(*socket));
                       }
                       if (!self->stopped_) {
                         self->accept_next();
                       }
                     }));
  }

  void add_session(tcp::socket socket) {
    std::weak_ptr<Impl> weak_self = shared_from_this();
    auto session = std::make_shared<RespSession>(
        std::move(socket), command_handler_, limits_,
        [weak_self](RespSession* closed_session) {
          auto self = weak_self.lock();
          if (!self) {
            return;
          }
          boost::asio::dispatch(
              self->strand_, [self, closed_session] {
                self->sessions_.erase(closed_session);
              });
        });
    sessions_.emplace(session.get(), session);
    session->start();
  }

  void stop_on_strand() {
    if (stopped_) {
      return;
    }
    stopped_ = true;

    boost::system::error_code ignored;
    acceptor_.cancel(ignored);
    acceptor_.close(ignored);
    std::vector<std::shared_ptr<RespSession>> sessions;
    sessions.reserve(sessions_.size());
    for (auto& entry : sessions_) {
      sessions.push_back(std::move(entry.second));
    }
    sessions_.clear();
    for (const auto& session : sessions) {
      session->stop();
    }
  }

  boost::asio::io_context& io_context_;
  boost::asio::strand<boost::asio::io_context::executor_type> strand_;
  tcp::acceptor acceptor_;
  CommandHandler command_handler_;
  RespLimits limits_;
  std::unordered_map<RespSession*, std::shared_ptr<RespSession>> sessions_;
  bool started_ = false;
  bool stopped_ = false;
};

std::shared_ptr<RespServer> RespServer::create(
    boost::asio::io_context& io_context, std::string listen_address,
    CommandHandler command_handler, RespLimits limits) {
  auto impl = std::make_shared<Impl>(io_context, std::move(listen_address),
                                     std::move(command_handler), limits);
  return std::shared_ptr<RespServer>(new RespServer(std::move(impl)));
}

RespServer::RespServer(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}

RespServer::~RespServer() { impl_->stop(); }

void RespServer::start() { impl_->start(); }

void RespServer::stop() { impl_->stop(); }

tcp::endpoint RespServer::local_endpoint() const {
  return impl_->local_endpoint();
}

}  // namespace kv::resp
