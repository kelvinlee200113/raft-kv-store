#pragma once
#include <msgpack.hpp>
#include <raft/proto.h>
#include <string>
#include <sys/types.h>
#include <unordered_map>
#include <vector>

namespace kv {

// Command types for KV operations
enum class CommandType : uint8_t { Set = 0, Del = 1 };

class KVStore {
public:
  KVStore() = default;

  bool get(const std::string &key, std::string &value) const;
  void set(const std::string &key, const std::string &value);
  bool del(const std::string &key);

  struct ApplyResult {
    CommandType type;
    uint64_t origin_node;
    uint64_t request_id;
    uint64_t affected;
    uint64_t applied_index;
  };

  ApplyResult apply(const proto::Entry &entry);

  // Snapshot support: serialize entire store to bytes / replace store from bytes
  std::vector<uint8_t> serialize() const;
  void deserialize(const std::vector<uint8_t>& data);

private:
  std::unordered_map<std::string, std::string> store_;
};

// Command structure for KV operations
struct Command {
  CommandType type;
  std::vector<std::string> strs;
  uint64_t origin_node;
  uint64_t request_id;

  Command()
      : type(CommandType::Set), origin_node(0), request_id(0) {}

  MSGPACK_DEFINE(type, strs, origin_node, request_id);
};

} // namespace kv

namespace msgpack {

MSGPACK_API_VERSION_NAMESPACE(MSGPACK_DEFAULT_API_NS) {
  namespace adaptor {

  template <> struct pack<kv::CommandType> {
    template <typename Stream>
    msgpack::packer<Stream> &operator()(msgpack::packer<Stream> &o,
                                        kv::CommandType const &v) const {
      return o.pack(static_cast<uint8_t>(v));
    }
  };

  template <> struct convert<kv::CommandType> {
    msgpack::object const &operator()(msgpack::object const &o,
                                      kv::CommandType &v) const {
      if (o.type != msgpack::type::POSITIVE_INTEGER) {
        throw msgpack::type_error();
      }
      v = static_cast<kv::CommandType>(o.via.u64);
      return o;
    }
  };

  } // namespace adaptor
}
} // namespace msgpack
