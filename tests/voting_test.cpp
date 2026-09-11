#include <gtest/gtest.h>
#include <raft/config.h>
#include <raft/raft.h>

#include <algorithm>

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

TEST(RaftConfigurationTest, RejectsMembershipThatIsNotExactlyThreeNodes) {
  kv::Config two_nodes;
  two_nodes.id = 1;
  two_nodes.peers = {1, 2};
  EXPECT_THROW(kv::Raft raft(two_nodes), std::invalid_argument);

  kv::Config four_nodes;
  four_nodes.id = 1;
  four_nodes.peers = {1, 2, 3, 4};
  EXPECT_THROW(kv::Raft raft(four_nodes), std::invalid_argument);
}

TEST(RaftConfigurationTest, RejectsZeroAsAMemberIdentity) {
  kv::Config config;
  config.id = 1;
  config.peers = {0, 1, 2};

  EXPECT_THROW(kv::Raft raft(config), std::invalid_argument);
}

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

TEST_F(RaftVotingTest, BecomeFollowerNeverMovesTermBackward) {
  kv::Raft raft(config);
  raft.become_candidate();
  ASSERT_EQ(raft.get_term(), 1u);

  raft.become_follower(0, 2);

  EXPECT_EQ(raft.get_term(), 1u);
  EXPECT_EQ(raft.get_state(), kv::State::Candidate);
}

TEST_F(RaftVotingTest, RejectsInvalidTimingConfiguration) {
  config.election_tick = 0;
  EXPECT_THROW(kv::Raft raft(config), std::invalid_argument);
}

TEST_F(RaftVotingTest, RejectsDuplicateClusterMembers) {
  config.peers = {1, 2, 2};
  EXPECT_THROW(kv::Raft raft(config), std::invalid_argument);
}

TEST_F(RaftVotingTest, PreCampaignRequestsTheNextTermWithoutAdvancingLocally) {
  kv::Raft raft(config);
  raft.become_pre_candidate();
  raft.pre_campaign();

  EXPECT_EQ(raft.get_term(), 0u);
  const auto messages = raft.read_messages();
  ASSERT_EQ(messages.size(), 2u);
  for (const auto &message : messages) {
    EXPECT_EQ(message.type, kv::proto::MsgPreVote);
    EXPECT_EQ(message.term, 1u);
  }
}

TEST_F(RaftVotingTest, RejectsStalePreVote) {
  config.id = 2;
  kv::Raft raft(config);
  raft.become_follower(2, 0);

  kv::proto::Message request;
  request.type = kv::proto::MsgPreVote;
  request.from = 1;
  request.to = 2;
  request.term = 2;
  request.last_log_index = 0;
  request.last_log_term = 0;

  EXPECT_FALSE(raft.handle_pre_vote(request).vote_granted);
}

TEST_F(RaftVotingTest, HigherTermPreVoteResponseStepsDown) {
  kv::Raft raft(config);
  raft.become_candidate();
  raft.become_pre_candidate();

  kv::proto::Message response;
  response.type = kv::proto::MsgPreVoteResponse;
  response.from = 2;
  response.to = 1;
  response.term = 2;
  response.vote_granted = false;
  raft.handle_pre_vote_response(response);

  EXPECT_EQ(raft.get_state(), kv::State::Follower);
  EXPECT_EQ(raft.get_term(), 2u);
}

TEST_F(RaftVotingTest, StalePreVoteResponseDoesNotCountInANewerTerm) {
  kv::Raft raft(config);
  raft.become_candidate();
  raft.become_pre_candidate();
  raft.pre_campaign();
  raft.read_messages();

  kv::proto::Message stale;
  stale.type = kv::proto::MsgPreVoteResponse;
  stale.from = 2;
  stale.to = 1;
  stale.term = 1;
  stale.pre_vote_term = 1;
  stale.vote_granted = true;
  raft.handle_pre_vote_response(stale);

  EXPECT_EQ(raft.get_state(), kv::State::PreCandidate);
  EXPECT_EQ(raft.get_term(), 1u);
}

