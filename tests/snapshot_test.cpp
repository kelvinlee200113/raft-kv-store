#include <gtest/gtest.h>
#include <raft/raft.h>
#include <server/kv_store.h>

#include <algorithm>
#include <chrono>
#include <filesystem>

namespace {

kv::Config follower_config() {
  kv::Config config;
  config.id = 2;
  config.peers = {1, 2, 3};
  return config;
}

kv::proto::Message snapshot_message(uint64_t index, uint64_t term,
                                    const std::vector<uint8_t> &state) {
  kv::proto::Message message;
  message.type = kv::proto::MsgInstallSnapshot;
  message.from = 1;
  message.to = 2;
  message.term = term;
  message.snapshot_index = index;
  message.snapshot_term = term;
  message.snapshot_data = state;
  return message;
}

TEST(SnapshotTest, InstallRestoresStateMachineBeforeAcknowledging) {
  kv::KVStore source;
  source.set("account", "42");

  kv::KVStore restored;
  kv::Raft follower(follower_config());
  bool restore_finished = false;
  follower.set_snapshot_restore_callback(
      [&](uint64_t index, const std::vector<uint8_t> &bytes) {
        EXPECT_EQ(index, 5u);
        restored.deserialize(bytes);
        restore_finished = true;
        return true;
      });

  const auto response =
      follower.handle_install_snapshot(snapshot_message(5, 2, source.serialize()));

  std::string value;
  EXPECT_TRUE(response.success);
  EXPECT_TRUE(restore_finished);
  EXPECT_TRUE(restored.get("account", value));
  EXPECT_EQ(value, "42");
  EXPECT_EQ(follower.get_log_offset(), 5u);
  EXPECT_EQ(follower.get_commit_index(), 5u);
  EXPECT_EQ(follower.get_last_applied(), 5u);
}

TEST(SnapshotTest, AppendEntriesMatchesAtCompactedBoundary) {
  kv::KVStore source;
  source.set("key", "snapshot-value");

  kv::Raft follower(follower_config());
  follower.set_snapshot_restore_callback(
      [](uint64_t, const std::vector<uint8_t> &) { return true; });
  ASSERT_TRUE(follower
                  .handle_install_snapshot(
                      snapshot_message(5, 2, source.serialize()))
                  .success);

  kv::proto::Message append;
  append.type = kv::proto::MsgAppendEntries;
  append.from = 1;
  append.to = 2;
  append.term = 2;
  append.prev_log_index = 5;
  append.prev_log_term = 2;
  kv::proto::Entry entry;
  entry.index = 6;
  entry.term = 2;
  entry.data = {1};
  append.entries.push_back(entry);
  append.leader_commit = 5;

  const auto response = follower.handle_append_entries(append);

  EXPECT_TRUE(response.success);
  ASSERT_EQ(follower.get_log().size(), 1u);
  EXPECT_EQ(follower.get_log().front().index, 6u);
}

TEST(SnapshotTest, RestartFiltersEntriesCoveredBySnapshot) {
  kv::Raft follower(follower_config());
  std::vector<kv::proto::Entry> recovered_entries;
  for (uint64_t index = 1; index <= 5; ++index) {
    kv::proto::Entry entry;
    entry.index = index;
    entry.term = index <= 3 ? 1 : 2;
    entry.data = {static_cast<uint8_t>(index)};
    recovered_entries.push_back(entry);
  }

  kv::wal::SnapshotMeta snapshot;
  snapshot.index = 3;
  snapshot.term = 1;
  snapshot.state = {9, 9, 9};
  follower.restore(kv::wal::HardStateProto{2, 0, 5}, recovered_entries,
                   snapshot);

  EXPECT_EQ(follower.get_log_offset(), 3u);
  EXPECT_EQ(follower.get_last_applied(), 3u);
  ASSERT_EQ(follower.get_log().size(), 2u);
  EXPECT_EQ(follower.get_log()[0].index, 4u);
  EXPECT_EQ(follower.get_log()[1].index, 5u);

  const auto first = follower.next_entry_to_apply();
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first->index, 4u);
  follower.advance(first->index);
  const auto second = follower.next_entry_to_apply();
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(second->index, 5u);
}

TEST(SnapshotTest, SnapshotCompactsOnlyThroughTheCapturedStateIndex) {
  kv::Raft raft(follower_config());
  std::vector<kv::proto::Entry> entries;
  for (uint64_t index = 1; index <= 3; ++index) {
    kv::proto::Entry entry;
    entry.index = index;
    entry.term = 1;
    entry.data = {static_cast<uint8_t>(index)};
    entries.push_back(entry);
  }
  raft.restore(kv::wal::HardStateProto{1, 0, 3}, entries);
  for (uint64_t index = 1; index <= 3; ++index) {
    raft.advance(index);
  }

  raft.take_snapshot(2, {9});

  EXPECT_EQ(raft.get_log_offset(), 2u);
  ASSERT_EQ(raft.get_log().size(), 1u);
  EXPECT_EQ(raft.get_log().front().index, 3u);
}

TEST(SnapshotTest, RestartRejectsACommittedIndexMissingFromTheWal) {
  kv::Raft raft(follower_config());
  kv::proto::Entry only_entry;
  only_entry.index = 1;
  only_entry.term = 1;

  EXPECT_THROW(
      raft.restore(kv::wal::HardStateProto{1, 0, 2}, {only_entry}),
      std::invalid_argument);
}

