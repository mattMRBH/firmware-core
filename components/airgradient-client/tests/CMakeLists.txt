if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.16)
    project(airgradient_client_tests CXX C)

    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_C_STANDARD 11)
    set(CMAKE_C_STANDARD_REQUIRED ON)

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

# Fetch cJSON for host tests.  ESP-IDF firmware build uses its own bundled
# cJSON via REQUIRES json -- this fetched copy is host-only.
include(FetchContent)
FetchContent_Declare(
    cjson
    GIT_REPOSITORY https://github.com/DaveGamble/cJSON.git
    GIT_TAG        v1.7.18
    GIT_SHALLOW    TRUE
)
# Disable cJSON optional features and installation to keep the test build
# lean.
set(ENABLE_CJSON_TEST    OFF CACHE BOOL "" FORCE)
set(ENABLE_CJSON_UTILS   OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_AND_STATIC_LIBS OFF CACHE BOOL "" FORCE)
set(CJSON_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(CJSON_OVERRIDE_BUILD_SHARED_LIBS ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(cjson)

add_library(airgradient_client_test_support
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-client/services/ag_client.cpp"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-client/services/payload_serializer.cpp"
)

target_include_directories(airgradient_client_test_support PUBLIC
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-client"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-common/include"
    "${cjson_SOURCE_DIR}"
)

target_compile_definitions(airgradient_client_test_support PUBLIC TEST_HOST)

target_link_libraries(airgradient_client_test_support PUBLIC cjson)

add_executable(airgradient_client_tests
    ag_client.tests.cpp
    payload_serializer.tests.cpp
)

target_link_libraries(airgradient_client_tests PRIVATE
    airgradient_client_test_support
    Catch2::Catch2WithMain
    trompeloeil::trompeloeil
)

catch_discover_tests(airgradient_client_tests)
