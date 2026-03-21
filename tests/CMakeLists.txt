cmake_minimum_required(VERSION 3.16)
project(my_project_tests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(AIRGRADIENT_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

# Fetch Catch2
include(FetchContent)
FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.5.0
)
FetchContent_MakeAvailable(Catch2)

# Fetch Trompeloeil
FetchContent_Declare(
    trompeloeil
    GIT_REPOSITORY https://github.com/rollbear/trompeloeil.git
    GIT_TAG v47  # or latest version
)
FetchContent_MakeAvailable(Catch2 trompeloeil)

# Enable testing and load Catch discovery helpers
list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(CTest)
include(Catch)

add_subdirectory("${AIRGRADIENT_REPO_ROOT}/components/airgradient-sensors/tests"
                 components_airgradient_sensors_tests)
add_subdirectory("${AIRGRADIENT_REPO_ROOT}/components/airgradient-payload-cache/tests"
                 components_airgradient_payload_cache_tests)
add_subdirectory("${AIRGRADIENT_REPO_ROOT}/components/airgradient-cellular/tests"
                 components_airgradient_cellular_tests)
