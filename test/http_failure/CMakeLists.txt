add_executable(HttpFailureTest HttpFailureTest.cpp ${REPO_ROOT}/src/network/HttpFailure.cpp)

target_include_directories(HttpFailureTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(HttpFailureTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(HttpFailureTest)
