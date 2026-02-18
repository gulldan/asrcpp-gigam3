cmake_minimum_required(VERSION 3.24...4.2)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

project(asr-server VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Coverage option
option(ASR_ENABLE_COVERAGE "Enable code coverage" OFF)
if(ASR_ENABLE_COVERAGE)
    add_compile_options(--coverage -fprofile-arcs -ftest-coverage)
    add_link_options(--coverage)
endif()

# ---------------------------------------------------------------------------
# FetchContent for all dependencies
# ---------------------------------------------------------------------------
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)
set(FETCHCONTENT_BASE_DIR "${CMAKE_SOURCE_DIR}/build/_shared_deps/fetchcontent" CACHE PATH
    "Shared FetchContent cache directory across CMake presets")

# 1. spdlog (uses bundled fmt)
set(SPDLOG_FMT_EXTERNAL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG        v1.16.0
    GIT_SHALLOW    TRUE
)

# 2. nlohmann_json
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.12.0
    GIT_SHALLOW    TRUE
)

# 3. prometheus-cpp
set(ENABLE_PUSH OFF CACHE BOOL "" FORCE)
set(ENABLE_COMPRESSION OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_Declare(prometheus-cpp
    GIT_REPOSITORY https://github.com/jupp0r/prometheus-cpp.git
    GIT_TAG        v1.3.0
    GIT_SHALLOW    TRUE
)

# 4. libsamplerate
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(LIBSAMPLERATE_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(libsamplerate
    GIT_REPOSITORY https://github.com/libsndfile/libsamplerate.git
    GIT_TAG        0.2.2
    GIT_SHALLOW    TRUE
)

# 5. drogon (pulls its own trantor)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_CTL OFF CACHE BOOL "" FORCE)
set(BUILD_ORM OFF CACHE BOOL "" FORCE)
set(BUILD_BROTLI OFF CACHE BOOL "" FORCE)
set(BUILD_YAML_CONFIG OFF CACHE BOOL "" FORCE)
set(BUILD_POSTGRESQL OFF CACHE BOOL "" FORCE)
set(BUILD_MYSQL OFF CACHE BOOL "" FORCE)
set(BUILD_SQLITE OFF CACHE BOOL "" FORCE)
set(BUILD_REDIS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(drogon
    GIT_REPOSITORY https://github.com/drogonframework/drogon.git
    GIT_TAG        v1.9.11
    GIT_SHALLOW    TRUE
)

# Make all FetchContent targets available
FetchContent_MakeAvailable(spdlog nlohmann_json prometheus-cpp libsamplerate drogon)

# 6. onnxruntime (prebuilt binaries — not a CMake project, download manually)
option(ASR_ENABLE_CUDA "Enable GPU onnxruntime" OFF)
set(ONNXRUNTIME_VERSION "1.23.2")
set(ONNXRUNTIME_CACHE_ROOT "${CMAKE_SOURCE_DIR}/build/_shared_deps/onnxruntime" CACHE PATH
    "Shared ONNX Runtime cache directory across CMake presets")
if(ASR_ENABLE_CUDA)
    set(ONNXRUNTIME_PACKAGE_NAME "onnxruntime-linux-x64-gpu-${ONNXRUNTIME_VERSION}")
    set(ONNXRUNTIME_SHA256 "295b0572870c89b3cfb9fd7893090da5e4b42e40933d8e5c7781de1f927381f2")
else()
    set(ONNXRUNTIME_PACKAGE_NAME "onnxruntime-linux-x64-${ONNXRUNTIME_VERSION}")
    set(ONNXRUNTIME_SHA256 "1fa4dcaef22f6f7d5cd81b28c2800414350c10116f5fdd46a2160082551c5f9b")
endif()
set(ONNXRUNTIME_URL "https://github.com/microsoft/onnxruntime/releases/download/v${ONNXRUNTIME_VERSION}/${ONNXRUNTIME_PACKAGE_NAME}.tgz")
set(ONNXRUNTIME_ARCHIVE "${ONNXRUNTIME_CACHE_ROOT}/${ONNXRUNTIME_PACKAGE_NAME}.tgz")
set(ONNXRUNTIME_DIR "${ONNXRUNTIME_CACHE_ROOT}/${ONNXRUNTIME_PACKAGE_NAME}")
set(ONNXRUNTIME_LIB "${ONNXRUNTIME_DIR}/lib/libonnxruntime.so")
set(ONNXRUNTIME_HEADER "${ONNXRUNTIME_DIR}/include/onnxruntime_cxx_api.h")

if(NOT EXISTS "${ONNXRUNTIME_LIB}" OR NOT EXISTS "${ONNXRUNTIME_HEADER}")
    file(MAKE_DIRECTORY "${ONNXRUNTIME_CACHE_ROOT}")

    if(EXISTS "${ONNXRUNTIME_ARCHIVE}")
        file(SHA256 "${ONNXRUNTIME_ARCHIVE}" ONNXRUNTIME_ARCHIVE_SHA256)
        if(NOT ONNXRUNTIME_ARCHIVE_SHA256 STREQUAL ONNXRUNTIME_SHA256)
            message(WARNING "Removing stale ONNX Runtime archive with unexpected hash: ${ONNXRUNTIME_ARCHIVE}")
            file(REMOVE "${ONNXRUNTIME_ARCHIVE}")
        endif()
    endif()

    if(NOT EXISTS "${ONNXRUNTIME_ARCHIVE}")
        message(STATUS "Downloading onnxruntime v${ONNXRUNTIME_VERSION} (CUDA=${ASR_ENABLE_CUDA})...")
        file(DOWNLOAD "${ONNXRUNTIME_URL}" "${ONNXRUNTIME_ARCHIVE}"
            SHOW_PROGRESS
            STATUS dl_status
            EXPECTED_HASH SHA256=${ONNXRUNTIME_SHA256})
        list(GET dl_status 0 dl_code)
        if(NOT dl_code EQUAL 0)
            message(FATAL_ERROR "Failed to download onnxruntime: ${dl_status}")
        endif()
    endif()

    if(EXISTS "${ONNXRUNTIME_DIR}")
        message(WARNING "Removing incomplete ONNX Runtime directory: ${ONNXRUNTIME_DIR}")
        file(REMOVE_RECURSE "${ONNXRUNTIME_DIR}")
    endif()

    file(ARCHIVE_EXTRACT INPUT "${ONNXRUNTIME_ARCHIVE}" DESTINATION "${ONNXRUNTIME_CACHE_ROOT}")
endif()

if(NOT EXISTS "${ONNXRUNTIME_LIB}" OR NOT EXISTS "${ONNXRUNTIME_HEADER}")
    message(FATAL_ERROR
        "ONNX Runtime is missing required files after extraction:\n"
        "  lib: ${ONNXRUNTIME_LIB}\n"
        "  hdr: ${ONNXRUNTIME_HEADER}")
endif()

# ---------------------------------------------------------------------------
# 7. sherpa-onnx (from source via FetchContent)
#    Built with C++17 because openfst uses deprecated allocator APIs removed in C++20
#    Uses our pre-downloaded onnxruntime via env vars
# ---------------------------------------------------------------------------
set(SHERPA_ONNX_ENABLE_C_API ON CACHE BOOL "" FORCE)
set(SHERPA_ONNX_ENABLE_BINARY OFF CACHE BOOL "" FORCE)
set(SHERPA_ONNX_ENABLE_PYTHON OFF CACHE BOOL "" FORCE)
set(SHERPA_ONNX_ENABLE_PORTAUDIO OFF CACHE BOOL "" FORCE)
set(SHERPA_ONNX_ENABLE_WEBSOCKET OFF CACHE BOOL "" FORCE)
set(SHERPA_ONNX_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(SHERPA_ONNX_ENABLE_TTS OFF CACHE BOOL "" FORCE)
set(SHERPA_ONNX_ENABLE_SPEAKER_DIARIZATION OFF CACHE BOOL "" FORCE)
set(SHERPA_ONNX_LINK_LIBSTDCPP_STATICALLY OFF CACHE BOOL "" FORCE)
set(SHERPA_ONNX_USE_PRE_INSTALLED_ONNXRUNTIME_IF_AVAILABLE ON CACHE BOOL "" FORCE)

# Point sherpa-onnx to our downloaded onnxruntime
set(ENV{SHERPA_ONNXRUNTIME_INCLUDE_DIR} "${ONNXRUNTIME_DIR}/include")
set(ENV{SHERPA_ONNXRUNTIME_LIB_DIR} "${ONNXRUNTIME_DIR}/lib")

FetchContent_Declare(sherpa_onnx
    GIT_REPOSITORY https://github.com/k2-fsa/sherpa-onnx.git
    GIT_TAG        v1.12.24
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(sherpa_onnx)

# sherpa-onnx C/C++ API targets unconditionally include nlohmann/json.hpp but
# fetch the library only when TTS is enabled. Provide include path from our copy.
if(TARGET nlohmann_json::nlohmann_json)
    get_target_property(_nlohmann_inc nlohmann_json::nlohmann_json INTERFACE_INCLUDE_DIRECTORIES)
    foreach(_sherpa_api_target IN ITEMS sherpa-onnx-c-api sherpa-onnx-cxx-api)
        if(TARGET ${_sherpa_api_target})
            target_include_directories(${_sherpa_api_target} PRIVATE ${_nlohmann_inc})
        endif()
    endforeach()
endif()

# Create imported targets for our pre-downloaded onnxruntime
if(NOT TARGET onnxruntime_headers)
    add_library(onnxruntime_headers INTERFACE)
    target_include_directories(onnxruntime_headers INTERFACE "${ONNXRUNTIME_DIR}/include")
endif()

if(NOT TARGET onnxruntime)
    add_library(onnxruntime SHARED IMPORTED)
    set_target_properties(onnxruntime PROPERTIES
        IMPORTED_LOCATION "${ONNXRUNTIME_DIR}/lib/libonnxruntime.so"
        INTERFACE_INCLUDE_DIRECTORIES "${ONNXRUNTIME_DIR}/include"
    )
endif()

# ---------------------------------------------------------------------------
# Core library (all source files except main.cpp)
# ---------------------------------------------------------------------------
add_library(asr_core STATIC
    src/config.cpp
    src/audio.cpp
    src/vad.cpp
    src/recognizer.cpp
    src/handler.cpp
    src/metrics.cpp
    src/server.cpp
)

target_include_directories(asr_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)

# Compiler warnings for our code
target_compile_options(asr_core PRIVATE -Wall -Wextra -Wpedantic -Wshadow)

target_link_libraries(asr_core PUBLIC
    drogon
    prometheus-cpp::core
    onnxruntime
    onnxruntime_headers
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    samplerate
    sherpa-onnx-c-api
)

# ---------------------------------------------------------------------------
# Main executable
# ---------------------------------------------------------------------------
add_executable(asr-server src/main.cpp)
target_compile_options(asr-server PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
target_link_libraries(asr-server PRIVATE asr_core)

# ---------------------------------------------------------------------------
# Quality targets (scripts/)
# ---------------------------------------------------------------------------
set(ASR_SCRIPTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/scripts")

add_custom_target(format
    COMMAND "${ASR_SCRIPTS_DIR}/format.sh" fix
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Running clang-format"
    USES_TERMINAL
)

add_custom_target(format-check
    COMMAND "${ASR_SCRIPTS_DIR}/format.sh" check
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Checking clang-format"
    USES_TERMINAL
)

add_custom_target(lint
    COMMAND "${ASR_SCRIPTS_DIR}/lint.sh" "${CMAKE_BINARY_DIR}"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Running clang-tidy"
    USES_TERMINAL
)

add_custom_target(cppcheck
    COMMAND "${ASR_SCRIPTS_DIR}/cppcheck.sh" "${CMAKE_BINARY_DIR}"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Running cppcheck"
    USES_TERMINAL
)

add_custom_target(iwyu
    COMMAND "${ASR_SCRIPTS_DIR}/iwyu.sh" "${CMAKE_BINARY_DIR}"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Running include-what-you-use"
    USES_TERMINAL
)

add_custom_target(quality
    DEPENDS format-check lint cppcheck
)

add_custom_target(quality-full
    DEPENDS format-check lint cppcheck iwyu
)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
option(ASR_BUILD_TESTS "Build tests" ON)
if(ASR_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
install(TARGETS asr-server RUNTIME DESTINATION bin)
install(DIRECTORY static/ DESTINATION share/asr/static)
