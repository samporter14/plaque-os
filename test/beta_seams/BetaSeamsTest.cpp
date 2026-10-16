// Where the Register branch and the X4 Classic / university Wi-Fi branch meet, as pure logic:
//
//  - the Register wake plan (RegisterWake.cpp) composed with the Wi-Fi branch's wake join and its budgets
//  (NetworkJoin.h,
//    steptimer::pollBudgetMs) and with the running step timer, the way main.cpp's timer-wake path composes them;
//  - a Pro with no network profile behaves as the Register branch alone did: the same poll budget, the same join, the
//  same
//    sleep-screen decision for every input, the same Messages rows;
//  - the rows Settings > System > Messages shows, board by board.
//
// The device-side halves (the real join code against a fake radio, and the order of the calls in the sources) are
// test/network_profile/glue (GlueTest.cpp, "Register") and test/test_register_seams.py.
#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

#include "MessageRows.h"
#include "MethodWake.h"
#include "PlaqueBoard.h"
#include "RegisterWake.h"
#include "SleepFrame.h"
#include "StepTimer.h"
#include "network/NetworkJoin.h"

namespace {

constexpr uint64_t SEC = 1000000;

struct JoinCase {
  const char* name;
  bool profile;
  bool eapTls;
};
const JoinCase JOINS[] = {
    {"no profile", false, false}, {"open or PSK profile", true, false}, {"EAP-TLS profile", true, true}};

int64_t budgetFor(const JoinCase& join) {
  return steptimer::pollBudgetMs(netjoin::wakeJoinWorstMs(join.profile, join.eapTls));
}

// The decision main.cpp makes first on every ambient timer wake (a Register poll and a Register boundary wake included:
// both arrive as the same timer wake): is the poll's worst case over before a running step timer ends?
bool wakeRunsOnTime(const JoinCase& join, const bool timerRunning, const int64_t remainingMs) {
  return steptimer::pollFitsBeforeEnd(timerRunning, remainingMs, budgetFor(join));
}

// The Stage C hook, standing in for STEP_TIMER: the real arithmetic over a timer the test sets.
struct FakeTimer {
  bool running = false;
  int64_t remainingMs = 0;
};
FakeTimer fakeTimer;
uint64_t timerHook(const uint64_t pollMicros) {
  return steptimer::nextWakeMicros(pollMicros, fakeTimer.running, fakeTimer.remainingMs);
}

reg::WakeInputs registerWakeInputs() {
  reg::WakeInputs in;
  in.now = 1792000000;  // a time that is a clock
  in.pollEnabled = true;
  in.pollIntervalS = 15 * 60;
  in.pollDueAt = in.now - 5;  // due
  in.boundaryWakes = true;
  in.boundaryAt = 0;
  in.batteryPercent = 80;
  return in;
}

}  // namespace

// ---- a Pro with no profile is the Register branch alone ------------------------------------------------------------

TEST(BetaSeams, ARegisterPollWithNoProfileHasTheOldBudgetAndTheOldJoin) {
  // The Register branch alone: pollFitsBeforeEnd(running, remaining) with its default budget of 150 s, and a wake join
  // that is the last-used network alone with 10 s.
  EXPECT_EQ(150000, steptimer::POLL_BUDGET_MS);
  EXPECT_EQ(steptimer::POLL_BUDGET_MS, budgetFor(JOINS[0]));
  EXPECT_EQ(10000u, netjoin::wakeJoinWorstMs(false, false));
  for (const auto last : {netjoin::Joined::Nobody, netjoin::Joined::Profile, netjoin::Joined::Saved}) {
    netjoin::Situation s;
    s.savedCount = 1;
    s.lastUsed = 0;
    const auto plan = netjoin::wakePlan(s, last);
    ASSERT_EQ(1u, plan.size()) << "who joined last does not matter when there is no profile";
    EXPECT_EQ(0u, plan[0].candidate);
    EXPECT_EQ(10000u, plan[0].budgetMs);
  }
  // A saved network that was never joined: no candidate at all, as before.
  netjoin::Situation none;
  none.savedCount = 2;
  EXPECT_TRUE(netjoin::wakePlan(none, netjoin::Joined::Nobody).empty());
}

