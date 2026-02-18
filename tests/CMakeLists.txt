# googletest via FetchContent
FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.17.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(googletest)

add_executable(asr_tests
    test_config.cpp
    test_audio.cpp
    test_vad.cpp
    test_recognizer.cpp
    test_handler.cpp
    test_metrics.cpp
    test_integration.cpp
)

target_link_libraries(asr_tests PRIVATE
    asr_core
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(asr_tests
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