TEST(SnapshotTest, RestartRejectsAGapAfterTheSnapshot) {
  kv::Raft raft(follower_config());
  kv::wal::SnapshotMeta snapshot{3, 1, {9}};
  kv::proto::Entry entry;
  entry.index = 5;
  entry.term = 2;

  EXPECT_THROW(
      raft.restore(kv::wal::HardStateProto{2, 0, 5}, {entry}, snapshot),
      std::invalid_argument);
}

TEST(SnapshotTest, OlderSnapshotNeverRollsBackACommittedSuffix) {
  kv::Raft follower(follower_config());
  std::vector<kv::proto::Entry> entries;
  for (uint64_t index = 1; index <= 5; ++index) {
    kv::proto::Entry entry;
    entry.index = index;
    entry.term = 2;
    entries.push_back(entry);
  }
  follower.restore(kv::wal::HardStateProto{2, 0, 5}, entries);
  bool restored = false;
  follower.set_snapshot_restore_callback(
      [&](uint64_t, const std::vector<uint8_t> &) {
        restored = true;
        return true;
      });

  const auto response =
      follower.handle_install_snapshot(snapshot_message(3, 2, {9}));

  EXPECT_TRUE(response.success);
  EXPECT_EQ(response.match_index, 5u);
  EXPECT_FALSE(restored);
  EXPECT_EQ(follower.get_commit_index(), 5u);
  EXPECT_EQ(follower.get_log_offset(), 0u);
}

TEST(SnapshotTest, RejectedSnapshotIsNotPersisted) {
  namespace fs = std::filesystem;
  const auto unique = std::chrono::steady_clock::now()
                          .time_since_epoch()
                          .count();
  const fs::path directory =
      fs::temp_directory_path() /
      ("raft_rejected_snapshot_" + std::to_string(unique));

  {
    kv::Raft follower(follower_config());
    auto wal = kv::wal::WAL::create(directory.string());
    ASSERT_NE(wal, nullptr);
    follower.set_wal(std::move(wal));
    follower.set_snapshot_restore_callback(
        [](uint64_t, const std::vector<uint8_t> &) { return false; });

    EXPECT_FALSE(
        follower.handle_install_snapshot(snapshot_message(5, 2, {9})).success);
  }

  auto reopened = kv::wal::WAL::open(directory.string());
  ASSERT_NE(reopened, nullptr);
  std::vector<kv::proto::Entry> entries;
  kv::wal::SnapshotMeta recovered;
  reopened->recover(entries, &recovered);
  EXPECT_TRUE(recovered.is_empty());
  reopened.reset();
  fs::remove_all(directory);
}

TEST(SnapshotTest, LeaderCatchesUpLaggingFollowerThenReplicatesLogSuffix) {
  kv::Config leader_config;
  leader_config.id = 1;
  leader_config.peers = {1, 2, 3};
  kv::Raft leader(leader_config);
  leader.become_candidate();
  leader.become_leader();
  leader.read_messages();

  for (uint8_t value = 1; value <= 4; ++value) {
    ASSERT_TRUE(leader.propose({value}).has_value());
  }
  leader.read_messages();

  kv::proto::Message replicated;
  replicated.type = kv::proto::MsgAppendEntriesResponse;
  replicated.from = 3;
  replicated.to = 1;
  replicated.term = leader.get_term();
  replicated.success = true;
  replicated.match_index = 4;
  leader.handle_append_entries_response(replicated);
  ASSERT_EQ(leader.get_commit_index(), 4u);
  for (uint64_t index = 1; index <= 3; ++index) {
    leader.advance(index);
  }

  kv::KVStore snapshot_source;
  snapshot_source.set("account", "42");
  leader.take_snapshot(3, snapshot_source.serialize());
  ASSERT_EQ(leader.get_log_offset(), 3u);

  leader.read_messages();
  leader.broadcast_heartbeat();
  const auto outbound = leader.read_messages();
  const auto snapshot_request = std::find_if(
      outbound.begin(), outbound.end(), [](const auto &message) {
        return message.to == 2 &&
               message.type == kv::proto::MsgInstallSnapshot;
      });
  ASSERT_NE(snapshot_request, outbound.end());
  EXPECT_EQ(snapshot_request->snapshot_index, 3u);

  kv::KVStore restored;
  kv::Raft lagging(follower_config());
  lagging.set_snapshot_restore_callback(
      [&restored](uint64_t, const std::vector<uint8_t> &state) {
        restored.deserialize(state);
        return true;
      });
  const auto snapshot_response =
      lagging.handle_install_snapshot(*snapshot_request);
  ASSERT_TRUE(snapshot_response.success);
  leader.handle_install_snapshot_response(snapshot_response);

  const auto resumed = leader.read_messages();
  const auto append =
      std::find_if(resumed.begin(), resumed.end(), [](const auto &message) {
        return message.to == 2 && message.type == kv::proto::MsgAppendEntries;
      });
  ASSERT_NE(append, resumed.end());
  EXPECT_EQ(append->prev_log_index, 3u);
  ASSERT_EQ(append->entries.size(), 1u);
  EXPECT_EQ(append->entries.front().index, 4u);

  const auto append_response = lagging.handle_append_entries(*append);
  EXPECT_TRUE(append_response.success);
  EXPECT_EQ(lagging.get_log_offset(), 3u);
  ASSERT_EQ(lagging.get_log().size(), 1u);
  EXPECT_EQ(lagging.get_log().front().index, 4u);
  EXPECT_EQ(lagging.get_commit_index(), 4u);

  std::string value;
  EXPECT_TRUE(restored.get("account", value));
  EXPECT_EQ(value, "42");
}

} // namespace
