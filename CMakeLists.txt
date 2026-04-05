cmake_minimum_required(VERSION 3.24...4.2)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

if(APPLE AND (NOT DEFINED CMAKE_OSX_SYSROOT OR CMAKE_OSX_SYSROOT STREQUAL ""))
    execute_process(
        COMMAND xcrun --show-sdk-path
        OUTPUT_VARIABLE ASR_MACOS_SDK
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(NOT ASR_MACOS_SDK)
        set(ASR_MACOS_SDK "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk")
    endif()
    if(ASR_MACOS_SDK AND EXISTS "${ASR_MACOS_SDK}")
        set(CMAKE_OSX_SYSROOT "${ASR_MACOS_SDK}" CACHE PATH
            "macOS SDK used for compiler and dependency discovery" FORCE)
        set(ENV{SDKROOT} "${CMAKE_OSX_SYSROOT}")
    endif()
endif()

project(asr-server VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_SUPPRESS_DEVELOPER_WARNINGS 1 CACHE INTERNAL
    "Suppress developer warnings from vendored dependencies" FORCE)
set(CMAKE_WARN_DEPRECATED OFF CACHE BOOL
    "Suppress deprecated CMake warnings from vendored dependencies" FORCE)

set(ASR_SANITIZER_BUILD OFF)
foreach(_asr_flag_var IN ITEMS
        CMAKE_C_FLAGS
        CMAKE_CXX_FLAGS
        CMAKE_EXE_LINKER_FLAGS
        CMAKE_SHARED_LINKER_FLAGS)
    if(DEFINED ${_asr_flag_var} AND "${${_asr_flag_var}}" MATCHES "(^| )-fsanitize=")
        set(ASR_SANITIZER_BUILD ON)
    endif()
endforeach()

# libsamplerate probes clipping behavior with try_run(). On macOS sanitizer
# builds those probes can hang inside the temporary executable, blocking the
# whole configure step for ASan/TSan presets.
if(APPLE AND ASR_SANITIZER_BUILD)
    set(CPU_CLIPS_POSITIVE FALSE CACHE BOOL
        "Target processor clips on positive float to int conversion" FORCE)
    set(CPU_CLIPS_NEGATIVE FALSE CACHE BOOL
        "Target processor clips on negative float to int conversion" FORCE)
    set(CXX_FILESYSTEM_NO_LINK_NEEDED TRUE CACHE BOOL
        "Sanitizer runtime can execute std::filesystem without extra link flags"
        FORCE)
endif()

# Coverage option
option(ASR_ENABLE_COVERAGE "Enable code coverage" OFF)
if(ASR_ENABLE_COVERAGE)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        add_compile_options(-fprofile-instr-generate -fcoverage-mapping)
        add_link_options(-fprofile-instr-generate -fcoverage-mapping)
    else()
        add_compile_options(--coverage -fprofile-arcs -ftest-coverage)
        add_link_options(--coverage)
    endif()
endif()

function(asr_suppress_external_warnings target_name)
    if(TARGET "${target_name}")
        target_compile_options("${target_name}" PRIVATE
            -Wno-cast-align
            -Wno-deprecated-declarations
            -Wno-shadow
            -Wno-unused-parameter
        )
    endif()
endfunction()

function(asr_mark_external_target_system target_name)
    if(TARGET "${target_name}")
        set_property(TARGET "${target_name}" PROPERTY SYSTEM ON)
    endif()
endfunction()

# ---------------------------------------------------------------------------
# FetchContent for all dependencies
# ---------------------------------------------------------------------------
include(FetchContent)
find_package(PkgConfig REQUIRED)
pkg_check_modules(OPUS REQUIRED IMPORTED_TARGET opus)
pkg_check_modules(OPUSFILE IMPORTED_TARGET opusfile)

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

# Make core FetchContent targets available.
# nlohmann_json is made available later only if sherpa-onnx didn't create it.
FetchContent_MakeAvailable(spdlog prometheus-cpp libsamplerate drogon)
foreach(_asr_external_target IN ITEMS spdlog civetweb core pull samplerate trantor drogon)
    asr_suppress_external_warnings(${_asr_external_target})
    asr_mark_external_target_system(${_asr_external_target})
endforeach()

# 6. onnxruntime (prebuilt binaries — not a CMake project, download manually)
option(ASR_ENABLE_CUDA "Enable GPU onnxruntime" OFF)
set(ONNXRUNTIME_VERSION "1.23.2")
set(ONNXRUNTIME_CACHE_ROOT "${CMAKE_SOURCE_DIR}/build/_shared_deps/onnxruntime" CACHE PATH
    "Shared ONNX Runtime cache directory across CMake presets")

string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" ASR_SYSTEM_PROCESSOR)
if(ASR_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64)$")
    set(ONNXRUNTIME_ARCH "x64")
elseif(ASR_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    set(ONNXRUNTIME_ARCH "aarch64")
else()
    message(FATAL_ERROR
        "Unsupported CPU architecture for prebuilt ONNX Runtime: ${CMAKE_SYSTEM_PROCESSOR}.\n"
        "Supported values: x86_64/amd64, aarch64/arm64.")
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    if(ASR_ENABLE_CUDA)
        if(NOT ONNXRUNTIME_ARCH STREQUAL "x64")
            message(FATAL_ERROR
                "CUDA prebuilt ONNX Runtime is only available for x64.\n"
                "Current architecture: ${CMAKE_SYSTEM_PROCESSOR}.\n"
                "Use CPU build or build Docker image with --platform=linux/amd64.")
        endif()
        set(ONNXRUNTIME_PACKAGE_NAME "onnxruntime-linux-${ONNXRUNTIME_ARCH}-gpu-${ONNXRUNTIME_VERSION}")
        set(ONNXRUNTIME_SHA256 "2083e361072a79ce16a90dcd5f5cb3ab92574a82a3ce0ac01e5cfa3158176f53")
    else()
        set(ONNXRUNTIME_PACKAGE_NAME "onnxruntime-linux-${ONNXRUNTIME_ARCH}-${ONNXRUNTIME_VERSION}")
        if(ONNXRUNTIME_ARCH STREQUAL "x64")
            set(ONNXRUNTIME_SHA256 "1fa4dcaef22f6f7d5cd81b28c2800414350c10116f5fdd46a2160082551c5f9b")
        elseif(ONNXRUNTIME_ARCH STREQUAL "aarch64")
            set(ONNXRUNTIME_SHA256 "7c63c73560ed76b1fac6cff8204ffe34fe180e70d6582b5332ec094810241e5c")
        endif()
    endif()
    set(ONNXRUNTIME_LIB_BASENAME "libonnxruntime.so")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    if(ASR_ENABLE_CUDA)
        message(FATAL_ERROR "CUDA prebuilt ONNX Runtime is not available on macOS.")
    endif()
    set(ONNXRUNTIME_PACKAGE_NAME "onnxruntime-osx-universal2-${ONNXRUNTIME_VERSION}")
    set(ONNXRUNTIME_SHA256 "49ae8e3a66ccb18d98ad3fe7f5906b6d7887df8a5edd40f49eb2b14e20885809")
    set(ONNXRUNTIME_LIB_BASENAME "libonnxruntime.dylib")
else()
    message(FATAL_ERROR
        "Unsupported operating system for prebuilt ONNX Runtime: ${CMAKE_SYSTEM_NAME}")
endif()

message(STATUS
    "Using ONNX Runtime package '${ONNXRUNTIME_PACKAGE_NAME}' "
    "for system '${CMAKE_SYSTEM_NAME}' and processor '${CMAKE_SYSTEM_PROCESSOR}'")
set(ONNXRUNTIME_URL "https://github.com/microsoft/onnxruntime/releases/download/v${ONNXRUNTIME_VERSION}/${ONNXRUNTIME_PACKAGE_NAME}.tgz")
set(ONNXRUNTIME_ARCHIVE "${ONNXRUNTIME_CACHE_ROOT}/${ONNXRUNTIME_PACKAGE_NAME}.tgz")
set(ONNXRUNTIME_DIR "${ONNXRUNTIME_CACHE_ROOT}/${ONNXRUNTIME_PACKAGE_NAME}")
set(ONNXRUNTIME_LIB "${ONNXRUNTIME_DIR}/lib/${ONNXRUNTIME_LIB_BASENAME}")
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

# Pin to the exact commit behind v1.12.31 so FetchContent stays reproducible
# even if the upstream tag is ever re-pointed.
set(SHERPA_ONNX_GIT_TAG "e0ab4a8beb10477d25ef48e518a537995c38c698")

FetchContent_Declare(sherpa_onnx
    GIT_REPOSITORY https://github.com/k2-fsa/sherpa-onnx.git
    GIT_TAG        ${SHERPA_ONNX_GIT_TAG}
)
FetchContent_MakeAvailable(sherpa_onnx)
foreach(_asr_external_target IN ITEMS
    kissfft
    ssentencepiece_core
    fst
    fstfar
    kaldi-native-fbank-core
    kaldifst_core
    kaldi-decoder-core
    sherpa-onnx-core
    sherpa-onnx-c-api
    sherpa-onnx-cxx-api
)
    asr_suppress_external_warnings(${_asr_external_target})
    asr_mark_external_target_system(${_asr_external_target})
endforeach()

# sherpa-onnx >= v1.12.28 fetches nlohmann/json internally.
# Keep backward compatibility with older sherpa-onnx versions that may not.
if(NOT TARGET nlohmann_json::nlohmann_json)
    FetchContent_MakeAvailable(nlohmann_json)
endif()

# Ensure sherpa-onnx C/C++ API targets see nlohmann/json headers.
# (Needed across sherpa-onnx versions with different json fetch behavior.)
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
        IMPORTED_LOCATION "${ONNXRUNTIME_LIB}"
        INTERFACE_INCLUDE_DIRECTORIES "${ONNXRUNTIME_DIR}/include"
    )
endif()

# ---------------------------------------------------------------------------
# Core library (all source files except main.cpp)
# ---------------------------------------------------------------------------
add_library(asr_core STATIC
    src/config.cpp
    src/audio/base64.cpp
    src/audio/pcm.cpp
    src/audio/resampler.cpp
    src/audio/realtime_opus.cpp
    src/audio/decode.cpp
    src/executor.cpp
    src/vad.cpp
    src/recognizer.cpp
    src/handler.cpp
    src/metrics.cpp
    src/server.cpp
    src/realtime_session.cpp
    src/offline_transcription.cpp
    src/whisper_api.cpp
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
    PkgConfig::OPUS
    sherpa-onnx-c-api
)

if(OPUSFILE_FOUND)
    target_link_libraries(asr_core PUBLIC PkgConfig::OPUSFILE)
    target_compile_definitions(asr_core PUBLIC ASR_HAS_OPUSFILE=1)
endif()

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
