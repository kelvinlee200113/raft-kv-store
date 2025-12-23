#include <raft/raft.h>

namespace kv {


Raft::Raft(const Config& config) : id_(config.id), 
            term_(0), lead_(0), voted_for_(0),
            commit_index_(0), last_applied_(0),
            state_(State::Follower), peers_(config.peers) {
    
}

void Raft::become_follower(uint64_t term, uint64_t leader) {
    state_ = State::Follower;
    term_ = term;
    lead_ = leader;
    voted_for_ = 0;
}

void Raft::become_candidate() {
    state_ = State::Candidate;
    term_++;
    lead_ = 0;
    voted_for_ = id_;
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




}