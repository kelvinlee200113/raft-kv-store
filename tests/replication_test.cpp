#include <gtest/gtest.h>
#include <raft/raft.h>

#include <algorithm>

using namespace kv;

// Helper function to create a 3-node Raft cluster
class ReplicationTest : public ::testing::Test {
protected:
  void SetUp() override {
    // Create 3-node cluster (nodes 1, 2, 3)
    peers_ = {1, 2, 3};

    // Node 1
    Config config1;
    config1.id = 1;
    config1.peers = peers_;
    config1.election_tick = 10;
    config1.heartbeat_tick = 1;
    node1_ = std::make_unique<Raft>(config1);

    // Node 2
    Config config2;
    config2.id = 2;
    config2.peers = peers_;
    config2.election_tick = 10;
    config2.heartbeat_tick = 1;
    node2_ = std::make_unique<Raft>(config2);

    // Node 3
    Config config3;
    config3.id = 3;
    config3.peers = peers_;
    config3.election_tick = 10;
    config3.heartbeat_tick = 1;
    node3_ = std::make_unique<Raft>(config3);
  }

  // Helper: Get node by ID
  Raft *get_node(uint64_t id) {
    if (id == 1)
      return node1_.get();
    if (id == 2)
      return node2_.get();
    if (id == 3)
      return node3_.get();
    return nullptr;
  }

  // Helper: Deliver all messages between nodes (one round)
  void deliver_messages() {
    std::vector<proto::Message> all_msgs;

    // Collect messages from all nodes
    auto msgs1 = node1_->read_messages();
    auto msgs2 = node2_->read_messages();
    auto msgs3 = node3_->read_messages();

    all_msgs.insert(all_msgs.end(), msgs1.begin(), msgs1.end());
    all_msgs.insert(all_msgs.end(), msgs2.begin(), msgs2.end());
    all_msgs.insert(all_msgs.end(), msgs3.begin(), msgs3.end());

    // Deliver each message to its recipient
    for (const auto &msg : all_msgs) {
      Raft *recipient = get_node(msg.to);
      if (!recipient)
        continue;

      if (msg.type == proto::MsgRequestVote) {
        auto response = recipient->handle_request_vote(msg);
        recipient->send(response);
      } else if (msg.type == proto::MsgRequestVoteResponse) {
        recipient->handle_request_vote_response(msg);
      } else if (msg.type == proto::MsgAppendEntries) {
        auto response = recipient->handle_append_entries(msg);
        recipient->send(response);
      } else if (msg.type == proto::MsgAppendEntriesResponse) {
        recipient->handle_append_entries_response(msg);
      }
    }
  }

  // Helper: Make node 1 the leader
  void make_node1_leader() {
    // Force node 1 to become candidate
    node1_->become_candidate();
    node1_->campaign();

    // Deliver RequestVote messages and responses
    deliver_messages(); // Deliver RequestVote to followers
    deliver_messages(); // Deliver responses back to candidate

    ASSERT_EQ(node1_->get_state(), State::Leader);
    ASSERT_EQ(node1_->get_leader(), 1);
  }

  std::vector<uint64_t> peers_;
  std::unique_ptr<Raft> node1_;
  std::unique_ptr<Raft> node2_;
  std::unique_ptr<Raft> node3_;
};

TEST_F(ReplicationTest, CandidateStepsDownForSameTermLeader) {
  node3_->become_candidate();
  ASSERT_EQ(node3_->get_state(), State::Candidate);
  ASSERT_EQ(node3_->get_term(), 1u);
  ASSERT_EQ(node3_->get_voted_for(), 3u);

  proto::Message heartbeat;
  heartbeat.type = proto::MsgAppendEntries;
  heartbeat.from = 2;
  heartbeat.to = 3;
  heartbeat.term = 1;
  heartbeat.prev_log_index = 0;
  heartbeat.prev_log_term = 0;
  heartbeat.leader_commit = 0;

  const auto response = node3_->handle_append_entries(heartbeat);

  EXPECT_TRUE(response.success);
  EXPECT_EQ(node3_->get_state(), State::Follower);
  EXPECT_EQ(node3_->get_leader(), 2u);
  EXPECT_EQ(node3_->get_voted_for(), 3u)
      << "Stepping down in the same term must not grant a second vote";
}

