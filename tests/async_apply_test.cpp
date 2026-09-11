#include <gtest/gtest.h>

#include <raft/raft.h>

#include <cstdint>

namespace {

kv::Raft make_raft() {
  kv::Config config;
  config.id = 1;
  config.peers = {1, 2, 3};
  return kv::Raft(config);
}

void append_entries(kv::Raft &raft, std::uint64_t first,
                    std::uint64_t last) {
  for (std::uint64_t index = first; index <= last; ++index) {
    kv::proto::Entry entry;
    entry.type = kv::proto::EntryNormal;
    entry.index = index;
    entry.term = 1;
    entry.data = {static_cast<std::uint8_t>(index & 0xffU)};
    raft.test_append_log_entry(entry);
  }
}

}  // namespace

TEST(AsyncApplyTest, UncommittedEntriesAreNotExposed) {
  auto raft = make_raft();
  append_entries(raft, 1, 2);

  EXPECT_FALSE(raft.next_entry_to_apply().has_value());

  raft.test_set_commit_index(1);
  const auto entry = raft.next_entry_to_apply();
  ASSERT_TRUE(entry.has_value());
  EXPECT_EQ(entry->index, 1u);
}

TEST(AsyncApplyTest, NextEntryRepeatsUntilAcknowledged) {
  auto raft = make_raft();
  append_entries(raft, 1, 3);
  raft.test_set_commit_index(3);

  const auto first = raft.next_entry_to_apply();
  const auto repeated = raft.next_entry_to_apply();
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(repeated.has_value());
  EXPECT_EQ(first->index, 1u);
  EXPECT_EQ(repeated->index, 1u);

  raft.advance(first->index);
  const auto second = raft.next_entry_to_apply();
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(second->index, 2u);
}

TEST(AsyncApplyTest, LaterCommitsDoNotReplaceTheEntryInFlight) {
  auto raft = make_raft();
  append_entries(raft, 1, 5);
  raft.test_set_commit_index(2);

  const auto in_flight = raft.next_entry_to_apply();
  ASSERT_TRUE(in_flight.has_value());
  ASSERT_EQ(in_flight->index, 1u);

  raft.test_set_commit_index(5);
  const auto still_in_flight = raft.next_entry_to_apply();
  ASSERT_TRUE(still_in_flight.has_value());
  EXPECT_EQ(still_in_flight->index, 1u);

  raft.advance(in_flight->index);
  const auto next = raft.next_entry_to_apply();
  ASSERT_TRUE(next.has_value());
  EXPECT_EQ(next->index, 2u);
}

TEST(AsyncApplyTest, LargeCommittedBacklogAppliesWithoutLoss) {
  auto raft = make_raft();
  constexpr std::uint64_t kEntryCount = 10001;
  append_entries(raft, 1, kEntryCount);
  raft.test_set_commit_index(kEntryCount);

  for (std::uint64_t expected = 1; expected <= kEntryCount; ++expected) {
    const auto entry = raft.next_entry_to_apply();
    ASSERT_TRUE(entry.has_value());
    ASSERT_EQ(entry->index, expected);
    raft.advance(entry->index);
  }

  EXPECT_EQ(raft.get_last_applied(), kEntryCount);
  EXPECT_FALSE(raft.next_entry_to_apply().has_value());
}

TEST(AsyncApplyTest, AdvanceRequiresTheNextCommittedIndex) {
  auto raft = make_raft();
  append_entries(raft, 1, 3);
  raft.test_set_commit_index(3);

  raft.advance(2);
  EXPECT_EQ(raft.get_last_applied(), 0u);

  raft.advance(1);
  EXPECT_EQ(raft.get_last_applied(), 1u);

  raft.advance(1);
  EXPECT_EQ(raft.get_last_applied(), 1u);

  raft.advance(3);
  EXPECT_EQ(raft.get_last_applied(), 1u);

  raft.advance(2);
  EXPECT_EQ(raft.get_last_applied(), 2u);

  raft.advance(3);
  EXPECT_EQ(raft.get_last_applied(), 3u);
  EXPECT_FALSE(raft.next_entry_to_apply().has_value());
}
