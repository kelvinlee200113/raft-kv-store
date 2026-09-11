#pragma once
#include <raft/config.h>
#include <raft/proto.h>
#include <wal/wal.h>
#include <stdint.h>
#include <unordered_map>
#include <vector>
#include <optional>
#include <random>
#include <functional>

namespace kv {

// Raft node states
enum class State { Follower, PreCandidate, Candidate, Leader };

struct ReadIndexToken {
  uint64_t context = 0;
  uint64_t safe_index = 0;

  bool operator==(const ReadIndexToken &other) const {
    return context == other.context && safe_index == other.safe_index;
  }
};

// Progress tracks replication progress for each follower
struct Progress {
  uint64_t match; // Highest log index known to be replicated on this follower
  uint64_t next;  // Next log index to send to this follower

  // Logs are 1-indexed
  Progress() : match(0), next(1) {}
};

class Raft {
public:
  explicit Raft(const Config &config);

  void become_follower(uint64_t term, uint64_t leader);

  void become_pre_candidate();

  void become_candidate();

  void become_leader();

  void tick();

  void reset_randomized_election_timeout();

  proto::Message handle_request_vote(const proto::Message &msg);

  void handle_request_vote_response(const proto::Message &msg);

  void campaign();

  void pre_campaign();

  proto::Message handle_pre_vote(const proto::Message &msg);

  void handle_pre_vote_response(const proto::Message &msg);

  void send(proto::Message msg);

  std::vector<proto::Message> read_messages();

  void broadcast_heartbeat();

  proto::Message handle_append_entries(const proto::Message &msg);

  void handle_append_entries_response(const proto::Message &msg);

  proto::Message handle_install_snapshot(const proto::Message &msg);

  void handle_install_snapshot_response(const proto::Message &msg);

  std::optional<uint64_t> propose(const std::vector<uint8_t> &data);

  // Return the next committed entry until the state-machine owner advances it.
  std::optional<proto::Entry> next_entry_to_apply() const;
  void advance(uint64_t index);

  // Snapshot: Compact log by taking a snapshot of the state machine
  // state_snapshot: serialized state machine bytes (from KVStore::serialize())
  void take_snapshot(const std::vector<uint8_t>& state_snapshot);
  void take_snapshot(uint64_t applied_index,
                     const std::vector<uint8_t> &state_snapshot);

  // Check if a snapshot should be taken (threshold crossed)
  // Returns true if (last_applied - last_snapshot_index) >= threshold
  bool should_snapshot() const;

  // ReadIndex: Linearizable reads without going through the log.
  // The token identifies both the quorum round and its applied-index fence.
  std::optional<ReadIndexToken> read_index();

  // Check if ReadIndex confirmation is ready (majority responded to heartbeat)
  bool read_index_ready(const ReadIndexToken &read);

  // Release the active ReadIndex round after all coalesced callers have read.
  void finish_read_index(const ReadIndexToken &read);

  // Attach a WAL for crash recovery (optional — tests may omit this)
  void set_wal(std::unique_ptr<wal::WAL> w) {
    wal_ = std::move(w);
    storage_failed_ = false;
  }
  bool storage_healthy() const { return !storage_failed_; }

  void set_snapshot_restore_callback(
      std::function<bool(uint64_t, const std::vector<uint8_t> &)> callback) {
    snapshot_restore_ = std::move(callback);
  }

  // Restore Raft state from WAL recovery (call once at startup, before event loop)
  // Loads log entries and HardState atomically. last_applied stays at 0 —
  // caller must replay entries [1..commit_index] into the state machine.
  void restore(const wal::HardStateProto& hard_state, const std::vector<proto::Entry>& entries);
  void restore(const wal::HardStateProto &hard_state,
               const std::vector<proto::Entry> &entries,
               const wal::SnapshotMeta &snapshot);

  // Test helpers: For testing only
  void test_set_commit_index(uint64_t index) { commit_index_ = index; }
  void test_append_log_entry(const proto::Entry& entry) { log_.push_back(entry); }
  size_t test_get_log_size() const { return log_.size(); }

