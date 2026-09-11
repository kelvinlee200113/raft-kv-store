#include <app/node_runtime.h>

#include <raft/config.h>
#include <raft/raft.h>
#include <server/kv_store.h>
#include <server/resp_codec.h>
#include <server/resp_server.h>
#include <transport/peer.h>
#include <transport/server.h>
#include <wal/wal.h>

#include <boost/asio.hpp>
#include <msgpack.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <charconv>
#include <csignal>
#include <filesystem>
#include <future>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kv::app {
namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr auto kRequestTimeout = 5s;
constexpr auto kRequestPollInterval = 1ms;
constexpr auto kSnapshotRestoreTimeout = 5s;

std::string_view option_value(std::string_view argument,
                              std::string_view option) {
  if (argument.substr(0, option.size()) != option) {
    return {};
  }
  return argument.substr(option.size());
}

template <typename Integer>
Integer parse_integer(std::string_view text, std::string_view name) {
  Integer value = 0;
  const auto result =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (text.empty() || result.ec != std::errc{} ||
      result.ptr != text.data() + text.size()) {
    throw std::invalid_argument(std::string(name) + " must be an integer");
  }
  return value;
}

PeerAddress parse_peer(std::string_view value) {
  const auto separator = value.find('@');
  if (separator == std::string_view::npos || separator == 0 ||
      separator + 1 == value.size()) {
    throw std::invalid_argument("--peer must have the form ID@IP:port");
  }
  PeerAddress peer;
  peer.id = parse_integer<std::uint64_t>(value.substr(0, separator),
                                         "peer ID");
  peer.address = std::string(value.substr(separator + 1));
  return peer;
}

Config raft_config_for(const NodeConfig &config) {
  Config raft_config;
  raft_config.id = config.id;
  raft_config.peers = {1, 2, 3};
  raft_config.election_tick = 10;
  raft_config.heartbeat_tick = 1;
  raft_config.snapshot_threshold = config.snapshot_threshold;
  return raft_config;
}

std::string uppercase_ascii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](char byte) {
    return static_cast<char>(
        std::toupper(static_cast<unsigned char>(byte)));
  });
  return value;
}

std::string state_name(State state) {
  switch (state) {
    case State::Follower:
      return "Follower";
    case State::PreCandidate:
      return "PreCandidate";
    case State::Candidate:
      return "Candidate";
    case State::Leader:
      return "Leader";
  }
  return "Unknown";
}

std::uint64_t initial_request_id(std::uint64_t node_id) {
  std::random_device random;
  const auto now = static_cast<std::uint64_t>(
      std::chrono::system_clock::now().time_since_epoch().count());
  std::uint64_t value =
      (static_cast<std::uint64_t>(random()) << 32U) ^ random() ^ now ^
      (node_id * 0x9e3779b97f4a7c15ULL);
  return value == 0 ? 1 : value;
}

bool contains_wal_file(const std::filesystem::path &directory) {
  if (!std::filesystem::exists(directory)) {
    return false;
  }
  for (const auto &entry : std::filesystem::directory_iterator(directory)) {
    if (entry.is_regular_file() && entry.path().extension() == ".wal") {
      return true;
    }
  }
  return false;
}

std::vector<std::uint8_t> pack_command(const Command &command) {
  msgpack::sbuffer buffer;
  msgpack::pack(buffer, command);
  return {reinterpret_cast<const std::uint8_t *>(buffer.data()),
          reinterpret_cast<const std::uint8_t *>(buffer.data()) +
              buffer.size()};
}

}  // namespace

