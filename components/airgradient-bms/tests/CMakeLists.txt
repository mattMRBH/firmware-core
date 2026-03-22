if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.16)
    project(airgradient_bms_tests CXX)

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
    FetchContent_MakeAvailable(Catch2)

    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
    include(CTest)
    include(Catch)
endif()

add_executable(bms_tests
    bms_types.tests.cpp
)

target_include_directories(bms_tests PRIVATE
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-bms"
)

target_compile_definitions(bms_tests PRIVATE TEST_HOST)

target_link_libraries(bms_tests PRIVATE
    Catch2::Catch2WithMain
)

catch_discover_tests(bms_tests)
