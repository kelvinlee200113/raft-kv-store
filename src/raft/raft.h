#pragma once
#include <stdint.h>
#include <vector>
#include <raft/proto.h>
#include <raft/config.h>

namespace kv {

// Raft node states
enum class State {
    Follower,
    Candidate,
    Leader
};

class Raft {
public: 
    explicit Raft(const Config& config);

    void become_follower(uint64_t term, uint64_t leader);
    void become_candidate();
    void become_leader();

    uint64_t get_term() const { return term_; }
    uint64_t get_id() const { return id_; }
    uint64_t get_leader() const { return lead_; }
    uint64_t get_voted_for() const { return voted_for_; }
    uint64_t get_commit_index() const { return commit_index_; }
    uint64_t get_last_applied() const { return last_applied_; }
    State get_state() const { return state_; }
    const std::vector<proto::Entry>& get_log() const { return log_; }
    const std::vector<uint64_t>& get_match_index() const { return match_index_; }
    const std::vector<uint64_t>& get_next_index() const { return next_index_; }
    const std::vector<uint64_t>& get_peers() const { return peers_; }


private:
    uint64_t id_;
    uint64_t term_;
    uint64_t lead_;
    uint64_t voted_for_;
    uint64_t commit_index_;
    uint64_t last_applied_;
    State state_;
    std::vector<proto::Entry> log_;
    std::vector<uint64_t> match_index_;
    std::vector<uint64_t> next_index_;
    std::vector<uint64_t> peers_;

};





}
