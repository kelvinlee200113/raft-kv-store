#include <gtest/gtest.h>
#include <raft/config.h>
#include <raft/proto.h>
#include <raft/raft.h>

namespace {

kv::proto::Message elect_leader(kv::Raft &raft) {
  raft.become_candidate();
  raft.campaign();
  raft.read_messages();

  kv::proto::Message vote;
  vote.type = kv::proto::MsgRequestVoteResponse;
  vote.from = 2;
  vote.to = 1;
  vote.term = 1;
  vote.vote_granted = true;
  raft.handle_request_vote_response(vote);
  EXPECT_EQ(raft.get_state(), kv::State::Leader);
  raft.read_messages();
  return vote;
}

kv::proto::Message successful_response(const kv::proto::Message &request) {
  kv::proto::Message response;
  response.type = kv::proto::MsgAppendEntriesResponse;
  response.from = request.to;
  response.to = request.from;
  response.term = request.term;
  response.success = true;
  response.match_index = request.prev_log_index + request.entries.size();
  response.read_context = request.read_context;
  return response;
}

class ReadIndexTest : public ::testing::Test {
protected:
  void SetUp() override {
    config.id = 1;
    config.peers = {1, 2, 3};
    config.election_tick = 10;
    config.heartbeat_tick = 1;
  }

