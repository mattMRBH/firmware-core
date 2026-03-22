if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.16)
    project(go_input_tests CXX)

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

add_library(go_input_test_support
    "${AIRGRADIENT_REPO_ROOT}/products/go/main/go_input.cpp"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-common/rtos.cpp"
)

target_include_directories(go_input_test_support PUBLIC
    "${AIRGRADIENT_REPO_ROOT}/products/go/main"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-common/include"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-touch/hal"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-gpio/hal"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-gps"
)

target_compile_definitions(go_input_test_support PRIVATE TEST_HOST)

add_executable(go_input_tests
    go_input.tests.cpp
)

target_link_libraries(go_input_tests PRIVATE
    go_input_test_support
    Catch2::Catch2WithMain
    trompeloeil::trompeloeil
)

catch_discover_tests(go_input_tests)

# ---------------------------------------------------------------------------
# go_storage tests
# ---------------------------------------------------------------------------

add_library(go_storage_test_support
    "${AIRGRADIENT_REPO_ROOT}/products/go/main/go_storage.cpp"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-payload-cache/services/payload_cache.cpp"
)

target_include_directories(go_storage_test_support PUBLIC
    "${AIRGRADIENT_REPO_ROOT}/products/go/main"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-payload-cache"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-nand-storage/hal"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-common/include"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-gps"
)

target_compile_definitions(go_storage_test_support PUBLIC
    TEST_HOST
    CONFIG_PAYLOAD_CACHE_TYPE_AGO=1
    CONFIG_PAYLOAD_CACHE_MAX_SIZE=16
)

add_executable(go_storage_tests
    go_storage.tests.cpp
)

target_link_libraries(go_storage_tests PRIVATE
    go_storage_test_support
    Catch2::Catch2WithMain
    trompeloeil::trompeloeil
)

catch_discover_tests(go_storage_tests)