TEST(BetaSeams, TheSkipNearAStepTimersEndIsTheRegisterBranchsForEveryTimeLeftWithNoProfile) {
  for (int64_t remaining = -5000; remaining <= 400000; remaining += 250) {
    EXPECT_EQ(steptimer::pollFitsBeforeEnd(true, remaining), wakeRunsOnTime(JOINS[0], true, remaining))
        << "remaining " << remaining;
    EXPECT_TRUE(wakeRunsOnTime(JOINS[0], false, remaining)) << "no timer: the wake always runs";
  }
  EXPECT_FALSE(wakeRunsOnTime(JOINS[0], true, 150000)) << "the registered edge of the Register branch: 150 s waits";
  EXPECT_TRUE(wakeRunsOnTime(JOINS[0], true, 150001));
}

TEST(BetaSeams, TheProDecidesItsSleepScreenAsTheRegisterBranchAloneForEveryInput) {
  const plaque::Capabilities pro = plaque::capabilitiesFor(plaque::Model::X4Pro);
  const plaque::Capabilities classic = plaque::capabilitiesFor(plaque::Model::X4Classic);
  const plaque::Capabilities other = plaque::capabilitiesFor(plaque::Model::Unsupported);
  for (int bits = 0; bits < 32; ++bits) {
    const bool methodStep = bits & 1;
    const bool useRegister = bits & 2;
    const bool prepared = bits & 4;  // a schedule is on the card and the battery allows the draw
    const bool useNote = bits & 8;
    const bool hasContent = bits & 16;
    // Register branch alone (main.cpp enterDeepSleep), with `x4Pro` the board's identity.
    const auto alone = [&](const bool x4Pro) {
      const bool registerSleep = !methodStep && x4Pro && useRegister && prepared;
      return !methodStep && x4Pro && (registerSleep || (useNote && hasContent));
    };
    // The merged code: the sleep screen is a capability, and Register's selection is too.
    const auto merged = [&](const plaque::Capabilities& caps) {
      const bool registerSelected = plaque::offersRegister(caps) && useRegister;
      const bool registerSleep = !methodStep && registerSelected && prepared;
      return !methodStep && plaque::ambientSleepScreen(caps) && (registerSleep || (useNote && hasContent));
    };
    EXPECT_EQ(alone(true), merged(pro)) << "bits " << bits;
    EXPECT_EQ(merged(pro), merged(classic)) << "the Classic decides as the Pro does, bits " << bits;
    EXPECT_EQ(alone(false), merged(other)) << "another board: never, bits " << bits;
    EXPECT_FALSE(merged(other));
    // ... and what the sleep does with the answer is the same pure function either way.
    const auto framePro = sleepframe::planSleep({pro.method, true, "", false, merged(pro)});
    const auto frameAlone = sleepframe::planSleep({true, true, "", false, alone(true)});
    EXPECT_EQ(static_cast<int>(frameAlone.frame), static_cast<int>(framePro.frame)) << "bits " << bits;
  }
}

TEST(BetaSeams, TheProArmsTheWakeAndPollsAsItDidAndOtherBoardsNever) {
  const plaque::Capabilities pro = plaque::capabilitiesFor(plaque::Model::X4Pro);
  const plaque::Capabilities classic = plaque::capabilitiesFor(plaque::Model::X4Classic);
  const plaque::Capabilities other = plaque::capabilitiesFor(plaque::Model::Unsupported);
  for (const uint64_t micros : {uint64_t{0}, 60 * SEC, 900 * SEC}) {
    EXPECT_EQ(micros, plaque::armedWakeMicros(pro, micros));
    EXPECT_EQ(micros, plaque::armedWakeMicros(classic, micros));
    EXPECT_EQ(0u, plaque::armedWakeMicros(other, micros));
  }
  // AmbientSettings::pollingEnabled() asks scheduledDownloads where the Register branch asked for the Pro.
  EXPECT_TRUE(pro.scheduledDownloads && classic.scheduledDownloads && !other.scheduledDownloads);
  EXPECT_TRUE(plaque::offersRegister(pro) && plaque::offersRegister(classic) && !plaque::offersRegister(other));
}

// ---- Messages rows --------------------------------------------------------------------------------------------------

