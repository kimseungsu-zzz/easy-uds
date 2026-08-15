cmake_minimum_required(VERSION 3.20)

project(easy_uds VERSION 0.7.0 LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "easy-uds currently requires Linux (epoll and SO_PEERCRED)")
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
option(EASY_UDS_BUILD_BENCHMARKS "Build RPC, allocation, and streaming benchmarks" OFF)
option(EASY_UDS_BUILD_EXPERIMENTS "Build standalone UDS/io_uring experiment probes" OFF)
option(EASY_UDS_BUILD_SIMPLE_EXPERIMENTS
    "Build Simple API experiment and diagnostic targets" OFF)
option(EASY_UDS_TRACE_SPIN_MISS "Count client session spin-window misses (diagnostic)" OFF)
option(EASY_UDS_TRACE_SESSION_CONTENTION
    "Measure client shared-session lock contention (diagnostic)" OFF)
set(EASY_UDS_SESSION_SPIN_US "100" CACHE STRING
    "Build-time Session response spin window in microseconds (0 disables spinning)")
if(NOT EASY_UDS_SESSION_SPIN_US MATCHES "^[0-9]+$")
    message(FATAL_ERROR "EASY_UDS_SESSION_SPIN_US must be a non-negative integer")
endif()
set(EASY_UDS_SESSION_INFLIGHT_SHARDS "16" CACHE STRING
    "Client Session in-flight table shard count (1-64)")
if(NOT EASY_UDS_SESSION_INFLIGHT_SHARDS MATCHES "^[1-9][0-9]*$" OR
   EASY_UDS_SESSION_INFLIGHT_SHARDS GREATER 64)
    message(FATAL_ERROR "EASY_UDS_SESSION_INFLIGHT_SHARDS must be an integer from 1 to 64")
endif()

find_package(Threads REQUIRED)

add_library(easy_uds
    src/system/core/error.cpp
    src/system/runtime/server.cpp
    src/system/runtime/client.cpp
    src/system/runtime/session.cpp
    src/system/runtime/trace.hpp
    src/system/transport/transport.cpp
    src/system/transport/transport.hpp
    src/system/reactor/dispatch.cpp
    src/system/reactor/event_loop.cpp
    src/system/reactor/flow_control.cpp
    src/system/reactor/output.cpp
    src/system/reactor/parser.cpp
    src/system/reactor/stream.cpp
    src/system/reactor/workers.cpp
    src/system/reactor/common.hpp
    src/system/reactor/core.hpp
    src/system/reactor/parser.hpp
    src/system/reactor/stream_io.hpp
    src/system/protocol/codec.hpp
    src/system/transport/io.hpp
)
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
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/user/cpp/core>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/user/cpp/simple>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(easy_uds PUBLIC Threads::Threads)
target_compile_definitions(easy_uds PRIVATE
    EASY_UDS_SESSION_SPIN_US=${EASY_UDS_SESSION_SPIN_US}
    EASY_UDS_SESSION_INFLIGHT_SHARDS=${EASY_UDS_SESSION_INFLIGHT_SHARDS}
    $<$<BOOL:${EASY_UDS_TRACE_SPIN_MISS}>:EASY_UDS_TRACE_SPIN_MISS>
    $<$<BOOL:${EASY_UDS_TRACE_SESSION_CONTENTION}>:EASY_UDS_TRACE_SESSION_CONTENTION>
)

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

    add_executable(easy_uds_streaming_server_example
        examples/streaming_server.cpp)
    target_link_libraries(easy_uds_streaming_server_example PRIVATE
        easy_uds::easy_uds)

    add_executable(easy_uds_streaming_client_example
        examples/streaming_client.cpp)
    target_link_libraries(easy_uds_streaming_client_example PRIVATE
        easy_uds::easy_uds)

    foreach(example_target IN ITEMS
            easy_uds_server_example
            easy_uds_client_example
            easy_uds_streaming_server_example
            easy_uds_streaming_client_example)
        target_compile_options(${example_target} PRIVATE
            $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>
        )
        if(EASY_UDS_WARNINGS_AS_ERRORS)
            target_compile_options(${example_target} PRIVATE
                $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Werror>
            )
        endif()
    endforeach()

    add_executable(easy_uds_robot_hal_server_example
        examples/robot_hal_server.cpp)
    target_link_libraries(easy_uds_robot_hal_server_example PRIVATE
        easy_uds::easy_uds)
    target_compile_options(easy_uds_robot_hal_server_example PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>
    )
    if(EASY_UDS_WARNINGS_AS_ERRORS)
        target_compile_options(easy_uds_robot_hal_server_example PRIVATE
            $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Werror>
        )
    endif()
endif()

