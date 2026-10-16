# The check validates the mailbox reply with the Method list parser, which reads JSON with ArduinoJson (fetched by
# the x4pro build), as test/method_check does.
set(CONNECTION_CHECK_ARDUINOJSON_DIR "${REPO_ROOT}/.pio/libdeps/x4pro/ArduinoJson/src")

add_executable(ConnectionCheckTest ConnectionCheckTest.cpp
  ${REPO_ROOT}/src/network/ConnectionCheck.cpp
  ${REPO_ROOT}/src/network/ClockProvenance.cpp
  ${REPO_ROOT}/src/network/CertificateDates.cpp
  ${REPO_ROOT}/src/network/HttpFailure.cpp
  ${REPO_ROOT}/src/ambient/MethodProtocol.cpp
  ${REPO_ROOT}/src/ambient/MethodTimers.cpp
  ${REPO_ROOT}/src/ambient/MessageProtocol.cpp)

target_include_directories(ConnectionCheckTest PRIVATE
  ${REPO_ROOT}/src
  ${REPO_ROOT}/src/ambient
  ${REPO_ROOT}/lib/Memory
  ${CONNECTION_CHECK_ARDUINOJSON_DIR}
)

target_link_libraries(ConnectionCheckTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ConnectionCheckTest)