TEST(BetaSeams, MessagesShowsItsNineOriginalRowsWhereThereIsNoRegisterAndTwelveWhereThereIs) {
  using namespace messagerows;
  const std::vector<Item> original = {VIEW, CHECK, URL, DEVICE_ID, TOKEN, INTERVAL, SLEEP_SCREEN, POLLING, LOCAL_HTTP};
  const std::vector<Item> withRegister = {VIEW,  CHECK,    REGISTER_VIEW, REGISTER_CHECK, URL,     DEVICE_ID,
                                          TOKEN, INTERVAL, SLEEP_SCREEN,  REGISTER_SLEEP, POLLING, LOCAL_HTTP};
  const Rows without = rowsFor(false);
  const Rows with = rowsFor(true);
  EXPECT_EQ(original, std::vector<Item>(without.items, without.items + without.count));
  EXPECT_EQ(withRegister, std::vector<Item>(with.items, with.items + with.count));
  EXPECT_EQ(ITEM_COUNT, with.count);
  // Each row is a row of Register's, or it is one of the nine.
  for (int i = 0; i < ITEM_COUNT; ++i) {
    const Item item = static_cast<Item>(i);
    const bool inOriginal = std::find(original.begin(), original.end(), item) != original.end();
    EXPECT_EQ(!inOriginal, isRegisterRow(item));
  }
}

TEST(BetaSeams, EachBoardGetsTheRowsItsCapabilitiesSay) {
  for (const auto model : {plaque::Model::X4Pro, plaque::Model::X4Classic}) {
    EXPECT_EQ(messagerows::ITEM_COUNT,
              messagerows::rowsFor(plaque::offersRegister(plaque::capabilitiesFor(model))).count);
  }
  EXPECT_EQ(9, messagerows::rowsFor(plaque::offersRegister(plaque::capabilitiesFor(plaque::Model::Unsupported))).count);
  // Take each thing Register needs away and the three rows go with it.
  plaque::Capabilities c = plaque::capabilitiesFor(plaque::Model::X4Classic);
  c.mailbox = false;
  EXPECT_EQ(9, messagerows::rowsFor(plaque::offersRegister(c)).count);
  c = plaque::capabilitiesFor(plaque::Model::X4Classic);
  c.timedWake = false;
  EXPECT_EQ(9, messagerows::rowsFor(plaque::offersRegister(c)).count);
  c = plaque::capabilitiesFor(plaque::Model::X4Classic);
  c.registerScreen = false;
  EXPECT_EQ(9, messagerows::rowsFor(plaque::offersRegister(c)).count);
}

// ---- the Register wake plan with the Wi-Fi branch's join budgets ----------------------------------------------------

TEST(BetaSeams, ARegisterPollIsInsideThePollBudgetOfEveryJoin) {
  // pollBudgetMs = the wake join + the note check (two requests, each a 5 s socket timeout and a 20 s transfer budget)
  // + the Method book check (method::wake::BUDGET_MS). A Register poll makes one of those requests, so it is inside
  // the same budget with room to spare, whichever join the reader has.
  constexpr int64_t REQUEST_MS = 5000 + 20000;
  for (const JoinCase& join : JOINS) {
    const int64_t joinMs = netjoin::wakeJoinWorstMs(join.profile, join.eapTls);
    const int64_t registerPoll = joinMs + 1 * REQUEST_MS + method::wake::BUDGET_MS;
    const int64_t notePoll = joinMs + 2 * REQUEST_MS + method::wake::BUDGET_MS;
    EXPECT_EQ(notePoll, budgetFor(join)) << join.name << ": the budget is the note poll's worst case";
    EXPECT_LT(registerPoll, budgetFor(join)) << join.name;
  }
  EXPECT_EQ(150000, budgetFor(JOINS[0]));
  EXPECT_EQ(160000, budgetFor(JOINS[1]));
  EXPECT_EQ(175000, budgetFor(JOINS[2]));
}

