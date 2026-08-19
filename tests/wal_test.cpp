#include <gtest/gtest.h>
#include <wal/wal.h>
#include <wal/proto.h>
#include <raft/proto.h>
#include <string>
#include <vector>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Fixture: creates a unique temp directory per test, removes it on teardown
// ---------------------------------------------------------------------------
class WALTest : public ::testing::Test {
protected:
  std::string dir_;

  void SetUp() override {
    const auto directory = std::string("raft-kv-wal-test-") +
                           std::to_string(static_cast<long long>(getpid())) +
                           "-" + GetCurrentTestName();
    dir_ = (std::filesystem::temp_directory_path() / directory).string();

    std::error_code error;
    std::filesystem::remove_all(dir_, error);
    ASSERT_FALSE(error) << error.message();
  }

  void TearDown() override {
    std::error_code error;
    std::filesystem::remove_all(dir_, error);
    EXPECT_FALSE(error) << error.message();
  }

private:
  static const char* GetCurrentTestName() {
    return ::testing::UnitTest::GetInstance()->current_test_info()->name();
  }
};

// ---------------------------------------------------------------------------
// Test 1: Write a HardState, close, reopen, recover — get it back exactly
// ---------------------------------------------------------------------------
TEST_F(WALTest, RecoverHardState) {
  // --- write phase ---
  auto w = kv::wal::WAL::create(dir_);
  ASSERT_NE(w, nullptr);

  kv::wal::HardStateProto hs{/*term=*/5, /*vote=*/2, /*commit=*/3};
  w->save_hard_state(hs);
  ASSERT_TRUE(w->sync());

  // Destroy WAL → closes underlying file
  w.reset();

  // --- recover phase ---
  auto w2 = kv::wal::WAL::open(dir_);
  ASSERT_NE(w2, nullptr);

  std::vector<kv::proto::Entry> entries;
  auto recovered = w2->recover(entries);

  EXPECT_EQ(recovered.term,   5u);
  EXPECT_EQ(recovered.vote,   2u);
  EXPECT_EQ(recovered.commit, 3u);
  EXPECT_TRUE(entries.empty());  // no entries were written
}

// ---------------------------------------------------------------------------
// Test 2: Write multiple entries, close, reopen, recover — get them back
// ---------------------------------------------------------------------------
TEST_F(WALTest, RecoverEntries) {
  auto w = kv::wal::WAL::create(dir_);
  ASSERT_NE(w, nullptr);

  // Write 3 entries
  for (int i = 1; i <= 3; i++) {
    kv::proto::Entry e;
    e.type  = kv::proto::EntryNormal;
    e.term  = 1;
    e.index = static_cast<uint64_t>(i);
    e.data  = {static_cast<uint8_t>('a' + i - 1)};  // 'a', 'b', 'c'
    w->save_entry(e);
  }
  ASSERT_TRUE(w->sync());
  w.reset();

  // --- recover ---
  auto w2 = kv::wal::WAL::open(dir_);
  ASSERT_NE(w2, nullptr);

  std::vector<kv::proto::Entry> entries;
  auto hs = w2->recover(entries);

  // No HardState was written → all zeros
  EXPECT_TRUE(hs.is_empty());

  // All 3 entries recovered in order
  ASSERT_EQ(entries.size(), 3u);
  for (int i = 0; i < 3; i++) {
    EXPECT_EQ(entries[i].term,  1u);
    EXPECT_EQ(entries[i].index, static_cast<uint64_t>(i + 1));
    EXPECT_EQ(entries[i].data, (std::vector<uint8_t>{static_cast<uint8_t>('a' + i)}));
  }
}

