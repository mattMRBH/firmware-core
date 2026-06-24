if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.16)
    project(airgradient_local_server_tests CXX C)

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
    FetchContent_MakeAvailable(Catch2)

    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
    include(CTest)
    include(Catch)
endif()

# cJSON for host tests (firmware uses ESP-IDF's bundled copy via the
# `json` managed component).
if(NOT TARGET cjson_host)
    include(FetchContent)
    FetchContent_Declare(
        cjson
        GIT_REPOSITORY https://github.com/DaveGamble/cJSON.git
        GIT_TAG        v1.7.18
        GIT_SHALLOW    TRUE
    )
    set(ENABLE_CJSON_TEST OFF CACHE BOOL "" FORCE)
    set(ENABLE_CJSON_UTILS OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(CJSON_OVERRIDE_BUILD_SHARED_LIBS ON CACHE BOOL "" FORCE)
    set(CJSON_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(cjson)
    target_include_directories(cjson PUBLIC
        "$<BUILD_INTERFACE:${cjson_SOURCE_DIR}>")
    add_library(cjson_host ALIAS cjson)
endif()

add_library(local_server_test_support
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-local-server/internal/measures_json.cpp"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-local-server/internal/config_json.cpp"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-local-server/services/local_server.cpp"
)

target_include_directories(local_server_test_support PUBLIC
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-local-server"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-local-server/tests"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-http-server"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-http-server/tests"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-common/include"
)

target_compile_definitions(local_server_test_support PUBLIC TEST_HOST)
target_link_libraries(local_server_test_support PUBLIC cjson)

add_executable(airgradient_local_server_tests
    measures_json.tests.cpp
    config_json.tests.cpp
    handler.tests.cpp
)

target_link_libraries(airgradient_local_server_tests PRIVATE
    local_server_test_support
    Catch2::Catch2WithMain
)

catch_discover_tests(airgradient_local_server_tests)
