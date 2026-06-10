if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.16)
    project(airgradient_ota_tests CXX)

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

add_library(ota_test_support
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-ota/services/ota_updater.cpp"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-ota/services/ota_url.cpp"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-common/rtos.cpp"
)

target_include_directories(ota_test_support PUBLIC
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-ota"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-common/include"
)

target_compile_definitions(ota_test_support PUBLIC TEST_HOST)

add_executable(airgradient_ota_tests
    ota_url.tests.cpp
    ota_updater.tests.cpp
)

target_include_directories(airgradient_ota_tests PRIVATE
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-ota"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(airgradient_ota_tests PRIVATE
    ota_test_support
    Catch2::Catch2WithMain
    trompeloeil::trompeloeil
)

catch_discover_tests(airgradient_ota_tests)

# ---------------------------------------------------------------------------
# TinyCBOR (pure C library, no ESP-IDF dependency) — Control/Status CBOR on the
# BLE push path. The `tinycbor` target is shared across component test
# subdirectories in the aggregated build, so guard against a double definition.
# ---------------------------------------------------------------------------

set(OTA_TINYCBOR_DIR
    "${AIRGRADIENT_REPO_ROOT}/products/go/managed_components/espressif__cbor/tinycbor/src")

if(NOT TARGET tinycbor)
add_library(tinycbor STATIC
    "${OTA_TINYCBOR_DIR}/cborencoder.c"
    "${OTA_TINYCBOR_DIR}/cborencoder_close_container_checked.c"
    "${OTA_TINYCBOR_DIR}/cborencoder_float.c"
    "${OTA_TINYCBOR_DIR}/cborerrorstrings.c"
    "${OTA_TINYCBOR_DIR}/cborparser.c"
    "${OTA_TINYCBOR_DIR}/cborparser_dup_string.c"
    "${OTA_TINYCBOR_DIR}/cborparser_float.c"
    "${OTA_TINYCBOR_DIR}/cborvalidation.c"
)

target_include_directories(tinycbor PUBLIC "${OTA_TINYCBOR_DIR}")
endif()

# ---------------------------------------------------------------------------
# BLE push OTA service tests
# ---------------------------------------------------------------------------

add_library(ota_ble_test_support
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-ota/services/ota_ble_service.cpp"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-common/rtos.cpp"
)

target_include_directories(ota_ble_test_support PUBLIC
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-ota"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-common/include"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-ble"
)

target_link_libraries(ota_ble_test_support PUBLIC tinycbor)

target_compile_definitions(ota_ble_test_support PUBLIC TEST_HOST)

add_executable(airgradient_ota_ble_tests
    ota_ble_service.tests.cpp
)

target_include_directories(airgradient_ota_ble_tests PRIVATE
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-ota"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-ble"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(airgradient_ota_ble_tests PRIVATE
    ota_ble_test_support
    Catch2::Catch2WithMain
)

catch_discover_tests(airgradient_ota_ble_tests)