TEST_F(ReplicationTest, ProposalReportsLeaderAcceptanceAndLogIndex) {
  EXPECT_FALSE(node2_->propose({1, 2, 3}).has_value());

  make_node1_leader();
  const auto accepted = node1_->propose({1, 2, 3});

  ASSERT_TRUE(accepted.has_value());
  EXPECT_EQ(*accepted, 1u);
}

TEST_F(ReplicationTest, FollowerRejectsLeaderOutsideStaticCluster) {
  proto::Message append;
  append.type = proto::MsgAppendEntries;
  append.from = 99;
  append.to = 2;
  append.term = 100;
  append.prev_log_index = 0;

  const auto response = node2_->handle_append_entries(append);

  EXPECT_FALSE(response.success);
  EXPECT_EQ(response.term, 0u);
  EXPECT_EQ(node2_->get_term(), 0u);
  EXPECT_TRUE(node2_->get_log().empty());
}

TEST_F(ReplicationTest, CommittedEntryIsNeverReplacedByAConflictingLeader) {
  proto::Entry committed;
  committed.index = 1;
  committed.term = 1;
  committed.type = proto::EntryNormal;
  committed.data = {1};

  proto::Message first_append;
  first_append.type = proto::MsgAppendEntries;
  first_append.from = 1;
  first_append.to = 2;
  first_append.term = 1;
  first_append.prev_log_index = 0;
  first_append.prev_log_term = 0;
  first_append.entries = {committed};
  first_append.leader_commit = 1;
  ASSERT_TRUE(node2_->handle_append_entries(first_append).success);
  ASSERT_EQ(node2_->get_commit_index(), 1u);

  proto::Entry conflict = committed;
  conflict.term = 2;
  conflict.data = {9};

  proto::Message conflicting_append = first_append;
  conflicting_append.from = 3;
  conflicting_append.term = 2;
  conflicting_append.entries = {conflict};

  const auto response = node2_->handle_append_entries(conflicting_append);

  EXPECT_FALSE(response.success);
  ASSERT_EQ(node2_->get_log().size(), 1u);
  EXPECT_EQ(node2_->get_log().front().term, 1u);
  EXPECT_EQ(node2_->get_log().front().data, (std::vector<uint8_t>{1}));
  EXPECT_EQ(node2_->get_commit_index(), 1u);
}

TEST_F(ReplicationTest, LeaderIgnoresAppendResponseFromOutsideStaticCluster) {
  make_node1_leader();
  ASSERT_TRUE(node1_->propose({1}).has_value());

  proto::Message forged_response;
  forged_response.type = proto::MsgAppendEntriesResponse;
  forged_response.from = 99;
  forged_response.to = 1;
  forged_response.term = 100;
  forged_response.success = true;
  forged_response.match_index = 1;
  node1_->handle_append_entries_response(forged_response);

  EXPECT_EQ(node1_->get_state(), State::Leader);
  EXPECT_EQ(node1_->get_term(), 1u);
  EXPECT_EQ(node1_->get_commit_index(), 0u);
}

TEST_F(ReplicationTest, FailedAppendAtFirstIndexDoesNotUnderflowProgress) {
  make_node1_leader();
  node1_->read_messages();

  proto::Message failure;
  failure.type = proto::MsgAppendEntriesResponse;
  failure.from = 2;
  failure.to = 1;
  failure.term = 1;
  failure.success = false;
  node1_->handle_append_entries_response(failure);

  EXPECT_EQ(node1_->get_progress().at(2).next, 1u);
  const auto retry = node1_->read_messages();
  ASSERT_FALSE(retry.empty());
  EXPECT_TRUE(std::any_of(retry.begin(), retry.end(), [](const auto &message) {
    return message.to == 2 && message.type == proto::MsgAppendEntries;
  }));
  EXPECT_FALSE(std::any_of(retry.begin(), retry.end(), [](const auto &message) {
    return message.to == 2 && message.type == proto::MsgInstallSnapshot;
  }));
}

