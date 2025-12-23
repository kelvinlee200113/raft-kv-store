#include <raft/raft.h>
#include <cstdlib>

namespace kv {


Raft::Raft(const Config& config) : id_(config.id),
            term_(0), lead_(0), voted_for_(0),
            commit_index_(0), last_applied_(0),
            state_(State::Follower), peers_(config.peers),
            election_timeout_(config.election_tick),
            heartbeat_timeout_(config.heartbeat_tick),
            election_elapsed_(0), randomized_election_timeout_(0) {

    // Generate random election timeout (between election_timeout to 2*election_timeout)
    randomized_election_timeout_ = election_timeout_ + (rand() % election_timeout_);

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
    randomized_election_timeout_ = election_timeout_ + (rand() % election_timeout_);
}


}