if(EASY_UDS_BUILD_TESTS)
    include(CTest)
    if(BUILD_TESTING)
        add_executable(easy_uds_test
            tests/easy_uds_test/main.cpp
            tests/easy_uds_test/common.cpp
            tests/easy_uds_test/server_lifecycle.cpp
            tests/easy_uds_test/session.cpp
            tests/easy_uds_test/errors.cpp
            tests/easy_uds_test/request_context.cpp
            tests/easy_uds_test/stats.cpp
            tests/easy_uds_test/streaming.cpp
            tests/easy_uds_test/serialized.cpp
            tests/easy_uds_test/timeouts_concurrency.cpp
            tests/easy_uds_test/backpressure.cpp
            tests/easy_uds_test/shutdown.cpp
            tests/easy_uds_test/budgets.cpp
            tests/easy_uds_test/fd_passing.cpp
        )
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
        set_tests_properties(easy_uds.unit PROPERTIES
            TIMEOUT 20
            LABELS "rc;unit;integration;session;streaming;fd;serialized;latestwins;rejectifbusy;stats;budget;shutdown")

        add_executable(easy_uds_rc_adversarial
            tests/easy_uds_test/common.cpp
            tests/easy_uds_test/rc_adversarial.cpp
            tests/easy_uds_test/rc_main.cpp
        )
        target_link_libraries(easy_uds_rc_adversarial PRIVATE easy_uds::easy_uds)
        target_compile_options(easy_uds_rc_adversarial PRIVATE
            $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>
        )
        if(EASY_UDS_WARNINGS_AS_ERRORS)
            target_compile_options(easy_uds_rc_adversarial PRIVATE
                $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Werror>
            )
        endif()
        add_test(NAME easy_uds.rc_adversarial COMMAND easy_uds_rc_adversarial)
        set_tests_properties(easy_uds.rc_adversarial PROPERTIES
            TIMEOUT 20
            LABELS "rc;adversarial;session;serialized;latestwins;stats;shutdown")

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
        set_tests_properties(easy_uds.stress PROPERTIES
            TIMEOUT 30
            LABELS "rc;stress;session;streaming;shutdown;budget")

        find_program(EASY_UDS_BASH_EXECUTABLE bash)
        if(EASY_UDS_BASH_EXECUTABLE)
            add_test(NAME easy_uds.compile_error_ux
                COMMAND ${CMAKE_COMMAND} -E env
                    "CXX=${CMAKE_CXX_COMPILER}"
                    "${EASY_UDS_BASH_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/compile_error_smoke.sh")
            set_tests_properties(easy_uds.compile_error_ux PROPERTIES
                TIMEOUT 30
                LABELS "rc;compile-error;api")
        endif()
    endif()
endif()

if(EASY_UDS_BUILD_BENCHMARKS)
    add_executable(easy_uds_stream_benchmark tests/easy_uds_stream_benchmark.cpp)
    target_link_libraries(easy_uds_stream_benchmark PRIVATE easy_uds::easy_uds)
    add_executable(easy_uds_rpc_benchmark tests/easy_uds_rpc_benchmark.cpp)
    target_link_libraries(easy_uds_rpc_benchmark PRIVATE easy_uds::easy_uds)
    add_executable(easy_uds_session_benchmark tests/easy_uds_session_benchmark.cpp)
    target_link_libraries(easy_uds_session_benchmark PRIVATE easy_uds::easy_uds)
    target_compile_definitions(easy_uds_session_benchmark PRIVATE
        $<$<BOOL:${EASY_UDS_TRACE_SPIN_MISS}>:EASY_UDS_TRACE_SPIN_MISS>
        $<$<BOOL:${EASY_UDS_TRACE_SESSION_CONTENTION}>:EASY_UDS_TRACE_SESSION_CONTENTION>
    )
    add_executable(easy_uds_allocation_benchmark tests/easy_uds_allocation_benchmark.cpp)
    target_link_libraries(easy_uds_allocation_benchmark PRIVATE easy_uds::easy_uds)
    target_compile_options(easy_uds_stream_benchmark PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>
    )
    target_compile_options(easy_uds_rpc_benchmark PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>
    )
    target_compile_options(easy_uds_session_benchmark PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>
    )
    target_compile_options(easy_uds_allocation_benchmark PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>
    )
    if(EASY_UDS_WARNINGS_AS_ERRORS)
        target_compile_options(easy_uds_stream_benchmark PRIVATE
            $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Werror>
        )
        target_compile_options(easy_uds_rpc_benchmark PRIVATE
            $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Werror>
        )
        target_compile_options(easy_uds_session_benchmark PRIVATE
            $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Werror>
        )
        target_compile_options(easy_uds_allocation_benchmark PRIVATE
            $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Werror>
        )
    endif()
endif()

