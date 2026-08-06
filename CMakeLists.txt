cmake_minimum_required(VERSION 3.20)
project(easy-uds VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# ===== 옵션 정의 (빠진 부분 추가) =====
option(EASY_UDS_BUILD_EXAMPLES "Build easy-uds examples" ON)
option(EASY_UDS_BUILD_TESTS "Build easy-uds tests" OFF)  # ← 추가됨

# ===== 소스 파일 자동 수집 (링크 에러 방지) =====
file(GLOB SRC_FILES src/*.cpp)
add_library(easy-uds ${SRC_FILES})
add_library(easy-uds::easy-uds ALIAS easy-uds)

# ===== C++17 및 include 경로 설정 =====
target_compile_features(easy-uds PUBLIC cxx_std_17)
target_include_directories(
    easy-uds
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

# ===== 컴파일러 워닝 설정 =====
if(MSVC)
    target_compile_options(easy-uds PRIVATE /W4 /permissive-)
else()
    target_compile_options(easy-uds PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ===== Windows 소켓 링크 =====
if(WIN32)
    target_link_libraries(easy-uds PRIVATE ws2_32)
endif()

# ===== 예제 빌드 =====
if(EASY_UDS_BUILD_EXAMPLES)
    add_executable(easy-uds-server examples/server.cpp)
    target_link_libraries(easy-uds-server PRIVATE easy-uds::easy-uds)

    add_executable(easy-uds-client examples/client.cpp)
    target_link_libraries(easy-uds-client PRIVATE easy-uds::easy-uds)
endif()

# ===== 테스트 빌드 (옵션 추가됨) =====
if(EASY_UDS_BUILD_TESTS)
    enable_testing()
    add_executable(easy-uds-tests tests/easy_uds_test.cpp)
    target_link_libraries(easy-uds-tests PRIVATE easy-uds::easy-uds)
    add_test(NAME easy-uds-tests COMMAND ${CMAKE_CURRENT_BINARY_DIR}/easy-uds-tests)
    set_tests_properties(easy-uds-tests PROPERTIES TIMEOUT 15)
endif()

# ===== 설치 규칙 =====
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

# ===== 패키지 설정 파일 (Config 파일이 실제로 존재할 경우만) =====
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/easy-udsConfig.cmake.in")
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
endif()