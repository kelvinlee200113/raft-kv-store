#pragma once
#include <raft/config.h>
#include <raft/proto.h>
#include <stdint.h>
#include <unordered_map>
#include <vector>

namespace kv {

// Raft node states
enum class State { Follower, PreCandidate, Candidate, Leader };

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

  void propose(const std::vector<uint8_t> &data);

  std::vector<proto::Entry> get_entries_to_apply();

  void advance(uint64_t index);

  // ReadIndex: Linearizable reads without going through the log
  // Returns the commit index that can be safely read once confirmed
  uint64_t read_index();

  // Check if ReadIndex confirmation is ready (majority responded to heartbeat)
  bool read_index_ready(uint64_t read_index);

  uint64_t get_term() const { return term_; }
  uint64_t get_id() const { return id_; }
  uint64_t get_leader() const { return lead_; }
  uint64_t get_voted_for() const { return voted_for_; }
  uint64_t get_commit_index() const { return commit_index_; }
  uint64_t get_last_applied() const { return last_applied_; }
  State get_state() const { return state_; }
  const std::vector<proto::Entry> &get_log() const { return log_; }
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
  uint64_t id_;
  uint64_t term_;
  uint64_t lead_;
  uint64_t voted_for_;
  uint64_t commit_index_;
  uint64_t last_applied_;
  State state_;
  std::vector<proto::Entry> log_;
  std::unordered_map<uint64_t, Progress> progress_;
  std::vector<uint64_t> peers_;

  // Timeout configuration
  uint32_t election_timeout_;  // Base election timeout in ticks
  uint32_t heartbeat_timeout_; // Heartbeat interval in ticks

  // Election timing
  uint32_t election_elapsed_;            // Ticks since last reset
  uint32_t randomized_election_timeout_; // Random timeout for this election
  uint32_t heartbeat_elapsed_;           // Ticks since last heartbeat

  // Voting
  std::unordered_map<uint64_t, bool>
      votes_; // Track votes received (node_id -> granted)
  std::unordered_map<uint64_t, bool>
      pre_votes_; // Track pre-votes received (node_id -> granted)

  // ReadIndex state
  bool read_index_pending_;               // Is there a pending ReadIndex request?
  uint64_t pending_read_index_;           // Commit index when read was requested
  std::unordered_map<uint64_t, bool> read_index_acks_;  // Track which peers acked

  // Outgoing messages queue
  std::vector<proto::Message> msgs_;
};

} // namespace kv
