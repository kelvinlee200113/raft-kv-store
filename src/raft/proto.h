#pragma once
#include <stdint.h>
#include <vector>

namespace kv {

namespace proto {

typedef uint8_t EntryType;
const EntryType EntryNormal = 0;
const EntryType EntryConfChange = 1;

struct Entry {
  EntryType type;
  uint64_t term;
  uint64_t index;
  std::vector<uint8_t> data;

  Entry() : type(EntryNormal), term(0), index(0) {}
};

typedef uint8_t MessageType;
const MessageType MsgRequestVote = 0;
const MessageType MsgRequestVoteResponse = 1;
const MessageType MsgAppendEntries = 2;
const MessageType MsgAppendEntriesResponse = 3;

// Message for RPC communication between nodes
struct Message {
  MessageType type;
  uint64_t from;
  uint64_t to;
  uint64_t term;

  // RequestVote Request fields
  uint64_t last_log_index; // For RequestVote: candidate's last log index
  uint64_t last_log_term; // For RequestVote: term of candidate's last log entry

  // RequestVote Response fields
  bool vote_granted;

  // AppendEntries fields
  uint64_t prev_log_index; // For AppendEntries: index of log entry immediately
                           // before new ones
  uint64_t prev_log_term;  // For AppendEntries: term of prev_log_index entry
  std::vector<Entry> entries;
  uint64_t leader_commit;

  // AppendEntries Response fields
  bool success;
  uint64_t match_index; // index the follower successfully matched

  Message()
      : type(MsgRequestVote), from(0), to(0), term(0), last_log_index(0),
        last_log_term(0), vote_granted(false), prev_log_index(0),
        prev_log_term(0), leader_commit(0), success(false), match_index(0) {}
};

} // namespace proto
} // namespace kv