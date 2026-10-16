cmake_minimum_required(VERSION 3.16)
project(reader_identity_host_tests CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
enable_testing()
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(ARDUINOJSON_DIR "${REPO_ROOT}/.pio/libdeps/x4pro/ArduinoJson/src" CACHE PATH "ArduinoJson 7 include directory")
if(NOT EXISTS "${ARDUINOJSON_DIR}/ArduinoJson.h")
  message(FATAL_ERROR "Build the x4pro environment first, or pass -DARDUINOJSON_DIR=/path/to/ArduinoJson/src")
endif()
# PlaqueBoard.cpp: the capability tokens and model names the file carries come from the capability layer itself.
add_executable(ReaderIdentityTest ReaderIdentityTest.cpp "${REPO_ROOT}/src/ambient/ReaderIdentity.cpp"
                                  "${REPO_ROOT}/lib/PlaqueBoard/PlaqueBoard.cpp")
target_include_directories(ReaderIdentityTest PRIVATE "${REPO_ROOT}/src/ambient" "${REPO_ROOT}/lib/PlaqueBoard"
                                                      "${ARDUINOJSON_DIR}")
target_compile_options(ReaderIdentityTest PRIVATE -Wall -Wextra -pedantic)
add_test(NAME ReaderIdentity COMMAND ReaderIdentityTest)
