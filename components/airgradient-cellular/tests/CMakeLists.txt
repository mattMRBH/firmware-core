if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.16)
    project(airgradient_cellular_tests CXX)

    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)

    get_filename_component(AIRGRADIENT_REPO_ROOT
                           "${CMAKE_CURRENT_SOURCE_DIR}/../../.."
                           ABSOLUTE)

    include(FetchContent)

    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.5.0
    )
    FetchContent_MakeAvailable(Catch2)

    FetchContent_Declare(
        trompeloeil
        GIT_REPOSITORY https://github.com/rollbear/trompeloeil.git
        GIT_TAG v47
    )
    FetchContent_MakeAvailable(trompeloeil)

    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
    include(CTest)
    include(Catch)
endif()

add_library(cellular_test_support
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-cellular/services/at_command_handler.cpp"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-cellular/drivers/simcom_a7672x/simcom_a7672x.cpp"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-common/rtos.cpp"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-serial/airgradient_serial.cpp"
)

target_include_directories(cellular_test_support PUBLIC
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-cellular"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-common/include"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-gpio/hal"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-serial/include"
)

target_compile_definitions(cellular_test_support PRIVATE TEST_HOST)

add_executable(airgradient_cellular_tests
    cellular_component.tests.cpp
    at_command_handler.tests.cpp
    simcom_a7672x.tests.cpp
)

target_include_directories(airgradient_cellular_tests PRIVATE
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-cellular"
)

target_link_libraries(airgradient_cellular_tests PRIVATE
    cellular_test_support
    Catch2::Catch2WithMain
    trompeloeil::trompeloeil
)

catch_discover_tests(airgradient_cellular_tests)