if(EASY_UDS_BUILD_EXPERIMENTS)
    set(EASY_UDS_EXPERIMENT_TARGETS
        easy_uds_fd_passing_probe
        easy_uds_io_uring_probe
        easy_uds_false_sharing_probe
        easy_uds_zero_copy_probe
        easy_uds_shm_transport_probe
        easy_uds_shm_framed_probe
        easy_uds_shm_process_probe
    )
    add_executable(easy_uds_fd_passing_probe
        experiments/0.6/easy_uds_fd_passing_probe.cpp)
    add_executable(easy_uds_io_uring_probe
        experiments/0.6/easy_uds_io_uring_probe.cpp)
    add_executable(easy_uds_false_sharing_probe
        experiments/0.6/easy_uds_false_sharing_probe.cpp)
    add_executable(easy_uds_zero_copy_probe
        experiments/0.6/easy_uds_zero_copy_probe.cpp)
    add_executable(easy_uds_shm_transport_probe
        experiments/0.6/easy_uds_shm_transport_probe.cpp)
    add_executable(easy_uds_shm_framed_probe
        experiments/0.6/easy_uds_shm_framed_probe.cpp
        src/system/core/error.cpp
    )
    add_executable(easy_uds_shm_process_probe
        experiments/0.6/easy_uds_shm_process_probe.cpp
        src/system/core/error.cpp
    )
    target_include_directories(easy_uds_shm_framed_probe PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/system
        ${CMAKE_CURRENT_SOURCE_DIR}/src/user/cpp/core
        ${CMAKE_CURRENT_SOURCE_DIR}/src/user/cpp/simple
    )
    target_include_directories(easy_uds_shm_process_probe PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/system
        ${CMAKE_CURRENT_SOURCE_DIR}/src/user/cpp/core
        ${CMAKE_CURRENT_SOURCE_DIR}/src/user/cpp/simple
    )
    find_library(EASY_UDS_LIBURING uring)
    find_path(EASY_UDS_LIBURING_INCLUDE_DIR liburing.h)
    if(EASY_UDS_LIBURING AND EASY_UDS_LIBURING_INCLUDE_DIR)
        add_executable(easy_uds_io_uring_echo_probe
            experiments/0.6/easy_uds_io_uring_echo_probe.cpp)
        target_include_directories(easy_uds_io_uring_echo_probe PRIVATE
            ${EASY_UDS_LIBURING_INCLUDE_DIR}
        )
        target_compile_features(easy_uds_io_uring_echo_probe PRIVATE cxx_std_17)
        target_compile_options(easy_uds_io_uring_echo_probe PRIVATE
            $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>
        )
        target_link_libraries(easy_uds_io_uring_echo_probe PRIVATE ${EASY_UDS_LIBURING})
        list(APPEND EASY_UDS_EXPERIMENT_TARGETS easy_uds_io_uring_echo_probe)
    else()
        message(STATUS "liburing library/header not found; easy_uds_io_uring_echo_probe disabled")
    endif()
    foreach(experiment_target IN LISTS EASY_UDS_EXPERIMENT_TARGETS)
        target_compile_features(${experiment_target} PRIVATE cxx_std_17)
        target_compile_options(${experiment_target} PRIVATE
            $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>
        )
        if(EASY_UDS_WARNINGS_AS_ERRORS)
            target_compile_options(${experiment_target} PRIVATE
                $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Werror>
            )
        endif()
    endforeach()
endif()

