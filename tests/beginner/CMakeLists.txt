cmake_minimum_required(VERSION 3.20)
project(easy_uds_beginner_consumer LANGUAGES CXX)

find_package(easy_uds CONFIG REQUIRED)

add_executable(easy_uds_beginner_server server.cpp)
target_link_libraries(easy_uds_beginner_server PRIVATE easy_uds::easy_uds)

add_executable(easy_uds_beginner_client client.cpp)
target_link_libraries(easy_uds_beginner_client PRIVATE easy_uds::easy_uds)