NodeConfig parse_node_config(int argc, char **argv) {
  NodeConfig config;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (const auto value = option_value(argument, "--id="); !value.empty()) {
      config.id = parse_integer<std::uint64_t>(value, "node ID");
    } else if (const auto value = option_value(argument, "--raft=");
               !value.empty()) {
      config.raft_address = value;
    } else if (const auto value = option_value(argument, "--client=");
               !value.empty()) {
      config.client_address = value;
    } else if (const auto value = option_value(argument, "--peer=");
               !value.empty()) {
      config.peers.push_back(parse_peer(value));
    } else if (const auto value = option_value(argument, "--data=");
               !value.empty()) {
      config.data_directory = value;
    } else if (const auto value = option_value(argument, "--tick-ms=");
               !value.empty()) {
      config.tick_milliseconds =
          parse_integer<std::uint32_t>(value, "tick interval");
    } else if (const auto value =
                   option_value(argument, "--snapshot-threshold=");
               !value.empty()) {
      config.snapshot_threshold =
          parse_integer<std::uint64_t>(value, "snapshot threshold");
    } else {
      throw std::invalid_argument("unknown or empty option: " +
                                  std::string(argument));
    }
  }

  if (config.id < 1 || config.id > 3) {
    throw std::invalid_argument("--id must be 1, 2, or 3");
  }
  if (config.raft_address.empty() || config.client_address.empty() ||
      config.data_directory.empty()) {
    throw std::invalid_argument("--raft, --client, and --data are required");
  }
  if (config.tick_milliseconds == 0 || config.tick_milliseconds > 1000) {
    throw std::invalid_argument("--tick-ms must be between 1 and 1000");
  }
  if (config.peers.size() != 2) {
    throw std::invalid_argument("exactly two --peer options are required");
  }

  std::set<std::uint64_t> member_ids{config.id};
  for (const auto &peer : config.peers) {
    if (peer.id < 1 || peer.id > 3 || peer.id == config.id ||
        !member_ids.insert(peer.id).second) {
      throw std::invalid_argument(
          "peer IDs must name the other two members of {1,2,3}");
    }
  }
  if (member_ids != std::set<std::uint64_t>{1, 2, 3}) {
    throw std::invalid_argument("the fixed cluster members are {1,2,3}");
  }
  return config;
}

std::string node_usage(const char *program) {
  const std::string name = program == nullptr ? "raft-kv" : program;
  return "Usage:\n  " + name +
         " --id=<1|2|3> --raft=<IP:port> --client=<IP:port>\n"
         "      --peer=<ID>@<IP:port> --peer=<ID>@<IP:port> --data=<dir>\n"
         "      [--tick-ms=<1..1000>] [--snapshot-threshold=<entries>]\n";
}

class NodeRuntime::Impl {
 public:
  explicit Impl(NodeConfig input)
      : config_(std::move(input)),
        raft_work_(boost::asio::make_work_guard(raft_io_)),
        peer_work_(boost::asio::make_work_guard(peer_io_)),
        store_work_(boost::asio::make_work_guard(store_io_)),
        raft_(raft_config_for(config_)),
        tick_timer_(raft_io_),
        request_timer_(store_io_),
        signals_(raft_io_, SIGINT, SIGTERM),
        next_request_id_(initial_request_id(config_.id)) {
    recover_storage();
    create_network_services();
    raft_.set_snapshot_restore_callback(
        [this](std::uint64_t index,
               const std::vector<std::uint8_t> &snapshot) {
          return restore_snapshot(index, snapshot);
        });
  }

  int run() {
    peer_server_->start();
    client_server_->start();
    schedule_tick();
    schedule_request_poll();
    signals_.async_wait([this](const boost::system::error_code &error, int) {
      if (!error) {
        request_stop({});
      }
    });

    std::cout << "node " << config_.id << " raft=" << config_.raft_address
              << " client=" << config_.client_address
              << " data=" << config_.data_directory << '\n';
    log_status_if_changed(true);

    std::thread raft_thread([this] { run_context("raft", raft_io_); });
    std::thread peer_thread([this] { run_context("peer", peer_io_); });
    std::thread store_thread([this] { run_context("store", store_io_); });

    raft_thread.join();
    peer_thread.join();
    store_thread.join();
    return exit_code_.load(std::memory_order_acquire);
  }

 private:
  using WorkGuard =
      boost::asio::executor_work_guard<boost::asio::io_context::executor_type>;

  struct PendingWrite {
    resp::ReplyCallback reply;
    Clock::time_point deadline;
  };

  struct PendingRead {
    std::string key;
    resp::ReplyCallback reply;
    Clock::time_point deadline;
  };