// Test 1: Basic Replication - leader proposes, followers replicate
TEST_F(ReplicationTest, BasicReplication) {
  // Make node 1 the leader
  make_node1_leader();

  // Leader proposes an entry
  std::vector<uint8_t> data = {1, 2, 3, 4, 5};
  node1_->propose(data);

  // Check leader's log
  EXPECT_EQ(node1_->get_log().size(), 1);
  EXPECT_EQ(node1_->get_log()[0].index, 1);
  EXPECT_EQ(node1_->get_log()[0].term, 1);
  EXPECT_EQ(node1_->get_log()[0].data, data);

  // Deliver AppendEntries to followers
  deliver_messages();

  // Check followers received the entry
  EXPECT_EQ(node2_->get_log().size(), 1);
  EXPECT_EQ(node2_->get_log()[0].index, 1);
  EXPECT_EQ(node2_->get_log()[0].term, 1);
  EXPECT_EQ(node2_->get_log()[0].data, data);

  EXPECT_EQ(node3_->get_log().size(), 1);
  EXPECT_EQ(node3_->get_log()[0].index, 1);
  EXPECT_EQ(node3_->get_log()[0].term, 1);
  EXPECT_EQ(node3_->get_log()[0].data, data);

  // Deliver AppendEntriesResponse back to leader
  deliver_messages();

  // Leader should advance commit_index
  EXPECT_EQ(node1_->get_commit_index(), 1);

  // Check Progress tracking
  auto &progress = node1_->get_progress();
  EXPECT_EQ(progress.at(2).match, 1);
  EXPECT_EQ(progress.at(2).next, 2);
  EXPECT_EQ(progress.at(3).match, 1);
  EXPECT_EQ(progress.at(3).next, 2);
}

// Test 2: Multiple Entries - leader proposes 3 entries, all replicate
TEST_F(ReplicationTest, MultipleEntries) {
  make_node1_leader();

  // Propose 3 entries
  std::vector<uint8_t> data1 = {1};
  std::vector<uint8_t> data2 = {2};
  std::vector<uint8_t> data3 = {3};

  node1_->propose(data1);
  node1_->propose(data2);
  node1_->propose(data3);

  EXPECT_EQ(node1_->get_log().size(), 3);
  EXPECT_EQ(node1_->get_commit_index(), 0); // Not committed yet

  // Deliver to followers
  deliver_messages();

  // All followers should have 3 entries
  EXPECT_EQ(node2_->get_log().size(), 3);
  EXPECT_EQ(node3_->get_log().size(), 3);

  // Deliver responses back to leader
  deliver_messages();

  // Leader should commit all 3 entries (majority replication)
  EXPECT_EQ(node1_->get_commit_index(), 3);

  // Check Progress
  auto &progress = node1_->get_progress();
  EXPECT_EQ(progress.at(2).match, 3);
  EXPECT_EQ(progress.at(3).match, 3);
}

// Test 3: Follower Commit Update - follower updates commit_index from
// leader_commit
TEST_F(ReplicationTest, FollowerCommitUpdate) {
  make_node1_leader();

  // Propose entry
  std::vector<uint8_t> data = {1, 2, 3};
  node1_->propose(data);

  // Deliver to followers
  deliver_messages();

  // Followers should have entry but commit_index = 0 (leader_commit was 0)
  EXPECT_EQ(node2_->get_log().size(), 1);
  EXPECT_EQ(node2_->get_commit_index(), 0);

  // Deliver responses to leader
  deliver_messages();

  // Leader commits
  EXPECT_EQ(node1_->get_commit_index(), 1);

  // Send another heartbeat (which will include leader_commit = 1)
  node1_->broadcast_heartbeat();
  deliver_messages();

  // Followers should now update commit_index
  EXPECT_EQ(node2_->get_commit_index(), 1);
  EXPECT_EQ(node3_->get_commit_index(), 1);
}

