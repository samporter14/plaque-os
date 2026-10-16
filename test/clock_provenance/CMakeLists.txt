add_executable(ClockProvenanceTest ClockProvenanceTest.cpp
  ${REPO_ROOT}/src/network/ClockProvenance.cpp
  ${REPO_ROOT}/src/network/CertificateDates.cpp)

target_include_directories(ClockProvenanceTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(ClockProvenanceTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ClockProvenanceTest)
