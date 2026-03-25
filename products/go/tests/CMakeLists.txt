if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.16)
    project(go_input_tests C CXX)

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

# ---------------------------------------------------------------------------
# TinyCBOR (pure C library, no ESP-IDF dependency)
# ---------------------------------------------------------------------------

set(TINYCBOR_DIR
    "${AIRGRADIENT_REPO_ROOT}/products/go/managed_components/espressif__cbor/tinycbor/src")

add_library(tinycbor STATIC
    "${TINYCBOR_DIR}/cborencoder.c"
    "${TINYCBOR_DIR}/cborencoder_close_container_checked.c"
    "${TINYCBOR_DIR}/cborencoder_float.c"
    "${TINYCBOR_DIR}/cborerrorstrings.c"
    "${TINYCBOR_DIR}/cborparser.c"
    "${TINYCBOR_DIR}/cborparser_dup_string.c"
    "${TINYCBOR_DIR}/cborparser_float.c"
    "${TINYCBOR_DIR}/cborvalidation.c"
)

target_include_directories(tinycbor PUBLIC "${TINYCBOR_DIR}")

# ---------------------------------------------------------------------------

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

target_compile_definitions(go_input_test_support PUBLIC TEST_HOST)

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

# ---------------------------------------------------------------------------
# go_power tests
# ---------------------------------------------------------------------------

add_library(go_power_test_support
    "${AIRGRADIENT_REPO_ROOT}/products/go/main/go_power.cpp"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-common/common.cpp"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-common/rtos.cpp"
)

target_include_directories(go_power_test_support PUBLIC
    "${AIRGRADIENT_REPO_ROOT}/products/go/main"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-bms"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-common/include"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-gpio/hal"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-config/hal"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-gps"
)

target_compile_definitions(go_power_test_support PUBLIC
    TEST_HOST
)

add_executable(go_power_tests
    go_power.tests.cpp
)

target_link_libraries(go_power_tests PRIVATE
    go_power_test_support
    Catch2::Catch2WithMain
    trompeloeil::trompeloeil
)

catch_discover_tests(go_power_tests)

# ---------------------------------------------------------------------------
# go_ui tests
# ---------------------------------------------------------------------------

add_library(go_ui_test_support
    "${AIRGRADIENT_REPO_ROOT}/products/go/main/go_ui.cpp"
)

target_include_directories(go_ui_test_support PUBLIC
    "${AIRGRADIENT_REPO_ROOT}/products/go/main"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-common/include"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-config/hal"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-gps"
)

target_compile_definitions(go_ui_test_support PUBLIC
    TEST_HOST
)

add_executable(go_ui_tests
    go_ui.tests.cpp
)

target_link_libraries(go_ui_tests PRIVATE
    go_ui_test_support
    Catch2::Catch2WithMain
)

catch_discover_tests(go_ui_tests)

# ---------------------------------------------------------------------------
# go_orchestrator tests
# ---------------------------------------------------------------------------

add_library(go_orchestrator_test_support
    "${AIRGRADIENT_REPO_ROOT}/products/go/main/go_orchestrator.cpp"
    "${AIRGRADIENT_REPO_ROOT}/products/go/main/go_ui.cpp"
    "${AIRGRADIENT_REPO_ROOT}/products/go/main/go_settings.cpp"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-common/common.cpp"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-common/rtos.cpp"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-payload-cache/services/payload_cache.cpp"
    go_orchestrator_stubs.cpp
)

target_include_directories(go_orchestrator_test_support PUBLIC
    "${AIRGRADIENT_REPO_ROOT}/products/go/main"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-common/include"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-config/hal"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-gps"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-gps/types"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-bms"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-bms/types"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-sensors"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-sensors/hal"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-nand-storage/hal"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-payload-cache"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-touch/hal"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-gpio/hal"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-ble"
)

target_compile_definitions(go_orchestrator_test_support PUBLIC
    TEST_HOST
    CONFIG_PAYLOAD_CACHE_TYPE_AGO=1
    CONFIG_PAYLOAD_CACHE_MAX_SIZE=16
)

add_executable(go_orchestrator_tests
    go_orchestrator.tests.cpp
)

target_link_libraries(go_orchestrator_tests PRIVATE
    go_orchestrator_test_support
    Catch2::Catch2WithMain
    trompeloeil::trompeloeil
)

catch_discover_tests(go_orchestrator_tests)

# ---------------------------------------------------------------------------
# go_ble tests
# ---------------------------------------------------------------------------

add_library(go_ble_test_support
    "${AIRGRADIENT_REPO_ROOT}/products/go/main/go_ble.cpp"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-common/common.cpp"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-common/rtos.cpp"
)

target_include_directories(go_ble_test_support PUBLIC
    "${AIRGRADIENT_REPO_ROOT}/products/go/main"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-common/include"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-ble/hal"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-ble"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-config/hal"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-gps"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-gps/types"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-bms"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-bms/types"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-nand-storage/hal"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-payload-cache"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-gpio/hal"
)

target_link_libraries(go_ble_test_support PUBLIC tinycbor)

target_compile_definitions(go_ble_test_support PUBLIC
    TEST_HOST
    CONFIG_PAYLOAD_CACHE_TYPE_AGO=1
    CONFIG_PAYLOAD_CACHE_MAX_SIZE=16
)

add_executable(go_ble_tests
    go_ble.tests.cpp
)

target_link_libraries(go_ble_tests PRIVATE
    go_ble_test_support
    Catch2::Catch2WithMain
)

catch_discover_tests(go_ble_tests)
