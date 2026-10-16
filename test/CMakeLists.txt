cmake_minimum_required(VERSION 3.16)
project(crosspoint_reader_tests C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)

FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.17.0
)

set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

add_library(crosspoint_test_common INTERFACE)
target_include_directories(crosspoint_test_common INTERFACE
  ${REPO_ROOT}
  ${REPO_ROOT}/lib
)
target_compile_options(crosspoint_test_common INTERFACE
  -Wall
  -Wextra
  -pedantic
)

enable_testing()
include(GoogleTest)

add_subdirectory(streaming_json_parser)
add_subdirectory(differential_rounding)
add_subdirectory(hyphenation_eval)
add_subdirectory(utf8_compose)
add_subdirectory(opds_filename)
add_subdirectory(html_to_plain_text)
add_subdirectory(library_text)
add_subdirectory(library_format)
add_subdirectory(library_index_file)
add_subdirectory(library_builder)
add_subdirectory(minibidi_arabic)
add_subdirectory(combining_marks)
add_subdirectory(credential_integrity)
add_subdirectory(xml_parser_utils)
add_subdirectory(token_boundary)
add_subdirectory(koreader_xpath_resolver)
add_subdirectory(progress_comparison)
add_subdirectory(font_cache_manager)
add_subdirectory(css_parser)
add_subdirectory(chapter_html_slim_parser)
add_subdirectory(korean_line_breaking)
add_subdirectory(ligature_guard)
add_subdirectory(page_link)
add_subdirectory(content_opf_parser)
add_subdirectory(chapter_position)
add_subdirectory(home_button)
add_subdirectory(button_press_navigation)
add_subdirectory(home_layout)

# Register: the model and the layout are pure (no JSON); the layout is measured with the reader's real font tables.
add_subdirectory(register_model)
add_subdirectory(register_layout)

# Standalone like method_book: it needs ArduinoJson, which only exists after a PlatformIO build.
if(EXISTS "${REPO_ROOT}/.pio/libdeps/x4pro/ArduinoJson/src/ArduinoJson.h")
  add_subdirectory(sleep_frame)
endif()

# Needs a host Mbed TLS 3.6.6 (date-disabled, like the reader's): pass MBEDTLS_SOURCE and MBEDTLS_BUILD as test/message_tls
# does, or -DNETWORK_PROFILE_FETCH_MBEDTLS=ON to fetch and build the release here.
if(DEFINED MBEDTLS_SOURCE OR NETWORK_PROFILE_FETCH_MBEDTLS)
  add_subdirectory(network_profile)
endif()

add_subdirectory(inflate_stream)
add_subdirectory(fs_helpers)
add_subdirectory(txt_to_html)

add_subdirectory(absolute_grayscale)

add_subdirectory(sd_card_font)
add_subdirectory(glyph_bitmap)

add_subdirectory(reader_session)
add_subdirectory(plugin_manifest_classification)
add_subdirectory(protected_paths)

# Connection check (SPEC-UNIVERSITY-WIFI sections 7 and 8): time servers, clock provenance, HTTPS failure classes.
add_subdirectory(time_servers)
add_subdirectory(clock_provenance)
add_subdirectory(http_failure)

# Method host tests: pure C++, but they parse JSON with the ArduinoJson sources the x4pro build fetches,
# so they register only where those exist.
if(EXISTS "${REPO_ROOT}/.pio/libdeps/x4pro/ArduinoJson/src/ArduinoJson.h")
  add_subdirectory(method_book)
  add_subdirectory(method_library)
  add_subdirectory(method_format)
  add_subdirectory(method_check)
  add_subdirectory(method_layout)
  add_subdirectory(connection_check)
  add_subdirectory(register_data)
  add_subdirectory(register_wake)
endif()

# X4 Classic support: the capability layer (with the step timer, sleep-frame and wake decisions it feeds), the
# reader.json it writes, the timer itself, and the board tag the SD card installer checks.
if(EXISTS "${REPO_ROOT}/.pio/libdeps/x4pro/ArduinoJson/src/ArduinoJson.h")
  add_subdirectory(plaque_board)
  add_subdirectory(beta_seams)
  add_subdirectory(reader_identity)
  add_subdirectory(step_timer)
  add_subdirectory(method_wake)
endif()
add_subdirectory(firmware_board_tag)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME TrustedTimeTimezone COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_trusted_time.py)
