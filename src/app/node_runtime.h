#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace kv::app {

struct PeerAddress {
  std::uint64_t id = 0;
  std::string address;
};

struct NodeConfig {
  std::uint64_t id = 0;
  std::string raft_address;
  std::string client_address;
  std::vector<PeerAddress> peers;
  std::string data_directory;
  std::uint32_t tick_milliseconds = 50;
  std::uint64_t snapshot_threshold = 100;
};

NodeConfig parse_node_config(int argc, char **argv);
std::string node_usage(const char *program);

class NodeRuntime {
 public:
  explicit NodeRuntime(NodeConfig config);
  ~NodeRuntime();

  NodeRuntime(const NodeRuntime &) = delete;
  NodeRuntime &operator=(const NodeRuntime &) = delete;

  int run();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace kv::app
