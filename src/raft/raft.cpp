#include "raft/proto.h"
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <raft/raft.h>

namespace kv {

Raft::Raft(const Config &config)
    : id_(config.id), term_(0), lead_(0), voted_for_(0), commit_index_(0),
      last_applied_(0), state_(State::Follower), peers_(config.peers),
      election_timeout_(config.election_tick),
      heartbeat_timeout_(config.heartbeat_tick), election_elapsed_(0),
      randomized_election_timeout_(0), heartbeat_elapsed_(0),
      read_index_pending_(false), pending_read_index_(0) {

  // Generate random election timeout (between election_timeout to
  // 2*election_timeout)
  randomized_election_timeout_ =
      election_timeout_ + (rand() % election_timeout_);
}

void Raft::become_follower(uint64_t term, uint64_t leader) {
  state_ = State::Follower;
  term_ = term;
  lead_ = leader;
  voted_for_ = 0;
  reset_randomized_election_timeout();
}

void Raft::become_pre_candidate() {
  state_ = State::PreCandidate;
  // Don't increment term in PreVote!
  lead_ = 0;
  pre_votes_.clear();
  reset_randomized_election_timeout();
}

void Raft::become_candidate() {
  state_ = State::Candidate;
  term_++;
  lead_ = 0;
  voted_for_ = id_;
  votes_.clear();
  reset_randomized_election_timeout();
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
    progress.next = log_.size() + 1;
    progress.match = 0;
    progress_[peer_id] = progress;
  }

  // Send initial heartbeat immediately
  broadcast_heartbeat();
}

void Raft::tick() {
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
  randomized_election_timeout_ =
      election_timeout_ + (rand() % election_timeout_);
}

proto::Message Raft::handle_request_vote(const proto::Message &msg) {
  proto::Message response;
  response.type = proto::MsgRequestVoteResponse;
  response.from = id_;
  response.to = msg.from;
  response.vote_granted = false;

  // Update term if candidate's term is higher
  if (msg.term > term_) {
    become_follower(msg.term, 0);
  }

  // Check if should grant vote
  if (msg.term >= term_ && (voted_for_ == 0 || voted_for_ == msg.from)) {
    response.vote_granted = true;
    voted_for_ = msg.from;
    reset_randomized_election_timeout();
  }

  // Set response term
  response.term = term_;

  return response;
}

