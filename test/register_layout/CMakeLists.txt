cmake_minimum_required(VERSION 3.16)
project(register_layout_host_tests CXX C)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
enable_testing()
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
# The layout measured with the reader's real fonts. EpdFont.cpp and the UTF-8 reader are the firmware's own.
set(FONT_SOURCES
  "${REPO_ROOT}/lib/EpdFont/EpdFont.cpp"
  "${REPO_ROOT}/lib/EpdFont/EpdFontFamily.cpp"
  "${REPO_ROOT}/lib/Utf8/Utf8.cpp")
add_executable(RegisterLayoutTest RegisterLayoutTest.cpp
  "${REPO_ROOT}/src/ambient/RegisterLayout.cpp"
  "${REPO_ROOT}/src/ambient/RegisterModel.cpp"
  ${FONT_SOURCES})
target_include_directories(RegisterLayoutTest PRIVATE
  "${REPO_ROOT}/src/ambient" "${REPO_ROOT}/lib" "${REPO_ROOT}/lib/EpdFont" "${REPO_ROOT}/lib/Utf8" "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(RegisterLayoutTest PRIVATE -Wall -Wextra -pedantic)
add_test(NAME RegisterLayout COMMAND RegisterLayoutTest)

# Not a test: draws the screens to 1-bit images with the same glyph bitmaps the panel gets (see Preview.cpp).
add_executable(RegisterPreview Preview.cpp
  "${REPO_ROOT}/src/ambient/RegisterLayout.cpp"
  "${REPO_ROOT}/src/ambient/RegisterModel.cpp"
  "${REPO_ROOT}/lib/InflateReader/InflateReader.cpp"
  "${REPO_ROOT}/lib/uzlib/src/tinflate.c"
  ${FONT_SOURCES})
target_include_directories(RegisterPreview PRIVATE
  "${REPO_ROOT}/src/ambient" "${REPO_ROOT}/lib" "${REPO_ROOT}/lib/EpdFont" "${REPO_ROOT}/lib/Utf8" "${REPO_ROOT}/lib/InflateReader"
  "${REPO_ROOT}/lib/uzlib/src" "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(RegisterPreview PRIVATE -Wall -Wextra)