  uint64_t get_term() const { return term_; }
  uint64_t get_id() const { return id_; }
  uint64_t get_leader() const { return lead_; }
  uint64_t get_voted_for() const { return voted_for_; }
  uint64_t get_commit_index() const { return commit_index_; }
  uint64_t get_last_applied() const { return last_applied_; }
  State get_state() const { return state_; }
  const std::vector<proto::Entry> &get_log() const { return log_; }
  uint64_t get_log_offset() const { return log_offset_; }
  const std::unordered_map<uint64_t, Progress> &get_progress() const {
    return progress_;
  }
  const std::vector<uint64_t> &get_peers() const { return peers_; }
  uint32_t get_election_timeout() const { return election_timeout_; }
  uint32_t get_heartbeat_timeout() const { return heartbeat_timeout_; }
  uint32_t get_election_elapsed() const { return election_elapsed_; }
  uint32_t get_randomized_election_timeout() const {
    return randomized_election_timeout_;
  }
  const std::unordered_map<uint64_t, bool> &get_votes() const { return votes_; }

private:
  // Log index helpers (account for log_offset_ after compaction)
  uint64_t last_log_index() const { return log_offset_ + log_.size(); }
  const proto::Entry& log_entry(uint64_t index) const { return log_[index - log_offset_ - 1]; }
  uint64_t term_at(uint64_t index) const;
  uint64_t last_log_term() const {
    return log_.empty() ? log_offset_term_ : log_.back().term;
  }
  bool is_log_up_to_date(uint64_t candidate_index,
                         uint64_t candidate_term) const;
  bool is_member(uint64_t node_id) const;
  bool prepare_for_leader_rpc(const proto::Message &msg);
  proto::Message make_install_snapshot(uint64_t peer_id) const;
  bool has_committed_entry_in_current_term() const;
  bool sync_wal();

  uint64_t id_;
  uint64_t term_;
  uint64_t lead_;
  uint64_t voted_for_;
  uint64_t commit_index_;
  uint64_t last_applied_;
  State state_;
  std::vector<proto::Entry> log_;
  uint64_t log_offset_;  // Index of last compacted entry (0 = nothing compacted)
  uint64_t log_offset_term_;
  std::unordered_map<uint64_t, Progress> progress_;
  std::vector<uint64_t> peers_;

  // Timeout configuration
  uint32_t election_timeout_;  // Base election timeout in ticks
  uint32_t heartbeat_timeout_; // Heartbeat interval in ticks

  // Snapshot configuration
  uint64_t snapshot_threshold_;   // Take snapshot every N applied entries (0 = disabled)
  uint64_t last_snapshot_index_;  // Index of last snapshot taken

  // Election timing
  uint32_t election_elapsed_;            // Ticks since last reset
  uint32_t randomized_election_timeout_; // Random timeout for this election
  uint32_t heartbeat_elapsed_;           // Ticks since last heartbeat
  std::mt19937_64 election_rng_;

  // Voting
  std::unordered_map<uint64_t, bool>
      votes_; // Track votes received (node_id -> granted)
  std::unordered_map<uint64_t, bool>
      pre_votes_; // Track pre-votes received (node_id -> granted)
  uint64_t pending_pre_vote_term_;

  // ReadIndex state
  bool read_index_pending_;               // Is there a pending ReadIndex request?
  uint64_t pending_read_index_;           // Commit index when read was requested
  uint64_t pending_read_context_;
  uint64_t next_read_context_;
  std::unordered_map<uint64_t, bool> read_index_acks_;  // Track which peers acked

  // Snapshot cache (for InstallSnapshot RPC)
  std::vector<uint8_t> last_snapshot_data_;  // Cached snapshot data to send to followers
  std::function<bool(uint64_t, const std::vector<uint8_t> &)>
      snapshot_restore_;

  // WAL for crash recovery (nullptr if not attached)
  std::unique_ptr<wal::WAL> wal_;
  bool storage_failed_;

  // Outgoing messages queue
  std::vector<proto::Message> msgs_;
};

} // namespace kv
