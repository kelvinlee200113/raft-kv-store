#include <server/kv_store.h>

#include <stdexcept>

namespace kv {

bool KVStore::get(const std::string &key, std::string &value) const {
  auto it = store_.find(key);
  if (it != store_.end()) {
    value = it->second;
    return true;
  }
  return false;
}

void KVStore::set(const std::string &key, const std::string &value) {
  store_[key] = value;
}

bool KVStore::del(const std::string &key) { return store_.erase(key) != 0; }

KVStore::ApplyResult KVStore::apply(const proto::Entry &entry) {
  msgpack::object_handle oh = msgpack::unpack(
      reinterpret_cast<const char *>(entry.data.data()), entry.data.size());
  Command cmd = oh.get().as<Command>();

  ApplyResult result{cmd.type, cmd.origin_node, cmd.request_id, 0,
                     entry.index};
  if (cmd.type == CommandType::Set) {
    if (cmd.strs.size() != 2) {
      throw std::invalid_argument("SET command requires key and value");
    }
    set(cmd.strs[0], cmd.strs[1]);
    result.affected = 1;
  } else if (cmd.type == CommandType::Del) {
    if (cmd.strs.empty()) {
      throw std::invalid_argument("DEL command requires at least one key");
    }
    for (const auto &key : cmd.strs) {
      if (del(key)) {
        ++result.affected;
      }
    }
  } else {
    throw std::invalid_argument("unknown KV command type");
  }
  return result;
}

std::vector<uint8_t> KVStore::serialize() const {
  msgpack::sbuffer sbuf;
  msgpack::pack(sbuf, store_);
  return std::vector<uint8_t>(
      reinterpret_cast<const uint8_t*>(sbuf.data()),
      reinterpret_cast<const uint8_t*>(sbuf.data()) + sbuf.size());
}

void KVStore::deserialize(const std::vector<uint8_t>& data) {
  msgpack::object_handle oh = msgpack::unpack(
      reinterpret_cast<const char*>(data.data()), data.size());
  std::unordered_map<std::string, std::string> restored;
  oh.get().convert(restored);
  store_.swap(restored);
}

} // namespace kv
