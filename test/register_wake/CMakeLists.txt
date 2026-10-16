cmake_minimum_required(VERSION 3.16)
project(register_wake_host_tests CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
enable_testing()
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(ARDUINOJSON_DIR "${REPO_ROOT}/.pio/libdeps/x4pro/ArduinoJson/src" CACHE PATH "ArduinoJson 7 include directory")
if(NOT EXISTS "${ARDUINOJSON_DIR}/ArduinoJson.h")
  message(FATAL_ERROR "Build the x4pro environment first, or pass -DARDUINOJSON_DIR=/path/to/ArduinoJson/src")
endif()
# The wake is the sooner of the poll and the boundary, handed to sleepframe::rearmMicros() and the step timer's real
# wake arithmetic (StepTimer.cpp). SleepFrame.cpp asks MethodProtocol.cpp whether a path is a Method book, which pulls
# in MethodTimers.cpp and MessageProtocol.cpp, as in test/sleep_frame.
add_executable(RegisterWakeTest RegisterWakeTest.cpp
  "${REPO_ROOT}/src/ambient/RegisterWake.cpp"
  "${REPO_ROOT}/src/ambient/RegisterModel.cpp"
  "${REPO_ROOT}/src/ambient/SleepFrame.cpp"
  "${REPO_ROOT}/src/ambient/StepTimer.cpp"
  "${REPO_ROOT}/src/ambient/MethodProtocol.cpp"
  "${REPO_ROOT}/src/ambient/MethodTimers.cpp"
  "${REPO_ROOT}/src/ambient/MessageProtocol.cpp")
target_include_directories(RegisterWakeTest PRIVATE "${REPO_ROOT}/src/ambient" "${ARDUINOJSON_DIR}")
target_compile_options(RegisterWakeTest PRIVATE -Wall -Wextra -pedantic)
add_test(NAME RegisterWake COMMAND RegisterWakeTest)