  void recover_storage() {
    namespace fs = std::filesystem;
    const fs::path data_directory(config_.data_directory);
    const fs::path wal_directory = data_directory / "wal";
    fs::create_directories(data_directory);

    std::unique_ptr<wal::WAL> recovered_wal;
    const bool had_wal = contains_wal_file(wal_directory);
    if (had_wal) {
      recovered_wal = wal::WAL::open(wal_directory.string());
      if (!recovered_wal) {
        throw std::runtime_error("failed to open existing WAL at " +
                                 wal_directory.string());
      }
    } else {
      if (fs::exists(wal_directory) && !fs::is_empty(wal_directory)) {
        throw std::runtime_error(
            "data WAL directory is nonempty but contains no usable WAL");
      }
      recovered_wal = wal::WAL::create(wal_directory.string());
      if (!recovered_wal) {
        throw std::runtime_error("failed to create WAL at " +
                                 wal_directory.string());
      }
    }

    if (had_wal) {
      std::vector<proto::Entry> entries;
      wal::SnapshotMeta snapshot;
      const auto hard_state = recovered_wal->recover(entries, &snapshot);
      if (!snapshot.is_empty()) {
        store_.deserialize(snapshot.state);
        store_applied_index_ = snapshot.index;
      }

      raft_.restore(hard_state, entries, snapshot);
      for (const auto &entry : entries) {
        if (entry.index <= snapshot.index) {
          continue;
        }
        if (entry.index > raft_.get_commit_index()) {
          break;
        }
        if (entry.index != store_applied_index_ + 1) {
          throw std::runtime_error(
              "committed WAL replay contains an index gap");
        }
        if (entry.type == proto::EntryNormal && !entry.data.empty()) {
          store_.apply(entry);
        }
        store_applied_index_ = entry.index;
        raft_.advance(entry.index);
      }
      if (store_applied_index_ != raft_.get_commit_index()) {
        throw std::runtime_error("WAL is missing committed state-machine data");
      }

      std::cout << "node " << config_.id << " recovered term="
                << raft_.get_term() << " commit=" << raft_.get_commit_index()
                << " snapshot=" << snapshot.index << '\n';
    }

    raft_.set_wal(std::move(recovered_wal));
  }

  void create_network_services() {
    for (const auto &peer : config_.peers) {
      peers_.emplace(peer.id,
                     Peer::create(peer.id, peer.address, peer_io_));
    }

    peer_server_ = Server::create(
        peer_io_, config_.raft_address, [this](proto::Message message) {
          boost::asio::post(
              raft_io_, [this, message = std::move(message)]() mutable {
                if (stopping_.load(std::memory_order_acquire)) {
                  return;
                }
                handle_raft_message(std::move(message));
              });
        });

    client_server_ = resp::RespServer::create(
        store_io_, config_.client_address,
        [this](resp::RespCommand command, resp::ReplyCallback reply) {
          handle_client_command(std::move(command), std::move(reply));
        });
  }

  void schedule_tick() {
    tick_timer_.expires_after(
        std::chrono::milliseconds(config_.tick_milliseconds));
    tick_timer_.async_wait([this](const boost::system::error_code &error) {
      if (error || stopping_.load(std::memory_order_acquire)) {
        return;
      }
      raft_.tick();
      after_raft_activity();
      schedule_tick();
    });
  }

  void schedule_request_poll() {
    request_timer_.expires_after(kRequestPollInterval);
    request_timer_.async_wait([this](const boost::system::error_code &error) {
      if (error || stopping_.load(std::memory_order_acquire)) {
        return;
      }
      expire_client_requests();
      schedule_request_poll();
    });
  }

  void handle_raft_message(proto::Message message) {
    switch (message.type) {
      case proto::MsgRequestVote:
        raft_.send(raft_.handle_request_vote(message));
        break;
      case proto::MsgRequestVoteResponse:
        raft_.handle_request_vote_response(message);
        break;
      case proto::MsgPreVote:
        raft_.send(raft_.handle_pre_vote(message));
        break;
      case proto::MsgPreVoteResponse:
        raft_.handle_pre_vote_response(message);
        break;
      case proto::MsgAppendEntries:
        raft_.send(raft_.handle_append_entries(message));
        break;
      case proto::MsgAppendEntriesResponse:
        raft_.handle_append_entries_response(message);
        break;
      case proto::MsgInstallSnapshot:
        raft_.send(raft_.handle_install_snapshot(message));
        break;
      case proto::MsgInstallSnapshotResponse:
        raft_.handle_install_snapshot_response(message);
        break;
      default:
        return;
    }
    after_raft_activity();
  }

