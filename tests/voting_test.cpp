#include <gtest/gtest.h>
#include <raft/config.h>
#include <raft/raft.h>

// Test fixture for Raft voting tests
class RaftVotingTest : public ::testing::Test {
protected:
  void SetUp() override {
    // Create a 3-node cluster config
    config.id = 1;
    config.peers = {1, 2, 3};
    config.election_tick = 10;
    config.heartbeat_tick = 1;
  }

  kv::Config config;
};

// Test: Node becomes candidate and increments term
TEST_F(RaftVotingTest, BecomeCandidateIncrementsTermAndVotesForSelf) {
  kv::Raft raft(config);

  // Initially a follower at term 0
  EXPECT_EQ(raft.get_state(), kv::State::Follower);
  EXPECT_EQ(raft.get_term(), 0);

  // Become candidate
  raft.become_candidate();

  // Check state transitions
  EXPECT_EQ(raft.get_state(), kv::State::Candidate);
  EXPECT_EQ(raft.get_term(), 1);      // Term incremented
  EXPECT_EQ(raft.get_voted_for(), 1); // Voted for self
}

// Test: Campaign generates correct RequestVote messages
TEST_F(RaftVotingTest, CampaignGeneratesRequestVoteMessages) {
  kv::Raft raft(config);
  raft.become_candidate();

  // Campaign
  raft.campaign();

  // Read messages
  auto msgs = raft.read_messages();

  // Should have 2 messages (for peers 2 and 3, not for self)
  ASSERT_EQ(msgs.size(), 2);

  // Check message types and fields
  for (const auto &msg : msgs) {
    EXPECT_EQ(msg.type, kv::proto::MsgRequestVote);
    EXPECT_EQ(msg.from, 1);
    EXPECT_EQ(msg.term, 1);
    EXPECT_EQ(msg.last_log_index, 0); // Empty log
    EXPECT_EQ(msg.last_log_term, 0);
  }

  // Check that messages go to correct peers
  bool has_msg_to_2 = false;
  bool has_msg_to_3 = false;
  for (const auto &msg : msgs) {
    if (msg.to == 2)
      has_msg_to_2 = true;
    if (msg.to == 3)
      has_msg_to_3 = true;
  }
  EXPECT_TRUE(has_msg_to_2);
  EXPECT_TRUE(has_msg_to_3);
}

// Test: Campaign records vote for self
TEST_F(RaftVotingTest, CampaignRecordsVoteForSelf) {
  kv::Raft raft(config);
  raft.become_candidate();
  raft.campaign();

  // Check votes map
  auto votes = raft.get_votes();
  EXPECT_EQ(votes.size(), 1);
  EXPECT_TRUE(votes.at(1)); // Voted for self (node 1)
}

// Test: read_messages() clears the queue
TEST_F(RaftVotingTest, ReadMessagesClearsQueue) {
  kv::Raft raft(config);
  raft.become_candidate();
  raft.campaign();

  // First read
  auto msgs1 = raft.read_messages();
  EXPECT_EQ(msgs1.size(), 2);

  // Second read - queue should be empty
  auto msgs2 = raft.read_messages();
  EXPECT_EQ(msgs2.size(), 0);
}

// Test: Handle RequestVote - grant vote to first requester
TEST_F(RaftVotingTest, HandleRequestVoteGrantsVoteToFirstRequester) {
  config.id = 2; // Create node 2
  kv::Raft raft(config);

  // Create RequestVote from node 1
  kv::proto::Message req;
  req.type = kv::proto::MsgRequestVote;
  req.from = 1;
  req.to = 2;
  req.term = 1;
  req.last_log_index = 0;
  req.last_log_term = 0;

  // Handle the request
  auto response = raft.handle_request_vote(req);

  // Check response
  EXPECT_EQ(response.type, kv::proto::MsgRequestVoteResponse);
  EXPECT_EQ(response.from, 2);
  EXPECT_EQ(response.to, 1);
  EXPECT_EQ(response.term, 1);
  EXPECT_TRUE(response.vote_granted);

  // Check internal state
  EXPECT_EQ(raft.get_voted_for(), 1);
  EXPECT_EQ(raft.get_term(), 1);
}

// Test: Handle RequestVote - reject second requester in same term
TEST_F(RaftVotingTest, HandleRequestVoteRejectsSecondRequesterInSameTerm) {
  config.id = 2;
  kv::Raft raft(config);

  // First request from node 1
  kv::proto::Message req1;
  req1.type = kv::proto::MsgRequestVote;
  req1.from = 1;
  req1.to = 2;
  req1.term = 1;
  req1.last_log_index = 0;
  req1.last_log_term = 0;

  auto response1 = raft.handle_request_vote(req1);
  EXPECT_TRUE(response1.vote_granted);

  // Second request from node 3 in same term
  kv::proto::Message req2;
  req2.type = kv::proto::MsgRequestVote;
  req2.from = 3;
  req2.to = 2;
  req2.term = 1;
  req2.last_log_index = 0;
  req2.last_log_term = 0;

  auto response2 = raft.handle_request_vote(req2);
  EXPECT_FALSE(response2.vote_granted); // Rejected!
  EXPECT_EQ(raft.get_voted_for(), 1);   // Still voted for node 1
}

