if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.16)
    project(airgradient_payload_cache_tests CXX)

    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)

    get_filename_component(AIRGRADIENT_REPO_ROOT
                           "${CMAKE_CURRENT_SOURCE_DIR}/../../.."
                           ABSOLUTE)

    include(FetchContent)

    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        v3.5.0
        GIT_SHALLOW    TRUE
    )
    FetchContent_Declare(
        trompeloeil
        GIT_REPOSITORY https://github.com/rollbear/trompeloeil.git
        GIT_TAG        v47
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(Catch2 trompeloeil)

    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
    include(CTest)
    include(Catch)
endif()

add_library(payload_cache_test_support
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-payload-cache/services/payload_cache.cpp"
)

target_include_directories(payload_cache_test_support PUBLIC
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-payload-cache"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-common/include"
)

target_compile_definitions(payload_cache_test_support PUBLIC TEST_HOST)

add_executable(payload_cache_tests
    payload_cache.tests.cpp
)

target_link_libraries(payload_cache_tests PRIVATE
    payload_cache_test_support
    Catch2::Catch2WithMain
    trompeloeil::trompeloeil
)

catch_discover_tests(payload_cache_tests)