  kv::Config config;
};

TEST_F(ReadIndexTest, FollowerCannotStartReadIndex) {
  kv::Raft raft(config);
  EXPECT_FALSE(raft.read_index().has_value());
}

TEST_F(ReadIndexTest, ReadCarriesANonzeroContextToEveryPeer) {
  kv::Raft raft(config);
  elect_leader(raft);

  const auto read = raft.read_index();
  const auto requests = raft.read_messages();

  ASSERT_TRUE(read.has_value());
  ASSERT_EQ(read->safe_index, 1u);
  ASSERT_EQ(requests.size(), 2u);
  ASSERT_NE(requests.front().read_context, 0u);
  EXPECT_EQ(requests.front().read_context, read->context);
  EXPECT_EQ(requests[0].read_context, requests[1].read_context);
}

TEST_F(ReadIndexTest, ReadWaitsForCurrentTermCommitAndApply) {
  kv::Raft raft(config);
  elect_leader(raft);

  const auto read = raft.read_index();
  const auto requests = raft.read_messages();
  ASSERT_TRUE(read.has_value());
  ASSERT_EQ(read->safe_index, 1u);
  ASSERT_EQ(requests.size(), 2u);
  ASSERT_EQ(requests.front().entries.size(), 1u)
      << "A fresh leader must establish a current-term commit barrier";
  EXPECT_TRUE(requests.front().entries.front().data.empty());

  raft.handle_append_entries_response(successful_response(requests.front()));

  ASSERT_EQ(raft.get_commit_index(), read->safe_index);
  EXPECT_FALSE(raft.read_index_ready(*read))
      << "Committed is not enough; the state machine must apply the barrier";

  raft.advance(read->safe_index);
  EXPECT_TRUE(raft.read_index_ready(*read));
}

TEST_F(ReadIndexTest, DelayedResponseFromPriorRoundCannotConfirmNewRead) {
  kv::Raft raft(config);
  elect_leader(raft);

  const auto first_read = raft.read_index();
  const auto first_requests = raft.read_messages();
  ASSERT_TRUE(first_read.has_value());
  ASSERT_EQ(first_requests.size(), 2u);
  raft.handle_append_entries_response(successful_response(first_requests.front()));
  raft.advance(first_read->safe_index);
  ASSERT_TRUE(raft.read_index_ready(*first_read));
  raft.finish_read_index(*first_read);

  const auto second_read = raft.read_index();
  const auto second_requests = raft.read_messages();
  ASSERT_TRUE(second_read.has_value());
  ASSERT_EQ(second_requests.size(), 2u);
  ASSERT_NE(first_requests.front().read_context,
            second_requests.front().read_context);

  raft.handle_append_entries_response(successful_response(first_requests.front()));
  EXPECT_FALSE(raft.read_index_ready(*second_read));

  raft.handle_append_entries_response(successful_response(second_requests.front()));
  EXPECT_TRUE(raft.read_index_ready(*second_read));
}

TEST_F(ReadIndexTest, CurrentTermReadAckFromOutsideClusterCannotConfirmRead) {
  kv::Raft raft(config);
  elect_leader(raft);

  const auto barrier_read = raft.read_index();
  const auto barrier_requests = raft.read_messages();
  ASSERT_TRUE(barrier_read.has_value());
  ASSERT_EQ(barrier_requests.size(), 2u);
  raft.handle_append_entries_response(
      successful_response(barrier_requests.front()));
  raft.advance(barrier_read->safe_index);
  ASSERT_TRUE(raft.read_index_ready(*barrier_read));
  raft.finish_read_index(*barrier_read);

  const auto read = raft.read_index();
  const auto requests = raft.read_messages();
  ASSERT_TRUE(read.has_value());
  ASSERT_EQ(requests.size(), 2u);

  auto outsider_ack = successful_response(requests.front());
  outsider_ack.from = 99;
  raft.handle_append_entries_response(outsider_ack);

  EXPECT_FALSE(raft.read_index_ready(*read));

  raft.handle_append_entries_response(successful_response(requests.front()));
  EXPECT_TRUE(raft.read_index_ready(*read));
}

TEST_F(ReadIndexTest, SameInFlightReadRoundCanBeCoalesced) {
  kv::Raft raft(config);
  elect_leader(raft);

  const auto first_read = raft.read_index();
  const auto requests = raft.read_messages();
  const auto second_read = raft.read_index();

  ASSERT_TRUE(first_read.has_value());
  ASSERT_TRUE(second_read.has_value());
  EXPECT_EQ(first_read->context, second_read->context);
  EXPECT_EQ(first_read->safe_index, second_read->safe_index);
  EXPECT_TRUE(raft.read_messages().empty())
      << "Concurrent reads may safely share one quorum-confirmed round";

  raft.handle_append_entries_response(successful_response(requests.front()));
  raft.advance(first_read->safe_index);
  EXPECT_TRUE(raft.read_index_ready(*first_read));
}

TEST_F(ReadIndexTest, LeadershipLossInvalidatesThePendingRound) {
  kv::Raft raft(config);
  elect_leader(raft);

  const auto read = raft.read_index();
  ASSERT_TRUE(read.has_value());
  raft.read_messages();

  kv::proto::Message heartbeat;
  heartbeat.type = kv::proto::MsgAppendEntries;
  heartbeat.from = 2;
  heartbeat.to = 1;
  heartbeat.term = raft.get_term();
  heartbeat.prev_log_index = 0;
  heartbeat.prev_log_term = 0;
  raft.handle_append_entries(heartbeat);

  EXPECT_EQ(raft.get_state(), kv::State::Follower);
  EXPECT_FALSE(raft.read_index_ready(*read));
  raft.finish_read_index(*read);
  EXPECT_FALSE(raft.read_index().has_value());
}

TEST_F(ReadIndexTest, FinishingPriorRoundCannotClearNewRoundAtSameSafeIndex) {
  kv::Raft raft(config);
  elect_leader(raft);

  const auto first_read = raft.read_index();
  const auto first_requests = raft.read_messages();
  ASSERT_TRUE(first_read.has_value());
  ASSERT_EQ(first_requests.size(), 2u);
  raft.handle_append_entries_response(successful_response(first_requests.front()));
  raft.advance(first_read->safe_index);
  ASSERT_TRUE(raft.read_index_ready(*first_read));
  raft.finish_read_index(*first_read);

  const auto second_read = raft.read_index();
  const auto second_requests = raft.read_messages();
  ASSERT_TRUE(second_read.has_value());
  ASSERT_EQ(second_requests.size(), 2u);
  ASSERT_EQ(first_read->safe_index, second_read->safe_index);
  ASSERT_NE(first_read->context, second_read->context);

  raft.finish_read_index(*first_read);
  raft.handle_append_entries_response(successful_response(second_requests.front()));

  EXPECT_TRUE(raft.read_index_ready(*second_read));
}

} // namespace
