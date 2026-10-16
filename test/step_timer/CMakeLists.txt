cmake_minimum_required(VERSION 3.16)
project(step_timer_host_tests CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
enable_testing()
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(ARDUINOJSON_DIR "${REPO_ROOT}/.pio/libdeps/x4pro/ArduinoJson/src" CACHE PATH "ArduinoJson 7 include directory")
if(NOT EXISTS "${ARDUINOJSON_DIR}/ArduinoJson.h")
  message(FATAL_ERROR "Build the x4pro environment first, or pass -DARDUINOJSON_DIR=/path/to/ArduinoJson/src")
endif()
# StepTimer.cpp asks MethodProtocol.cpp whether a path is a Method book (ownedBookPath), which pulls in
# MethodTimers.cpp and MessageProtocol.cpp, as in test/sleep_frame.
add_executable(StepTimerTest StepTimerTest.cpp
  "${REPO_ROOT}/src/ambient/StepTimer.cpp"
  "${REPO_ROOT}/src/ambient/StepTimerStore.cpp"
  "${REPO_ROOT}/src/ambient/MethodProtocol.cpp"
  "${REPO_ROOT}/src/ambient/MethodTimers.cpp"
  "${REPO_ROOT}/src/ambient/MessageProtocol.cpp")
target_include_directories(StepTimerTest PRIVATE "${REPO_ROOT}/src/ambient" "${ARDUINOJSON_DIR}")
target_compile_options(StepTimerTest PRIVATE -Wall -Wextra -pedantic)
add_test(NAME StepTimer COMMAND StepTimerTest)
