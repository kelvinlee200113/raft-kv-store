#pragma once

#include <boost/asio/io_context.hpp>
#include <raft/proto.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace kv {

class Server {
public:
  using MessageHandler = std::function<void(proto::Message)>;

  static std::shared_ptr<Server> create(boost::asio::io_context &io_context,
                                        const std::string &address,
                                        MessageHandler handler);

  ~Server();

  Server(const Server &) = delete;
  Server &operator=(const Server &) = delete;

  void start();
  void stop();
  std::uint16_t local_port() const;

private:
  struct Impl;
  explicit Server(std::shared_ptr<Impl> impl);

  std::shared_ptr<Impl> impl_;
};

using ServerPtr = std::shared_ptr<Server>;

} // namespace kv