  void after_raft_activity() {
    if (!raft_.storage_healthy()) {
      request_stop("WAL synchronization failed");
      return;
    }
    dispatch_next_committed_entry();
    poll_read_indexes();
    maybe_capture_snapshot();
    flush_raft_messages();
    log_status_if_changed(false);
  }

  void flush_raft_messages() {
    for (auto &message : raft_.read_messages()) {
      const auto peer = peers_.find(message.to);
      if (peer == peers_.end()) {
        continue;
      }
      auto shared_message =
          std::make_shared<proto::Message>(std::move(message));
      const auto connection = peer->second;
      try {
        if (!connection->send(shared_message)) {
          request_stop("peer transport outbound queue exhausted");
          return;
        }
      } catch (const std::exception &error) {
        request_stop(std::string("peer transport encode failed: ") +
                     error.what());
        return;
      }
    }
  }

  void handle_client_command(resp::RespCommand command,
                             resp::ReplyCallback reply) {
    if (command.arguments.empty()) {
      reply(resp::encode_error("ERR empty command"));
      return;
    }

    const std::string name = uppercase_ascii(command.arguments.front());
    if (name == "PING") {
      if (command.arguments.size() == 1) {
        reply(resp::encode_simple_string("PONG"));
      } else if (command.arguments.size() == 2) {
        reply(resp::encode_bulk_string(command.arguments[1]));
      } else {
        reply(resp::encode_error(
            "ERR wrong number of arguments for 'ping' command"));
      }
      return;
    }

    if (name == "GET") {
      if (command.arguments.size() != 2) {
        reply(resp::encode_error(
            "ERR wrong number of arguments for 'get' command"));
        return;
      }
      begin_read(command.arguments[1], std::move(reply));
      return;
    }

    Command replicated;
    if (name == "SET") {
      if (command.arguments.size() != 3) {
        reply(resp::encode_error(
            "ERR wrong number of arguments for 'set' command"));
        return;
      }
      replicated.type = CommandType::Set;
      replicated.strs = {command.arguments[1], command.arguments[2]};
    } else if (name == "DEL") {
      if (command.arguments.size() < 2) {
        reply(resp::encode_error(
            "ERR wrong number of arguments for 'del' command"));
        return;
      }
      replicated.type = CommandType::Del;
      replicated.strs.assign(command.arguments.begin() + 1,
                             command.arguments.end());
    } else {
      reply(resp::encode_error("ERR unknown command '" +
                               command.arguments.front() + "'"));
      return;
    }

    begin_write(std::move(replicated), std::move(reply));
  }

  std::uint64_t allocate_request_id() {
    const std::uint64_t allocated = next_request_id_++;
    if (next_request_id_ == 0) {
      next_request_id_ = 1;
    }
    return allocated == 0 ? allocate_request_id() : allocated;
  }

  void begin_write(Command command, resp::ReplyCallback reply) {
    const std::uint64_t request_id = allocate_request_id();
    command.origin_node = config_.id;
    command.request_id = request_id;
    pending_writes_.emplace(
        request_id,
        PendingWrite{std::move(reply), Clock::now() + kRequestTimeout});
    auto bytes = pack_command(command);

    boost::asio::post(
        raft_io_, [this, request_id, bytes = std::move(bytes)]() mutable {
          if (stopping_.load(std::memory_order_acquire)) {
            return;
          }
          const auto accepted = raft_.propose(bytes);
          if (!accepted.has_value()) {
            const std::string error = raft_.storage_healthy()
                                          ? not_leader_error()
                                          : "TRYAGAIN storage unavailable";
            boost::asio::post(store_io_, [this, request_id, error] {
              reject_write(request_id, error);
            });
          }
          after_raft_activity();
        });
  }