// ---------------------------------------------------------------------------
// Test 3: Interleaved HardState and entries — recover returns both correctly
// ---------------------------------------------------------------------------
TEST_F(WALTest, RecoverMixedRecords) {
  auto w = kv::wal::WAL::create(dir_);
  ASSERT_NE(w, nullptr);

  // HardState, Entry, Entry, HardState, Entry  (interleaved)
  kv::wal::HardStateProto hs1{1, 1, 0};
  w->save_hard_state(hs1);

  kv::proto::Entry e1; e1.term = 1; e1.index = 1; e1.data = {10};
  kv::proto::Entry e2; e2.term = 1; e2.index = 2; e2.data = {20};
  w->save_entry(e1);
  w->save_entry(e2);

  kv::wal::HardStateProto hs2{2, 2, 1};  // term advanced, commit advanced
  w->save_hard_state(hs2);

  kv::proto::Entry e3; e3.term = 2; e3.index = 3; e3.data = {30};
  w->save_entry(e3);

  ASSERT_TRUE(w->sync());
  w.reset();

  // --- recover ---
  auto w2 = kv::wal::WAL::open(dir_);
  ASSERT_NE(w2, nullptr);

  std::vector<kv::proto::Entry> entries;
  auto hs = w2->recover(entries);

  // Last HardState wins
  EXPECT_EQ(hs.term,   2u);
  EXPECT_EQ(hs.vote,   2u);
  EXPECT_EQ(hs.commit, 1u);

  // All 3 entries in order
  ASSERT_EQ(entries.size(), 3u);
  EXPECT_EQ(entries[0].index, 1u);
  EXPECT_EQ(entries[0].data,  std::vector<uint8_t>{10});
  EXPECT_EQ(entries[1].index, 2u);
  EXPECT_EQ(entries[1].data,  std::vector<uint8_t>{20});
  EXPECT_EQ(entries[2].index, 3u);
  EXPECT_EQ(entries[2].data,  std::vector<uint8_t>{30});
}

// ---------------------------------------------------------------------------
// Test 4: Multiple HardStates → recover returns the LAST one
// ---------------------------------------------------------------------------
TEST_F(WALTest, LastHardStateWins) {
  auto w = kv::wal::WAL::create(dir_);
  ASSERT_NE(w, nullptr);

  w->save_hard_state(kv::wal::HardStateProto{1, 1, 0});
  w->save_hard_state(kv::wal::HardStateProto{3, 2, 1});
  w->save_hard_state(kv::wal::HardStateProto{7, 3, 5});  // ← this one wins
  ASSERT_TRUE(w->sync());
  w.reset();

  auto w2 = kv::wal::WAL::open(dir_);
  ASSERT_NE(w2, nullptr);

  std::vector<kv::proto::Entry> entries;
  auto hs = w2->recover(entries);

  EXPECT_EQ(hs.term,   7u);
  EXPECT_EQ(hs.vote,   3u);
  EXPECT_EQ(hs.commit, 5u);
  EXPECT_TRUE(entries.empty());
}

// ---------------------------------------------------------------------------
// Test 5: Corrupt the payload of the second record → recover returns only
//         the first record (stops at CRC mismatch)
// ---------------------------------------------------------------------------
TEST_F(WALTest, CorruptionStopsRecovery) {
  // Write two entries, sync, close
  auto w = kv::wal::WAL::create(dir_);
  ASSERT_NE(w, nullptr);

  kv::proto::Entry e1; e1.term = 1; e1.index = 1; e1.data = {0xAA, 0xBB};
  kv::proto::Entry e2; e2.term = 1; e2.index = 2; e2.data = {0xCC, 0xDD};
  w->save_entry(e1);
  ASSERT_TRUE(w->sync());   // flush first entry
  w->save_entry(e2);
  ASSERT_TRUE(w->sync());   // flush second entry
  w.reset();                 // close file

  // --- corrupt the .wal file ---
  // The file is: 0000000000000000-0000000000000000.wal
  std::string wal_path = dir_ + "/0000000000000000-0000000000000000.wal";

  // Read the whole file into memory
  FILE* f = fopen(wal_path.c_str(), "rb");
  ASSERT_NE(f, nullptr);
  fseek(f, 0, SEEK_END);
  long file_size = ftell(f);
  ASSERT_GT(file_size, 0);
  fseek(f, 0, SEEK_SET);

  std::vector<uint8_t> raw(static_cast<size_t>(file_size));
  size_t got = fread(raw.data(), 1, raw.size(), f);
  fclose(f);
  ASSERT_EQ(got, raw.size());

  // Record layout: [header:8B][payload:N]
  // First record header at offset 0, payload at offset 8.
  // First record payload length is in bytes 1-3 (little-endian).
  uint32_t first_len = raw[1] | (raw[2] << 8) | (raw[3] << 16);
  // Second record starts right after first record
  size_t second_record_start = 8 + first_len;
  // Second record payload starts 8 bytes after that (skip its header)
  size_t second_payload_start = second_record_start + 8;

  ASSERT_LT(second_payload_start, raw.size());

  // Flip a byte in the second record's payload → CRC will mismatch
  raw[second_payload_start] ^= 0xFF;

  // Write corrupted data back
  f = fopen(wal_path.c_str(), "wb");
  ASSERT_NE(f, nullptr);
  fwrite(raw.data(), 1, raw.size(), f);
  fclose(f);

  // --- recover should get only the first entry ---
  auto w2 = kv::wal::WAL::open(dir_);
  ASSERT_NE(w2, nullptr);

  std::vector<kv::proto::Entry> entries;
  auto hs = w2->recover(entries);

  EXPECT_TRUE(hs.is_empty());          // no HardState written
  ASSERT_EQ(entries.size(), 1u);       // only the first entry survived
  EXPECT_EQ(entries[0].index, 1u);
  EXPECT_EQ(entries[0].data, (std::vector<uint8_t>{0xAA, 0xBB}));
}

