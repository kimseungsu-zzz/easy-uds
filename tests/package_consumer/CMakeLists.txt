cmake_minimum_required(VERSION 3.20)
project(easy_uds_package_consumer LANGUAGES CXX)

find_package(easy_uds 1.0.0 CONFIG REQUIRED)

add_executable(easy_uds_package_consumer main.cpp)
target_link_libraries(easy_uds_package_consumer PRIVATE easy_uds::easy_uds)

# Compile every installed header in an otherwise empty translation unit. The
# sources stay generated in the consumer build tree instead of creating a set
# of tiny checked-in files solely for include-order coverage.
set(EASY_UDS_PUBLIC_HEADERS
    easy_uds
    client
    error
    fd
    options
    peer_credentials
    posix
    request
    request_context
    response
    server
    session
    simple
    stats
    stream
    version
)
foreach(header IN LISTS EASY_UDS_PUBLIC_HEADERS)
    set(source "${CMAKE_CURRENT_BINARY_DIR}/header_${header}.cpp")
    file(GENERATE OUTPUT "${source}" CONTENT "#include <easy_uds/${header}.hpp>\n")
    add_library("easy_uds_header_${header}" OBJECT "${source}")
    target_link_libraries("easy_uds_header_${header}" PRIVATE easy_uds::easy_uds)
endforeach()