  void begin_read(std::string key, resp::ReplyCallback reply) {
    const std::uint64_t request_id = allocate_request_id();
    pending_reads_.emplace(
        request_id,
        PendingRead{std::move(key), std::move(reply),
                    Clock::now() + kRequestTimeout});

    boost::asio::post(raft_io_, [this, request_id] {
      if (stopping_.load(std::memory_order_acquire)) {
        return;
      }
      const auto read = raft_.read_index();
      if (!read.has_value()) {
        const std::string error = not_leader_error();
        boost::asio::post(store_io_, [this, request_id, error] {
          reject_read(request_id, error);
        });
      } else {
        raft_read_indexes_[request_id] = *read;
      }
      after_raft_activity();
    });
  }

  std::string not_leader_error() const {
    const auto leader = raft_.get_leader();
    return leader == 0 ? "NOT_LEADER UNKNOWN"
                       : "NOT_LEADER " + std::to_string(leader);
  }

  void reject_write(std::uint64_t request_id, const std::string &error) {
    const auto pending = pending_writes_.find(request_id);
    if (pending == pending_writes_.end()) {
      return;
    }
    auto reply = std::move(pending->second.reply);
    pending_writes_.erase(pending);
    reply(resp::encode_error(error));
  }

  void reject_read(std::uint64_t request_id, const std::string &error) {
    const auto pending = pending_reads_.find(request_id);
    if (pending == pending_reads_.end()) {
      return;
    }
    auto reply = std::move(pending->second.reply);
    pending_reads_.erase(pending);
    reply(resp::encode_error(error));
  }

  void poll_read_indexes() {
    if (raft_read_indexes_.empty()) {
      return;
    }
    if (raft_.get_state() != State::Leader) {
      const std::string error = not_leader_error();
      std::vector<std::uint64_t> rejected;
      rejected.reserve(raft_read_indexes_.size());
      for (const auto &pending : raft_read_indexes_) {
        rejected.push_back(pending.first);
      }
      raft_read_indexes_.clear();
      for (const auto request_id : rejected) {
        boost::asio::post(store_io_, [this, request_id, error] {
          reject_read(request_id, error);
        });
      }
      return;
    }

    std::optional<ReadIndexToken> ready_read;
    for (const auto &pending : raft_read_indexes_) {
      if (raft_.read_index_ready(pending.second)) {
        ready_read = pending.second;
        break;
      }
    }
    if (!ready_read.has_value()) {
      return;
    }

    std::vector<std::uint64_t> completed;
    for (auto pending = raft_read_indexes_.begin();
         pending != raft_read_indexes_.end();) {
      if (pending->second == *ready_read) {
        completed.push_back(pending->first);
        pending = raft_read_indexes_.erase(pending);
      } else {
        ++pending;
      }
    }
    raft_.finish_read_index(*ready_read);
    for (const auto request_id : completed) {
      boost::asio::post(store_io_, [this, request_id] {
        complete_read(request_id);
      });
    }
  }

  void complete_read(std::uint64_t request_id) {
    const auto pending = pending_reads_.find(request_id);
    if (pending == pending_reads_.end()) {
      return;
    }
    std::string value;
    const bool found = store_.get(pending->second.key, value);
    auto reply = std::move(pending->second.reply);
    pending_reads_.erase(pending);
    reply(found ? resp::encode_bulk_string(value)
                : resp::encode_nil_bulk_string());
  }

  void dispatch_next_committed_entry() {
    if (apply_in_flight_.has_value() ||
        stopping_.load(std::memory_order_acquire)) {
      return;
    }
    auto entry = raft_.next_entry_to_apply();
    if (!entry.has_value()) {
      return;
    }

    apply_in_flight_ = entry->index;
    boost::asio::post(
        store_io_, [this, entry = std::move(*entry)]() mutable {
          apply_committed_entry(std::move(entry));
        });
  }

  void apply_committed_entry(proto::Entry entry) {
    if (stopping_.load(std::memory_order_acquire)) {
      return;
    }
    if (entry.index <= store_applied_index_) {
      acknowledge_applied(entry.index);
      return;
    }
    if (entry.index != store_applied_index_ + 1) {
      request_stop("state-machine apply handoff contains an index gap");
      return;
    }

    try {
      if (entry.type == proto::EntryNormal && !entry.data.empty()) {
        const auto result = store_.apply(entry);
        if (result.origin_node == config_.id) {
          complete_write(result);
        }
      }
    } catch (const std::exception &error) {
      request_stop(std::string("state-machine apply failed: ") +
                   error.what());
      return;
    }

    store_applied_index_ = entry.index;
    acknowledge_applied(entry.index);
  }