// Test 4: Conflict Resolution - follower has conflicting entry
TEST_F(ReplicationTest, ConflictResolution) {
  // Node 3 starts with an uncommitted entry from an earlier term.
  proto::Entry conflict_entry;
  conflict_entry.index = 1;
  conflict_entry.term = 2;
  conflict_entry.data = {2, 2, 2};
  conflict_entry.type = proto::EntryNormal;
  node3_->test_append_log_entry(conflict_entry);
  node3_->become_follower(2, 0);

  // Node 1 is the later-term leader with a different entry at index 1.
  node1_->become_candidate();        // term 1
  node1_->become_follower(2, 0);     // observe term 2
  node1_->become_candidate();        // term 3
  node1_->become_leader();
  const std::vector<uint8_t> leader_data = {1, 1, 1};
  ASSERT_TRUE(node1_->propose(leader_data).has_value());

  proto::Message append_msg;
  append_msg.type = proto::MsgAppendEntries;
  append_msg.from = 1;
  append_msg.to = 3;
  append_msg.term = 3;
  append_msg.prev_log_index = 0;
  append_msg.prev_log_term = 0;
  append_msg.entries.push_back(node1_->get_log()[0]);
  append_msg.leader_commit = 0;

  auto response = node3_->handle_append_entries(append_msg);

  // Node3 should detect conflict at index 1 and replace it
  EXPECT_EQ(node3_->get_log().size(), 1);
  EXPECT_EQ(node3_->get_log()[0].term, 3);
  EXPECT_EQ(node3_->get_log()[0].data, leader_data);
}

TEST_F(ReplicationTest, LeaderAloneCannotCommitAProposal) {
  make_node1_leader();
  node1_->read_messages();

  ASSERT_TRUE(node1_->propose({1, 2, 3}).has_value());

  EXPECT_EQ(node1_->get_commit_index(), 0u);
  EXPECT_EQ(node2_->get_log().size(), 0u);
  EXPECT_EQ(node3_->get_log().size(), 0u);
}

// One follower plus the leader is the required two-of-three majority.
TEST_F(ReplicationTest, LeaderAndOneFollowerFormAMajority) {
  make_node1_leader();

  // Propose entry
  std::vector<uint8_t> data = {1, 2, 3};
  node1_->propose(data);

  // Only deliver to node2 (not node3)
  auto msgs = node1_->read_messages();
  for (const auto &msg : msgs) {
    if (msg.to == 2 && msg.type == proto::MsgAppendEntries) {
      auto response = node2_->handle_append_entries(msg);
      node1_->handle_append_entries_response(response);
    }
  }

  // Node2 has the entry
  EXPECT_EQ(node2_->get_log().size(), 1);

  // Node3 does NOT have the entry
  EXPECT_EQ(node3_->get_log().size(), 0);

  // The leader and node 2 are two of the three configured members.
  EXPECT_EQ(node1_->get_commit_index(), 1);
}

TEST_F(ReplicationTest,
       OlderTermEntryCommitsOnlyWithAReplicatedCurrentTermEntry) {
  proto::Entry older_entry;
  older_entry.index = 1;
  older_entry.term = 1;
  older_entry.type = proto::EntryNormal;
  older_entry.data = {1};

  proto::Message older_append;
  older_append.type = proto::MsgAppendEntries;
  older_append.from = 3;
  older_append.term = 1;
  older_append.prev_log_index = 0;
  older_append.prev_log_term = 0;
  older_append.entries = {older_entry};
  older_append.leader_commit = 0;

  older_append.to = 1;
  ASSERT_TRUE(node1_->handle_append_entries(older_append).success);
  older_append.to = 2;
  ASSERT_TRUE(node2_->handle_append_entries(older_append).success);

  node1_->become_candidate();
  node1_->campaign();
  const auto vote_requests = node1_->read_messages();
  const auto vote_request = std::find_if(
      vote_requests.begin(), vote_requests.end(), [](const auto &message) {
        return message.type == proto::MsgRequestVote && message.to == 2;
      });
  ASSERT_NE(vote_request, vote_requests.end());
  node1_->handle_request_vote_response(
      node2_->handle_request_vote(*vote_request));
  ASSERT_EQ(node1_->get_state(), State::Leader);
  ASSERT_EQ(node1_->get_term(), 2u);

  const auto heartbeats = node1_->read_messages();
  const auto node2_heartbeat = std::find_if(
      heartbeats.begin(), heartbeats.end(), [](const auto &message) {
        return message.type == proto::MsgAppendEntries && message.to == 2;
      });
  ASSERT_NE(node2_heartbeat, heartbeats.end());
  node1_->handle_append_entries_response(
      node2_->handle_append_entries(*node2_heartbeat));

  ASSERT_EQ(node1_->get_progress().at(2).match, 1u);
  EXPECT_EQ(node1_->get_commit_index(), 0u)
      << "A majority cannot directly commit an entry from an older term";

  ASSERT_EQ(node1_->propose({2}), 2u);
  const auto appends = node1_->read_messages();
  const auto node2_append = std::find_if(
      appends.begin(), appends.end(), [](const auto &message) {
        return message.type == proto::MsgAppendEntries && message.to == 2;
      });
  ASSERT_NE(node2_append, appends.end());
  node1_->handle_append_entries_response(
      node2_->handle_append_entries(*node2_append));

  EXPECT_EQ(node1_->get_commit_index(), 2u)
      << "Committing a current-term entry also commits its log prefix";
}

