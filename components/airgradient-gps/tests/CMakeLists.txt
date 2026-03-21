if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.16)
    project(airgradient_gps_tests CXX C)

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

# ---------------------------------------------------------------------------
# libnmea — built natively, replicating the COMPILE_DEFINITIONS rename trick
# from components/libnmea-esp32/CMakeLists.txt so that each parser's
# static functions (init, parse, allocate_data, free_data, set_default)
# are renamed to nmea_<parser>_<fn> without symbol conflicts.
# ---------------------------------------------------------------------------
set(LIBNMEA_ESP32 "${AIRGRADIENT_REPO_ROOT}/components/libnmea-esp32")
set(LIBNMEA       "${LIBNMEA_ESP32}/libnmea")

add_library(nmea_native STATIC
    "${LIBNMEA}/src/nmea/nmea.c"
    "${LIBNMEA}/src/nmea/parser_static.c"
    "${LIBNMEA}/src/parsers/parse.c"
)

target_include_directories(nmea_native PUBLIC
    "${LIBNMEA}/src/nmea"
    "${LIBNMEA}/src/parsers"
)

set(LIBNMEA_PARSERS gpgga gpgll gprmc gpgsa gpgsv gptxt gpvtg)

foreach(parser ${LIBNMEA_PARSERS})
    set(src_file "${LIBNMEA}/src/parsers/${parser}.c")
    target_sources(nmea_native PRIVATE "${src_file}")

    set(prefix "nmea_${parser}_")
    set(defs
        "allocate_data=${prefix}allocate_data"
        "free_data=${prefix}free_data"
        "init=${prefix}init"
        "parse=${prefix}parse"
        "set_default=${prefix}set_default"
    )
    set_source_files_properties("${src_file}" PROPERTIES COMPILE_DEFINITIONS "${defs}")

    string(TOUPPER "${parser}" parser_upper)
    target_compile_definitions(nmea_native PUBLIC "ENABLE_${parser_upper}=1")
endforeach()

list(LENGTH LIBNMEA_PARSERS nmea_parser_count)
target_compile_definitions(nmea_native PUBLIC "PARSER_COUNT=${nmea_parser_count}")
target_compile_options(nmea_native PRIVATE "-Wno-strict-prototypes")

# ---------------------------------------------------------------------------
# GPS component support library
# ---------------------------------------------------------------------------
add_library(gps_test_support STATIC
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-gps/drivers/nmea_gps/nmea_gps.cpp"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-serial/airgradient_serial.cpp"
)

target_include_directories(gps_test_support PUBLIC
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-gps"
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-serial/include"
    "${LIBNMEA}/src/nmea"
    "${LIBNMEA}/src/parsers"
)

target_compile_definitions(gps_test_support PUBLIC TEST_HOST)
target_link_libraries(gps_test_support PUBLIC nmea_native)

# ---------------------------------------------------------------------------
# Test executable
# ---------------------------------------------------------------------------
add_executable(airgradient_gps_tests
    gps_component.tests.cpp
    nmea_gps.tests.cpp
)

target_include_directories(airgradient_gps_tests PRIVATE
    "${AIRGRADIENT_REPO_ROOT}/components/airgradient-gps"
)

target_link_libraries(airgradient_gps_tests PRIVATE
    gps_test_support
    Catch2::Catch2WithMain
    trompeloeil::trompeloeil
)

catch_discover_tests(airgradient_gps_tests)