// ---------------------------------------------------------------------------
// Test 6: Write a snapshot, close, reopen, recover — get it back exactly
// ---------------------------------------------------------------------------
TEST_F(WALTest, RecoverSnapshot) {
  auto w = kv::wal::WAL::create(dir_);
  ASSERT_NE(w, nullptr);

  kv::wal::SnapshotMeta snap{/*index=*/10, /*term=*/3, /*state=*/{0x01, 0x02, 0x03}};
  w->save_snapshot(snap);
  ASSERT_TRUE(w->sync());
  w.reset();

  // --- recover ---
  auto w2 = kv::wal::WAL::open(dir_);
  ASSERT_NE(w2, nullptr);

  std::vector<kv::proto::Entry> entries;
  kv::wal::SnapshotMeta recovered;
  auto hs = w2->recover(entries, &recovered);

  EXPECT_TRUE(hs.is_empty());
  EXPECT_TRUE(entries.empty());
  EXPECT_EQ(recovered.index, 10u);
  EXPECT_EQ(recovered.term,  3u);
  EXPECT_EQ(recovered.state, (std::vector<uint8_t>{0x01, 0x02, 0x03}));
}

// ---------------------------------------------------------------------------
// Test 7: Multiple snapshots → recover returns the LAST one
// ---------------------------------------------------------------------------
TEST_F(WALTest, LastSnapshotWins) {
  auto w = kv::wal::WAL::create(dir_);
  ASSERT_NE(w, nullptr);

  w->save_snapshot(kv::wal::SnapshotMeta{5,  1, {0xAA}});
  w->save_snapshot(kv::wal::SnapshotMeta{20, 4, {0xBB, 0xCC}});  // ← this one wins
  ASSERT_TRUE(w->sync());
  w.reset();

  auto w2 = kv::wal::WAL::open(dir_);
  ASSERT_NE(w2, nullptr);

  std::vector<kv::proto::Entry> entries;
  kv::wal::SnapshotMeta recovered;
  w2->recover(entries, &recovered);

  EXPECT_EQ(recovered.index, 20u);
  EXPECT_EQ(recovered.term,  4u);
  EXPECT_EQ(recovered.state, (std::vector<uint8_t>{0xBB, 0xCC}));
}

// ---------------------------------------------------------------------------
// Test 8: Realistic recovery: a local snapshot compacts its covered prefix and
//         retains entries appended after the snapshot.
// ---------------------------------------------------------------------------
TEST_F(WALTest, SnapshotWithEntriesAndHardState) {
  auto w = kv::wal::WAL::create(dir_);
  ASSERT_NE(w, nullptr);

  // Entries 1-3 (will be baked into the snapshot)
  for (int i = 1; i <= 3; i++) {
    kv::proto::Entry e;
    e.term  = 1;
    e.index = static_cast<uint64_t>(i);
    e.data  = {static_cast<uint8_t>(i * 10)};
    w->save_entry(e);
  }

  // Snapshot at index 3 — captures state after entries 1-3
  w->save_snapshot(kv::wal::SnapshotMeta{3, 1, {0xDE, 0xAD}});

  // HardState reflecting commit up to 5
  w->save_hard_state(kv::wal::HardStateProto{2, 1, 5});

  // Entries 4-5 (after the snapshot, need replay)
  for (int i = 4; i <= 5; i++) {
    kv::proto::Entry e;
    e.term  = 2;
    e.index = static_cast<uint64_t>(i);
    e.data  = {static_cast<uint8_t>(i * 10)};
    w->save_entry(e);
  }

  ASSERT_TRUE(w->sync());
  w.reset();

  // --- recover ---
  auto w2 = kv::wal::WAL::open(dir_);
  ASSERT_NE(w2, nullptr);

  std::vector<kv::proto::Entry> entries;
  kv::wal::SnapshotMeta snap;
  auto hs = w2->recover(entries, &snap);

  // HardState
  EXPECT_EQ(hs.term,   2u);
  EXPECT_EQ(hs.commit, 5u);

  // Snapshot
  EXPECT_EQ(snap.index, 3u);
  EXPECT_EQ(snap.term,  1u);
  EXPECT_EQ(snap.state, (std::vector<uint8_t>{0xDE, 0xAD}));

  ASSERT_EQ(entries.size(), 2u);
  EXPECT_EQ(entries[0].index, 4u);
  EXPECT_EQ(entries[0].data,  (std::vector<uint8_t>{40}));
  EXPECT_EQ(entries[1].index, 5u);
  EXPECT_EQ(entries[1].data,  (std::vector<uint8_t>{50}));
}

