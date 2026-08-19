#pragma once
#include <msgpack.hpp>
#include <stdint.h>
#include <vector>

namespace kv {
namespace wal {

// WAL record types
enum RecordType : uint8_t {
  Invalid = 0,      // Invalid/corrupted record
  Entry = 1,        // Log entry
  HardState = 2,    // Raft critical state (term, vote, commit)
  CRCRecord = 3,    // CRC for corruption detection
  Snapshot = 4      // Snapshot (index, term, serialized state machine)
};

// HardState: Critical Raft state that must survive crashes
// This is the MINIMAL state needed to prevent:
// 1. Going backwards in time (term)
// 2. Double-voting (vote)
// 3. Losing committed entries (commit)
struct HardStateProto {
  uint64_t term;    // Current term
  uint64_t vote;    // Who we voted for in this term (0 = no vote)
  uint64_t commit;  // Highest committed index

  HardStateProto() : term(0), vote(0), commit(0) {}
  HardStateProto(uint64_t t, uint64_t v, uint64_t c)
      : term(t), vote(v), commit(c) {}

  // Equality check (for testing)
  bool operator==(const HardStateProto& other) const {
    return term == other.term && vote == other.vote && commit == other.commit;
  }

  bool operator!=(const HardStateProto& other) const {
    return !(*this == other);
  }

  bool is_empty() const {
    return term == 0 && vote == 0 && commit == 0;
  }

  MSGPACK_DEFINE(term, vote, commit);
};

// SnapshotMeta: persisted snapshot of the state machine
// index + term identify the exact log position baked into this snapshot.
// state is an opaque blob — only the state machine (KVStore) knows how to read it.
struct SnapshotMeta {
  uint64_t index;                // Last log index included in this snapshot
  uint64_t term;                 // Term of that log entry
  std::vector<uint8_t> state;   // Serialized state machine
  bool discard_suffix;          // InstallSnapshot resets the prior local log

  SnapshotMeta() : index(0), term(0), discard_suffix(false) {}
  SnapshotMeta(uint64_t i, uint64_t t, std::vector<uint8_t> s,
               bool discard = false)
      : index(i), term(t), state(std::move(s)), discard_suffix(discard) {}

  bool is_empty() const { return index == 0 && term == 0 && state.empty(); }

  MSGPACK_DEFINE(index, term, state, discard_suffix);
};

// WAL record header (8 bytes)
// Format: [type:1B][len:3B][crc:4B]
struct RecordHeader {
  uint8_t type;      // RecordType
  uint32_t len;      // Length of data (only 3 bytes used)
  uint32_t crc;      // CRC32 checksum of data

  RecordHeader() : type(RecordType::Invalid), len(0), crc(0) {}
  RecordHeader(uint8_t t, uint32_t l, uint32_t c) : type(t), len(l), crc(c) {}

  // Encode header to bytes (8 bytes)
  void encode(std::vector<uint8_t>& out) const {
    out.push_back(type);

    // Length (3 bytes, little-endian)
    out.push_back(len & 0xFF);
    out.push_back((len >> 8) & 0xFF);
    out.push_back((len >> 16) & 0xFF);

    // CRC (4 bytes, little-endian)
    out.push_back(crc & 0xFF);
    out.push_back((crc >> 8) & 0xFF);
    out.push_back((crc >> 16) & 0xFF);
    out.push_back((crc >> 24) & 0xFF);
  }

  // Decode header from bytes (8 bytes)
  static RecordHeader decode(const uint8_t* data) {
    RecordHeader hdr;
    hdr.type = data[0];

    // Length (3 bytes, little-endian)
    hdr.len = static_cast<uint32_t>(data[1]) |
              (static_cast<uint32_t>(data[2]) << 8U) |
              (static_cast<uint32_t>(data[3]) << 16U);

    // CRC (4 bytes, little-endian)
    hdr.crc = static_cast<uint32_t>(data[4]) |
              (static_cast<uint32_t>(data[5]) << 8U) |
              (static_cast<uint32_t>(data[6]) << 16U) |
              (static_cast<uint32_t>(data[7]) << 24U);

    return hdr;
  }
};

} // namespace wal
} // namespace kv