TEST_F(RaftVotingTest, CountsEligiblePreVoteFromALaggingTermPeer) {
  kv::Raft candidate(config);
  candidate.become_follower(5, 0);
  candidate.become_pre_candidate();
  candidate.pre_campaign();
  const auto requests = candidate.read_messages();
  const auto request_it = std::find_if(
      requests.begin(), requests.end(), [](const auto &message) {
        return message.to == 3;
      });
  ASSERT_NE(request_it, requests.end());
  const auto request = *request_it;
  ASSERT_EQ(request.term, 6u);

  config.id = 3;
  kv::Raft lagging_peer(config);
  lagging_peer.become_follower(4, 0);
  const auto response = lagging_peer.handle_pre_vote(request);
  ASSERT_TRUE(response.vote_granted);
  ASSERT_EQ(response.term, 4u);
  ASSERT_EQ(response.pre_vote_term, 6u);

  candidate.handle_pre_vote_response(response);

  EXPECT_EQ(candidate.get_state(), kv::State::Candidate);
  EXPECT_EQ(candidate.get_term(), 6u);
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

TEST_F(RaftVotingTest, HandleRequestVoteRejectsCandidateWithStaleLogTerm) {
  config.id = 2;
  kv::Raft raft(config);

  kv::proto::Entry local_entry;
  local_entry.index = 1;
  local_entry.term = 2;
  raft.test_append_log_entry(local_entry);

  kv::proto::Message request;
  request.type = kv::proto::MsgRequestVote;
  request.from = 1;
  request.to = 2;
  request.term = 3;
  request.last_log_index = 10;
  request.last_log_term = 1;

  const auto response = raft.handle_request_vote(request);

  EXPECT_EQ(response.term, 3u);
  EXPECT_FALSE(response.vote_granted);
  EXPECT_EQ(raft.get_voted_for(), 0u);
}

TEST_F(RaftVotingTest, HandleRequestVoteRejectsShorterLogInSameTerm) {
  config.id = 2;
  kv::Raft raft(config);

  for (uint64_t index = 1; index <= 2; ++index) {
    kv::proto::Entry entry;
    entry.index = index;
    entry.term = 2;
    raft.test_append_log_entry(entry);
  }

  kv::proto::Message request;
  request.type = kv::proto::MsgRequestVote;
  request.from = 1;
  request.to = 2;
  request.term = 3;
  request.last_log_index = 1;
  request.last_log_term = 2;

  const auto response = raft.handle_request_vote(request);

  EXPECT_FALSE(response.vote_granted);
  EXPECT_EQ(raft.get_voted_for(), 0u);
}

TEST_F(RaftVotingTest, HandleRequestVoteGrantsCandidateWithNewerLogTerm) {
  config.id = 2;
  kv::Raft raft(config);

  for (uint64_t index = 1; index <= 3; ++index) {
    kv::proto::Entry entry;
    entry.index = index;
    entry.term = 1;
    raft.test_append_log_entry(entry);
  }

  kv::proto::Message request;
  request.type = kv::proto::MsgRequestVote;
  request.from = 1;
  request.to = 2;
  request.term = 3;
  request.last_log_index = 1;
  request.last_log_term = 2;

  const auto response = raft.handle_request_vote(request);

  EXPECT_TRUE(response.vote_granted);
  EXPECT_EQ(raft.get_voted_for(), 1u);
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

TEST_F(RaftVotingTest, DuplicateVoteResponseCannotFabricateAQuorum) {
  kv::Raft raft(config);
  raft.become_candidate();

  kv::proto::Message granted;
  granted.type = kv::proto::MsgRequestVoteResponse;
  granted.from = 2;
  granted.to = 1;
  granted.term = raft.get_term();
  granted.vote_granted = true;

  raft.handle_request_vote_response(granted);
  ASSERT_EQ(raft.get_state(), kv::State::Candidate);

  raft.handle_request_vote_response(granted);

  EXPECT_EQ(raft.get_state(), kv::State::Candidate);
  ASSERT_EQ(raft.get_votes().size(), 1u);
  EXPECT_TRUE(raft.get_votes().at(2));
}

TEST_F(RaftVotingTest, StaleVoteResponseCannotElectANewerTermCandidate) {
  kv::Raft raft(config);
  raft.become_candidate();
  raft.campaign();
  raft.read_messages();
  const auto stale_term = raft.get_term();

  raft.become_candidate();
  raft.campaign();
  raft.read_messages();
  ASSERT_GT(raft.get_term(), stale_term);

  kv::proto::Message stale_vote;
  stale_vote.type = kv::proto::MsgRequestVoteResponse;
  stale_vote.from = 2;
  stale_vote.to = 1;
  stale_vote.term = stale_term;
  stale_vote.vote_granted = true;
  raft.handle_request_vote_response(stale_vote);

  EXPECT_EQ(raft.get_state(), kv::State::Candidate);
  ASSERT_EQ(raft.get_votes().size(), 1u);
  EXPECT_TRUE(raft.get_votes().at(1));
}

TEST_F(RaftVotingTest, DelayedHigherTermVoteResponseStepsDownANewLeader) {
  kv::Raft raft(config);
  raft.become_candidate();
  raft.campaign();
  raft.read_messages();

  kv::proto::Message granted;
  granted.type = kv::proto::MsgRequestVoteResponse;
  granted.from = 2;
  granted.to = 1;
  granted.term = 1;
  granted.vote_granted = true;
  raft.handle_request_vote_response(granted);
  ASSERT_EQ(raft.get_state(), kv::State::Leader);

  kv::proto::Message delayed_rejection;
  delayed_rejection.type = kv::proto::MsgRequestVoteResponse;
  delayed_rejection.from = 3;
  delayed_rejection.to = 1;
  delayed_rejection.term = 2;
  delayed_rejection.vote_granted = false;
  raft.handle_request_vote_response(delayed_rejection);

  EXPECT_EQ(raft.get_state(), kv::State::Follower);
  EXPECT_EQ(raft.get_term(), 2u);
}

TEST_F(RaftVotingTest, CandidateIgnoresVoteFromOutsideStaticCluster) {
  kv::Raft raft(config);
  raft.become_candidate();
  raft.campaign();
  raft.read_messages();

  kv::proto::Message forged_vote;
  forged_vote.type = kv::proto::MsgRequestVoteResponse;
  forged_vote.from = 99;
  forged_vote.to = 1;
  forged_vote.term = 100;
  forged_vote.vote_granted = true;
  raft.handle_request_vote_response(forged_vote);

  EXPECT_EQ(raft.get_state(), kv::State::Candidate);
  EXPECT_EQ(raft.get_term(), 1u);
}

TEST_F(RaftVotingTest, FollowerRejectsVoteRequestFromOutsideStaticCluster) {
  config.id = 2;
  kv::Raft raft(config);

  kv::proto::Message request;
  request.type = kv::proto::MsgRequestVote;
  request.from = 99;
  request.to = 2;
  request.term = 100;
  request.last_log_index = 0;
  request.last_log_term = 0;

  const auto response = raft.handle_request_vote(request);

  EXPECT_FALSE(response.vote_granted);
  EXPECT_EQ(response.term, 0u);
  EXPECT_EQ(raft.get_term(), 0u);
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

  // Clear initial heartbeat from become_leader()
  raft.read_messages();

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
