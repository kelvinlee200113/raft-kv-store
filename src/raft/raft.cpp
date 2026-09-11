#include "raft/proto.h"
#include <algorithm>
#include <cstdint>
#include <chrono>
#include <set>
#include <stdexcept>
#include <raft/raft.h>

namespace {

uint64_t election_seed(const kv::Config &config) {
  if (config.random_seed != 0) {
    return config.random_seed;
  }
  std::random_device device;
  const auto now = static_cast<uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());
  return (static_cast<uint64_t>(device()) << 32U) ^ device() ^ now ^
         (config.id * 0x9e3779b97f4a7c15ULL);
}

} // namespace

namespace kv {

Raft::Raft(const Config &config)
    : id_(config.id), term_(0), lead_(0), voted_for_(0), commit_index_(0),
      last_applied_(0), state_(State::Follower), log_offset_(0),
      log_offset_term_(0), peers_(config.peers),
      election_timeout_(config.election_tick),
      heartbeat_timeout_(config.heartbeat_tick),
      snapshot_threshold_(config.snapshot_threshold), last_snapshot_index_(0),
      election_elapsed_(0),
      randomized_election_timeout_(0), heartbeat_elapsed_(0),
      election_rng_(election_seed(config)),
      pending_pre_vote_term_(0),
      read_index_pending_(false), pending_read_index_(0),
      pending_read_context_(0), next_read_context_(1),
      storage_failed_(false) {

  const std::set<uint64_t> unique_peers(peers_.begin(), peers_.end());
  if (id_ == 0 || election_timeout_ == 0 || heartbeat_timeout_ == 0 ||
      peers_.size() != 3 || unique_peers.size() != peers_.size() ||
      std::find(peers_.begin(), peers_.end(), 0) != peers_.end() ||
      std::find(peers_.begin(), peers_.end(), id_) == peers_.end()) {
    throw std::invalid_argument("invalid Raft configuration");
  }

  // Generate random election timeout (between election_timeout to
  // 2*election_timeout)
  reset_randomized_election_timeout();
}

void Raft::restore(const wal::HardStateProto& hard_state, const std::vector<proto::Entry>& entries) {
  restore(hard_state, entries, wal::SnapshotMeta{});
}

void Raft::restore(const wal::HardStateProto &hard_state,
                   const std::vector<proto::Entry> &entries,
                   const wal::SnapshotMeta &snapshot) {
  log_offset_ = snapshot.index;
  log_offset_term_ = snapshot.term;
  last_snapshot_index_ = snapshot.index;
  last_snapshot_data_ = snapshot.state;
  log_.clear();
  uint64_t expected_index = log_offset_ + 1;
  for (const auto &entry : entries) {
    if (entry.index > log_offset_) {
      if (entry.index != expected_index) {
        throw std::invalid_argument("recovered Raft log contains an index gap");
      }
      log_.push_back(entry);
      ++expected_index;
    }
  }
  term_ = hard_state.term;
  voted_for_ = hard_state.vote;
  commit_index_ = std::max(snapshot.index, hard_state.commit);
  if (commit_index_ > last_log_index()) {
    throw std::invalid_argument(
        "recovered Raft log is missing a committed entry");
  }
  last_applied_ = snapshot.index;
}

uint64_t Raft::term_at(uint64_t index) const {
  if (index == 0) {
    return 0;
  }
  if (index == log_offset_) {
    return log_offset_term_;
  }
  if (index < log_offset_ || index > last_log_index()) {
    return 0;
  }
  return log_entry(index).term;
}

bool Raft::sync_wal() {
  if (!wal_ || wal_->sync()) {
    return true;
  }
  storage_failed_ = true;
  return false;
}

void Raft::become_follower(uint64_t term, uint64_t leader) {
  if (term < term_) {
    return;
  }
  const bool advanced_term = term > term_;
  state_ = State::Follower;
  term_ = term;
  lead_ = leader;
  if (advanced_term) {
    voted_for_ = 0;
  }
  read_index_pending_ = false;
  pending_read_index_ = 0;
  pending_read_context_ = 0;
  read_index_acks_.clear();
  pending_pre_vote_term_ = 0;
  reset_randomized_election_timeout();

  if (advanced_term && wal_) {
    wal_->save_hard_state({term_, voted_for_, commit_index_});
    sync_wal();
  }
}

bool Raft::is_log_up_to_date(uint64_t candidate_index,
                             uint64_t candidate_term) const {
  const uint64_t local_term = last_log_term();
  if (candidate_term != local_term) {
    return candidate_term > local_term;
  }
  return candidate_index >= last_log_index();
}

bool Raft::is_member(uint64_t node_id) const {
  return node_id != 0 &&
         std::find(peers_.begin(), peers_.end(), node_id) != peers_.end();
}

bool Raft::prepare_for_leader_rpc(const proto::Message &msg) {
  if (msg.to != id_ || msg.from == id_ || !is_member(msg.from)) {
    return false;
  }
  if (msg.term > term_) {
    become_follower(msg.term, msg.from);
  }
  if (storage_failed_ || msg.term < term_) {
    return false;
  }
  if (state_ != State::Follower) {
    become_follower(msg.term, msg.from);
  }
  reset_randomized_election_timeout();
  lead_ = msg.from;
  return true;
}

bool Raft::has_committed_entry_in_current_term() const {
  return commit_index_ > 0 && term_at(commit_index_) == term_;
}

void Raft::become_pre_candidate() {
  state_ = State::PreCandidate;
  // Don't increment term in PreVote!
  lead_ = 0;
  pre_votes_.clear();
  pending_pre_vote_term_ = 0;
  reset_randomized_election_timeout();
}

void Raft::become_candidate() {
  state_ = State::Candidate;
  term_++;
  lead_ = 0;
  voted_for_ = id_;
  votes_.clear();
  pending_pre_vote_term_ = 0;
  reset_randomized_election_timeout();

  // WAL-first: persist new term + self-vote before campaign() sends messages
  if (wal_) {
    wal_->save_hard_state({term_, voted_for_, commit_index_});
    sync_wal();
  }
}

void Raft::become_leader() {
  state_ = State::Leader;
  lead_ = id_;
  heartbeat_elapsed_ = 0; // Reset heartbeat timer

  // Remove stale data
  progress_.clear();

  for (uint64_t peer_id : peers_) {
    if (peer_id == id_) {
      continue;
    }
    Progress progress;
    progress.next = last_log_index() + 1;
    progress.match = 0;
    progress_[peer_id] = progress;
  }

  // Send initial heartbeat immediately
  broadcast_heartbeat();
}

void Raft::tick() {
  if (storage_failed_) {
    return;
  }
  if (state_ == State::Leader) {
    // Leader sends periodic heartbeats
    heartbeat_elapsed_++;
    if (heartbeat_elapsed_ >= heartbeat_timeout_) {
      heartbeat_elapsed_ = 0;
      broadcast_heartbeat();
    }
  } else {
    // Followers and candidates track election timeout
    election_elapsed_++;

    // Check if election timeout has passed
    if (election_elapsed_ >= randomized_election_timeout_) {
      election_elapsed_ = 0;

      // Followers start with PreVote, PreCandidates and Candidates re-campaign
      if (state_ == State::Follower) {
        become_pre_candidate();
        pre_campaign(); // Send PreVote messages to all peers
      } else if (state_ == State::PreCandidate) {
        // PreVote failed, retry
        become_pre_candidate();
        pre_campaign();
      } else if (state_ == State::Candidate) {
        // Real vote failed, go back to PreVote
        become_pre_candidate();
        pre_campaign();
      }
    }
  }
}

void Raft::reset_randomized_election_timeout() {
  election_elapsed_ = 0;
  std::uniform_int_distribution<uint32_t> jitter(0, election_timeout_ - 1);
  randomized_election_timeout_ = election_timeout_ + jitter(election_rng_);
}

proto::Message Raft::handle_request_vote(const proto::Message &msg) {
  proto::Message response;
  response.type = proto::MsgRequestVoteResponse;
  response.from = id_;
  response.to = msg.from;
  response.vote_granted = false;

  if (msg.to != id_ || msg.from == id_ || !is_member(msg.from)) {
    response.term = term_;
    return response;
  }

  // Update term if candidate's term is higher
  if (msg.term > term_) {
    become_follower(msg.term, 0);
  }

  if (storage_failed_) {
    response.term = term_;
    return response;
  }

  // Check if should grant vote
  if (msg.term == term_ && (voted_for_ == 0 || voted_for_ == msg.from) &&
      is_log_up_to_date(msg.last_log_index, msg.last_log_term)) {
    response.vote_granted = true;
    voted_for_ = msg.from;
    reset_randomized_election_timeout();

    // WAL-first: persist vote before sending response
    if (wal_) {
      wal_->save_hard_state({term_, voted_for_, commit_index_});
      if (!sync_wal()) {
        response.vote_granted = false;
      }
    }
  }

  // Set response term
  response.term = term_;

  return response;
}

void Raft::handle_request_vote_response(const proto::Message &msg) {
  if (msg.to != id_ || msg.from == id_ || !is_member(msg.from)) {
    return;
  }
  // If response is from higher term, step down
  if (msg.term > term_) {
    become_follower(msg.term, 0);
    return;
  }
  if (msg.term < term_ || state_ != State::Candidate) {
    return;
  }

  // Record granted vote
  if (msg.vote_granted) {
    votes_[msg.from] = true;
  }

  // Count total votes
  uint64_t total_votes = 0;
  for (const auto &vote : votes_) {
    if (vote.second) {
      total_votes++;
    }
  }

  // If majority, become leader
  if (total_votes > peers_.size() / 2) {
    become_leader();
  }
}

// Handle PreVote request (like RequestVote but doesn't increment term)
proto::Message Raft::handle_pre_vote(const proto::Message &msg) {
  proto::Message response;
  response.type = proto::MsgPreVoteResponse;
  response.from = id_;
  response.to = msg.from;
  response.pre_vote_term = msg.term;
  response.vote_granted = false;

  if (msg.to != id_ || msg.from == id_ || !is_member(msg.from)) {
    response.term = term_;
    return response;
  }

  if (storage_failed_ || msg.term <= term_) {
    response.term = term_;
    return response;
  }

  // Reject if we have a leader and it's still sending heartbeats
  // (election_elapsed_ is low)
  if (lead_ != 0 && election_elapsed_ < election_timeout_) {
    response.term = term_;
    return response; // vote_granted = false
  }

  // Grant pre-vote if candidate's log is at least as up-to-date as ours
  // Same logic as RequestVote
  if (is_log_up_to_date(msg.last_log_index, msg.last_log_term)) {
    response.vote_granted = true;
  }

  response.term = term_;
  return response;
}

// Handle PreVote response
void Raft::handle_pre_vote_response(const proto::Message &msg) {
  if (msg.to != id_ || msg.from == id_ || !is_member(msg.from)) {
    return;
  }
  if (msg.term > term_) {
    become_follower(msg.term, 0);
    return;
  }
  // Ensure we are still pre-candidate
  if (state_ != State::PreCandidate) {
    return;
  }

  if (msg.pre_vote_term == 0 ||
      msg.pre_vote_term != pending_pre_vote_term_) {
    return;
  }

  // Record granted pre-vote
  if (msg.vote_granted) {
    pre_votes_[msg.from] = true;
  }

  // Count total pre-votes
  uint64_t total_pre_votes = 0;
  for (const auto &vote : pre_votes_) {
    if (vote.second) {
      total_pre_votes++;
    }
  }

  // If majority, transition to real candidate and start real election
  if (total_pre_votes > peers_.size() / 2) {
    become_candidate();
    campaign();
  }
}

// PreCandidate pre-campaigning for itself
void Raft::pre_campaign() {
  if (storage_failed_) {
    return;
  }
  // Record pre-vote for self
  pending_pre_vote_term_ = term_ + 1;
  pre_votes_[id_] = true;
  for (uint64_t peer_id : peers_) {
    if (peer_id != id_) {
      // Ask whether peers would vote for us in the next term without changing
      // our local term yet.
      proto::Message msg;
      msg.type = proto::MsgPreVote;
      msg.from = id_;
      msg.to = peer_id;
      msg.term = pending_pre_vote_term_;
      msg.last_log_index = last_log_index();
      msg.last_log_term = last_log_term();
      msgs_.push_back(msg);
    }
  }
}

// Candidate campaigning for itself
void Raft::campaign() {
  if (storage_failed_) {
    return;
  }
  // Record vote for self
  votes_[id_] = true;
  for (uint64_t peer_id : peers_) {
    if (peer_id != id_) {
      // Create RequestVote message
      proto::Message msg;
      msg.type = proto::MsgRequestVote;
      msg.from = id_;
      msg.to = peer_id;
      msg.term = term_;
      msg.last_log_index = last_log_index();
      msg.last_log_term = last_log_term();
      msgs_.push_back(msg);
    }
  }
}

void Raft::send(proto::Message msg) { msgs_.push_back(msg); }

std::vector<proto::Message> Raft::read_messages() {
  std::vector<proto::Message> msgs;
  msgs.swap(msgs_);
  return msgs;
}

proto::Message Raft::make_install_snapshot(uint64_t peer_id) const {
  proto::Message message;
  message.type = proto::MsgInstallSnapshot;
  message.from = id_;
  message.to = peer_id;
  message.term = term_;
  message.snapshot_index = log_offset_;
  message.snapshot_term = log_offset_term_;
  message.snapshot_data = last_snapshot_data_;
  return message;
}

// Broadcast AppendEntries message (empty for heartbeats)
void Raft::broadcast_heartbeat() {
  for (uint64_t peer_id : peers_) {
    if (peer_id != id_) {
      uint64_t next_index = progress_[peer_id].next;

      // Check if this peer needs a snapshot (next index is already compacted)
      if (next_index <= log_offset_) {
        msgs_.push_back(make_install_snapshot(peer_id));
        continue;
      }

      // Send AppendEntries message
      proto::Message msg;
      msg.type = proto::MsgAppendEntries;
      msg.from = id_;
      msg.to = peer_id;
      msg.term = term_;

      uint64_t prev_index = next_index - 1;
      msg.prev_log_index = prev_index;
      if (prev_index == 0) {
        msg.prev_log_term = 0;
      } else {
        msg.prev_log_term = term_at(prev_index);
      }

      msg.entries.clear();
      for (uint64_t i = next_index; i <= last_log_index(); ++i) {
        msg.entries.push_back(log_entry(i));
      }

      msg.leader_commit = commit_index_;
      msg.read_context =
          read_index_pending_ ? pending_read_context_ : 0;
      msgs_.push_back(msg);
    }
  }
}

proto::Message Raft::handle_append_entries(const proto::Message &msg) {
  proto::Message response;
  response.type = proto::MsgAppendEntriesResponse;
  response.from = id_;
  response.to = msg.from;
  response.success = false;
  response.read_context = msg.read_context;

  if (!prepare_for_leader_rpc(msg)) {
    response.term = term_;
    return response;
  }

  // Log consistency check
  bool log_ok = false;

  if (msg.prev_log_index == 0) {
    log_ok = true;
  } else if (msg.prev_log_index >= log_offset_ &&
             last_log_index() >= msg.prev_log_index) {
    log_ok = term_at(msg.prev_log_index) == msg.prev_log_term;
  }

  response.term = term_;

  if (!log_ok) {
    response.match_index = 0;
    return response;
  }

  // Append entries to follower's log (starting from the match_index + 1)
  for (uint64_t i = 0; i < msg.entries.size(); ++i) {
    uint64_t index = msg.prev_log_index + i + 1;

    if (index <= log_offset_) {
      continue;
    }
    if (index <= last_log_index()) {
      if (term_at(index) != msg.entries[i].term) {
        if (index <= get_commit_index()) {
          response.match_index = get_commit_index();
          return response;
        }
        // Conflict: truncate from this index onward, then append
        log_.erase(log_.begin() + (index - log_offset_ - 1), log_.end());
        log_.push_back(msg.entries[i]);

        // WAL: persist the new entry after conflict resolution
        if (wal_) {
          wal_->save_entry(msg.entries[i]);
        }
      }
    } else {
      log_.push_back(msg.entries[i]);

      // WAL: persist each new entry as it's appended
      if (wal_) {
        wal_->save_entry(msg.entries[i]);
      }
    }
  }

  // WAL: flush all entries in one sync (batched)
  if (wal_ && !msg.entries.empty()) {
    if (!sync_wal()) {
      response.term = term_;
      return response;
    }
  }

  response.success = true;
  response.match_index = msg.prev_log_index + msg.entries.size();

  // Update commit index based on leader's commit
  if (msg.leader_commit > commit_index_) {
    commit_index_ = std::min(msg.leader_commit, last_log_index());

    // WAL: persist new commit index
    if (wal_) {
      wal_->save_hard_state({term_, voted_for_, commit_index_});
      if (!sync_wal()) {
        response.success = false;
        response.term = term_;
        return response;
      }
    }
  }

  return response;
}

void Raft::handle_append_entries_response(const proto::Message &msg) {
  if (msg.to != id_ || msg.from == id_ || !is_member(msg.from)) {
    return;
  }
  if (msg.term > term_) {
    become_follower(msg.term, 0);
    return;
  }
  if (storage_failed_) {
    return;
  }
  if (state_ != State::Leader) {
    return;
  }

  if (msg.term < term_) {
    return;
  }

  if (msg.success) {
    progress_[msg.from].match = msg.match_index;
    progress_[msg.from].next = progress_[msg.from].match + 1;

    // Track successful heartbeat response for ReadIndex
    if (read_index_pending_ && msg.read_context != 0 &&
        msg.read_context == pending_read_context_) {
      read_index_acks_[msg.from] = true;
    }

    // Try to advance commit index
    const uint64_t old_commit = commit_index_;

    // Check each index from commit_index + 1 to last log index
    for (uint64_t i = commit_index_ + 1; i <= last_log_index(); ++i) {
      // Only commit entries from current term
      if (log_entry(i).term != term_) {
        continue;
      }

      // Count how many nodes have replicated this entry
      uint64_t replicas = 1; // Count self
      for (const auto &pair : progress_) {
        if (pair.second.match >= i) {
          replicas++;
        }
      }

      // If majority has replicated, commit it
      if (replicas > peers_.size() / 2) {
        commit_index_ = i;
      }
    }

    if (commit_index_ > old_commit) {
      // WAL: persist new commit index before applying
      if (wal_) {
        wal_->save_hard_state({term_, voted_for_, commit_index_});
        if (!sync_wal()) {
          return;
        }
      }
    }

  } else {
    // AppendEntries failed - follower doesn't have matching log entry
    // Decrement next and retry
    auto &progress = progress_[msg.from];
    if (progress.next > 1) {
      --progress.next;
    }

    // Check if follower needs a snapshot (next index is already compacted)
    if (log_offset_ > 0 && progress.next <= log_offset_) {
      msgs_.push_back(make_install_snapshot(msg.from));
    } else {
      // Follower just needs earlier entries - retry with AppendEntries
      broadcast_heartbeat();
    }
  }
}

std::optional<uint64_t> Raft::propose(const std::vector<uint8_t> &data) {
  if (state_ != State::Leader || storage_failed_) {
    return std::nullopt;
  }

  proto::Entry entry;
  entry.type = proto::EntryNormal;
  entry.data = data;
  entry.index = last_log_index() + 1;
  entry.term = term_;

  log_.push_back(entry);

  // WAL: persist entry before broadcasting to followers
  if (wal_) {
    wal_->save_entry(entry);
    if (!sync_wal()) {
      log_.pop_back();
      return std::nullopt;
    }
  }

  broadcast_heartbeat();
  return entry.index;
}

std::optional<proto::Entry> Raft::next_entry_to_apply() const {
  if (last_applied_ >= commit_index_) {
    return std::nullopt;
  }
  return log_entry(last_applied_ + 1);
}

void Raft::advance(uint64_t index) {
  if (index > commit_index_ || index != last_applied_ + 1) {
    return;
  }
  last_applied_ = index;
}

bool Raft::should_snapshot() const {
  return snapshot_threshold_ > 0 && last_applied_ >= last_snapshot_index_ &&
         (last_applied_ - last_snapshot_index_) >= snapshot_threshold_;
}

void Raft::take_snapshot(const std::vector<uint8_t>& state_snapshot) {
  take_snapshot(last_applied_, state_snapshot);
}

void Raft::take_snapshot(uint64_t applied_index,
                         const std::vector<uint8_t> &state_snapshot) {
  if (applied_index == 0 || applied_index <= log_offset_ ||
      applied_index > last_applied_) {
    return;  // Nothing to snapshot
  }

  uint64_t snap_term = term_at(applied_index);

  // Create snapshot metadata
  wal::SnapshotMeta snap{applied_index, snap_term, state_snapshot};

  // WAL-first: persist snapshot before truncating log
  if (wal_) {
    wal_->save_snapshot(snap);
    if (!sync_wal()) {
      return;
    }
  }

  // Truncate log: keep only entries newer than the captured state.
  // Find array position of first entry to keep
  size_t keep_from = 0;
  for (size_t i = 0; i < log_.size(); ++i) {
    if (log_[i].index > applied_index) {
      keep_from = i;
      break;
    }
  }

  // Erase everything before keep_from
  if (keep_from > 0) {
    log_.erase(log_.begin(), log_.begin() + keep_from);
  } else if (!log_.empty() && log_.back().index <= applied_index) {
    // All entries are <= last_applied_, clear the entire log
    log_.clear();
  }

  // Update offset
  log_offset_ = applied_index;
  log_offset_term_ = snap_term;
  last_snapshot_index_ = applied_index;

  // Cache snapshot data for InstallSnapshot RPC
  last_snapshot_data_ = state_snapshot;
}

// ReadIndex: Initiate a linearizable read
// Returns a token containing the quorum-round identity and applied-index fence.
std::optional<ReadIndexToken> Raft::read_index() {
  // Only leader can serve ReadIndex
  if (state_ != State::Leader || storage_failed_) {
    return std::nullopt;
  }

  if (read_index_pending_) {
    return ReadIndexToken{pending_read_context_, pending_read_index_};
  }

  read_index_pending_ = true;
  pending_read_context_ = next_read_context_++;
  if (next_read_context_ == 0) {
    next_read_context_ = 1;
  }
  read_index_acks_.clear(); // Clear previous acks

  if (!has_committed_entry_in_current_term()) {
    pending_read_index_ = last_log_index() + 1;
    if (!propose({}).has_value()) {
      const ReadIndexToken failed{pending_read_context_, pending_read_index_};
      finish_read_index(failed);
      return std::nullopt;
    }
  } else {
    pending_read_index_ = commit_index_;
    broadcast_heartbeat();
  }

  return ReadIndexToken{pending_read_context_, pending_read_index_};
}

// Check if ReadIndex is confirmed (majority acked heartbeat)
bool Raft::read_index_ready(const ReadIndexToken &read) {
  if (state_ != State::Leader || storage_failed_) {
    return false;
  }

  // If this is not the pending read request, it's stale
  if (read.context != pending_read_context_ ||
      read.safe_index != pending_read_index_) {
    return false;
  }

  if (!has_committed_entry_in_current_term()) {
    return false;
  }

  if (last_applied_ < read.safe_index) {
    return false;
  }

  // Need majority of peers to ack (including self)
  uint64_t quorum = peers_.size() / 2 + 1;

  // Count acks from unique peers (+ ourselves = 1)
  uint64_t ack_count = 1; // Count self
  for (const auto &ack : read_index_acks_) {
    if (ack.second) {
      ack_count++;
    }
  }

  return ack_count >= quorum;
}

void Raft::finish_read_index(const ReadIndexToken &read) {
  if (read.context != pending_read_context_ ||
      read.safe_index != pending_read_index_) {
    return;
  }
  read_index_pending_ = false;
  pending_read_index_ = 0;
  pending_read_context_ = 0;
  read_index_acks_.clear();
}

// Handle InstallSnapshot RPC (follower receives snapshot from leader)
proto::Message Raft::handle_install_snapshot(const proto::Message &msg) {
  proto::Message response;
  response.type = proto::MsgInstallSnapshotResponse;
  response.from = id_;
  response.to = msg.from;
  response.success = false;

  if (!prepare_for_leader_rpc(msg)) {
    response.term = term_;
    return response;
  }

  // Validate snapshot metadata
  if (msg.snapshot_index == 0 || msg.snapshot_term == 0 ||
      msg.snapshot_term > msg.term || msg.snapshot_data.empty()) {
    response.term = term_;
    return response; // Invalid snapshot
  }

  const uint64_t committed = commit_index_;
  const uint64_t applied = last_applied_;

  // A stale snapshot must never roll committed or applied state backward.
  if (committed > msg.snapshot_index || applied >= msg.snapshot_index) {
    response.term = term_;
    response.success = true;
    response.match_index = std::max(committed, applied);
    return response;
  }

  if (!snapshot_restore_) {
    response.term = term_;
    return response;
  }

  if (!snapshot_restore_(msg.snapshot_index, msg.snapshot_data)) {
    response.term = term_;
    return response;
  }

  if (wal_) {
    wal::SnapshotMeta snap{msg.snapshot_index, msg.snapshot_term,
                           msg.snapshot_data, true};
    wal_->save_snapshot(snap);
    if (!sync_wal()) {
      response.term = term_;
      return response;
    }
  }

  // Discard entire log and replace with snapshot
  log_.clear();
  log_offset_ = msg.snapshot_index;
  log_offset_term_ = msg.snapshot_term;
  last_snapshot_index_ = msg.snapshot_index;
  last_snapshot_data_ = msg.snapshot_data;

  // Update applied and commit indices
  last_applied_ = msg.snapshot_index;
  commit_index_ = msg.snapshot_index;

  // WAL: persist hard state after snapshot install
  if (wal_) {
    wal_->save_hard_state({term_, voted_for_, commit_index_});
    if (!sync_wal()) {
      response.term = term_;
      return response;
    }
  }

  response.term = term_;
  response.success = true;
  response.match_index = msg.snapshot_index;

  return response;
}

// Handle InstallSnapshot response (leader receives confirmation)
void Raft::handle_install_snapshot_response(const proto::Message &msg) {
  if (msg.to != id_ || msg.from == id_ || !is_member(msg.from)) {
    return;
  }
  if (msg.term > term_) {
    become_follower(msg.term, 0);
    return;
  }
  if (state_ != State::Leader) {
    return;
  }

  if (msg.term < term_) {
    return;
  }

  if (msg.success) {
    // Update progress for this follower
    progress_[msg.from].match = msg.match_index;
    progress_[msg.from].next = progress_[msg.from].match + 1;

    // Try replicating remaining entries via AppendEntries
    broadcast_heartbeat();
  }
}

} // namespace kv