TEST_F(WALTest, InstalledSnapshotDiscardsThePriorLocalSuffix) {
  auto wal = kv::wal::WAL::create(dir_);
  ASSERT_NE(wal, nullptr);
  for (uint64_t index = 1; index <= 5; ++index) {
    kv::proto::Entry entry;
    entry.index = index;
    entry.term = 1;
    entry.data = {static_cast<uint8_t>(index)};
    wal->save_entry(entry);
  }
  wal->save_snapshot(kv::wal::SnapshotMeta{3, 2, {9}, true});

  kv::proto::Entry replacement;
  replacement.index = 4;
  replacement.term = 2;
  replacement.data = {44};
  wal->save_entry(replacement);
  ASSERT_TRUE(wal->sync());
  wal.reset();

  auto reopened = kv::wal::WAL::open(dir_);
  ASSERT_NE(reopened, nullptr);
  std::vector<kv::proto::Entry> entries;
  kv::wal::SnapshotMeta snapshot;
  reopened->recover(entries, &snapshot);

  EXPECT_TRUE(snapshot.discard_suffix);
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries.front().index, 4u);
  EXPECT_EQ(entries.front().term, 2u);
  EXPECT_EQ(entries.front().data, (std::vector<uint8_t>{44}));
}

TEST_F(WALTest, ReplacementEntryTruncatesRecoveredSuffix) {
  auto wal = kv::wal::WAL::create(dir_);
  ASSERT_NE(wal, nullptr);

  for (uint64_t index = 1; index <= 3; ++index) {
    kv::proto::Entry entry;
    entry.index = index;
    entry.term = 1;
    entry.data = {static_cast<uint8_t>(index)};
    wal->save_entry(entry);
  }
  ASSERT_TRUE(wal->sync());

  for (uint64_t index = 2; index <= 3; ++index) {
    kv::proto::Entry replacement;
    replacement.index = index;
    replacement.term = 2;
    replacement.data = {static_cast<uint8_t>(index + 10)};
    wal->save_entry(replacement);
  }
  ASSERT_TRUE(wal->sync());
  wal.reset();

  auto reopened = kv::wal::WAL::open(dir_);
  ASSERT_NE(reopened, nullptr);
  std::vector<kv::proto::Entry> recovered;
  reopened->recover(recovered);

  ASSERT_EQ(recovered.size(), 3u);
  EXPECT_EQ(recovered[0].index, 1u);
  EXPECT_EQ(recovered[0].term, 1u);
  EXPECT_EQ(recovered[1].index, 2u);
  EXPECT_EQ(recovered[1].term, 2u);
  EXPECT_EQ(recovered[1].data, (std::vector<uint8_t>{12}));
  EXPECT_EQ(recovered[2].index, 3u);
  EXPECT_EQ(recovered[2].term, 2u);
  EXPECT_EQ(recovered[2].data, (std::vector<uint8_t>{13}));
}