if(EASY_UDS_BUILD_SIMPLE_EXPERIMENTS)
    add_executable(easy_uds_simple_server_experiment
        experiments/simple_api/simple_server.cpp)
    target_include_directories(easy_uds_simple_server_experiment PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/experiments/simple_api)
    target_link_libraries(easy_uds_simple_server_experiment PRIVATE
        easy_uds::easy_uds)

    add_executable(easy_uds_simple_client_experiment
        experiments/simple_api/simple_client.cpp)
    target_include_directories(easy_uds_simple_client_experiment PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/experiments/simple_api)
    target_link_libraries(easy_uds_simple_client_experiment PRIVATE
        easy_uds::easy_uds)

    add_executable(easy_uds_simple_api_benchmark
        experiments/simple_api/simple_benchmark.cpp)
    target_include_directories(easy_uds_simple_api_benchmark PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/experiments/simple_api)
    target_compile_definitions(easy_uds_simple_api_benchmark PRIVATE
        EASY_UDS_SIMPLE_BENCHMARK=1)
    target_link_libraries(easy_uds_simple_api_benchmark PRIVATE
        easy_uds::easy_uds)

    add_executable(easy_uds_core_api_benchmark
        experiments/simple_api/simple_benchmark.cpp)
    target_include_directories(easy_uds_core_api_benchmark PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/experiments/simple_api)
    target_link_libraries(easy_uds_core_api_benchmark PRIVATE
        easy_uds::easy_uds)

    add_executable(easy_uds_simple_api_allocation_benchmark
        experiments/simple_api/simple_allocation_benchmark.cpp)
    target_compile_definitions(easy_uds_simple_api_allocation_benchmark PRIVATE
        EASY_UDS_SIMPLE_BENCHMARK=1)
    target_link_libraries(easy_uds_simple_api_allocation_benchmark PRIVATE
        easy_uds::easy_uds)

    add_executable(easy_uds_core_api_allocation_benchmark
        experiments/simple_api/simple_allocation_benchmark.cpp)
    target_link_libraries(easy_uds_core_api_allocation_benchmark PRIVATE
        easy_uds::easy_uds)

    foreach(simple_target IN ITEMS
            easy_uds_simple_server_experiment
            easy_uds_simple_client_experiment
            easy_uds_simple_api_benchmark
            easy_uds_core_api_benchmark
            easy_uds_simple_api_allocation_benchmark
            easy_uds_core_api_allocation_benchmark)
        target_compile_features(${simple_target} PRIVATE cxx_std_17)
        target_compile_options(${simple_target} PRIVATE
            $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>
        )
        if(EASY_UDS_WARNINGS_AS_ERRORS)
            target_compile_options(${simple_target} PRIVATE
                $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Werror>
            )
        endif()
    endforeach()

    if(EASY_UDS_BUILD_TESTS AND BUILD_TESTING)
        add_executable(easy_uds_simple_api_experiment_test
            experiments/simple_api/simple_api_test.cpp)
        target_include_directories(easy_uds_simple_api_experiment_test PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/experiments/simple_api)
        target_link_libraries(easy_uds_simple_api_experiment_test PRIVATE
            easy_uds::easy_uds)
        target_compile_features(easy_uds_simple_api_experiment_test PRIVATE cxx_std_17)
        target_compile_options(easy_uds_simple_api_experiment_test PRIVATE
            $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>
        )
        if(EASY_UDS_WARNINGS_AS_ERRORS)
            target_compile_options(easy_uds_simple_api_experiment_test PRIVATE
                $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Werror>
            )
        endif()
        add_test(NAME easy_uds.simple_api_experiment
            COMMAND easy_uds_simple_api_experiment_test)
        set_tests_properties(easy_uds.simple_api_experiment PROPERTIES
            TIMEOUT 20
            LABELS "experimental;simple;api")

        find_program(EASY_UDS_SIMPLE_BASH_EXECUTABLE bash)
        if(EASY_UDS_SIMPLE_BASH_EXECUTABLE)
            add_test(NAME easy_uds.simple_compile_error
                COMMAND ${CMAKE_COMMAND} -E env
                    "CXX=${CMAKE_CXX_COMPILER}"
                    "${EASY_UDS_SIMPLE_BASH_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/experiments/simple_api/compile_error_smoke.sh")
            set_tests_properties(easy_uds.simple_compile_error PROPERTIES
                TIMEOUT 30
                LABELS "experimental;simple;compile-error")
        endif()
    endif()
endif()

if(EASY_UDS_BUILD_FUZZERS)
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        message(FATAL_ERROR "EASY_UDS_BUILD_FUZZERS requires upstream Clang with libFuzzer")
    endif()

    add_executable(easy_uds_protocol_fuzz
        tests/protocol_fuzz.cpp
        src/system/core/error.cpp
    )
    target_include_directories(easy_uds_protocol_fuzz PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/system
        ${CMAKE_CURRENT_SOURCE_DIR}/src/user/cpp/core
        ${CMAKE_CURRENT_SOURCE_DIR}/src/user/cpp/simple
    )
    target_compile_features(easy_uds_protocol_fuzz PRIVATE cxx_std_17)
    target_compile_options(easy_uds_protocol_fuzz PRIVATE
        -Wall -Wextra -Wpedantic -Werror
        -fsanitize=fuzzer,address,undefined
        -fno-omit-frame-pointer
    )
    target_link_options(easy_uds_protocol_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)

    add_executable(easy_uds_session_fuzz tests/easy_uds_session_fuzz.cpp)
    target_link_libraries(easy_uds_session_fuzz PRIVATE easy_uds::easy_uds)
    target_compile_features(easy_uds_session_fuzz PRIVATE cxx_std_17)
    target_compile_options(easy_uds_session_fuzz PRIVATE
        -Wall -Wextra -Wpedantic -Werror
        -fsanitize=fuzzer,address,undefined
        -fno-omit-frame-pointer
    )
    target_link_options(easy_uds_session_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
endif()

install(TARGETS easy_uds
    EXPORT easy_udsTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(DIRECTORY src/user/cpp/core/easy_uds/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/easy_uds)
install(DIRECTORY src/user/cpp/simple/easy_uds/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/easy_uds)

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
