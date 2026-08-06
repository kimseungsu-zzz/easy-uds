cmake_minimum_required(VERSION 3.20)

project(easy-uds VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(EASY_UDS_BUILD_EXAMPLES "Build easy-uds examples" ON)
    - .gitignore
    - README.md

add_library(easy-uds src/easy_uds.cpp)
add_library(easy-uds::easy-uds ALIAS easy-uds)

target_compile_features(easy-uds PUBLIC cxx_std_17)
target_include_directories(
    easy-uds
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

if(MSVC)
    target_compile_options(easy-uds PRIVATE /W4 /permissive-)
else()
    target_compile_options(easy-uds PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(WIN32)
    target_link_libraries(easy-uds PRIVATE ws2_32)
endif()

if(EASY_UDS_BUILD_EXAMPLES)
    add_executable(easy-uds-server examples/server.cpp)
    target_link_libraries(easy-uds-server PRIVATE easy-uds::easy-uds)

    add_executable(easy-uds-client examples/client.cpp)
    target_link_libraries(easy-uds-client PRIVATE easy-uds::easy-uds)
endif()

if(EASY_UDS_BUILD_TESTS)
    enable_testing()
    add_executable(easy-uds-tests tests/easy_uds_test.cpp)
    target_link_libraries(easy-uds-tests PRIVATE easy-uds::easy-uds)
    add_test(NAME easy-uds-tests COMMAND ${CMAKE_CURRENT_BINARY_DIR}/easy-uds-tests)
    set_tests_properties(easy-uds-tests PROPERTIES TIMEOUT 15)
endif()

install(
    TARGETS easy-uds
    EXPORT easy-udsTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(
    EXPORT easy-udsTargets
    FILE easy-udsTargets.cmake
    NAMESPACE easy-uds::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/easy-uds
)

configure_package_config_file(
    cmake/easy-udsConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/easy-udsConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/easy-uds
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/easy-udsConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(
    FILES
        ${CMAKE_CURRENT_BINARY_DIR}/easy-udsConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/easy-udsConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/easy-uds
)