TEST(BetaSeams, AProfileLengthensTheSkipWindowByItsJoinAndNothingElse) {
  // A poll wake (or a boundary wake) with a step timer `remaining` from its end: skipped when the poll's worst case
  // would run past it. With a profile the window is as much longer as the join may be.
  struct Case {
    int64_t remainingMs;
    bool runsNoProfile, runsOpen, runsEap;
  };
  const Case cases[] = {
      {100000, false, false, false}, {149999, false, false, false}, {150001, true, false, false},
      {155000, true, false, false},  {160001, true, true, false},   {170000, true, true, false},
      {175001, true, true, true},    {600000, true, true, true},
  };
  for (const Case& c : cases) {
    EXPECT_EQ(c.runsNoProfile, wakeRunsOnTime(JOINS[0], true, c.remainingMs)) << c.remainingMs;
    EXPECT_EQ(c.runsOpen, wakeRunsOnTime(JOINS[1], true, c.remainingMs)) << c.remainingMs;
    EXPECT_EQ(c.runsEap, wakeRunsOnTime(JOINS[2], true, c.remainingMs)) << c.remainingMs;
  }
  // A longer join never lets in a wake the shorter one skipped, and never keeps out one it let in beyond its own share.
  for (int64_t remaining = 0; remaining <= 300000; remaining += 500) {
    if (wakeRunsOnTime(JOINS[2], true, remaining)) EXPECT_TRUE(wakeRunsOnTime(JOINS[1], true, remaining));
    if (wakeRunsOnTime(JOINS[1], true, remaining)) EXPECT_TRUE(wakeRunsOnTime(JOINS[0], true, remaining));
  }
}

TEST(BetaSeams, ASkippedRegisterWakeSleepsOnForTheTimersEndSoItsAlertIsNotLate) {
  sleepframe::setWakeHook(timerHook);
  // A boundary wake (or a poll) with the step timer ending in 170 s, on a reader that has an EAP-TLS profile: the poll
  // would run for up to 175 s, so it is skipped and the reader sleeps for the timer's end, a little early.
  fakeTimer = {true, 170000};
  EXPECT_FALSE(wakeRunsOnTime(JOINS[2], true, fakeTimer.remainingMs));
  const uint64_t rearm = sleepframe::rearmMicros(0, 80);
  EXPECT_EQ(static_cast<uint64_t>(170000 - steptimer::WAKE_LEAD_MS) * 1000, rearm);
  // ... and with no profile the same wake runs (the Register branch alone's behaviour at 170 s).
  EXPECT_TRUE(wakeRunsOnTime(JOINS[0], true, fakeTimer.remainingMs));
  // Whatever poll wake the Register screen plans, the timer's end comes first when it is sooner.
  fakeTimer = {true, 100000};
  reg::WakeInputs in = registerWakeInputs();
  in.pollDueAt = in.now + 600;
  const reg::WakePlan plan = reg::planWake(in);
  EXPECT_EQ(reg::WakeFor::StepTimer, plan.reason) << "the timer's end is sooner than the poll";
  EXPECT_EQ(static_cast<uint64_t>(100000 - steptimer::WAKE_LEAD_MS) * 1000, plan.micros);
  fakeTimer = {};
  sleepframe::setWakeHook(nullptr);
}

TEST(BetaSeams, ARegisterPollsWorstCaseEndsBeforeTheSmallestPollIntervalIsOut) {
  // The poll's worst case (175 s with an EAP-TLS profile) is shorter than the smallest interval the settings allow
  // (5 minutes), so a poll never runs into the next one.
  for (const JoinCase& join : JOINS) EXPECT_LT(budgetFor(join), 5 * 60 * 1000) << join.name;
  // And the next poll is planned an interval after this one finished, not after it began.
  EXPECT_EQ(1792000000 + 900, reg::pollDueAfter(1792000000, 900));
}

TEST(BetaSeams, AWakeThatIsNotThePollsDoesNotNeedTheJoinAtAll) {
  // A boundary wake is told from a poll wake by the clock, and joins nothing: the budget only matters to the skip.
  reg::WakeInputs in = registerWakeInputs();
  in.pollDueAt = in.now + 600;
  EXPECT_FALSE(reg::pollDue(in));
  in.pollDueAt = in.now - 5;
  EXPECT_TRUE(reg::pollDue(in));
  in.pollEnabled = false;
  EXPECT_FALSE(reg::pollDue(in));
}
