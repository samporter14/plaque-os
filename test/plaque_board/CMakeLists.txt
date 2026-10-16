cmake_minimum_required(VERSION 3.16)
project(plaque_board_host_tests CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
enable_testing()
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(ARDUINOJSON_DIR "${REPO_ROOT}/.pio/libdeps/x4pro/ArduinoJson/src" CACHE PATH "ArduinoJson 7 include directory")
if(NOT EXISTS "${ARDUINOJSON_DIR}/ArduinoJson.h")
  message(FATAL_ERROR "Build the x4pro environment first, or pass -DARDUINOJSON_DIR=/path/to/ArduinoJson/src")
endif()

# The pure capability layer, walked through the step timer, sleep-frame and wake decisions it feeds (StepTimer.cpp asks
# MethodProtocol.cpp whether a path is a Method book, which pulls in MethodTimers.cpp and MessageProtocol.cpp).
add_executable(PlaqueBoardTest PlaqueBoardTest.cpp
  "${REPO_ROOT}/lib/PlaqueBoard/PlaqueBoard.cpp"
  "${REPO_ROOT}/src/ambient/MethodWake.cpp"
  "${REPO_ROOT}/src/ambient/SleepFrame.cpp"
  "${REPO_ROOT}/src/ambient/StepTimer.cpp"
  "${REPO_ROOT}/src/ambient/MethodProtocol.cpp"
  "${REPO_ROOT}/src/ambient/MethodTimers.cpp"
  "${REPO_ROOT}/src/ambient/MessageProtocol.cpp")
target_include_directories(PlaqueBoardTest PRIVATE "${REPO_ROOT}/lib/PlaqueBoard" "${REPO_ROOT}/src/ambient"
                                                   "${ARDUINOJSON_DIR}")
target_compile_options(PlaqueBoardTest PRIVATE -Wall -Wextra -pedantic)
add_test(NAME PlaqueBoard COMMAND PlaqueBoardTest)

# The device side, against the SDK's real BoardConfig.h profiles, once per firmware environment's device flags. The
# stubs are the few Arduino calls BoardConfig.h's inline helpers make.
set(SDK_BOARDCONFIG "${REPO_ROOT}/freeink-sdk/libs/hardware/BoardConfig/include")
foreach(env x4pro x4c x4 sticky papermono)
  if(env STREQUAL "x4pro")
    set(flags FREEINK_DEVICE_X4PRO=1 FREEINK_CAP_USB_MSC=1 EXPECT_MODEL="x4pro")
  elseif(env STREQUAL "x4c")
    set(flags FREEINK_DEVICE_X4CLASSIC=1 FREEINK_CAP_USB_MSC=1 EXPECT_MODEL="x4c")
  elseif(env STREQUAL "x4")
    set(flags FREEINK_DEVICE_X4=1 FREEINK_DEVICE_X3=1 EXPECT_MODEL="none")
  elseif(env STREQUAL "papermono")
    set(flags FREEINK_DEVICE_PAPERMONO=1 EXPECT_MODEL="none")
  else()
    set(flags FREEINK_DEVICE_STICKY=1 EXPECT_MODEL="none")
  endif()
  add_executable(PlaqueBoardProfileTest_${env} PlaqueBoardProfileTest.cpp
    "${REPO_ROOT}/lib/PlaqueBoard/PlaqueBoard.cpp"
    "${REPO_ROOT}/lib/PlaqueBoard/PlaqueBoardDevice.cpp")
  target_include_directories(PlaqueBoardProfileTest_${env} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/stubs"
                                                                   "${SDK_BOARDCONFIG}" "${REPO_ROOT}/lib/PlaqueBoard")
  target_compile_definitions(PlaqueBoardProfileTest_${env} PRIVATE ${flags})
  # The SDK header is not ours to tidy: its warnings are not this test's.
  target_compile_options(PlaqueBoardProfileTest_${env} PRIVATE -Wall -Wextra -Wno-missing-field-initializers
                                                               -Wno-unused-parameter)
  add_test(NAME PlaqueBoardProfile_${env} COMMAND PlaqueBoardProfileTest_${env})
endforeach()