// Test 6: Get Entries To Apply
TEST_F(ReplicationTest, GetEntriesToApply) {
  make_node1_leader();

  // Leader proposes 3 entries
  node1_->propose({1, 2, 3});
  node1_->propose({4, 5, 6});
  node1_->propose({7, 8, 9});

  node1_->broadcast_heartbeat();

  // Replicate to followers
  auto msgs = node1_->read_messages();
  for (const auto &msg : msgs) {
    if (msg.type == proto::MsgAppendEntries) {
      if (msg.to == 2) {
        auto response = node2_->handle_append_entries(msg);
        node1_->handle_append_entries_response(response);
      }
      if (msg.to == 3) {
        auto response = node3_->handle_append_entries(msg);
        node1_->handle_append_entries_response(response);
      }
    }
  }

  // All 3 entries should be committed now
  EXPECT_EQ(node1_->get_commit_index(), 3);

  // The state-machine owner receives only the next committed entry.
  const auto entry = node1_->next_entry_to_apply();
  ASSERT_TRUE(entry.has_value());
  EXPECT_EQ(entry->index, 1u);

  // Until it acknowledges application, the same entry remains next.
  const auto repeated = node1_->next_entry_to_apply();
  ASSERT_TRUE(repeated.has_value());
  EXPECT_EQ(repeated->index, 1u);
}

// Test 7: Advance - update last_applied after applying entries
TEST_F(ReplicationTest, Advance) {
  make_node1_leader();

  // Propose and commit 5 entries
  for (int i = 1; i <= 5; i++) {
    node1_->propose({static_cast<uint8_t>(i)});
  }

  node1_->broadcast_heartbeat();
  deliver_messages();  // Replicate to followers
  deliver_messages();  // Deliver responses back to leader

  EXPECT_EQ(node1_->get_commit_index(), 5);
  EXPECT_EQ(node1_->get_last_applied(), 0);  // Not applied yet

  // Apply the first three entries in order.
  for (std::uint64_t expected = 1; expected <= 3; ++expected) {
    const auto entry = node1_->next_entry_to_apply();
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->index, expected);
    node1_->advance(entry->index);
  }
  EXPECT_EQ(node1_->get_last_applied(), 3);

  const auto fourth = node1_->next_entry_to_apply();
  ASSERT_TRUE(fourth.has_value());
  EXPECT_EQ(fourth->index, 4u);

  node1_->advance(fourth->index);
  const auto fifth = node1_->next_entry_to_apply();
  ASSERT_TRUE(fifth.has_value());
  EXPECT_EQ(fifth->index, 5u);
  node1_->advance(fifth->index);
  EXPECT_EQ(node1_->get_last_applied(), 5);

  // No more entries to apply
  EXPECT_FALSE(node1_->next_entry_to_apply().has_value());

  // Test invariant: Can't go backward
  node1_->advance(3);
  EXPECT_EQ(node1_->get_last_applied(), 5);  // Should stay at 5

  // Test invariant: Can't advance beyond commit_index
  node1_->advance(10);
  EXPECT_EQ(node1_->get_last_applied(), 5);  // Should stay at 5
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
