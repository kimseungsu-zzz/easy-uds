cmake_minimum_required(VERSION 3.20)

project(easy_uds VERSION 0.4.1 LANGUAGES CXX)

if(NOT UNIX)
    message(FATAL_ERROR "easy-uds currently requires POSIX Unix-domain socket APIs")
endif()

include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(EASY_UDS_TOP_LEVEL ON)
else()
    set(EASY_UDS_TOP_LEVEL OFF)
endif()

option(EASY_UDS_BUILD_EXAMPLES "Build easy-uds examples" ${EASY_UDS_TOP_LEVEL})
option(EASY_UDS_BUILD_TESTS "Build easy-uds tests" ${EASY_UDS_TOP_LEVEL})
option(EASY_UDS_WARNINGS_AS_ERRORS "Treat easy-uds compiler warnings as errors" OFF)
option(EASY_UDS_BUILD_FUZZERS "Build libFuzzer protocol targets (Clang only)" OFF)
option(EASY_UDS_BUILD_BENCHMARKS "Build RPC and streaming throughput benchmarks" OFF)

find_package(Threads REQUIRED)

add_library(easy_uds src/easy_uds.cpp src/protocol_internal.hpp)
add_library(easy_uds::easy_uds ALIAS easy_uds)

if(PROJECT_VERSION_MAJOR EQUAL 0)
    set(EASY_UDS_SOVERSION "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}")
else()
    set(EASY_UDS_SOVERSION "${PROJECT_VERSION_MAJOR}")
endif()

set_target_properties(easy_uds PROPERTIES
    OUTPUT_NAME "easy-uds"
    EXPORT_NAME easy_uds
    VERSION ${PROJECT_VERSION}
    SOVERSION ${EASY_UDS_SOVERSION}
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)

target_compile_features(easy_uds PUBLIC cxx_std_17)
target_include_directories(easy_uds
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(easy_uds PUBLIC Threads::Threads)

target_compile_options(easy_uds PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>
)
if(EASY_UDS_WARNINGS_AS_ERRORS)
    target_compile_options(easy_uds PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Werror>
    )
endif()

if(EASY_UDS_BUILD_EXAMPLES)
    add_executable(easy_uds_server_example examples/server.cpp)
    target_link_libraries(easy_uds_server_example PRIVATE easy_uds::easy_uds)

    add_executable(easy_uds_client_example examples/client.cpp)
    target_link_libraries(easy_uds_client_example PRIVATE easy_uds::easy_uds)
endif()

if(EASY_UDS_BUILD_TESTS)
    include(CTest)
    if(BUILD_TESTING)
        add_executable(easy_uds_test tests/easy_uds_test.cpp)
        target_link_libraries(easy_uds_test PRIVATE easy_uds::easy_uds)
        target_compile_options(easy_uds_test PRIVATE
            $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>
        )
        if(EASY_UDS_WARNINGS_AS_ERRORS)
            target_compile_options(easy_uds_test PRIVATE
                $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Werror>
            )
        endif()
        add_test(NAME easy_uds.unit COMMAND easy_uds_test)
        set_tests_properties(easy_uds.unit PROPERTIES TIMEOUT 20)

        add_executable(easy_uds_stress_test tests/easy_uds_stress_test.cpp)
        target_link_libraries(easy_uds_stress_test PRIVATE easy_uds::easy_uds)
        target_compile_options(easy_uds_stress_test PRIVATE
            $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>
        )
        if(EASY_UDS_WARNINGS_AS_ERRORS)
            target_compile_options(easy_uds_stress_test PRIVATE
                $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Werror>
            )
        endif()
        add_test(NAME easy_uds.stress COMMAND easy_uds_stress_test)
        set_tests_properties(easy_uds.stress PROPERTIES TIMEOUT 30)
    endif()
endif()

if(EASY_UDS_BUILD_BENCHMARKS)
    add_executable(easy_uds_stream_benchmark tests/easy_uds_stream_benchmark.cpp)
    target_link_libraries(easy_uds_stream_benchmark PRIVATE easy_uds::easy_uds)
    add_executable(easy_uds_rpc_benchmark tests/easy_uds_rpc_benchmark.cpp)
    target_link_libraries(easy_uds_rpc_benchmark PRIVATE easy_uds::easy_uds)
    target_compile_options(easy_uds_stream_benchmark PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>
    )
    target_compile_options(easy_uds_rpc_benchmark PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>
    )
    if(EASY_UDS_WARNINGS_AS_ERRORS)
        target_compile_options(easy_uds_stream_benchmark PRIVATE
            $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Werror>
        )
        target_compile_options(easy_uds_rpc_benchmark PRIVATE
            $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Werror>
        )
    endif()
endif()

if(EASY_UDS_BUILD_FUZZERS)
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        message(FATAL_ERROR "EASY_UDS_BUILD_FUZZERS requires upstream Clang with libFuzzer")
    endif()

    add_executable(easy_uds_protocol_fuzz tests/protocol_fuzz.cpp)
    target_include_directories(easy_uds_protocol_fuzz PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_compile_features(easy_uds_protocol_fuzz PRIVATE cxx_std_17)
    target_compile_options(easy_uds_protocol_fuzz PRIVATE
        -Wall -Wextra -Wpedantic -Werror
        -fsanitize=fuzzer,address,undefined
        -fno-omit-frame-pointer
    )
    target_link_options(easy_uds_protocol_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
endif()

install(TARGETS easy_uds
    EXPORT easy_udsTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

configure_package_config_file(
    cmake/easy_udsConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/easy_udsConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/easy_uds
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/easy_udsConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMinorVersion
)

install(EXPORT easy_udsTargets
    FILE easy_udsTargets.cmake
    NAMESPACE easy_uds::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/easy_uds
)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/easy_udsConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/easy_udsConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/easy_uds
)
install(FILES LICENSE README.md README.ko.md CHANGELOG.md SECURITY.md DESTINATION ${CMAKE_INSTALL_DOCDIR})
install(DIRECTORY docs/ DESTINATION ${CMAKE_INSTALL_DOCDIR}/docs)
