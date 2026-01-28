#pragma once
#include <boost/asio.hpp>
#include <memory>
#include <raft/proto.h>

namespace kv {

// Forward declaration
class Raft;

// Server listens for incoming TCP connections and creates ServerSession
// instances to handle them
class Server {
public:
  virtual ~Server() = default;

  // Start accepting connections
  virtual void start() = 0;

  // Stop the server
  virtual void stop() = 0;

  // Factory method
  static std::shared_ptr<Server> create(boost::asio::io_context &io_ctx,
                                        const std::string &host,
                                        Raft* raft);
};

typedef std::shared_ptr<Server> ServerPtr;

} // namespace kv
