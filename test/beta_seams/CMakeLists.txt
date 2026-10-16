# Where the two branches meet (Register, and the X4 Classic / university Wi-Fi work): the Register wake plan composed with
# the Wi-Fi branch's join budgets, the poll skipped near a step timer's end, what a Pro with no profile does, and which
# rows Messages shows. Pure logic only; the device-side halves are test/network_profile/glue and test/test_register_seams.py.
# Like network_profile, it shares the top-level project (gtest) and needs the ArduinoJson sources the x4pro build fetches.
add_executable(BetaSeamsTest
  BetaSeamsTest.cpp
  ${REPO_ROOT}/lib/PlaqueBoard/PlaqueBoard.cpp
  ${REPO_ROOT}/src/network/NetworkJoin.cpp
  ${REPO_ROOT}/src/ambient/RegisterWake.cpp
  ${REPO_ROOT}/src/ambient/RegisterModel.cpp
  ${REPO_ROOT}/src/ambient/SleepFrame.cpp
  ${REPO_ROOT}/src/ambient/StepTimer.cpp
  ${REPO_ROOT}/src/ambient/MethodProtocol.cpp
  ${REPO_ROOT}/src/ambient/MethodTimers.cpp
  ${REPO_ROOT}/src/ambient/MessageProtocol.cpp
)
target_include_directories(BetaSeamsTest PRIVATE
  ${REPO_ROOT}/src
  ${REPO_ROOT}/src/ambient
  ${REPO_ROOT}/lib/PlaqueBoard
  ${REPO_ROOT}/.pio/libdeps/x4pro/ArduinoJson/src
)
target_link_libraries(BetaSeamsTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(BetaSeamsTest DISCOVERY_TIMEOUT 60)
