#include <gtest/gtest.h>

#include <vector>

#include "NetworkJoin.h"
#include "StepTimer.h"

using netjoin::Mode;
using netjoin::Situation;

namespace {

Situation situation(const bool profile, const size_t saved, const int last, const bool hidden = false) {
  Situation s;
  s.profile = profile;
  s.savedCount = saved;
  s.lastUsed = last;
  s.profileHidden = hidden;
  return s;
}

using Order = std::vector<size_t>;

}  // namespace

TEST(NetworkJoin, TheProfileIsNumberedAfterTheSavedNetworks) {
  const Situation s = situation(true, 3, 1);
  EXPECT_EQ(3u, netjoin::profileIndex(s));
  EXPECT_TRUE(netjoin::isProfile(s, 3));
  EXPECT_FALSE(netjoin::isProfile(s, 2));
  EXPECT_EQ(4u, netjoin::candidateCount(s));
  const Situation none = situation(false, 3, 1);
  EXPECT_FALSE(netjoin::isProfile(none, 3));
  EXPECT_EQ(3u, netjoin::candidateCount(none));
}

TEST(NetworkJoin, WithoutAProfileBehaviourIsAsItAlwaysWas) {
  EXPECT_EQ(2, netjoin::beforeScan(Mode::Manual, situation(false, 4, 2)));
  EXPECT_EQ(-1, netjoin::beforeScan(Mode::Manual, situation(false, 4, -1)));
  EXPECT_EQ(2, netjoin::beforeScan(Mode::Wake, situation(false, 4, 2)));
  EXPECT_EQ(Order({1, 3}), netjoin::afterScan(Mode::Manual, situation(false, 4, 2), false, {1, 3}));
  EXPECT_EQ(Order({2}), netjoin::wakeOrder(situation(false, 4, 2)));
  EXPECT_EQ(Order(), netjoin::wakeOrder(situation(false, 4, -1)));
}

TEST(NetworkJoin, AManualCheckWithAProfileScansFirst) {
  EXPECT_EQ(-1, netjoin::beforeScan(Mode::Manual, situation(true, 4, 2)));
  EXPECT_TRUE(netjoin::scans(Mode::Manual));
}

TEST(NetworkJoin, AfterTheScanTheProfileComesFirstThenTheLastUsedThenTheRest) {
  const Situation s = situation(true, 4, 2);
  EXPECT_EQ(Order({4, 2, 1, 3}), netjoin::afterScan(Mode::Manual, s, true, {1, 2, 3}));
  // The profile's network was not in range: only the saved networks seen, last used first.
  EXPECT_EQ(Order({2, 1, 3}), netjoin::afterScan(Mode::Manual, s, false, {1, 2, 3}));
}

TEST(NetworkJoin, AHiddenProfileIsTriedEvenThoughAScanCannotSeeIt) {
  const Situation s = situation(true, 2, 0, true);
  const Order order = netjoin::afterScan(Mode::Manual, s, false, {0, 1});
  ASSERT_FALSE(order.empty());
  EXPECT_EQ(2u, order.front());
}

TEST(NetworkJoin, AProfileThatFailsFallsBackToTheSavedNetworks) {
  const Situation s = situation(true, 2, 1);
  const Order order = netjoin::afterScan(Mode::Manual, s, true, {0, 1});
  EXPECT_EQ(Order({2, 1, 0}), order);
}

TEST(NetworkJoin, AWakeNeverScans) {
  EXPECT_FALSE(netjoin::scans(Mode::Wake));
  EXPECT_EQ(2, netjoin::beforeScan(Mode::Wake, situation(true, 2, 1)));  // the profile first
  EXPECT_EQ(Order({2, 1}), netjoin::wakeOrder(situation(true, 2, 1)));   // then the last used only
  EXPECT_EQ(Order({2}), netjoin::wakeOrder(situation(true, 2, -1)));
  EXPECT_EQ(Order({3, 0}), netjoin::wakeOrder(situation(true, 3, 0)));  // other saved networks are not tried
}

// ---- S4: a timer wake's join
// --------------------------------------------------------------------------------------------

using netjoin::Joined;
using netjoin::WakeStep;

namespace {
Situation wake(const bool profile, const bool eapTls, const size_t saved, const int last) {
  Situation s = situation(profile, saved, last);
  s.profileEapTls = eapTls;
  return s;
}
}  // namespace

