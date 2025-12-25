#include "raft/proto.h"
#include <cstdlib>
#include <raft/raft.h>

namespace kv {

Raft::Raft(const Config &config)
    : id_(config.id), term_(0), lead_(0), voted_for_(0), commit_index_(0),
      last_applied_(0), state_(State::Follower), peers_(config.peers),
      election_timeout_(config.election_tick),
      heartbeat_timeout_(config.heartbeat_tick), election_elapsed_(0),
      randomized_election_timeout_(0) {

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

void Raft::become_candidate() {
  state_ = State::Candidate;
  term_++;
  lead_ = 0;
  voted_for_ = id_;
  reset_randomized_election_timeout();
}

void Raft::become_leader() {
  state_ = State::Leader;
  lead_ = id_;

  next_index_.resize(peers_.size());
  match_index_.resize(peers_.size());

  for (size_t i = 0; i < peers_.size(); ++i) {
    next_index_[i] = log_.size() + 1;
    match_index_[i] = 0;
  }
}

void Raft::tick() {
  election_elapsed_++;

  // Check if election timeout has passed
  if (election_elapsed_ >= randomized_election_timeout_) {
    election_elapsed_ = 0;

    // Only followers and candidates can start elections
    if (state_ == State::Follower || state_ == State::Candidate) {
      become_candidate();
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

void Raft::broadcast_heartbeat() {
  for (uint64_t peer_id : peers_) {
    if (peer_id != id_) {
      // Send AppendEntries message
      proto::Message msg;
      msg.type = proto::MsgAppendEntries;
      msg.from = id_;
      msg.to = peer_id;
      msg.term = term_;
      if (log_.empty()) {
        msg.prev_log_term = 0;
        msg.prev_log_index = 0;
      } else {
        // Get last entry from log
        proto::Entry last_entry = log_[log_.size() - 1];
        msg.prev_log_term = last_entry.term;
        msg.prev_log_index = last_entry.index;
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

  response.success = true;
  response.term = term_;

  return response;
}

} // namespace kv