TEST_F(WALTest, RecoveryTruncatesTornTailBeforeFutureAppend) {
  const std::string wal_path =
      dir_ + "/0000000000000000-0000000000000000.wal";

  auto wal = kv::wal::WAL::create(dir_);
  ASSERT_NE(wal, nullptr);
  kv::proto::Entry first;
  first.index = 1;
  first.term = 1;
  first.data = {1};
  wal->save_entry(first);
  ASSERT_TRUE(wal->sync());
  wal.reset();

  FILE *file = fopen(wal_path.c_str(), "ab");
  ASSERT_NE(file, nullptr);
  const uint8_t torn_header[] = {2, 4, 0};
  ASSERT_EQ(fwrite(torn_header, 1, sizeof(torn_header), file),
            sizeof(torn_header));
  fclose(file);

  auto reopened = kv::wal::WAL::open(dir_);
  ASSERT_NE(reopened, nullptr);
  std::vector<kv::proto::Entry> first_recovery;
  reopened->recover(first_recovery);
  ASSERT_EQ(first_recovery.size(), 1u);

  kv::proto::Entry second;
  second.index = 2;
  second.term = 1;
  second.data = {2};
  reopened->save_entry(second);
  ASSERT_TRUE(reopened->sync());
  reopened.reset();

  auto final_open = kv::wal::WAL::open(dir_);
  ASSERT_NE(final_open, nullptr);
  std::vector<kv::proto::Entry> final_recovery;
  final_open->recover(final_recovery);

  ASSERT_EQ(final_recovery.size(), 2u);
  EXPECT_EQ(final_recovery[0].index, 1u);
  EXPECT_EQ(final_recovery[1].index, 2u);
}

TEST_F(WALTest, CreateNeverOverwritesAnExistingWal) {
  auto wal = kv::wal::WAL::create(dir_);
  ASSERT_NE(wal, nullptr);
  kv::proto::Entry entry;
  entry.index = 1;
  entry.term = 1;
  entry.data = {42};
  wal->save_entry(entry);
  ASSERT_TRUE(wal->sync());
  wal.reset();

  EXPECT_EQ(kv::wal::WAL::create(dir_), nullptr);

  auto reopened = kv::wal::WAL::open(dir_);
  ASSERT_NE(reopened, nullptr);
  std::vector<kv::proto::Entry> recovered;
  reopened->recover(recovered);
  ASSERT_EQ(recovered.size(), 1u);
  EXPECT_EQ(recovered.front().data, (std::vector<uint8_t>{42}));
}

TEST_F(WALTest, UnknownRecordTypeIsTreatedAsDamagedTail) {
  const std::string wal_path =
      dir_ + "/0000000000000000-0000000000000000.wal";
  auto wal = kv::wal::WAL::create(dir_);
  ASSERT_NE(wal, nullptr);
  kv::proto::Entry entry;
  entry.index = 1;
  entry.term = 1;
  entry.data = {1};
  wal->save_entry(entry);
  ASSERT_TRUE(wal->sync());
  wal.reset();

  FILE *file = fopen(wal_path.c_str(), "r+b");
  ASSERT_NE(file, nullptr);
  ASSERT_EQ(fputc(99, file), 99);
  fclose(file);

  auto reopened = kv::wal::WAL::open(dir_);
  ASSERT_NE(reopened, nullptr);
  std::vector<kv::proto::Entry> recovered;
  reopened->recover(recovered);
  EXPECT_TRUE(recovered.empty());

  kv::proto::Entry replacement;
  replacement.index = 1;
  replacement.term = 2;
  replacement.data = {2};
  reopened->save_entry(replacement);
  ASSERT_TRUE(reopened->sync());
  reopened.reset();

  auto final_open = kv::wal::WAL::open(dir_);
  ASSERT_NE(final_open, nullptr);
  final_open->recover(recovered);
  ASSERT_EQ(recovered.size(), 1u);
  EXPECT_EQ(recovered.front().term, 2u);
}

TEST_F(WALTest, OversizedSnapshotIsRejectedWithoutPoisoningTheWal) {
  auto wal = kv::wal::WAL::create(dir_);
  ASSERT_NE(wal, nullptr);
  kv::wal::SnapshotMeta oversized{
      1, 1, std::vector<uint8_t>(1U << 24U, static_cast<uint8_t>(7))};

  EXPECT_THROW(wal->save_snapshot(oversized), std::length_error);

  kv::proto::Entry entry;
  entry.index = 1;
  entry.term = 1;
  entry.data = {9};
  wal->save_entry(entry);
  ASSERT_TRUE(wal->sync());
  wal.reset();

  auto reopened = kv::wal::WAL::open(dir_);
  ASSERT_NE(reopened, nullptr);
  std::vector<kv::proto::Entry> recovered;
  kv::wal::SnapshotMeta snapshot;
  reopened->recover(recovered, &snapshot);
  EXPECT_TRUE(snapshot.is_empty());
  ASSERT_EQ(recovered.size(), 1u);
  EXPECT_EQ(recovered.front().data, (std::vector<uint8_t>{9}));
}

