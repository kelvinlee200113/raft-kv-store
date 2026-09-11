#pragma once
#include <wal/proto.h>
#include <raft/proto.h>
#include <string>
#include <memory>
#include <vector>
#include <cstdio>

namespace kv {
namespace wal {

// WAL (Write-Ahead Log) for crash recovery
// Provides durability guarantees for Raft state (term, vote, commit) and log entries
//
// Usage:
//   auto wal = WAL::create("/data/wal", node_id);
//   wal->save_hard_state({term, vote, commit});
//   wal->sync();  // fsync - MUST call before sending network messages!
//   wal->save_entry(entry);
//
// Recovery:
//   auto wal = WAL::open("/data/wal");
//   auto state = wal->recover(entries);  // Returns HardState, fills entries
class WAL {
public:
  // Create a new WAL directory and initial WAL file
  // dir: Directory to store WAL files
  // Returns: WAL instance, or nullptr on failure
  static std::unique_ptr<WAL> create(const std::string& dir);

  // Open existing WAL directory for appending (after recovery)
  // dir: Directory containing WAL files
  // Returns: WAL instance, or nullptr if no WAL exists
  static std::unique_ptr<WAL> open(const std::string& dir);

  ~WAL();

  // Save HardState (term, vote, commit) to write buffer
  // MUST call sync() before sending network messages!
  void save_hard_state(const HardStateProto& hs);

  // Save log entry to write buffer
  // MUST call sync() before sending network messages!
  void save_entry(const proto::Entry& entry);

  // Save snapshot to write buffer
  // MUST call sync() before sending network messages!
  void save_snapshot(const SnapshotMeta& snap);

  // Flush buffer to disk (fwrite + fsync)
  // BLOCKS until data is physically on disk
  // Returns: true on success, false on error
  bool sync();

  // Recover state from all WAL files in the directory
  // Reads records sequentially, validates CRC, stops at first corruption
  // entries: Output - recovered log entries in order
  // snapshot: Output - last snapshot recovered (nullptr to skip)
  // Returns: Last valid HardState recovered
  HardStateProto recover(std::vector<proto::Entry>& entries, SnapshotMeta* snapshot = nullptr);

  const std::string& get_dir() const { return dir_; }

private:
  WAL(const std::string& dir, FILE* fp);

  // Append a record (header + data) to the in-memory write buffer
  void append_record(RecordType type, const uint8_t* data, size_t len);

  // Compute CRC32 checksum over data
  static uint32_t crc32(const uint8_t* data, size_t len);

  // Generate WAL filename: {seq:016x}-{index:016x}.wal
  static std::string make_wal_name(uint64_t seq, uint64_t index);

  // Parse WAL filename back to seq and index
  static bool parse_wal_name(const std::string& name, uint64_t& seq, uint64_t& index);

  // List all .wal files in dir, sorted by name
  static std::vector<std::string> list_wal_files(const std::string& dir);

  std::string dir_;                // WAL directory path
  FILE* fp_;                       // Current open WAL file (append mode)
  std::vector<uint8_t> buffer_;    // In-memory write buffer (flushed on sync)
  bool failed_ = false;            // A failed flush makes this handle unusable
};

} // namespace wal
} // namespace kv
