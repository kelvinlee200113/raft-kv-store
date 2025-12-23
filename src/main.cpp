#include <iostream>
#include <raft/raft.h>
#include <raft/config.h>

int main() {
    std::cout << "Starting Raft KV node..." << std::endl;

    // Create config for node 1 in 3-node cluster
    kv::Config config;
    config.id = 1;
    config.peers = {1, 2, 3};
    config.election_tick = 10;
    config.heartbeat_tick = 1;

    // Create a Raft instance
    kv::Raft raft(config);

    // Print initial state
    std::cout << "Node ID: " << raft.get_id() << std::endl;
    std::cout << "Term: " << raft.get_term() << std::endl;
    std::cout << "State: " << (raft.get_state() == kv::State::Follower ? "Follower" : "Other") << std::endl;

    std::cout << "Raft node initialized successfully!" << std::endl;

    return 0;
}