TEST_F(WALTest, LegacyThreeFieldSnapshotDefaultsToRetainingItsSuffix) {
  msgpack::sbuffer buffer;
  msgpack::packer<msgpack::sbuffer> packer(buffer);
  packer.pack_array(3);
  packer.pack(static_cast<uint64_t>(7));
  packer.pack(static_cast<uint64_t>(2));
  packer.pack(std::vector<uint8_t>{9});

  const auto object = msgpack::unpack(buffer.data(), buffer.size());
  const auto snapshot = object.get().as<kv::wal::SnapshotMeta>();

  EXPECT_EQ(snapshot.index, 7u);
  EXPECT_EQ(snapshot.term, 2u);
  EXPECT_FALSE(snapshot.discard_suffix);
}

TEST_F(WALTest, SyncSurvivesAbruptProcessExitWithoutFclose) {
  const pid_t child = fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    auto wal = kv::wal::WAL::create(dir_);
    if (!wal) {
      _exit(2);
    }
    kv::proto::Entry entry;
    entry.index = 1;
    entry.term = 1;
    entry.data = {42};
    wal->save_entry(entry);
    if (!wal->sync()) {
      _exit(3);
    }
    _exit(0);
  }

  int status = 0;
  ASSERT_EQ(waitpid(child, &status, 0), child);
  ASSERT_TRUE(WIFEXITED(status));
  ASSERT_EQ(WEXITSTATUS(status), 0);

  auto reopened = kv::wal::WAL::open(dir_);
  ASSERT_NE(reopened, nullptr);
  std::vector<kv::proto::Entry> recovered;
  reopened->recover(recovered);
  ASSERT_EQ(recovered.size(), 1u);
  EXPECT_EQ(recovered.front().index, 1u);
  EXPECT_EQ(recovered.front().data, (std::vector<uint8_t>{42}));
}

TEST_F(WALTest, RecoveryFailsClosedWhenDamagedTailCannotBeRepaired) {
  const std::string wal_path =
      dir_ + "/0000000000000000-0000000000000000.wal";
  auto wal = kv::wal::WAL::create(dir_);
  ASSERT_NE(wal, nullptr);
  kv::proto::Entry entry;
  entry.index = 1;
  entry.term = 1;
  entry.data = {1};
  wal->save_entry(entry);
  ASSERT_TRUE(wal->sync());
  wal.reset();

  FILE *file = fopen(wal_path.c_str(), "ab");
  ASSERT_NE(file, nullptr);
  const uint8_t torn[] = {1, 2, 3};
  ASSERT_EQ(fwrite(torn, 1, sizeof(torn), file), sizeof(torn));
  fclose(file);

  auto reopened = kv::wal::WAL::open(dir_);
  ASSERT_NE(reopened, nullptr);
  ASSERT_EQ(chmod(wal_path.c_str(), 0444), 0);
  std::vector<kv::proto::Entry> recovered;
  EXPECT_THROW(reopened->recover(recovered), std::runtime_error);
  EXPECT_EQ(chmod(wal_path.c_str(), 0644), 0);
}

TEST_F(WALTest, RecoveryFailsClosedWhenExistingWalDisappears) {
  const std::string wal_path =
      dir_ + "/0000000000000000-0000000000000000.wal";
  const std::string moved_path = wal_path + ".missing";

  auto wal = kv::wal::WAL::create(dir_);
  ASSERT_NE(wal, nullptr);
  wal->save_hard_state({3, 2, 1});
  kv::proto::Entry entry;
  entry.index = 1;
  entry.term = 3;
  entry.data = {42};
  wal->save_entry(entry);
  ASSERT_TRUE(wal->sync());
  wal.reset();

  auto reopened = kv::wal::WAL::open(dir_);
  ASSERT_NE(reopened, nullptr);
  ASSERT_EQ(::rename(wal_path.c_str(), moved_path.c_str()), 0);

  std::vector<kv::proto::Entry> recovered;
  EXPECT_THROW(reopened->recover(recovered), std::runtime_error);
}
