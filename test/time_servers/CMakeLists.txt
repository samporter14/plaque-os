add_executable(TimeServersTest TimeServersTest.cpp ${REPO_ROOT}/lib/TrustedTime/TimeServers.cpp)

target_link_libraries(TimeServersTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(TimeServersTest)
