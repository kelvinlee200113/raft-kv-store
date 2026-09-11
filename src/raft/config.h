#pragma once
#include <stdint.h>
#include <vector>

namespace kv {
    
struct Config {
    uint64_t id;
    std::vector<uint64_t> peers;
    uint32_t election_tick;
    uint32_t heartbeat_tick;
    uint64_t snapshot_threshold;  // Take snapshot every N applied entries (0 = disabled)
    uint64_t random_seed; // 0 selects an OS-derived, node-specific seed

    Config()
        : id(0), election_tick(10), heartbeat_tick(1), snapshot_threshold(100),
          random_seed(0) {}
};




}
