cmake_minimum_required(VERSION 3.16)
project(method_wake_host_tests CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
enable_testing()
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(ARDUINOJSON_DIR "${REPO_ROOT}/.pio/libdeps/x4pro/ArduinoJson/src" CACHE PATH "ArduinoJson 7 include directory")
if(NOT EXISTS "${ARDUINOJSON_DIR}/ArduinoJson.h")
  message(FATAL_ERROR "Build the x4pro environment first, or pass -DARDUINOJSON_DIR=/path/to/ArduinoJson/src")
endif()
# SleepFrame.cpp, StepTimer.cpp (planBoot), MethodProtocol.cpp and MessageProtocol.cpp: the interrupted-wake test walks
# the same decisions main.cpp makes on a button wake.
add_executable(MethodWakeTest MethodWakeTest.cpp
  "${REPO_ROOT}/src/ambient/MethodWake.cpp"
  "${REPO_ROOT}/src/ambient/SleepFrame.cpp"
  "${REPO_ROOT}/src/ambient/StepTimer.cpp"
  "${REPO_ROOT}/src/ambient/MethodProtocol.cpp"
  "${REPO_ROOT}/src/ambient/MethodTimers.cpp"
  "${REPO_ROOT}/src/ambient/MessageProtocol.cpp")
target_include_directories(MethodWakeTest PRIVATE "${REPO_ROOT}/src/ambient" "${ARDUINOJSON_DIR}")
target_compile_options(MethodWakeTest PRIVATE -Wall -Wextra -pedantic)
add_test(NAME MethodWake COMMAND MethodWakeTest)
