#include <transport/peer.h>

#include <common/bytebuffer.h>
#include <transport/proto.h>

#include <boost/asio.hpp>
#include <msgpack.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <stdexcept>
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
    throw std::invalid_argument("peer address is empty");
  }

  if (address.front() == '[') {
    const auto bracket = address.find(']');
    if (bracket == std::string::npos || bracket + 1 >= address.size() ||
        address[bracket + 1] != ':') {
      throw std::invalid_argument("invalid bracketed peer address");
    }
    const auto host = address.substr(1, bracket - 1);
    const auto service = address.substr(bracket + 2);
    if (host.empty() || service.empty()) {
      throw std::invalid_argument("peer address requires host and port");
    }
    return {host, service};
  }

  const auto separator = address.rfind(':');
  if (separator == std::string::npos || separator == 0 ||
      separator + 1 == address.size()) {
    throw std::invalid_argument("peer address must be host:port");
  }
  return {address.substr(0, separator), address.substr(separator + 1)};
}

std::shared_ptr<std::vector<std::uint8_t>>
encode_message(const proto::Message &message) {
  msgpack::sbuffer payload;
  msgpack::pack(payload, message);
  if (payload.size() > transport::kMaxPayloadSize) {
    throw std::length_error("serialized message exceeds transport limit");
  }

  const auto header = transport::encode_header(
      TransportTypeStream, static_cast<std::uint32_t>(payload.size()));
  ByteBuffer frame(transport::kHeaderSize + transport::kMaxPayloadSize);
  frame.append(header.data(), header.size());
  frame.append(payload.data(), payload.size());

  return std::make_shared<std::vector<std::uint8_t>>(
      frame.data(), frame.data() + frame.readable_bytes());
}

} // namespace

struct Peer::Impl : std::enable_shared_from_this<Peer::Impl> {
  using Tcp = boost::asio::ip::tcp;
  using Strand = boost::asio::strand<boost::asio::io_context::executor_type>;

  struct PendingFrame {
    std::shared_ptr<std::vector<std::uint8_t>> bytes;
    bool replaceable_append = false;
    std::uint64_t from = 0;
    std::uint64_t to = 0;
    std::uint64_t term = 0;
    std::uint64_t read_context = 0;
  };

  static constexpr std::size_t kMaxPendingFrames = 512;
  static constexpr std::size_t kMaxPendingBytes =
      transport::kMaxPayloadSize + transport::kHeaderSize;

  Impl(std::uint64_t peer_id, const std::string &address,
       boost::asio::io_context &io_context)
      : peer_id(peer_id), parts(split_address(address)),
        strand(boost::asio::make_strand(io_context)), resolver(strand),
        socket(strand), retry_timer(strand) {}

  bool reserve(std::size_t bytes) {
    std::lock_guard<std::mutex> lock(admission_mutex);
    if (stop_requested.load(std::memory_order_acquire) ||
        pending_frames >= kMaxPendingFrames ||
        bytes > kMaxPendingBytes - pending_bytes) {
      return false;
    }
    ++pending_frames;
    pending_bytes += bytes;
    return true;
  }

  void release(std::size_t bytes) {
    std::lock_guard<std::mutex> lock(admission_mutex);
    --pending_frames;
    pending_bytes -= bytes;
  }

