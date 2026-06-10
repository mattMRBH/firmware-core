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