// Test: Handle RequestVote - idempotent (same requester can ask again)
TEST_F(RaftVotingTest, HandleRequestVoteIsIdempotent) {
  config.id = 2;
  kv::Raft raft(config);

  // First request
  kv::proto::Message req;
  req.type = kv::proto::MsgRequestVote;
  req.from = 1;
  req.to = 2;
  req.term = 1;
  req.last_log_index = 0;
  req.last_log_term = 0;

  auto response1 = raft.handle_request_vote(req);
  EXPECT_TRUE(response1.vote_granted);

  // Same request again - should still grant
  auto response2 = raft.handle_request_vote(req);
  EXPECT_TRUE(response2.vote_granted);
}

// Test: Handle RequestVote with higher term updates local term
TEST_F(RaftVotingTest, HandleRequestVoteWithHigherTermUpdatesLocalTerm) {
  config.id = 2;
  kv::Raft raft(config);
  raft.become_candidate(); // Now at term 1

  EXPECT_EQ(raft.get_term(), 1);
  EXPECT_EQ(raft.get_state(), kv::State::Candidate);

  // Receive RequestVote with term 2
  kv::proto::Message req;
  req.type = kv::proto::MsgRequestVote;
  req.from = 1;
  req.to = 2;
  req.term = 2; // Higher term!
  req.last_log_index = 0;
  req.last_log_term = 0;

  auto response = raft.handle_request_vote(req);

  // Should update to term 2 and become follower
  EXPECT_EQ(raft.get_term(), 2);
  EXPECT_EQ(raft.get_state(), kv::State::Follower);
  EXPECT_EQ(response.term, 2);
  EXPECT_TRUE(response.vote_granted);
}

TEST_F(RaftVotingTest, CandidateBecomesLeaderWithMajorityVotes) {
  kv::Raft raft(config);

  // Become candidate and campaign (votes for self)
  raft.become_candidate();
  raft.campaign();

  // Clear campaign messages
  raft.read_messages();

  // Initially candidate with 1 vote (self)
  EXPECT_EQ(raft.get_state(), kv::State::Candidate);

  // Simulate vote response from node 2 (granted)
  kv::proto::Message vote_response_2;
  vote_response_2.type = kv::proto::MsgRequestVoteResponse;
  vote_response_2.from = 2;
  vote_response_2.to = 1;
  vote_response_2.term = 1;
  vote_response_2.vote_granted = true;

  raft.handle_request_vote_response(vote_response_2);

  // Now have 2/3 votes - should become leader (majority!)
  EXPECT_EQ(raft.get_state(), kv::State::Leader);
}

// Test: Leader broadcasts heartbeat messages
TEST_F(RaftVotingTest, LeaderBroadcastsHeartbeat) {
  kv::Raft raft(config);

  // Become leader
  raft.become_candidate();
  raft.campaign();
  raft.read_messages(); // Clear campaign messages

  // Simulate winning election
  kv::proto::Message vote_response;
  vote_response.type = kv::proto::MsgRequestVoteResponse;
  vote_response.from = 2;
  vote_response.to = 1;
  vote_response.term = 1;
  vote_response.vote_granted = true;
  raft.handle_request_vote_response(vote_response);

  EXPECT_EQ(raft.get_state(), kv::State::Leader);

  // Broadcast heartbeat
  raft.broadcast_heartbeat();

  // Read heartbeat messages
  auto msgs = raft.read_messages();

  // Should have 2 messages (for peers 2 and 3)
  ASSERT_EQ(msgs.size(), 2);

  // Check message fields
  for (const auto &msg : msgs) {
    EXPECT_EQ(msg.type, kv::proto::MsgAppendEntries);
    EXPECT_EQ(msg.from, 1);
    EXPECT_EQ(msg.term, 1);
    EXPECT_EQ(msg.prev_log_index, 0); // Empty log
    EXPECT_EQ(msg.prev_log_term, 0);
    EXPECT_EQ(msg.leader_commit, 0);
    EXPECT_TRUE(msg.entries.empty()); // Heartbeat has no entries
  }
}

// Test: Follower handles heartbeat and resets election timer
TEST_F(RaftVotingTest, FollowerHandlesHeartbeatAndResetsTimer) {
  config.id = 2;
  kv::Raft raft(config);

  // Simulate some ticks
  for (int i = 0; i < 5; i++) {
    raft.tick();
  }
  EXPECT_EQ(raft.get_election_elapsed(), 5);

  // Receive heartbeat from leader
  kv::proto::Message heartbeat;
  heartbeat.type = kv::proto::MsgAppendEntries;
  heartbeat.from = 1;
  heartbeat.to = 2;
  heartbeat.term = 1;
  heartbeat.prev_log_index = 0;
  heartbeat.prev_log_term = 0;
  heartbeat.leader_commit = 0;

  auto response = raft.handle_append_entries(heartbeat);

  // Check response
  EXPECT_EQ(response.type, kv::proto::MsgAppendEntriesResponse);
  EXPECT_EQ(response.from, 2);
  EXPECT_EQ(response.to, 1);
  EXPECT_TRUE(response.success);

  // Check internal state
  EXPECT_EQ(raft.get_term(), 1);
  EXPECT_EQ(raft.get_leader(), 1);
  EXPECT_EQ(raft.get_election_elapsed(), 0); // Timer reset!
}

// Main function to run all tests
int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
