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
    std::cout << "Initial State: Follower" << std::endl;
    std::cout << "Election timeout: " << raft.get_randomized_election_timeout() << std::endl;
    std::cout << "\nSimulating ticks..." << std::endl;

    // Simulate ticks until election timeout
    for (int i = 0; i < 25; i++) {
        raft.tick();
        std::cout << "Tick " << i + 1 
                  << " - Elapsed: " << raft.get_election_elapsed()
                  << " - Term: " << raft.get_term()
                  << " - State: " << (raft.get_state() == kv::State::Follower ? "Follower" : 
                                     raft.get_state() == kv::State::Candidate ? "Candidate" : "Leader")
                  << std::endl;
    }

    std::cout << "\nFinal state: " 
              << (raft.get_state() == kv::State::Candidate ? "Candidate" : "Other") 
              << std::endl;
              
    std::cout << "Raft node initialized successfully!" << std::endl;

    return 0;
}