void Raft::handle_request_vote_response(const proto::Message &msg) {
  // Ensure we are still the candidate
  if (state_ != State::Candidate) {
    return;
  }

  // Check term
  if (msg.term < term_) {
    // Stale
    return;
  }

  // If response is from higher term, step down
  if (msg.term > term_) {
    become_follower(msg.term, 0);
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
  response.vote_granted = false;

  // Reject if we have a leader and it's still sending heartbeats
  // (election_elapsed_ is low)
  if (lead_ != 0 && election_elapsed_ < election_timeout_) {
    response.term = term_;
    return response; // vote_granted = false
  }

  // Grant pre-vote if candidate's log is at least as up-to-date as ours
  // Same logic as RequestVote
  bool log_ok = false;
  if (log_.empty()) {
    log_ok = true; // We have no log, anyone is ok
  } else {
    uint64_t our_last_index = log_.size();
    uint64_t our_last_term = log_[our_last_index - 1].term;

    if (msg.last_log_term > our_last_term) {
      log_ok = true; // Candidate's last term is newer
    } else if (msg.last_log_term == our_last_term &&
               msg.last_log_index >= our_last_index) {
      log_ok = true; // Same term, candidate's log is at least as long
    }
  }

  if (log_ok) {
    response.vote_granted = true;
  }

  response.term = term_;
  return response;
}

// Handle PreVote response
void Raft::handle_pre_vote_response(const proto::Message &msg) {
  // Ensure we are still pre-candidate
  if (state_ != State::PreCandidate) {
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
  // Record pre-vote for self
  pre_votes_[id_] = true;
  for (uint64_t peer_id : peers_) {
    if (peer_id != id_) {
      // Create PreVote message (using CURRENT term, not term+1!)
      proto::Message msg;
      msg.type = proto::MsgPreVote;
      msg.from = id_;
      msg.to = peer_id;
      msg.term = term_; // Current term, not incremented!
      if (log_.empty()) {
        msg.last_log_index = 0;
        msg.last_log_term = 0;
      } else {
        proto::Entry last_entry = log_[log_.size() - 1];
        msg.last_log_index = last_entry.index;
        msg.last_log_term = last_entry.term;
      }
      msgs_.push_back(msg);
    }
  }
}

// Candidate campaigning for itself
void Raft::campaign() {
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
      if (log_.empty()) {
        msg.last_log_index = 0;
        msg.last_log_term = 0;
      } else {
        // Get last entry from log
        proto::Entry last_entry = log_[log_.size() - 1];
        msg.last_log_index = last_entry.index;
        msg.last_log_term = last_entry.term;
      }
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

// Broadcast AppendEntries message (empty for heartbeats)
void Raft::broadcast_heartbeat() {
  for (uint64_t peer_id : peers_) {
    if (peer_id != id_) {
      // Send AppendEntries message
      proto::Message msg;
      msg.type = proto::MsgAppendEntries;
      msg.from = id_;
      msg.to = peer_id;
      msg.term = term_;

      // Get the next index this peer needs
      uint64_t next_index = progress_[peer_id].next;
      uint64_t prev_index = next_index - 1;

      msg.prev_log_index = prev_index;
      if (prev_index == 0) {
        msg.prev_log_term = 0;
      } else {
        msg.prev_log_term =
            log_[prev_index - 1].term; // Raft Logs are 1-indexed
      }

      msg.entries.clear();
      for (uint64_t i = next_index; i <= log_.size(); ++i) {
        msg.entries.push_back(log_[i - 1]);
      }

      msg.leader_commit = commit_index_;
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

  // Update term if leader's term is higher
  if (msg.term > term_) {
    become_follower(msg.term, msg.from);
  }

  // Reject if term is lower
  if (msg.term < term_) {
    response.term = term_;
    return response;
  }

  reset_randomized_election_timeout();
  lead_ = msg.from;

  // Log consistency check
  bool log_ok = false;

  if (msg.prev_log_index == 0) {
    log_ok = true;
  } else if (log_.size() >= msg.prev_log_index) {
    log_ok = log_[msg.prev_log_index - 1].term == msg.prev_log_term;
  }

  response.term = term_;

  if (!log_ok) {
    response.match_index = 0;
    return response;
  }

  // Append entries to follower's log (starting from the match_index + 1)
  for (uint64_t i = 0; i < msg.entries.size(); ++i) {
    uint64_t index = msg.prev_log_index + i + 1;

    if (index <= log_.size()) {
      if (log_[index - 1].term != msg.entries[i].term) {
        // Conflict, need to remove the entries from index - 1 to the end
        log_.erase(log_.begin() + index - 1, log_.end());
        log_.push_back(msg.entries[i]);
      }
    } else {
      log_.push_back(msg.entries[i]);
    }
  }

  response.success = true;
  response.match_index = msg.prev_log_index + msg.entries.size();

  // Update commit index based on leader's commit
  if (msg.leader_commit > commit_index_) {
    commit_index_ =
        std::min(msg.leader_commit, static_cast<uint64_t>(log_.size()));
  }

  return response;
}

void Raft::handle_append_entries_response(const proto::Message &msg) {
  if (state_ != State::Leader) {
    return;
  }

  if (term_ > msg.term) {
    return;
  }

  if (term_ < msg.term) {
    become_follower(msg.term, 0);
  }

  if (msg.success) {
    progress_[msg.from].match = msg.match_index;
    progress_[msg.from].next = progress_[msg.from].match + 1;

    // Track successful heartbeat response for ReadIndex
    if (read_index_pending_) {
      read_index_acks_[msg.from] = true;
    }

    // Try to advance commit index
    // Check each index from commit_index + 1 to log_.size()
    for (uint64_t i = commit_index_ + 1; i <= log_.size(); ++i) {
      // Only commit entries from current term
      if (log_[i - 1].term != term_) {
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

  } else {
    // Retry with lower index
    progress_[msg.from].next--;
    broadcast_heartbeat();
  }
}

void Raft::propose(const std::vector<uint8_t> &data) {
  if (state_ != State::Leader) {
    return;
  }

  proto::Entry entry;
  entry.type = proto::EntryNormal;
  entry.data = data;
  entry.index = log_.size() + 1;
  entry.term = term_;

  log_.push_back(entry);
  broadcast_heartbeat();
}

std::vector<proto::Entry> Raft::get_entries_to_apply() {
  std::vector<proto::Entry> result;

  if (last_applied_ >= commit_index_) {
    return result;
  }

  for (uint64_t i = last_applied_ + 1; i <= commit_index_; ++i) {
    result.push_back(log_[i - 1]);
  }

  return result;
}

void Raft::advance(uint64_t index) {
  if (index > commit_index_ || index <= last_applied_) {
    return;
  }
  last_applied_ = index;
}

// ReadIndex: Initiate a linearizable read
// Returns the commit index that should be applied before reading
uint64_t Raft::read_index() {
  // Only leader can serve ReadIndex
  if (state_ != State::Leader) {
    return 0; // Not leader, can't serve reads
  }

  // Mark ReadIndex as pending
  read_index_pending_ = true;
  pending_read_index_ = commit_index_;
  read_index_acks_.clear(); // Clear previous acks

  // Send heartbeat to confirm leadership
  broadcast_heartbeat();

  return pending_read_index_;
}

// Check if ReadIndex is confirmed (majority acked heartbeat)
bool Raft::read_index_ready(uint64_t read_index) {
  if (state_ != State::Leader) {
    return false;
  }

  // If this is not the pending read request, it's stale
  if (read_index != pending_read_index_) {
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

} // namespace kv