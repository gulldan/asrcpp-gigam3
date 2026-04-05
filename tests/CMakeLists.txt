# googletest via FetchContent
FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.17.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(googletest)
foreach(_asr_external_target IN ITEMS gtest gtest_main gmock gmock_main)
    asr_suppress_external_warnings(${_asr_external_target})
endforeach()

add_executable(asr_tests
    test_config.cpp
    test_audio.cpp
    test_vad.cpp
    test_recognizer.cpp
    test_handler.cpp
    test_metrics.cpp
    test_logging.cpp
    test_executor.cpp
    test_integration.cpp
    test_realtime_session.cpp
    test_offline_transcription.cpp
    test_whisper_api.cpp
)

target_link_libraries(asr_tests PRIVATE
    asr_core
    GTest::gtest_main
)

include(GoogleTest)
set(_asr_gtest_discovery_mode POST_BUILD)
if(ASR_SANITIZER_BUILD)
    set(_asr_gtest_discovery_mode PRE_TEST)
endif()

gtest_discover_tests(asr_tests
    DISCOVERY_MODE "${_asr_gtest_discovery_mode}"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

# Allocation-counting benchmark (separate binary — overrides global operator new)
add_executable(asr_alloc_bench bench_alloc.cpp)
target_compile_options(asr_alloc_bench PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
target_link_libraries(asr_alloc_bench PRIVATE asr_core)
add_test(NAME alloc_bench
    COMMAND asr_alloc_bench
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

add_executable(asr_decision_bench bench_decisions.cpp)
target_compile_options(asr_decision_bench PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
target_link_libraries(asr_decision_bench PRIVATE nlohmann_json::nlohmann_json)
add_test(NAME decision_bench
    COMMAND asr_decision_bench
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