  bool enqueue(const proto::Message &message,
               std::shared_ptr<std::vector<std::uint8_t>> bytes) {
    const auto frame_size = bytes->size();
    if (!reserve(frame_size)) {
      return false;
    }

    PendingFrame frame;
    frame.bytes = std::move(bytes);
    frame.replaceable_append = message.type == proto::MsgAppendEntries;
    frame.from = message.from;
    frame.to = message.to;
    frame.term = message.term;
    frame.read_context = message.read_context;

    auto self = shared_from_this();
    try {
      boost::asio::post(
          strand, [self, frame = std::move(frame), frame_size]() mutable {
            if (self->stopping()) {
              self->release(frame_size);
              return;
            }

            const bool back_is_in_flight =
                self->write_in_progress && self->outbound.size() == 1U;
            // Consecutive AppendEntries retries for one term/read round are
            // idempotent and the newest frame carries the latest suffix and
            // commit index. Never cross an independent RPC or replace bytes
            // already owned by an in-flight write.
            if (!back_is_in_flight && frame.replaceable_append &&
                !self->outbound.empty()) {
              auto &previous = self->outbound.back();
              const bool same_append_stream =
                  previous.replaceable_append &&
                  previous.from == frame.from && previous.to == frame.to &&
                  previous.term == frame.term &&
                  previous.read_context == frame.read_context;
              if (same_append_stream) {
                const auto replaced_size = previous.bytes->size();
                previous = std::move(frame);
                self->release(replaced_size);
                if (self->connected) {
                  self->write_next();
                }
                return;
              }
            }

            self->outbound.push_back(std::move(frame));
            if (self->connected) {
              self->write_next();
            } else {
              self->connect();
            }
          });
    } catch (...) {
      release(frame_size);
      throw;
    }
    return true;
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

  void connect() {
    if (stopping() || connected || connect_in_progress || retry_pending ||
        outbound.empty()) {
      return;
    }

    connect_in_progress = true;
    auto self = shared_from_this();
    resolver.async_resolve(
        parts.host, parts.service,
        [self](const boost::system::error_code &error,
               const Tcp::resolver::results_type &results) {
          if (self->stopping()) {
            return;
          }
          if (error) {
            self->connect_in_progress = false;
            self->schedule_retry();
            return;
          }

          boost::system::error_code ignored;
          self->socket.close(ignored);
          self->socket = Tcp::socket(self->strand);
          boost::asio::async_connect(
              self->socket, results,
              [self](const boost::system::error_code &connect_error,
                     const Tcp::endpoint &) {
                self->connect_in_progress = false;
                if (self->stopping()) {
                  return;
                }
                if (connect_error) {
                  self->mark_disconnected();
                  self->schedule_retry();
                  return;
                }

                self->connected = true;
                self->retry_delay = std::chrono::milliseconds(20);
                self->write_next();
              });
        });
  }

  void write_next() {
    if (stopping() || !connected || write_in_progress || outbound.empty()) {
      return;
    }

    write_in_progress = true;
    auto self = shared_from_this();
    auto frame = outbound.front().bytes;
    boost::asio::async_write(
        socket, boost::asio::buffer(*frame),
        [self, frame](const boost::system::error_code &error, std::size_t) {
          self->write_in_progress = false;
          if (self->stopping()) {
            return;
          }
          if (error) {
            self->mark_disconnected();
            self->schedule_retry();
            return;
          }

          self->outbound.pop_front();
          self->release(frame->size());
          self->write_next();
        });
  }

  void schedule_retry() {
    if (stopping() || outbound.empty() || retry_pending) {
      return;
    }

    retry_pending = true;
    retry_timer.expires_after(retry_delay);
    retry_delay = std::min(retry_delay * 2, std::chrono::milliseconds(500));
    auto self = shared_from_this();
    retry_timer.async_wait([self](const boost::system::error_code &error) {
      self->retry_pending = false;
      if (!error && !self->stopping()) {
        self->connect();
      }
    });
  }

  bool stopping() const noexcept {
    return stopped || stop_requested.load(std::memory_order_acquire);
  }

  void mark_disconnected() {
    connected = false;
    write_in_progress = false;
    boost::system::error_code ignored;
    socket.close(ignored);
  }

  void stop_now() {
    if (stopped) {
      return;
    }
    stopped = true;
    connected = false;
    connect_in_progress = false;
    write_in_progress = false;
    retry_pending = false;
    for (const auto &frame : outbound) {
      release(frame.bytes->size());
    }
    outbound.clear();

    boost::system::error_code ignored;
    resolver.cancel();
    try {
      retry_timer.cancel();
    } catch (const boost::system::system_error &) {
      // Socket cancellation below is still sufficient to stop the peer.
    }
    socket.cancel(ignored);
    socket.shutdown(Tcp::socket::shutdown_both, ignored);
    socket.close(ignored);
  }

  const std::uint64_t peer_id;
  const AddressParts parts;
  Strand strand;
  Tcp::resolver resolver;
  Tcp::socket socket;
  boost::asio::steady_timer retry_timer;
  std::deque<PendingFrame> outbound;
  std::mutex admission_mutex;
  std::size_t pending_frames = 0;
  std::size_t pending_bytes = 0;
  std::atomic<bool> stop_requested{false};
  bool stopped = false;
  bool connected = false;
  bool connect_in_progress = false;
  bool write_in_progress = false;
  bool retry_pending = false;
  std::chrono::milliseconds retry_delay{20};
};

Peer::Peer(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}

Peer::~Peer() { stop(); }

std::shared_ptr<Peer> Peer::create(std::uint64_t id,
                                   const std::string &address,
                                   boost::asio::io_context &io_context) {
  return std::shared_ptr<Peer>(
      new Peer(std::make_shared<Impl>(id, address, io_context)));
}

bool Peer::send(proto::MessagePtr message) {
  if (!message) {
    throw std::invalid_argument("cannot send a null Raft message");
  }
  return impl_->enqueue(*message, encode_message(*message));
}

void Peer::stop() {
  if (impl_) {
    impl_->request_stop();
  }
}

std::uint64_t Peer::id() const noexcept { return impl_->peer_id; }

} // namespace kv
