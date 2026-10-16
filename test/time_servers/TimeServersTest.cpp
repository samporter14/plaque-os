#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "TrustedTime/TimeServers.h"

using trustedtime::acceptableServerName;
using trustedtime::DEFAULT_SERVERS;
using trustedtime::MAX_SERVERS;
using trustedtime::planServers;
using trustedtime::ServerPlan;
using trustedtime::SlotUse;

namespace {
void expectName(const ServerPlan& plan, size_t slot, const char* name) {
  ASSERT_EQ(plan.use[slot], SlotUse::Name) << "slot " << slot;
  ASSERT_NE(plan.name[slot], nullptr);
  EXPECT_STREQ(plan.name[slot], name);
}
}  // namespace

TEST(TimeServers, NothingSetAsksTodaysThreeServersInTodaysOrder) {
  const ServerPlan plan = planServers(nullptr, 0, 0);
  EXPECT_TRUE(plan.defaults);
  expectName(plan, 0, "pool.ntp.org");
  expectName(plan, 1, "time.google.com");
  expectName(plan, 2, "time.cloudflare.com");
  // The very pointers the old configTzTime() call passed are the defaults' texts.
  for (size_t i = 0; i < MAX_SERVERS; ++i) EXPECT_EQ(plan.name[i], DEFAULT_SERVERS[i]);
}

TEST(TimeServers, AnEmptyListMeansTheDefaults) {
  const char* none[] = {"ignored"};
  const ServerPlan plan = planServers(none, 0, 0);
  EXPECT_TRUE(plan.defaults);
  expectName(plan, 0, "pool.ntp.org");
}

TEST(TimeServers, ConfiguredServersReplaceTheDefaultsInOrder) {
  const char* two[] = {"ntp.example.edu", "10.1.2.3"};
  const ServerPlan plan = planServers(two, 2, 0);
  EXPECT_FALSE(plan.defaults);
  expectName(plan, 0, "ntp.example.edu");
  expectName(plan, 1, "10.1.2.3");
  EXPECT_EQ(plan.use[2], SlotUse::Clear);
  EXPECT_EQ(plan.name[2], nullptr);
}

TEST(TimeServers, OneConfiguredServerClearsTheOtherSlots) {
  const char* one[] = {"time.example.edu"};
  const ServerPlan plan = planServers(one, 1, 0);
  expectName(plan, 0, "time.example.edu");
  EXPECT_EQ(plan.use[1], SlotUse::Clear);
  EXPECT_EQ(plan.use[2], SlotUse::Clear);
}

TEST(TimeServers, NeverMoreThanThree) {
  const char* four[] = {"a.example", "b.example", "c.example", "d.example"};
  const ServerPlan plan = planServers(four, 4, 0);
  expectName(plan, 0, "a.example");
  expectName(plan, 1, "b.example");
  expectName(plan, 2, "c.example");
}

TEST(TimeServers, DhcpLeaseServerComesFirstThenTheDefaults) {
  const ServerPlan plan = planServers(nullptr, 0, 1);
  EXPECT_FALSE(plan.defaults);
  EXPECT_EQ(plan.use[0], SlotUse::Lease);
  EXPECT_EQ(plan.name[0], nullptr);
  expectName(plan, 1, "pool.ntp.org");
  expectName(plan, 2, "time.google.com");
}

TEST(TimeServers, DhcpLeaseServerComesFirstThenTheConfiguredServers) {
  const char* three[] = {"a.example", "b.example", "c.example"};
  const ServerPlan plan = planServers(three, 3, 1);
  EXPECT_EQ(plan.use[0], SlotUse::Lease);
  expectName(plan, 1, "a.example");
  expectName(plan, 2, "b.example");
}

TEST(TimeServers, NoLeaseServerLeavesTheConfiguredPlanAlone) {
  const char* one[] = {"a.example"};
  const ServerPlan plan = planServers(one, 1, 0);
  expectName(plan, 0, "a.example");
  EXPECT_EQ(plan.use[1], SlotUse::Clear);
}

TEST(TimeServers, LeaseSlotsAreCappedAtThree) {
  const ServerPlan plan = planServers(nullptr, 0, 7);
  for (size_t i = 0; i < MAX_SERVERS; ++i) EXPECT_EQ(plan.use[i], SlotUse::Lease);
}

TEST(TimeServers, AcceptableNames) {
  EXPECT_TRUE(acceptableServerName("pool.ntp.org"));
  EXPECT_TRUE(acceptableServerName("10.0.0.1"));
  EXPECT_TRUE(acceptableServerName(std::string(253, 'a').c_str()));
  EXPECT_FALSE(acceptableServerName(std::string(254, 'a').c_str()));
  EXPECT_FALSE(acceptableServerName(nullptr));
  EXPECT_FALSE(acceptableServerName(""));
  EXPECT_FALSE(acceptableServerName("time example"));
  EXPECT_FALSE(acceptableServerName("time\texample"));
  EXPECT_FALSE(acceptableServerName("time\x7f"));
  EXPECT_FALSE(acceptableServerName("t\xc3\xa9l\xc3\xa9"));
}
