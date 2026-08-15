cmake_minimum_required(VERSION 3.20)
project(easy_uds_windows_package_consumer LANGUAGES CXX)

find_package(easy_uds CONFIG REQUIRED)

add_executable(easy_uds_windows_package_consumer main.cpp)
target_link_libraries(easy_uds_windows_package_consumer PRIVATE easy_uds::easy_uds)