  void acknowledge_applied(std::uint64_t index) {
    boost::asio::post(raft_io_, [this, index] {
      if (stopping_.load(std::memory_order_acquire)) {
        return;
      }
      if (index <= raft_.get_last_applied()) {
        return;
      }
      if (!apply_in_flight_.has_value() || index != *apply_in_flight_ ||
          index != raft_.get_last_applied() + 1) {
        request_stop("state-machine apply acknowledgment is out of order");
        return;
      }
      raft_.advance(index);
      if (raft_.get_last_applied() != index) {
        request_stop("state-machine apply acknowledgment was rejected");
        return;
      }
      apply_in_flight_.reset();
      after_raft_activity();
    });
  }

  void complete_write(const KVStore::ApplyResult &result) {
    const auto pending = pending_writes_.find(result.request_id);
    if (pending == pending_writes_.end()) {
      return;
    }
    auto reply = std::move(pending->second.reply);
    pending_writes_.erase(pending);
    if (result.type == CommandType::Set) {
      reply(resp::encode_simple_string("OK"));
    } else {
      reply(resp::encode_integer(
          static_cast<std::int64_t>(result.affected)));
    }
  }

  void expire_client_requests() {
    const auto now = Clock::now();
    std::vector<std::uint64_t> expired_writes;
    for (const auto &pending : pending_writes_) {
      if (pending.second.deadline <= now) {
        expired_writes.push_back(pending.first);
      }
    }
    for (const auto request_id : expired_writes) {
      reject_write(request_id, "TRYAGAIN write outcome unknown");
    }

    std::vector<std::uint64_t> expired_reads;
    for (const auto &pending : pending_reads_) {
      if (pending.second.deadline <= now) {
        expired_reads.push_back(pending.first);
      }
    }
    for (const auto request_id : expired_reads) {
      reject_read(request_id, "TRYAGAIN read quorum unavailable");
      boost::asio::post(raft_io_, [this, request_id] {
        cancel_raft_read(request_id);
      });
    }
  }

  void cancel_raft_read(std::uint64_t request_id) {
    const auto pending = raft_read_indexes_.find(request_id);
    if (pending == raft_read_indexes_.end()) {
      return;
    }
    const ReadIndexToken read = pending->second;
    raft_read_indexes_.erase(pending);
    const bool round_still_used =
        std::any_of(raft_read_indexes_.begin(), raft_read_indexes_.end(),
                    [read](const auto &other) {
                      return other.second == read;
                    });
    if (!round_still_used) {
      raft_.finish_read_index(read);
    }
  }

  void maybe_capture_snapshot() {
    if (snapshot_capture_pending_ || !raft_.should_snapshot()) {
      return;
    }
    snapshot_capture_pending_ = true;
    boost::asio::post(store_io_, [this] {
      try {
        const auto index = store_applied_index_;
        auto state = store_.serialize();
        boost::asio::post(
            raft_io_, [this, index, state = std::move(state)]() mutable {
              snapshot_capture_pending_ = false;
              if (stopping_.load(std::memory_order_acquire)) {
                return;
              }
              raft_.take_snapshot(index, state);
              after_raft_activity();
            });
      } catch (const std::exception &error) {
        request_stop(std::string("snapshot serialization failed: ") +
                     error.what());
      }
    });
  }

  bool restore_snapshot(std::uint64_t index,
                        const std::vector<std::uint8_t> &snapshot) {
    if (apply_in_flight_.has_value() ||
        stopping_.load(std::memory_order_acquire)) {
      return false;
    }
    auto completion = std::make_shared<std::promise<bool>>();
    auto future = completion->get_future();
    auto state = std::make_shared<std::vector<std::uint8_t>>(snapshot);
    boost::asio::post(store_io_, [this, index, completion, state] {
      try {
        store_.deserialize(*state);
        store_applied_index_ = index;
        completion->set_value(true);
      } catch (...) {
        completion->set_value(false);
      }
    });
    if (future.wait_for(kSnapshotRestoreTimeout) !=
        std::future_status::ready) {
      request_stop("snapshot restore timed out");
      return false;
    }
    return future.get();
  }

