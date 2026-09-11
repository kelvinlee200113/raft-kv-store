#pragma once

#include <server/resp_codec.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <functional>
#include <memory>
#include <string>

namespace kv::resp {

using ReplyCallback = std::function<void(std::string)>;
using CommandHandler = std::function<void(RespCommand, ReplyCallback)>;

class RespServer {
 public:
  using ReplyCallback = resp::ReplyCallback;
  using CommandHandler = resp::CommandHandler;

  static std::shared_ptr<RespServer> create(
      boost::asio::io_context& io_context, std::string listen_address,
      CommandHandler command_handler, RespLimits limits = {});

  ~RespServer();

  RespServer(const RespServer&) = delete;
  RespServer& operator=(const RespServer&) = delete;

  void start();
  void stop();

  boost::asio::ip::tcp::endpoint local_endpoint() const;

 private:
  class Impl;

  explicit RespServer(std::shared_ptr<Impl> impl);

  std::shared_ptr<Impl> impl_;
};

}  // namespace kv::resp