TEST(NetworkJoin, AWakeWithoutAProfileIsTheLastUsedNetworkAloneForTenSeconds) {
  // Exactly what a wake always did: the last-used network, 10 s for it, whatever the memory of an earlier join says.
  for (const Joined remembered : {Joined::Nobody, Joined::Profile, Joined::Saved}) {
    const auto plan = netjoin::wakePlan(wake(false, false, 4, 2), remembered);
    ASSERT_EQ(1u, plan.size());
    EXPECT_EQ(2u, plan[0].candidate);
    EXPECT_EQ(10000u, plan[0].budgetMs);
  }
  EXPECT_TRUE(netjoin::wakePlan(wake(false, false, 4, -1), Joined::Saved).empty());  // as today: nothing to try
  EXPECT_EQ(10000u, netjoin::wakeJoinWorstMs(false, false));
}

TEST(NetworkJoin, AWakeTriesTheProfileFirstUnlessASavedNetworkJoinedLast) {
  const Situation s = wake(true, false, 3, 1);
  for (const Joined remembered : {Joined::Nobody, Joined::Profile}) {
    const auto plan = netjoin::wakePlan(s, remembered);
    ASSERT_EQ(2u, plan.size());
    EXPECT_EQ(3u, plan[0].candidate);  // the profile is numbered after the saved networks
    EXPECT_EQ(1u, plan[1].candidate);
  }
  const auto saved = netjoin::wakePlan(s, Joined::Saved);
  ASSERT_EQ(2u, saved.size());
  EXPECT_EQ(1u, saved[0].candidate);
  EXPECT_EQ(3u, saved[1].candidate);
}

TEST(NetworkJoin, EachCandidateOfAWakeHasItsOwnBudgetAndACertificateProfileHasARealisticOne) {
  const auto open = netjoin::wakePlan(wake(true, false, 3, 1), Joined::Nobody);
  EXPECT_EQ(10000u, open[0].budgetMs);
  EXPECT_EQ(10000u, open[1].budgetMs);
  const auto eap = netjoin::wakePlan(wake(true, true, 3, 1), Joined::Nobody);
  EXPECT_EQ(25000u, eap[0].budgetMs);  // scan, the EAP-TLS handshake, the 4-way handshake and the lease
  EXPECT_EQ(10000u, eap[1].budgetMs);  // the saved network keeps what it always had
  // The worst case is the sum, whichever comes first.
  EXPECT_EQ(20000u, netjoin::wakeJoinWorstMs(true, false));
  EXPECT_EQ(35000u, netjoin::wakeJoinWorstMs(true, true));
  const auto savedFirst = netjoin::wakePlan(wake(true, true, 3, 1), Joined::Saved);
  EXPECT_EQ(10000u, savedFirst[0].budgetMs);
  EXPECT_EQ(25000u, savedFirst[1].budgetMs);
}

TEST(NetworkJoin, ALastJoinThatCannotBeRepeatedIsIgnored) {
  // A saved network joined last, but none is the last-used now: the profile alone.
  const auto profileOnly = netjoin::wakePlan(wake(true, false, 3, -1), Joined::Saved);
  ASSERT_EQ(1u, profileOnly.size());
  EXPECT_EQ(3u, profileOnly[0].candidate);
  // The profile joined last, but it has been removed: the last-used network alone.
  const auto savedOnly = netjoin::wakePlan(wake(false, false, 3, 1), Joined::Profile);
  ASSERT_EQ(1u, savedOnly.size());
  EXPECT_EQ(1u, savedOnly[0].candidate);
}

TEST(NetworkJoin, WhoJoinedLastSurvivesTheRtcMemoryOnlyIfItIsIntact) {
  for (const Joined value : {Joined::Nobody, Joined::Profile, Joined::Saved})
    EXPECT_EQ(value, netjoin::decodeJoined(netjoin::encodeJoined(value)));
  EXPECT_EQ(Joined::Nobody, netjoin::decodeJoined(0));  // RTC memory that was never written
  for (int byte = 0; byte < 256; ++byte) {
    const Joined decoded = netjoin::decodeJoined(static_cast<uint8_t>(byte));
    if (decoded != Joined::Nobody) EXPECT_EQ(static_cast<uint8_t>(byte), netjoin::encodeJoined(decoded));
  }
}

TEST(NetworkJoin, ThePollBudgetFollowsTheWorstJoin) {
  // steptimer::POLL_BUDGET_MS is the wake of a reader with no profile, to the millisecond; a profile adds its join.
  EXPECT_EQ(steptimer::POLL_BUDGET_MS, steptimer::pollBudgetMs(netjoin::wakeJoinWorstMs(false, false)));
  EXPECT_EQ(steptimer::POLL_BUDGET_MS + 10000, steptimer::pollBudgetMs(netjoin::wakeJoinWorstMs(true, false)));
  EXPECT_EQ(steptimer::POLL_BUDGET_MS + 25000, steptimer::pollBudgetMs(netjoin::wakeJoinWorstMs(true, true)));
}
