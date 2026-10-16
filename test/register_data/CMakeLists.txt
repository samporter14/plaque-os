cmake_minimum_required(VERSION 3.16)
project(register_data_host_tests CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
enable_testing()
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(ARDUINOJSON_DIR "${REPO_ROOT}/.pio/libdeps/x4pro/ArduinoJson/src" CACHE PATH "ArduinoJson 7 include directory")
if(NOT EXISTS "${ARDUINOJSON_DIR}/ArduinoJson.h")
  message(FATAL_ERROR "Build the x4pro environment first, or pass -DARDUINOJSON_DIR=/path/to/ArduinoJson/src")
endif()
# RegisterData.cpp holds titles to the same text rule as a timer label (MethodTimers.cpp).
add_executable(RegisterDataTest RegisterDataTest.cpp
  "${REPO_ROOT}/src/ambient/RegisterData.cpp"
  "${REPO_ROOT}/src/ambient/MethodTimers.cpp")
target_include_directories(RegisterDataTest PRIVATE "${REPO_ROOT}/src/ambient" "${ARDUINOJSON_DIR}")
target_compile_options(RegisterDataTest PRIVATE -Wall -Wextra -pedantic)
add_test(NAME RegisterData COMMAND RegisterDataTest)
