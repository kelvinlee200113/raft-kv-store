#pragma once

#include <boost/asio/io_context.hpp>
#include <raft/proto.h>

#include <cstdint>
#include <memory>
#include <string>

namespace kv {

class Peer {
public:
  static std::shared_ptr<Peer> create(std::uint64_t id,
                                      const std::string &address,
                                      boost::asio::io_context &io_context);

  ~Peer();

  Peer(const Peer &) = delete;
  Peer &operator=(const Peer &) = delete;

  // Returns false when the bounded outbound queue cannot accept the message.
  bool send(proto::MessagePtr message);
  void stop();
  std::uint64_t id() const noexcept;

private:
  struct Impl;
  explicit Peer(std::shared_ptr<Impl> impl);

  std::shared_ptr<Impl> impl_;
};

using PeerPtr = std::shared_ptr<Peer>;

} // namespace kv
