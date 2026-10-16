cmake_minimum_required(VERSION 3.16)
project(method_check_host_tests CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
enable_testing()
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(ARDUINOJSON_DIR "${REPO_ROOT}/.pio/libdeps/x4pro/ArduinoJson/src" CACHE PATH "ArduinoJson 7 include directory")
if(NOT EXISTS "${ARDUINOJSON_DIR}/ArduinoJson.h")
  message(FATAL_ERROR "Build the x4pro environment first, or pass -DARDUINOJSON_DIR=/path/to/ArduinoJson/src")
endif()
add_executable(MethodCheckTest MethodCheckTest.cpp
  "${REPO_ROOT}/src/ambient/MethodCheck.cpp"
  "${REPO_ROOT}/src/ambient/MethodLibrary.cpp"
  "${REPO_ROOT}/src/ambient/MethodProtocol.cpp"
  "${REPO_ROOT}/src/ambient/MethodWake.cpp"
  "${REPO_ROOT}/src/ambient/MethodTimers.cpp"
  "${REPO_ROOT}/src/ambient/MessageProtocol.cpp")
target_include_directories(MethodCheckTest PRIVATE "${REPO_ROOT}/src/ambient" "${REPO_ROOT}/test/method_support"
  "${ARDUINOJSON_DIR}")
target_compile_options(MethodCheckTest PRIVATE -Wall -Wextra -pedantic)
add_test(NAME MethodCheck COMMAND MethodCheckTest)