  void log_status_if_changed(bool force) {
    const State state = raft_.get_state();
    const auto term = raft_.get_term();
    const auto leader = raft_.get_leader();
    if (!force && state == logged_state_ && term == logged_term_ &&
        leader == logged_leader_) {
      return;
    }
    logged_state_ = state;
    logged_term_ = term;
    logged_leader_ = leader;
    std::cout << "node " << config_.id << " state=" << state_name(state)
              << " term=" << term << " leader=";
    if (leader == 0) {
      std::cout << "unknown";
    } else {
      std::cout << leader;
    }
    std::cout << std::endl;
  }

  void request_stop(std::string reason) {
    if (!reason.empty()) {
      exit_code_.store(1, std::memory_order_release);
      std::cerr << "node " << config_.id << " fatal: " << reason << std::endl;
    }
    bool expected = false;
    if (!stopping_.compare_exchange_strong(expected, true,
                                           std::memory_order_acq_rel)) {
      return;
    }
    boost::asio::post(raft_io_, [this] { stop_on_raft_loop(); });
  }

  void stop_on_raft_loop() {
    boost::system::error_code ignored;
    tick_timer_.cancel();
    signals_.cancel(ignored);

    boost::asio::post(store_io_, [this] {
      request_timer_.cancel();
      for (auto &pending : pending_writes_) {
        pending.second.reply(resp::encode_error("TRYAGAIN node stopping"));
      }
      pending_writes_.clear();
      for (auto &pending : pending_reads_) {
        pending.second.reply(resp::encode_error("TRYAGAIN node stopping"));
      }
      pending_reads_.clear();
      client_server_->stop();
      store_work_.reset();
    });

    boost::asio::post(peer_io_, [this] {
      peer_server_->stop();
      for (const auto &peer : peers_) {
        peer.second->stop();
      }
      peer_work_.reset();
    });
    raft_work_.reset();
  }

  void run_context(const char *name, boost::asio::io_context &context) {
    try {
      context.run();
    } catch (const std::exception &error) {
      exit_code_.store(1, std::memory_order_release);
      std::cerr << "node " << config_.id << ' ' << name
                << " loop failed: " << error.what() << std::endl;
      stopping_.store(true, std::memory_order_release);
      raft_io_.stop();
      peer_io_.stop();
      store_io_.stop();
    }
  }

  NodeConfig config_;
  boost::asio::io_context raft_io_;
  boost::asio::io_context peer_io_;
  boost::asio::io_context store_io_;
  std::optional<WorkGuard> raft_work_;
  std::optional<WorkGuard> peer_work_;
  std::optional<WorkGuard> store_work_;

  Raft raft_;
  KVStore store_;
  boost::asio::steady_timer tick_timer_;
  boost::asio::steady_timer request_timer_;
  boost::asio::signal_set signals_;
  std::map<std::uint64_t, PeerPtr> peers_;
  ServerPtr peer_server_;
  std::shared_ptr<resp::RespServer> client_server_;

  std::unordered_map<std::uint64_t, PendingWrite> pending_writes_;
  std::unordered_map<std::uint64_t, PendingRead> pending_reads_;
  std::unordered_map<std::uint64_t, ReadIndexToken> raft_read_indexes_;
  std::uint64_t next_request_id_;
  std::uint64_t store_applied_index_ = 0;
  std::optional<std::uint64_t> apply_in_flight_;
  bool snapshot_capture_pending_ = false;

  State logged_state_ = State::Follower;
  std::uint64_t logged_term_ = std::numeric_limits<std::uint64_t>::max();
  std::uint64_t logged_leader_ = std::numeric_limits<std::uint64_t>::max();
  std::atomic<bool> stopping_{false};
  std::atomic<int> exit_code_{0};
};

NodeRuntime::NodeRuntime(NodeConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}

NodeRuntime::~NodeRuntime() = default;

int NodeRuntime::run() { return impl_->run(); }

}  // namespace kv::app
